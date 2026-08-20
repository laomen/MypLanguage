// process_bridge.c — Process Management（从 runtime.c 分离，按需链接）
// ---------------------------------------------------------------------------
// 仅用到 process 的程序链接（bridge 符号匹配，同 sdl/ttf/json）。
// ---------------------------------------------------------------------------
#include "mylang/runtime.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

// ======================
// Process Management
// ======================

#include <sys/wait.h>

// Run a command via system(). Returns the command's real exit code
// (0 = success); -1 if the shell itself could not be started.
int32_t myp_process_run(const char* cmd) {
    if (!cmd) return -1;
    int status = system(cmd);
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;  // 被信号终止等异常情况
}

// Run a command and capture its stdout output.
// Returns the output as a string (up to 64KB).
char* myp_process_output(const char* cmd) {
    if (!cmd) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    FILE* fp = popen(cmd, "r");
    if (!fp) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    // Read all output in chunks
    size_t cap = 4096;
    size_t len = 0;
    char* buf = (char*)myp_alloc(cap);
    if (!buf) { pclose(fp); return NULL; }
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp) - 1, fp)) > 0) {
        tmp[n] = '\0';
        if (len + n >= cap) {
            cap *= 2;
            char* new_buf = (char*)myp_alloc(cap);
            if (!new_buf) { pclose(fp); return buf; }
            memcpy(new_buf, buf, len);
            buf = new_buf;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    buf[len] = '\0';
    pclose(fp);
    return buf;
}

// Get current process ID
int32_t myp_process_get_pid(void) {
    return (int32_t)getpid();
}

// Get parent process ID
int32_t myp_process_get_ppid(void) {
    return (int32_t)getppid();
}

// Check if a process with the given PID is running.
// Returns 1 if running, 0 if not.
int32_t myp_process_is_running(int32_t pid) {
    if (pid <= 0) return 0;
    return kill(pid, 0) == 0 ? 1 : 0;
}

