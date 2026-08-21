// process_bridge.c — Process Management（从 runtime.c 分离，按需链接）
// ---------------------------------------------------------------------------
// 仅用到 process 的程序链接（bridge 符号匹配，同 sdl/ttf/json）。
// ---------------------------------------------------------------------------
#include "mylang/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

// ======================
// Process Management
// ======================

#if defined(_WIN32)
/* ---- Windows 移植：system/_popen/_getpid + OpenProcess/CreateProcess ---- */
#include <process.h>
#include <windows.h>
#ifndef popen
#define popen _popen
#endif
#ifndef pclose
#define pclose _pclose
#endif

// Run a command via system(). Windows system() 直接返回退出码（-1 无法启动）。
int32_t myp_process_run(const char* cmd) {
    if (!cmd) return -1;
    return system(cmd);
}

// Run a command and capture its stdout output.
char* myp_process_output(const char* cmd) {
    if (!cmd) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    FILE* fp = popen(cmd, "r");
    if (!fp) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
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

int32_t myp_process_get_pid(void) { return (int32_t)_getpid(); }
int32_t myp_process_get_ppid(void) { return -1; }  /* Windows 无 getppid */

int32_t myp_process_is_running(int32_t pid) {
    if (pid <= 0) return 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return 0;
    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    if (!ok) return 0;
    return (code == STILL_ACTIVE) ? 1 : 0;
}

// Spawn a command detached（CreateProcess，不等待）。返回 0 成功 / -1 失败。
int32_t myp_process_spawn(const char* cmd) {
    if (!cmd || !cmd[0]) return -1;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    char* c = (char*)malloc(strlen(cmd) + 1);
    if (!c) return -1;
    strcpy(c, cmd);
    BOOL ok = CreateProcessA(NULL, c, NULL, NULL, FALSE,
        CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi);
    free(c);
    if (!ok) return -1;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}
#else
#include <unistd.h>
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

// Spawn a command detached (new process/session), do NOT wait for it.
// Returns 0 on success, -1 on fork failure. The child keeps running after the
// caller exits (double-fork → reparented to init, reaped by init, no zombie).
// 用途：设计器「运行」按钮 → 后台启动 uix_run 弹独立预览窗口，设计器不阻塞。
int32_t myp_process_spawn(const char* cmd) {
    if (!cmd || cmd[0] == '\0') return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // 中间子进程：再 fork 出真正的后台进程后立即退出，父进程 waitpid 回收
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(1);
        if (pid2 == 0) {
            setsid();   // 脱离父进程会话/控制终端，独立存活
            execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
            _exit(127);
        }
        _exit(0);
    }
    // 父进程回收中间子进程（后台进程已被 init 收养，不会变僵尸）
    int st;
    waitpid(pid, &st, 0);
    return 0;
}
#endif /* !defined(_WIN32) */

