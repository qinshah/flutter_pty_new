#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <spawn.h>

#include <hilog/log.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "flutter_pty_new"
#define LOG_DOMAIN 0x0001

#define PTY_LOGE(fmt, ...) \
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)

extern char **environ;

/* Shared error buffer filled by pty_forkpty / pty_spawn */
static char g_pty_err[256] = {0};
static char g_pts_path[256] = {0};

const char *pty_forkpty_error(void) { return g_pty_err[0] ? g_pty_err : NULL; }
const char *pty_slave_path(void) { return g_pts_path; }

/*
 * HarmonyOS app sandbox forbids fork().
 * This function only opens the PTY master/slave pair.
 * The actual process is started by pty_spawn().
 */
pid_t pty_forkpty(
    int *master,
    int *slave,
    const struct termios *termp,
    const struct winsize *winp)
{
    g_pty_err[0] = '\0';

    int ptm = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (ptm < 0)
    {
        ptm = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (ptm < 0)
        {
            snprintf(g_pty_err, sizeof(g_pty_err),
                     "open ptmx failed: %s (errno=%d)", strerror(errno), errno);
            PTY_LOGE("%{public}s", g_pty_err);
            return -1;
        }
    }

    if (grantpt(ptm) != 0)
    {
        snprintf(g_pty_err, sizeof(g_pty_err),
                 "grantpt failed: %s (errno=%d)", strerror(errno), errno);
        PTY_LOGE("%{public}s", g_pty_err);
        close(ptm);
        return -1;
    }

    if (unlockpt(ptm) != 0)
    {
        snprintf(g_pty_err, sizeof(g_pty_err),
                 "unlockpt failed: %s (errno=%d)", strerror(errno), errno);
        PTY_LOGE("%{public}s", g_pty_err);
        close(ptm);
        return -1;
    }

    char *devname = ptsname(ptm);
    if (devname == NULL)
    {
        snprintf(g_pty_err, sizeof(g_pty_err),
                 "ptsname failed: %s (errno=%d)", strerror(errno), errno);
        PTY_LOGE("%{public}s", g_pty_err);
        close(ptm);
        return -1;
    }

    char slave_path[256];
    strncpy(slave_path, devname, sizeof(slave_path) - 1);
    slave_path[sizeof(slave_path) - 1] = '\0';
    PTY_LOGE("slave path: %{public}s", slave_path);

    int pts = open(slave_path, O_RDWR | O_NOCTTY);
    if (pts < 0)
    {
        snprintf(g_pty_err, sizeof(g_pty_err),
                 "open slave %s failed: %s (errno=%d)",
                 slave_path, strerror(errno), errno);
        PTY_LOGE("%{public}s", g_pty_err);
        close(ptm);
        return -1;
    }

    if (termp)
        tcsetattr(pts, TCSAFLUSH, termp);

    if (winp)
        ioctl(pts, TIOCSWINSZ, winp);

    *master = ptm;
    if (slave)
        *slave = pts;
    else
        close(pts);

    strncpy(g_pts_path, slave_path, sizeof(g_pts_path) - 1);
    g_pts_path[sizeof(g_pts_path) - 1] = '\0';

    PTY_LOGE("pty_forkpty ok: ptm=%{public}d pts=%{public}d", ptm, pts);
    return 0;
}

/*
 * Spawn the child process with PTY slave as stdio.
 *
 * POSIX_SPAWN_SETSID runs before the file actions, so opening the slave path
 * inside the child (rather than dup2'ing an fd opened by the parent) makes the
 * pty the controlling terminal and gives the shell job control.
 */
pid_t pty_spawn(
    int pts,
    const char *slave_path,
    const char *executable,
    char *const argv[],
    char *const envp[])
{
    PTY_LOGE("pty_spawn: exe=%{public}s pts=%{public}d", executable, pts);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (slave_path && slave_path[0])
    {
        posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, slave_path, O_RDWR, 0);
        posix_spawn_file_actions_adddup2(&fa, STDIN_FILENO, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, STDIN_FILENO, STDERR_FILENO);
    }
    else
    {
        posix_spawn_file_actions_adddup2(&fa, pts, STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&fa, pts, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, pts, STDERR_FILENO);
    }
    if (pts > STDERR_FILENO)
        posix_spawn_file_actions_addclose(&fa, pts);

    posix_spawnattr_t sa;
    posix_spawnattr_init(&sa);
    posix_spawnattr_setflags(&sa, POSIX_SPAWN_SETSID);

    pid_t pid = -1;
    int ret = posix_spawn(&pid, executable, &fa, &sa,
                          argv,
                          envp ? envp : environ);

    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&sa);

    if (ret != 0)
    {
        snprintf(g_pty_err, sizeof(g_pty_err),
                 "posix_spawn(%s) failed: %s (errno=%d)",
                 executable, strerror(ret), ret);
        PTY_LOGE("%{public}s", g_pty_err);
        return -1;
    }

    PTY_LOGE("pty_spawn ok: pid=%{public}d", (int)pid);
    return pid;
}

/*
 * Fallback spawn without a pty, for devices whose SELinux policy forbids
 * opening /dev/ptmx (e.g. retail HarmonyOS phones). The child gets plain
 * pipes as stdio and therefore has no controlling terminal.
 */
pid_t pipe_spawn(
    int child_stdin,
    int child_stdout,
    const char *executable,
    char *const argv[],
    char *const envp[])
{
    PTY_LOGE("pipe_spawn: exe=%{public}s", executable);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, child_stdin, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, child_stdout, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, child_stdout, STDERR_FILENO);
    if (child_stdin > STDERR_FILENO)
        posix_spawn_file_actions_addclose(&fa, child_stdin);
    if (child_stdout > STDERR_FILENO)
        posix_spawn_file_actions_addclose(&fa, child_stdout);

    posix_spawnattr_t sa;
    posix_spawnattr_init(&sa);

    pid_t pid = -1;
    int ret = posix_spawn(&pid, executable, &fa, &sa,
                          argv,
                          envp ? envp : environ);

    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&sa);

    if (ret != 0)
    {
        snprintf(g_pty_err, sizeof(g_pty_err),
                 "posix_spawn(%s) failed: %s (errno=%d)",
                 executable, strerror(ret), ret);
        PTY_LOGE("%{public}s", g_pty_err);
        return -1;
    }

    PTY_LOGE("pipe_spawn ok: pid=%{public}d", (int)pid);
    return pid;
}
