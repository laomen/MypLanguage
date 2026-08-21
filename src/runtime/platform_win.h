/* platform_win.h — MYP runtime Windows 平台适配层（MinGW-w64）
 *
 * 里程碑 1 目标：让 src/runtime/runtime.c 在 MinGW-w64 交叉编译下通过编译。
 * 提供 runtime.c 依赖但 Windows 缺失的 POSIX 模拟：
 *   - termios（raw 模式：仅 tcgetattr/tcsetattr + c_lflag/ICANON/ECHO/VMIN/VTIME）
 *   - ioctl + TIOCGWINSZ（终端尺寸）
 *   - dirent（opendir/readdir/closedir，基于 FindFirstFile/FindNextFile）
 *   - stat 宏 S_ISDIR/S_ISREG（MinGW 缺失）
 *
 * 说明：
 *   - pthread/semaphore 由 MinGW-w64 自带的 winpthreads 提供（链接 -lpthread）。
 *   - 协程切换走 __x86_64__ 汇编分支（runtime.c 已有），不需要 ucontext.h；
 *     coro_ctx_win 汇编属里程碑 3（链接阶段），本层不涉及。
 *   - 时间 API（clock_gettime/nanosleep）若 MinGW 提供则直接用；缺失时在
 *     MYP_PLATFORM_WIN_NO_TIME 下由本文件补。
 */
#ifndef MYP_PLATFORM_WIN_H
#define MYP_PLATFORM_WIN_H

#if defined(_WIN32)

/* winsock2.h 必须先于 windows.h 包含（避免 winsock.h/winsock2.h 冲突） */
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <process.h>
#include <direct.h>   /* _mkdir/_getcwd/_chdir */
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>   /* MinGW-w64 提供 read/write/close/STDOUT_FILENO 等 */
#include <fcntl.h>
#include <pthread.h>  /* winpthreads */
#include <semaphore.h>/* winpthreads（提供 sem_t/sem_*） */
#include <stdint.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

/* ================= termios（raw 模式模拟） =================
 * runtime.c 只用到：tcgetattr/tcsetattr + c_lflag（ICANON/ECHO）+ c_cc[VMIN/VTIME]
 * Windows 等价：GetConsoleMode/SetConsoleMode（ENABLE_LINE_INPUT/ENABLE_ECHO_INPUT） */
struct termios {
    unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_cc[2];   /* [VMIN, VTIME] */
};
#define TCSANOW 0
#define ICANON 0x0002
#define ECHO   0x0008
#define VMIN   0
#define VTIME  1
static inline int tcgetattr(int fd, struct termios* t) {
    (void)fd;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD m = 0;
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (!GetConsoleMode(h, &m)) return -1;
    t->c_lflag = m;
    return 0;
}
static inline int tcsetattr(int fd, int /*action*/, const struct termios* t) {
    (void)fd;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD m = t->c_lflag;
    m &= ~((DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)); /* raw：关行缓冲+回显 */
    SetConsoleMode(h, m);
    return 0;
}

/* ================= ioctl TIOCGWINSZ（终端尺寸） ================= */
#define TIOCGWINSZ 0x5413
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
static inline int ioctl(int fd, unsigned long request, void* arg) {
    (void)fd;
    if (request == TIOCGWINSZ && arg) {
        struct winsize* w = (struct winsize*)arg;
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
            w->ws_col = (unsigned short)info.dwSize.X;
            w->ws_row = (unsigned short)info.dwSize.Y;
            return 0;
        }
    }
    return -1;
}

/* ================= dirent（目录遍历） ================= */
struct dirent { char d_name[260]; };
typedef struct {
    HANDLE h;
    WIN32_FIND_DATAA fd;
    struct dirent ent;
    int first;
} DIR;
static inline DIR* opendir(const char* path) {
    DIR* d = (DIR*)calloc(1, sizeof(DIR));
    if (!d) return NULL;
    char pattern[512];
    _snprintf(pattern, sizeof(pattern), "%s\\*", (path && path[0]) ? path : ".");
    d->h = FindFirstFileA(pattern, &d->fd);
    d->first = 1;
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    return d;
}
static inline struct dirent* readdir(DIR* d) {
    if (!d) return NULL;
    for (;;) {
        int ok = d->first ? (d->h != INVALID_HANDLE_VALUE) : FindNextFileA(d->h, &d->fd);
        d->first = 0;
        if (!ok) return NULL;
        if (strcmp(d->fd.cFileName, ".") == 0 || strcmp(d->fd.cFileName, "..") == 0)
            continue;
        _snprintf(d->ent.d_name, sizeof(d->ent.d_name), "%s", d->fd.cFileName);
        return &d->ent;
    }
}
static inline int closedir(DIR* d) {
    if (!d) return -1;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d);
    return 0;
}

/* ================= stat 宏（MinGW 缺失 S_ISDIR/S_ISREG） ================= */
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

/* ================= 文件系统小函数 ================= */
/* POSIX mkdir(path, mode) → Windows _mkdir(path) */
#define mkdir(path, mode) _mkdir(path)
/* Windows 无符号链接：lstat == stat */
#define lstat stat

/* ================= 环境变量（setenv/unsetenv → _putenv） ================= */
static inline int setenv(const char* name, const char* value, int /*overwrite*/) {
    size_t n = strlen(name) + strlen(value) + 2;
    char* buf = (char*)malloc(n);
    if (!buf) return -1;
    _snprintf(buf, n, "%s=%s", name, value);
    int r = _putenv(buf);
    free(buf);
    return r;
}
static inline int unsetenv(const char* name) {
    size_t n = strlen(name) + 2;
    char* buf = (char*)malloc(n);
    if (!buf) return -1;
    _snprintf(buf, n, "%s=", name);
    int r = _putenv(buf);
    free(buf);
    return r;
}

/* ================= sysconf（CPU 数） ================= */
#define _SC_NPROCESSORS_ONLN 84
static inline long sysconf(int name) {
    if (name == _SC_NPROCESSORS_ONLN) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return (long)si.dwNumberOfProcessors;
    }
    return -1;
}

/* ================= poll（WSAPoll，协程 fd 就绪检测用） =================
 * MinGW winsock2.h 已定义 struct pollfd（= WSAPOLLFD，字段 fd/events/revents，
 * fd 为 SOCKET）与 POLLRDNORM/POLLWRNORM 等常量。这里只补 POSIX 名宏与 nfds_t，
 * poll() 直接转调 WSAPoll（runtime.c 的 poll 只监听 socket fd）。 */
#ifndef _WIN32_POLL_DEFINED
#define _WIN32_POLL_DEFINED
#include <malloc.h>   /* _alloca */
typedef unsigned int nfds_t;
/* winsock2.h 若未定义 POLLIN/POLLOUT（值 = 对应 NORM 组合），这里补齐 */
#ifndef POLLIN
#define POLLIN  (POLLRDNORM | POLLRDBAND)
#endif
#ifndef POLLOUT
#define POLLOUT (POLLWRNORM | POLLWRBAND)
#endif
#ifndef POLLERR
#define POLLERR 0x0001
#endif
#ifndef POLLHUP
#define POLLHUP 0x0002
#endif
#ifndef POLLNVAL
#define POLLNVAL 0x0004
#endif
static inline int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    return WSAPoll((WSAPOLLFD*)fds, (ULONG)nfds, timeout);
}
#endif /* _WIN32_POLL_DEFINED */

#endif /* _WIN32 */
#endif /* MYP_PLATFORM_WIN_H */
