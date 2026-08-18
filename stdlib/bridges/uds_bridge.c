// uds_bridge.c — MYP ↔ Unix domain socket 桥接（AF_UNIX，跨进程事件总线承载）
// ---------------------------------------------------------------------------
// 与 net.myp（AF_INET TCP）同接口模式，但走 Unix domain socket（本机进程间，
// 路径寻址 /tmp/xxx.sock）。send/recv/accept 语义与 TCP socket 一致。
// 链接由 mypc 通用桥接发现自动完成（无需改编译器）：程序引用 myp_uds_* 未定义
// 符号时自动编译+链接本文件；socket 为 libc，无需侧车 .libs。
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "mylang/runtime.h"

// 创建 AF_UNIX 监听 socket（bind+listen）。返回 fd；<0 失败（-2 bind, -3 listen）
int32_t myp_uds_server(const char* path) {
    if (!path) return -1;
    int fd = (int)socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -2; }
    if (listen(fd, 10) < 0) { close(fd); return -3; }
    return fd;
}

// 接受客户端连接（阻塞）。返回 client fd；-1 错误
int32_t myp_uds_accept(int32_t server_fd) {
    struct sockaddr_un client;
    socklen_t len = sizeof(client);
    return (int32_t)accept(server_fd, (struct sockaddr*)&client, &len);
}

// 连接 Unix domain socket 服务器。返回 fd；<0 失败（-2 connect）
int32_t myp_uds_connect(const char* path) {
    if (!path) return -1;
    int fd = (int)socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -2; }
    return fd;
}

// 发送字符串。返回发送字节数；0/-1 错误
int32_t myp_uds_send(int32_t fd, const char* data) {
    if (!data) return 0;
    size_t len = strlen(data);
    if (len == 0) return 0;
    return (int32_t)send(fd, data, len, 0);
}

// 接收最多 max_len 字节（GC 分配，同 myp_net_recv）
char* myp_uds_recv(int32_t fd, int32_t max_len) {
    if (max_len <= 0) max_len = 4096;
    char* buf = (char*)myp_alloc((size_t)max_len + 1);
    if (!buf) return NULL;
    int n = (int)recv(fd, buf, (size_t)max_len, 0);
    if (n <= 0) { buf[0] = '\0'; return buf; }
    buf[n] = '\0';
    return buf;
}

// 接收一行（直到 \n，不含 \r\n）
char* myp_uds_recv_line(int32_t fd) {
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

void myp_uds_close(int32_t fd) {
    if (fd >= 0) close(fd);
}

// 多路复用：poll 一组 fd（MYP int[] → fd 列表）是否有可读/可写/异常就绪。
// 返回**首个就绪 fd 的索引**（0 起，与传入数组下标一致）；超时/无就绪返回 -1；
// count<=0 或 fds 为空返回 -1。MYP 侧反复调用（timeout=0）可逐个处理全部就绪。
int32_t myp_uds_poll(int32_t* fds, int32_t count, int32_t timeout_ms) {
    if (!fds || count <= 0) return -1;
    if (timeout_ms < 0) timeout_ms = 0;
    struct pollfd* pfds = (struct pollfd*)malloc(sizeof(struct pollfd) * (size_t)count);
    if (!pfds) return -1;
    for (int32_t i = 0; i < count; i++) {
        pfds[i].fd = fds[i];
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }
    int r = poll(pfds, (nfds_t)count, (int)timeout_ms);
    if (r <= 0) { free(pfds); return -1; }
    int32_t hit = -1;
    for (int32_t i = 0; i < count; i++) {
        if (pfds[i].revents != 0) { hit = i; break; }
    }
    free(pfds);
    return hit;
}

// 删除 socket 文件（bind 前清理残留）
void myp_uds_unlink(const char* path) {
    if (path) unlink(path);
}
