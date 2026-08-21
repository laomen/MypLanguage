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
#define close(fd) closesocket(fd)
#endif

// Returns: server socket fd on success, -1 on error
int32_t myp_net_server(int32_t port) {
#if defined(_WIN32)
    if (myp_net_ws_init() != 0) return -1;
#endif
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -2; }
    if (listen(fd, 10) < 0) { close(fd); return -3; }
    return fd;
}

// Accept a client connection (blocks until connection arrives)
// Returns: client fd on success, -1 on error
int32_t myp_net_accept(int32_t server_fd) {
    struct sockaddr_in client_addr;
#if defined(_WIN32)
    int addr_len = (int)sizeof(client_addr);
    int fd = (int)accept((SOCKET)server_fd, (struct sockaddr*)&client_addr, &addr_len);
#else
    socklen_t addr_len = sizeof(client_addr);
    int fd = (int)accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
#endif
    return fd;
}

// Connect to a TCP server
// Returns: socket fd on success, -1 on error
int32_t myp_net_connect(const char* host, int32_t port) {
    if (!host) return -1;
#if defined(_WIN32)
    if (myp_net_ws_init() != 0) return -1;
#endif
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct hostent* he = gethostbyname(host);
    if (!he) { close(fd); return -2; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -3; }
    return fd;
}

// Send a string over a socket
// Returns: bytes sent on success, -1 on error
int32_t myp_net_send(int32_t fd, const char* data) {
    if (!data) return 0;
    size_t len = strlen(data);
    if (len == 0) return 0;
    return (int32_t)send(fd, data, len, 0);
}

// Receive up to max_len bytes from a socket
// Returns: string with received data (empty string on error/close)
char* myp_net_recv(int32_t fd, int32_t max_len) {
    if (max_len <= 0) max_len = 4096;
    char* buf = (char*)myp_alloc((size_t)max_len + 1);
    if (!buf) return NULL;
    int n = (int)recv(fd, buf, (size_t)max_len, 0);
    if (n <= 0) { buf[0] = '\0'; return buf; }
    buf[n] = '\0';
    return buf;
}

// Receive one line (until \n) from a socket
// Returns: string without trailing \r\n
char* myp_net_recv_line(int32_t fd) {
    // Read byte by byte until \n (simple but works)
    char buf[8192];
    int pos = 0;
    while (pos < 8191) {
        char c;
        int n = (int)recv(fd, &c, 1, 0);
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
    if (fd >= 0) close(fd);
}

// Set a socket fd non-blocking (§五-5 P2: used by async recv/send so the actual
// IO completes without blocking after fd readiness).
void myp_net_set_nonblock(int32_t fd) {
    if (fd < 0) return;
#if defined(_WIN32)
    u_long mode = 1;  /* FIONBIO */
    ioctlsocket((SOCKET)fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

