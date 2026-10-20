#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include <pthread.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>

#include "forkpty_ohos.c"
#include "flutter_pty.h"

#include "include/dart_api.h"
#include "include/dart_api_dl.h"
#include "include/dart_native_api.h"

typedef struct PtyHandle
{
    int read_fd;
    int write_fd;
    int pid;
    bool is_pty;
    pthread_mutex_t mutex;
    bool ackRead;
} PtyHandle;

typedef struct ReadLoopOptions
{
    int fd;
    pthread_mutex_t *mutex;
    Dart_Port port;
    bool waitForReadAck;
} ReadLoopOptions;

static char *error_message = NULL;
extern const char *pty_forkpty_error(void);
extern const char *pty_slave_path(void);

static void *read_loop(void *arg)
{
    ReadLoopOptions *options = (ReadLoopOptions *)arg;
    char buffer[4096];

    while (1)
    {
        if (options->waitForReadAck)
            pthread_mutex_lock(options->mutex);

        ssize_t n = read(options->fd, buffer, sizeof(buffer));

        if (n < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;

        Dart_CObject result;
        result.type = Dart_CObject_kTypedData;
        result.value.as_typed_data.type = Dart_TypedData_kUint8;
        result.value.as_typed_data.length = n;
        result.value.as_typed_data.values = (uint8_t *)buffer;
        Dart_PostCObject_DL(options->port, &result);
    }

    free(options);
    return NULL;
}

static void start_read_thread(int fd, Dart_Port port, pthread_mutex_t *mutex, bool waitForReadAck)
{
    ReadLoopOptions *options = malloc(sizeof(ReadLoopOptions));
    if (!options) return;

    options->fd = fd;
    options->port = port;
    options->mutex = mutex;
    options->waitForReadAck = waitForReadAck;

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, &read_loop, options) != 0)
        free(options);
    pthread_attr_destroy(&attr);
}

typedef struct WaitExitOptions
{
    int pid;
    Dart_Port port;
} WaitExitOptions;

static void *wait_exit_thread(void *arg)
{
    WaitExitOptions *options = (WaitExitOptions *)arg;
    int status;
    waitpid(options->pid, &status, 0);

    int exit_code = -1;
    if (WIFEXITED(status))
        exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        exit_code = -WTERMSIG(status);

    Dart_PostInteger_DL(options->port, exit_code);
    free(options);
    return NULL;
}

static void start_wait_exit_thread(int pid, Dart_Port port)
{
    WaitExitOptions *options = malloc(sizeof(WaitExitOptions));
    if (!options) return;

    options->pid = pid;
    options->port = port;

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, &wait_exit_thread, options) != 0)
        free(options);
    pthread_attr_destroy(&attr);
}

/*
 * Build a NULL-terminated envp array.
 * If PATH is not present in the provided environment, inject a sane default.
 */
static char **build_envp(char **environment)
{
    int count = 0;
    bool has_path = false;

    if (environment)
    {
        while (environment[count])
        {
            if (strncmp(environment[count], "PATH=", 5) == 0)
                has_path = true;
            count++;
        }
    }

    /* +1 for possible PATH injection, +1 for NULL terminator */
    char **envp = malloc((count + 2) * sizeof(char *));
    if (!envp) return NULL;

    for (int i = 0; i < count; i++)
        envp[i] = environment[i];

    int idx = count;
    if (!has_path)
    {
        /* Inject a default PATH covering standard OHOS locations */
        envp[idx++] = "PATH=/bin:/usr/bin:/usr/local/bin:/system/bin";
    }
    envp[idx] = NULL;
    return envp;
}

FFI_PLUGIN_EXPORT PtyHandle *pty_create(PtyOptions *options)
{
    if (!options || !options->executable)
    {
        error_message = "invalid options";
        return NULL;
    }

    struct winsize ws;
    ws.ws_row = options->rows;
    ws.ws_col = options->cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    int ptm = -1;
    int pts = -1;

    /* pty_forkpty sets up ptm/pts but does NOT fork on OHOS */
    pid_t dummy = pty_forkpty(&ptm, &pts, NULL, &ws);
    bool is_pty = !(dummy < 0 || ptm < 0 || pts < 0);
    if (!is_pty)
    {
        PTY_LOGE("pty unavailable, falling back to pipes: %{public}s",
                 pty_forkpty_error() ? pty_forkpty_error() : "unknown");
        ptm = -1;
        pts = -1;
    }

    /* Build envp */
    char **envp = build_envp(options->environment);

    /* Build shell wrapper command when a working directory is requested */
    char *cmd = NULL;
    if (options->working_directory && strlen(options->working_directory) > 0)
    {
        /*
         * posix_spawn has no portable chdir action on OHOS.
         * Wrap with: sh -c "cd '<dir>' && exec '<exe>' [args...]"
         * Build a shell command string.
         */
        size_t cmd_len = strlen(options->working_directory) + strlen(options->executable) + 32;
        /* Add space for each argument */
        if (options->arguments)
        {
            for (int i = 1; options->arguments[i] != NULL; i++)
                cmd_len += strlen(options->arguments[i]) + 4;
        }

        cmd = malloc(cmd_len);
        if (!cmd)
        {
            error_message = "malloc failed for cmd";
            free(envp);
            if (is_pty)
            {
                close(pts);
                close(ptm);
            }
            return NULL;
        }

        snprintf(cmd, cmd_len, "cd '%s' && exec '%s'",
                 options->working_directory, options->executable);

        if (options->arguments)
        {
            for (int i = 1; options->arguments[i] != NULL; i++)
            {
                strncat(cmd, " '", cmd_len - strlen(cmd) - 1);
                strncat(cmd, options->arguments[i], cmd_len - strlen(cmd) - 1);
                strncat(cmd, "'", cmd_len - strlen(cmd) - 1);
            }
        }
    }

    pid_t pid = -1;
    int read_fd = -1;
    int write_fd = -1;

    if (is_pty)
    {
        if (cmd)
        {
            char *sh_argv[] = { options->executable, "-c", cmd, NULL };
            pid = pty_spawn(pts, pty_slave_path(), options->executable,
                            (char *const *)sh_argv, envp);
        }
        else
        {
            pid = pty_spawn(pts, pty_slave_path(), options->executable,
                            (char *const *)options->arguments,
                            envp);
        }

        /* Close slave in parent — child has its own copy via open() */
        close(pts);
        pts = -1;
        read_fd = ptm;
        write_fd = ptm;
    }
    else
    {
        int in_pipe[2] = { -1, -1 };
        int out_pipe[2] = { -1, -1 };

        if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0)
        {
            error_message = "pipe failed";
        }
        else
        {
            if (cmd)
            {
                char *sh_argv[] = { options->executable, "-c", cmd, NULL };
                pid = pipe_spawn(in_pipe[0], out_pipe[1], options->executable,
                                 (char *const *)sh_argv, envp);
            }
            else
            {
                pid = pipe_spawn(in_pipe[0], out_pipe[1], options->executable,
                                 (char *const *)options->arguments,
                                 envp);
            }

            close(in_pipe[0]);
            close(out_pipe[1]);
            read_fd = out_pipe[0];
            write_fd = in_pipe[1];
        }
    }

    free(cmd);
    free(envp);

    if (pid < 0)
    {
        const char *detail = pty_forkpty_error();
        if (detail && detail[0])
            error_message = (char *)detail;
        else
            error_message = "spawn failed";
        if (read_fd >= 0)
            close(read_fd);
        if (write_fd >= 0 && write_fd != read_fd)
            close(write_fd);
        return NULL;
    }

    PtyHandle *handle = malloc(sizeof(PtyHandle));
    if (!handle)
    {
        error_message = "malloc failed";
        close(read_fd);
        if (write_fd != read_fd)
            close(write_fd);
        return NULL;
    }

    handle->read_fd = read_fd;
    handle->write_fd = write_fd;
    handle->is_pty = is_pty;
    handle->pid = (int)pid;
    handle->ackRead = options->ackRead;
    pthread_mutex_init(&handle->mutex, NULL);

    start_read_thread(read_fd, options->stdout_port, &handle->mutex, options->ackRead);
    start_wait_exit_thread((int)pid, options->exit_port);

    return handle;
}

FFI_PLUGIN_EXPORT void pty_write(PtyHandle *handle, char *buffer, int length)
{
    if (!handle || !buffer || length <= 0) return;

    if (!handle->is_pty)
    {
        /* Without a tty there is no ICRNL, so translate CR to LF ourselves */
        for (int i = 0; i < length; i++)
        {
            char c = buffer[i] == '\r' ? '\n' : buffer[i];
            ssize_t n;
            do
            {
                n = write(handle->write_fd, &c, 1);
            } while (n < 0 && errno == EINTR);
            if (n < 0) break;
        }
        return;
    }

    ssize_t written = 0;
    while (written < length)
    {
        ssize_t n = write(handle->write_fd, buffer + written, length - written);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        written += n;
    }
}

FFI_PLUGIN_EXPORT void pty_ack_read(PtyHandle *handle)
{
    if (handle && handle->ackRead)
        pthread_mutex_unlock(&handle->mutex);
}

FFI_PLUGIN_EXPORT int pty_resize(PtyHandle *handle, int rows, int cols)
{
    if (!handle || !handle->is_pty) return -1;

    struct winsize ws;
    ws.ws_row = rows;
    ws.ws_col = cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    return ioctl(handle->read_fd, TIOCSWINSZ, &ws);
}

FFI_PLUGIN_EXPORT int pty_getpid(PtyHandle *handle)
{
    if (!handle) return -1;
    return handle->pid;
}

FFI_PLUGIN_EXPORT char *pty_error(void)
{
    return error_message;
}

FFI_PLUGIN_EXPORT int pty_get_master_fd(PtyHandle *handle)
{
    if (!handle || !handle->is_pty) return -1;
    return handle->read_fd;
}

FFI_PLUGIN_EXPORT int pty_get_foreground_pgid(PtyHandle *handle)
{
    /* pipe fallback has no tty, so there is no foreground process group */
    if (!handle || !handle->is_pty) return -1;
    pid_t pgid = tcgetpgrp(handle->read_fd);
    if (pgid < 0) return -1;
    return (int)pgid;
}

FFI_PLUGIN_EXPORT int pty_get_shell_pgid(PtyHandle *handle)
{
    if (!handle || !handle->is_pty || handle->pid <= 0) return -1;
    /* pty_spawn sets POSIX_SPAWN_SETSID, so the child leads its own group */
    return handle->pid;
}
