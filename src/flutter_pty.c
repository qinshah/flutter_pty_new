#include "flutter_pty.h"

#include "include/dart_api_dl.c"

#if _WIN32
#include "flutter_pty_win.c"
#elif defined(__OHOS__)
// HarmonyOS 沙箱禁止 fork()，走 posix_spawn 的专用实现
#include "flutter_pty_ohos.c"
#else
#include "forkpty.c"
#include "flutter_pty_unix.c"
#endif