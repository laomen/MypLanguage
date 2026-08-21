// net_bridge.c — Networking (TCP Sockets)（从 runtime.c 分离，按需链接）
// ---------------------------------------------------------------------------
// 仅用到 net 的程序链接（bridge 符号匹配，同 sdl/ttf/json）。
// ---------------------------------------------------------------------------
#include "mylang/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

// ======================
// Networking (TCP Sockets)
// ======================

#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
/* Winsock 一次性初始化（进程内只调一次） */
static int myp_net_ws_init(void) {
    static int inited = 0;
    if (!inited) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        inited = 1;
    }
    return 0;
}
/* SOCKET fd 表：64 位 SOCKET → 小整数 int fd（runtime 协程 poll 也查表），
 * 避免把 UINT_PTR SOCKET 直接塞进 int（高 32 位截断）。 */
#define MYP_WIN_FD_MAX 512
static SOCKET myp_win_fds[MYP_WIN_FD_MAX];
static unsigned char myp_win_fd_used[MYP_WIN_FD_MAX];
static int myp_win_fd_inited = 0;
static void myp_win_fd_init(void) {
    if (!myp_win_fd_inited) {
        for (int i = 0; i < MYP_WIN_FD_MAX; i++) {
            myp_win_fds[i] = INVALID_SOCKET;
            myp_win_fd_used[i] = 0;
        }
        myp_win_fd_inited = 1;
    }
}
int myp_win_fd_alloc(SOCKET s) {
    myp_win_fd_init();
    for (int i = 0; i < MYP_WIN_FD_MAX; i++)
        if (!myp_win_fd_used[i]) { myp_win_fds[i] = s; myp_win_fd_used[i] = 1; return i; }
    return -1;
}
SOCKET myp_win_fd_lookup(int fd) {
    myp_win_fd_init();
    if (fd < 0 || fd >= MYP_WIN_FD_MAX || !myp_win_fd_used[fd]) return INVALID_SOCKET;
    return myp_win_fds[fd];
}
void myp_win_fd_free(int fd) {
    myp_win_fd_init();
    if (fd >= 0 && fd < MYP_WIN_FD_MAX && myp_win_fd_used[fd]) {
        myp_win_fds[fd] = INVALID_SOCKET;
        myp_win_fd_used[fd] = 0;
    }
}
/* close(int fd) → 查表 closesocket + 释放表项 */
static void myp_net_closefd(int fd) {
    SOCKET s = myp_win_fd_lookup(fd);
    if (s != INVALID_SOCKET) { closesocket(s); myp_win_fd_free(fd); }
}
#define close(fd) myp_net_closefd(fd)
#endif

// Returns: server socket fd on success, -1 on error
int32_t myp_net_server(int32_t port) {
#if defined(_WIN32)
    if (myp_net_ws_init() != 0) return -1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
#endif
    int opt = 1;
    /* Winsock setsockopt 的 optval 是 const char*（POSIX 是 const void*）→ 统一 cast */
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#if defined(_WIN32)
        closesocket(s);
#else
        close(s);
#endif
        return -2;
    }
    if (listen(s, 10) < 0) {
#if defined(_WIN32)
        closesocket(s);
#else
        close(s);
#endif
        return -3;
    }
#if defined(_WIN32)
    int fd = myp_win_fd_alloc(s);
    return fd >= 0 ? fd : -1;
#else
    return s;
#endif
}

// Accept a client connection (blocks until connection arrives)
// Returns: client fd on success, -1 on error
int32_t myp_net_accept(int32_t server_fd) {
    struct sockaddr_in client_addr;
#if defined(_WIN32)
    int addr_len = (int)sizeof(client_addr);
    SOCKET s = accept(myp_win_fd_lookup((int)server_fd), (struct sockaddr*)&client_addr, &addr_len);
    if (s == INVALID_SOCKET) return -1;
    int fd = myp_win_fd_alloc(s);
    return fd;
#else
    socklen_t addr_len = sizeof(client_addr);
    return (int32_t)accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
#endif
}

// Connect to a TCP server
// Returns: socket fd on success, -1 on error
int32_t myp_net_connect(const char* host, int32_t port) {
    if (!host) return -1;
#if defined(_WIN32)
    if (myp_net_ws_init() != 0) return -1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
#endif
    struct hostent* he = gethostbyname(host);
    if (!he) {
#if defined(_WIN32)
        closesocket(s);
#else
        close(s);
#endif
        return -2;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#if defined(_WIN32)
        closesocket(s);
#else
        close(s);
#endif
        return -3;
    }
#if defined(_WIN32)
    int fd = myp_win_fd_alloc(s);
    return fd >= 0 ? fd : -1;
#else
    return s;
#endif
}

// Send a string over a socket
// Returns: bytes sent on success, -1 on error
int32_t myp_net_send(int32_t fd, const char* data) {
    if (!data) return 0;
    size_t len = strlen(data);
    if (len == 0) return 0;
#if defined(_WIN32)
    SOCKET s = myp_win_fd_lookup((int)fd);
    if (s == INVALID_SOCKET) return -1;
    return (int32_t)send(s, data, (int)len, 0);
#else
    return (int32_t)send(fd, data, len, 0);
#endif
}

// Receive up to max_len bytes from a socket
// Returns: string with received data (empty string on error/close)
char* myp_net_recv(int32_t fd, int32_t max_len) {
    if (max_len <= 0) max_len = 4096;
    char* buf = (char*)myp_alloc((size_t)max_len + 1);
    if (!buf) return NULL;
#if defined(_WIN32)
    SOCKET s = myp_win_fd_lookup((int)fd);
    if (s == INVALID_SOCKET) { buf[0] = '\0'; return buf; }
    int n = (int)recv(s, buf, max_len, 0);
#else
    int n = (int)recv(fd, buf, (size_t)max_len, 0);
#endif
    if (n <= 0) { buf[0] = '\0'; return buf; }
    buf[n] = '\0';
    return buf;
}

// Receive one line (until \n) from a socket
// Returns: string without trailing \r\n
char* myp_net_recv_line(int32_t fd) {
#if defined(_WIN32)
    SOCKET s = myp_win_fd_lookup((int)fd);
    if (s == INVALID_SOCKET) { char* e = (char*)myp_alloc(1); if (e) e[0] = '\0'; return e; }
#endif
    // Read byte by byte until \n (simple but works)
    char buf[8192];
    int pos = 0;
    while (pos < 8191) {
        char c;
#if defined(_WIN32)
        int n = (int)recv(s, &c, 1, 0);
#else
        int n = (int)recv(fd, &c, 1, 0);
#endif
        if (n <= 0) break;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    char* r = (char*)myp_alloc((size_t)pos + 1);
    if (r) memcpy(r, buf, (size_t)pos + 1);
    return r;
}

// Close a socket
void myp_net_close(int32_t fd) {
    if (fd < 0) return;
#if defined(_WIN32)
    myp_net_closefd((int)fd);
#else
    close(fd);
#endif
}

// Set a socket fd non-blocking (§五-5 P2: used by async recv/send so the actual
// IO completes without blocking after fd readiness).
void myp_net_set_nonblock(int32_t fd) {
    if (fd < 0) return;
#if defined(_WIN32)
    SOCKET s = myp_win_fd_lookup((int)fd);
    if (s == INVALID_SOCKET) return;
    u_long mode = 1;  /* FIONBIO */
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

