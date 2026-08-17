// MYP Language runtime (compiled into generated programs).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 MYP Language authors
// See LICENSE for the full MIT license text.
#define _GNU_SOURCE
#include "mylang/runtime.h"

#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <poll.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <limits.h>

// ======================
// Checked size arithmetic + OOM handling (M3: overflow-safe allocation)
// ======================
// Every allocation size computation must detect overflow and treat it as OOM
// (a deterministic, diagnosable abort) rather than proceeding with a truncated
// size that later writes out of bounds. realloc callers must keep the original
// pointer until the new allocation succeeds (checked at each call site).
static void myp_oom(size_t size) {
    fprintf(stderr, "MYP runtime: out of memory / size overflow (%zu bytes)\n", size);
    abort();
}
static int myp_mul_overflow(size_t a, size_t b, size_t* out) {
    if (a != 0 && b > SIZE_MAX / a) return 1;
    *out = a * b;
    return 0;
}
static int myp_add_overflow(size_t a, size_t b, size_t* out) {
    if (b > SIZE_MAX - a) return 1;
    *out = a + b;
    return 0;
}

// ---- M9: deterministic allocation-failure injection ----
// Memory.failAllocEnable(N) (or env MYP_FAIL_ALLOC=N) makes the Nth allocation
// that reaches myp_xmalloc abort with a stable diagnostic instead of silently
// proceeding. OOM-sweep tests run the same program with N=1..K and assert each
// aborts cleanly (no corruption, distinct message). TLS: injection is per-thread.
static __thread int64_t myp_fail_alloc_at = 0;
static __thread int64_t myp_fail_alloc_seen = 0;
static __thread int myp_fail_alloc_env_read = 0;

void myp_fail_alloc_enable(int64_t nth) {
    myp_fail_alloc_at = (nth > 0) ? nth : 0;
    myp_fail_alloc_seen = 0;
}
void myp_fail_alloc_disable(void) { myp_fail_alloc_at = 0; }
int64_t myp_fail_alloc_get(void) { return myp_fail_alloc_at; }

static void myp_fail_alloc_check(void) {
    if (!myp_fail_alloc_env_read) {
        myp_fail_alloc_env_read = 1;
        const char* v = getenv("MYP_FAIL_ALLOC");
        if (v && v[0]) { myp_fail_alloc_at = atoll(v); myp_fail_alloc_seen = 0; }
    }
    if (myp_fail_alloc_at <= 0) return;
    myp_fail_alloc_seen++;
    if (myp_fail_alloc_seen >= myp_fail_alloc_at) {
        fprintf(stderr, "MYP runtime: injected allocation failure (allocation #%lld)\n",
                (long long)myp_fail_alloc_seen);
        abort();
    }
}

// Allocation helper that never returns NULL: overflow or malloc failure aborts
// deterministically instead of letting a NULL flow into GEP/memset.
static void* myp_xmalloc(size_t size) {
    myp_fail_alloc_check();   // M9: deterministic injection point (Nth alloc)
    void* p = malloc(size);
    if (!p) myp_oom(size);
    return p;
}

// ======================
// Terminal raw mode (for real-time keyboard input)
// ======================

static struct termios myp_orig_term;
static int myp_raw_mode = 0;

static void myp_enable_raw(void) {
    if (!myp_raw_mode) {
        struct termios raw;
        if (tcgetattr(0, &myp_orig_term) != 0) {
            // Not a terminal (e.g. pipe) — skip raw mode
            myp_raw_mode = 0;
            return;
        }
        raw = myp_orig_term;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw);
        myp_raw_mode = 1;
    }
}

static void myp_restore_term(void) {
    if (myp_raw_mode) {
        tcsetattr(0, TCSANOW, &myp_orig_term);
        myp_raw_mode = 0;
    }
}

// Check if a key is available (non-blocking)
int32_t myp_kbhit(void) {
    myp_enable_raw();
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    return select(1, &fds, NULL, NULL, &tv) > 0 ? 1 : 0;
}

// Read one character (non-blocking, raw mode). Returns 0 if no key available.
int32_t myp_getch(void) {
    myp_enable_raw();
    char c = 0;
    if (read(0, &c, 1) > 0) return (int32_t)(unsigned char)c;
    return 0;
}

// ======================
// File I/O
// ======================
// 每 File 独立句柄（多文件可同时打开）：fopen 把 FILE* 登记进句柄表，
// 无句柄的旧 intrinsic（deeplearning 等直接调用方）操作"当前活动文件"；
// File 类通过 select 切换当前文件后调用，实现多文件交替读写。

#define MYP_IO_MAX_FILES 64
static FILE* myp_io_table[MYP_IO_MAX_FILES] = {0};
static pthread_mutex_t myp_io_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t myp_io_locks[MYP_IO_MAX_FILES];
static pthread_once_t myp_io_once = PTHREAD_ONCE_INIT;
static __thread int32_t myp_io_cur = 0;     // 当前线程的活动句柄（0=无）

static void myp_io_init(void) {
    for (int i = 0; i < MYP_IO_MAX_FILES; i++)
        pthread_mutex_init(&myp_io_locks[i], NULL);
}

// Returns the locked FILE for a handle. The caller must unlock with
// myp_io_unlock_handle after completing the stdio operation.
static FILE* myp_io_lock_handle(int32_t handle) {
    if (handle < 1 || handle >= MYP_IO_MAX_FILES) return NULL;
    pthread_once(&myp_io_once, myp_io_init);
    pthread_mutex_lock(&myp_io_locks[handle]);
    FILE* fp = myp_io_table[handle];
    if (!fp) pthread_mutex_unlock(&myp_io_locks[handle]);
    return fp;
}

static void myp_io_unlock_handle(int32_t handle) {
    pthread_mutex_unlock(&myp_io_locks[handle]);
}

static FILE* myp_io_lock_current(int32_t* handle) {
    *handle = myp_io_cur;
    return myp_io_lock_handle(*handle);
}

int32_t myp_io_fopen(const char* path, const char* mode) {
    FILE* fp = fopen(path, mode);
    if (!fp) return -1;
    pthread_once(&myp_io_once, myp_io_init);
    pthread_mutex_lock(&myp_io_table_mutex);
    for (int i = 1; i < MYP_IO_MAX_FILES; i++) {
        pthread_mutex_lock(&myp_io_locks[i]);
        if (!myp_io_table[i]) {
            myp_io_table[i] = fp;
            myp_io_cur = i;
            pthread_mutex_unlock(&myp_io_locks[i]);
            pthread_mutex_unlock(&myp_io_table_mutex);
            return 0;
        }
        pthread_mutex_unlock(&myp_io_locks[i]);
    }
    pthread_mutex_unlock(&myp_io_table_mutex);
    fclose(fp);
    return -1;  // 句柄表满
}

// 当前活动句柄（File.open 成功后读取存入 handle_）
int32_t myp_io_current_handle(void) { return myp_io_cur; }

// 切换当前活动文件（多文件交替读写）
void myp_io_select(int32_t handle) {
    FILE* fp = myp_io_lock_handle(handle);
    if (!fp) return;
    myp_io_cur = handle;
    myp_io_unlock_handle(handle);
}

void myp_io_fclose(void) {
    int32_t handle = myp_io_cur;
    myp_io_cur = 0;
    if (handle < 1 || handle >= MYP_IO_MAX_FILES) return;
    pthread_once(&myp_io_once, myp_io_init);
    pthread_mutex_lock(&myp_io_table_mutex);
    pthread_mutex_lock(&myp_io_locks[handle]);
    FILE* fp = myp_io_table[handle];
    myp_io_table[handle] = NULL;
    pthread_mutex_unlock(&myp_io_table_mutex);
    if (fp) fclose(fp);
    pthread_mutex_unlock(&myp_io_locks[handle]);
}

// 读取一行并返回**新分配的**字符串（修复：原 static buf 共享缓冲，
// 多次 readLine 结果存数组会全部指向最后一行）。EOF 返回空串（文档契约）。
const char* myp_io_read_line(void) {
    int32_t handle;
    FILE* fp = myp_io_lock_current(&handle);
    if (!fp) return myp_strdup("");
    char buf[4096];
    if (!fgets(buf, sizeof(buf), fp)) {
        myp_io_unlock_handle(handle);
        return myp_strdup("");
    }
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    myp_io_unlock_handle(handle);
    return myp_strdup(buf);
}

void myp_io_write(const char* text) {
    int32_t handle;
    FILE* fp = myp_io_lock_current(&handle);
    if (!fp) return;
    fputs(text, fp);
    myp_io_unlock_handle(handle);
}

void myp_io_write_line(const char* text) {
    int32_t handle;
    FILE* fp = myp_io_lock_current(&handle);
    if (!fp) return;
    fprintf(fp, "%s\n", text);
    myp_io_unlock_handle(handle);
}

int32_t myp_io_has_next(void) {
    int32_t handle;
    FILE* fp = myp_io_lock_current(&handle);
    if (!fp) return 0;
    int32_t result = !feof(fp);
    myp_io_unlock_handle(handle);
    return result;
}

// Binary file I/O for IDX parsing
#include <stdint.h>
int32_t myp_io_read_byte(void) {
    int32_t handle;
    FILE* fp = myp_io_lock_current(&handle);
    if (!fp) return -1;
    unsigned char c;
    if (fread(&c, 1, 1, fp) != 1) {
        myp_io_unlock_handle(handle);
        return -1;
    }
    myp_io_unlock_handle(handle);
    return (int32_t)c;
}

int32_t myp_io_read_i32be(void) {
    int32_t handle;
    FILE* fp = myp_io_lock_current(&handle);
    if (!fp) return -1;
    unsigned char buf[4];
    int32_t result = -1;
    if (fread(buf, 1, 4, fp) == 4)
        result = ((int32_t)buf[0] << 24) | ((int32_t)buf[1] << 16) |
                 ((int32_t)buf[2] << 8) | (int32_t)buf[3];
    myp_io_unlock_handle(handle);
    return result;
}

int32_t myp_io_seek(int32_t offset, int32_t whence) {
    int32_t handle;
    FILE* fp = myp_io_lock_current(&handle);
    if (!fp) return -1;
    int32_t result = fseek(fp, offset, whence) == 0 ? 0 : -1;
    myp_io_unlock_handle(handle);
    return result;
}

// Binary file I/O for weight save/load
int32_t myp_io_write_byte(int32_t c) {
    int32_t handle;
    FILE* fp = myp_io_lock_current(&handle);
    if (!fp) return -1;
    unsigned char byte = (unsigned char)(c & 0xFF);
    int32_t result = fwrite(&byte, 1, 1, fp) == 1 ? 0 : -1;
    myp_io_unlock_handle(handle);
    return result;
}

int32_t myp_io_write_i32be(int32_t val) {
    int32_t handle;
    FILE* fp = myp_io_lock_current(&handle);
    if (!fp) return -1;
    unsigned char buf[4];
    buf[0] = (unsigned char)((val >> 24) & 0xFF);
    buf[1] = (unsigned char)((val >> 16) & 0xFF);
    buf[2] = (unsigned char)((val >> 8) & 0xFF);
    buf[3] = (unsigned char)(val & 0xFF);
    int32_t result = fwrite(buf, 1, 4, fp) == 4 ? 0 : -1;
    myp_io_unlock_handle(handle);
    return result;
}

int32_t myp_io_write_double(double val) {
    int32_t handle;
    FILE* fp = myp_io_lock_current(&handle);
    if (!fp) return -1;
    int32_t result = fwrite(&val, sizeof(double), 1, fp) == 1 ? 0 : -1;
    myp_io_unlock_handle(handle);
    return result;
}

double myp_io_read_double(void) {
    int32_t handle;
    FILE* fp = myp_io_lock_current(&handle);
    if (!fp) return 0.0;
    double val = 0.0;
    fread(&val, sizeof(double), 1, fp);
    myp_io_unlock_handle(handle);
    return val;
}

// ======================
// Basic I/O
// ======================

// ---- @test 输出捕获（阶段 1）----
// captureStart 后，所有 Console.* 输出（myp_print*）改写入捕获缓冲而非 stdout；
// captureEnd 停止；get 取回捕获串，contains/eq 供 Test.assertOutputContains/Eq
// 断言。非捕获态行为与原来完全一致（fputs/fwrite 到 stdout）。
static char* myp_capture_buf = NULL;
static size_t myp_capture_len = 0;
static size_t myp_capture_cap = 0;
static int myp_capture_on = 0;

static void myp_capture_write(const char* s, size_t n) {
    if (!myp_capture_on) return;
    if (myp_capture_len + n + 1 > myp_capture_cap) {
        size_t nc = myp_capture_cap ? myp_capture_cap * 2 : 256;
        while (nc < myp_capture_len + n + 1) nc *= 2;
        char* nb = (char*)realloc(myp_capture_buf, nc);
        if (!nb) return;
        myp_capture_buf = nb;
        myp_capture_cap = nc;
    }
    memcpy(myp_capture_buf + myp_capture_len, s, n);
    myp_capture_len += n;
    myp_capture_buf[myp_capture_len] = '\0';
}

// 统一出口：捕获态写入缓冲，否则写 stdout。
static void myp_out_write(const char* s) { myp_capture_write(s, strlen(s)); if (!myp_capture_on) fputs(s, stdout); }
static void myp_out_write_n(const char* s, size_t n) {
    myp_capture_write(s, n);
    if (!myp_capture_on) fwrite(s, 1, n, stdout);
}

void myp_test_capture_start(void) { myp_capture_len = 0; myp_capture_on = 1; }
void myp_test_capture_stop(void) { myp_capture_on = 0; }
const char* myp_test_capture_get(void) { return myp_capture_buf ? myp_capture_buf : ""; }
int32_t myp_test_capture_contains(const char* sub) {
    if (!sub) return 0;
    if (!myp_capture_buf) return sub[0] == '\0' ? 1 : 0;
    return strstr(myp_capture_buf, sub) ? 1 : 0;
}
int32_t myp_test_capture_eq(const char* expected) {
    if (!expected) return 0;
    const char* got = myp_capture_buf ? myp_capture_buf : "";
    return strcmp(got, expected) == 0 ? 1 : 0;
}

void myp_print(const char* str) { myp_out_write(str); fflush(stdout); }
void myp_println(const char* str) { myp_out_write(str); myp_out_write("\n"); fflush(stdout); }
void myp_print_int(int32_t val) { char b[32]; snprintf(b, sizeof b, "%d\n", val); myp_out_write(b); fflush(stdout); }
void myp_print_long(int64_t val) { char b[64]; snprintf(b, sizeof b, "%ld\n", (long)val); myp_out_write(b); fflush(stdout); }
void myp_print_float(double val) { char b[64]; snprintf(b, sizeof b, "%g", val); myp_out_write(b); fflush(stdout); }
void myp_print_bool(int32_t val) { myp_out_write(val ? "true" : "false"); fflush(stdout); }

// §P5 ② printf 风格格式化（CPU 回退路径的 kernel.printk / kernel.assert 用）。
// vsnprintf 进缓冲 → myp_out_write（与 myp_print* 同输出流）。
void myp_printf(const char* fmt, ...) {
    if (!fmt) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    myp_out_write(buf);
    fflush(stdout);
}

// §P5 ② kernel.assert CPU 回退硬失败（noreturn）——与 GPU staging 的 exit(1) 对齐。
void myp_assert_abort(const char* msg) {
    fprintf(stderr, "[myp] kernel.assert FAILED: %s\n", msg ? msg : "");
    exit(1);
}

void myp_flush(void) { fflush(stdout); }

// Terminal size (for TUI rendering)
int32_t myp_term_width(void) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return (int32_t)w.ws_col;
    return 80; // fallback
}
int32_t myp_term_height(void) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0)
        return (int32_t)w.ws_row;
    return 24; // fallback
}

// Read a line from stdin (for interactive input)
const char* myp_read_line(void) {
    static char buf[256];
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    // M8: return a COUNTED copy (never the static buffer — ARC would release
    // a static pointer's bogus header).
    return myp_strdup(buf);
}

// String length
int32_t myp_strlen(const char* s) {
    if (!s) return 0;
    return (int32_t)strlen(s);
}

// Convert a character code to a single-character string
const char* myp_chr(int32_t code) {
    // M8: return a COUNTED string (the old static buffer has no ARC header).
    char* r = (char*)myp_alloc(2);
    if (!r) return NULL;
    r[0] = (char)(code & 0xFF);
    r[1] = '\0';
    return r;
}

// ASCII code of the first character (0 if empty)
int32_t myp_ord(const char* s) {
    if (!s || !s[0]) return 0;
    return (int32_t)(unsigned char)s[0];
}

// ASCII code at index i, O(1) (no strlen). Caller guarantees 0 <= i < len.
// Negative or null → 0 (upper bound unchecked for O(1); lexer guards with its
// cached source length).
int32_t myp_charcode(const char* s, int32_t i) {
    if (!s || i < 0) return 0;
    return (int32_t)(unsigned char)s[i];
}

// String equality comparison (content, not pointer)
int32_t myp_str_eq(const char* a, const char* b) {
    if (!a || !b) return a == b ? 1 : 0;
    return strcmp(a, b) == 0 ? 1 : 0;
}

// P1（docs/type_system_design §6.4）：string 词法比较（strcmp 语义），供
// `< <= > >=` 操作符使用。返回 -1/0/1。
int32_t myp_str_cmp(const char* a, const char* b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    int c = strcmp(a, b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

// String to double (for parsing data files)
double myp_atof(const char* s) {
    if (!s) return 0.0;
    return atof(s);
}

// String to int (decimal; 非数字前缀解析失败返回 0)。供 stdlib Str.toInt 使用。
int32_t myp_str_to_int(const char* s) {
    if (!s) return 0;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return 0;   // 无有效数字
    return (int32_t)v;
}

// P2（docs/type_system_design §6.2）：parse* 全族。统一 strtol/strtoull/strtod
// 语义（带符号与基数，0x 前缀支持）；失败（无有效数字）回 0（保留现状）。
int32_t myp_str_parse_int(const char* s) {
    if (!s) return 0;
    char* end = NULL;
    long long v = strtoll(s, &end, 0);
    if (end == s) return 0;
    return (int32_t)v;
}
// §6.2 P4 parseIntOpt：可区分"合法 0"与"失败"（*ok = 是否有有效数字）。
int32_t myp_str_parse_int_opt(const char* s, bool* ok) {
    if (!s) { if (ok) *ok = false; return 0; }
    char* end = NULL;
    long long v = strtoll(s, &end, 0);
    if (end == s) { if (ok) *ok = false; return 0; }
    if (ok) *ok = true;
    return (int32_t)v;
}
int64_t myp_str_to_long(const char* s) {
    if (!s) return 0;
    char* end = NULL;
    long long v = strtoll(s, &end, 0);
    if (end == s) return 0;
    return (int64_t)v;
}
uint32_t myp_str_to_uint(const char* s) {
    if (!s) return 0;
    char* end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    if (end == s) return 0;
    return (uint32_t)v;
}
uint64_t myp_str_to_ulong(const char* s) {
    if (!s) return 0;
    char* end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    if (end == s) return 0;
    return (uint64_t)v;
}
float myp_str_to_float(const char* s) {
    if (!s) return 0.0f;
    return (float)strtod(s, NULL);
}
double myp_str_to_double(const char* s) {
    if (!s) return 0.0;
    return strtod(s, NULL);
}

// ======================
// String Utilities
// ======================

int32_t myp_str_contains(const char* s, const char* sub) {
    if (!s || !sub) return 0;
    return strstr(s, sub) != NULL ? 1 : 0;
}

int32_t myp_str_index_of(const char* s, const char* sub) {
    if (!s || !sub) return -1;
    const char* p = strstr(s, sub);
    if (p) return (int32_t)(p - s);
    return -1;
}

int32_t myp_str_starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return 0;
    size_t n = strlen(prefix);
    if (n > strlen(s)) return 0;
    return strncmp(s, prefix, n) == 0 ? 1 : 0;
}

int32_t myp_str_ends_with(const char* s, const char* suffix) {
    if (!s || !suffix) return 0;
    size_t sl = strlen(s);
    size_t sufl = strlen(suffix);
    if (sufl > sl) return 0;
    return strcmp(s + sl - sufl, suffix) == 0 ? 1 : 0;
}

char* myp_str_substring(const char* s, int32_t start, int32_t end) {
    if (!s) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
    int32_t len = (int32_t)strlen(s);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) {
        char* r = (char*)myp_alloc(1); r[0] = '\0'; return r;
    }
    int32_t new_len = end - start;
    char* result = (char*)myp_alloc((size_t)new_len + 1);
    memcpy(result, s + start, (size_t)new_len);
    result[new_len] = '\0';
    return result;
}

char* myp_str_replace(const char* s, const char* old_str, const char* new_str) {
    if (!s || !old_str || !new_str) {
        if (!s) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
        return myp_strcat(s, "");
    }
    const char* pos = strstr(s, old_str);
    if (!pos) return myp_strcat(s, "");
    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    size_t prefix_len = (size_t)(pos - s);
    size_t total_len = strlen(s) - old_len + new_len;
    char* result = (char*)myp_alloc(total_len + 1);
    memcpy(result, s, prefix_len);
    memcpy(result + prefix_len, new_str, new_len);
    strcpy(result + prefix_len + new_len, pos + old_len);
    return result;
}

char* myp_str_to_upper(const char* s) {
    if (!s) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
    char* r = myp_strcat(s, "");
    for (char* p = r; *p; p++) *p = (char)toupper((unsigned char)*p);
    return r;
}

char* myp_str_to_lower(const char* s) {
    if (!s) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
    char* r = myp_strcat(s, "");
    for (char* p = r; *p; p++) *p = (char)tolower((unsigned char)*p);
    return r;
}

char* myp_str_trim(const char* s) {
    if (!s) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
    while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    if (!*s) {
        char* r = (char*)myp_alloc(1); r[0] = '\0'; return r;
    }
    const char* end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
    size_t len = (size_t)(end - s + 1);
    char* r = (char*)myp_alloc(len + 1);
    memcpy(r, s, len);
    r[len] = '\0';
    return r;
}

int32_t myp_str_split_count(const char* s, const char* delim) {
    if (!s || !delim) return 0;
    int32_t count = 0;
    const char* p = s;
    size_t dlen = strlen(delim);
    while (*p) {
        const char* found = strstr(p, delim);
        if (found) {
            count++;
            p = found + dlen;
        } else {
            count++;
            break;
        }
    }
    return count;
}

char* myp_str_split_get(const char* s, const char* delim, int32_t index) {
    if (!s || !delim || index < 0) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
    size_t dlen = strlen(delim);
    const char* p = s;
    int32_t cur = 0;
    while (*p) {
        const char* found = strstr(p, delim);
        if (found) {
            if (cur == index) {
                size_t len = (size_t)(found - p);
                char* r = (char*)myp_alloc(len + 1);
                memcpy(r, p, len);
                r[len] = '\0';
                return r;
            }
            cur++;
            p = found + dlen;
        } else {
            if (cur == index) {
                char* r = myp_strcat(p, "");
                return r;
            }
            break;
        }
    }
    char* r = (char*)myp_alloc(1); r[0] = '\0'; return r;
}

// ======================
// JSON Parser (Simple Recursive Descent)
// ======================

enum JsonType {
    JSON_NULL = 0,
    JSON_BOOL = 1,
    JSON_INT  = 2,
    JSON_DOUBLE = 3,
    JSON_STRING = 4,
    JSON_ARRAY = 5,
    JSON_OBJECT = 6
};

typedef struct JsonNode {
    int type;
    char* key;
    char* str_val;
    double num_val;
    int bool_val;
    int child_count;
    struct JsonNode* children[256];  // fixed max children per node
} JsonNode;

static JsonNode* json_new_node(int type, const char* key) {
    JsonNode* n = (JsonNode*)calloc(1, sizeof(JsonNode));
    n->type = type;
    n->key = key ? strdup(key) : NULL;
    n->child_count = 0;
    return n;
}

static void json_add_child(JsonNode* parent, JsonNode* child) {
    if (parent->child_count >= 256) return;
    parent->children[parent->child_count++] = child;
}

static void json_free_node(JsonNode* n) {
    if (!n) return;
    free(n->key);
    free(n->str_val);
    for (int i = 0; i < n->child_count; i++)
        json_free_node(n->children[i]);
    free(n);
}

// Forward declarations
static const char* json_skip_ws(const char* p);
static JsonNode* json_parse_value(const char** pp);

static const char* json_skip_ws(const char* p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static JsonNode* json_parse_string(const char** pp) {
    const char* p = *pp;
    if (*p != '"') return NULL;
    p++;
    // Find end of string (simple: no escape handling for now)
    const char* start = p;
    const char* end = NULL;
    while (*p) {
        if (*p == '\\') {
            p += 2; // skip escaped char
            continue;
        }
        if (*p == '"') { end = p; break; }
        p++;
    }
    if (!end) return NULL;
    size_t len = (size_t)(end - start);
    char* val = (char*)malloc(len + 1);
    // Copy with unescape
    size_t wi = 0;
    for (const char* r = start; r < end; r++) {
        if (*r == '\\') {
            r++;
            switch (*r) {
                case '"': val[wi++] = '"'; break;
                case '\\': val[wi++] = '\\'; break;
                case '/': val[wi++] = '/'; break;
                case 'n': val[wi++] = '\n'; break;
                case 't': val[wi++] = '\t'; break;
                case 'r': val[wi++] = '\r'; break;
                default: val[wi++] = *r; break;
            }
        } else {
            val[wi++] = *r;
        }
    }
    val[wi] = '\0';
    JsonNode* n = json_new_node(JSON_STRING, NULL);
    n->str_val = val;
    *pp = end + 1;
    return n;
}

static JsonNode* json_parse_number(const char** pp) {
    const char* p = *pp;
    char* end = NULL;
    double d = strtod(p, &end);
    if (end == p) return NULL;
    JsonNode* n;
    const char* check = p;
    int is_int = 1;
    while (check < end) {
        if (*check == '.' || *check == 'e' || *check == 'E') { is_int = 0; break; }
        check++;
    }
    if (is_int) {
        n = json_new_node(JSON_INT, NULL);
        n->num_val = d;
        size_t nlen = (size_t)(end - p);
        char* tmp = (char*)malloc(nlen + 1);
        memcpy(tmp, p, nlen);
        tmp[nlen] = '\0';
        n->str_val = tmp;
    } else {
        n = json_new_node(JSON_DOUBLE, NULL);
        n->num_val = d;
        size_t nlen = (size_t)(end - p);
        char* tmp = (char*)malloc(nlen + 1);
        memcpy(tmp, p, nlen);
        tmp[nlen] = '\0';
        n->str_val = tmp;
    }
    *pp = end;
    return n;
}

static JsonNode* json_parse_keyword(const char** pp, const char* kw, int kwlen, int type, int bool_val) {
    const char* p = *pp;
    if (strncmp(p, kw, (size_t)kwlen) == 0) {
        JsonNode* n = json_new_node(type, NULL);
        if (type == JSON_BOOL) n->bool_val = bool_val;
        *pp = p + kwlen;
        return n;
    }
    return NULL;
}

static JsonNode* json_parse_array(const char** pp) {
    const char* p = *pp;
    if (*p != '[') return NULL;
    p++;
    JsonNode* n = json_new_node(JSON_ARRAY, NULL);
    p = json_skip_ws(p);
    if (*p == ']') { *pp = p + 1; return n; }
    while (1) {
        p = json_skip_ws(p);
        JsonNode* child = json_parse_value(&p);
        if (!child) { json_free_node(n); return NULL; }
        json_add_child(n, child);
        p = json_skip_ws(p);
        if (*p == ']') break;
        if (*p != ',') { json_free_node(n); return NULL; }
        p++;
    }
    *pp = p + 1;
    return n;
}

static JsonNode* json_parse_object(const char** pp) {
    const char* p = *pp;
    if (*p != '{') return NULL;
    p++;
    JsonNode* n = json_new_node(JSON_OBJECT, NULL);
    p = json_skip_ws(p);
    if (*p == '}') { *pp = p + 1; return n; }
    while (1) {
        p = json_skip_ws(p);
        // Parse key (string)
        JsonNode* key_node = json_parse_string(&p);
        if (!key_node || !key_node->str_val) { json_free_node(n); return NULL; }
        p = json_skip_ws(p);
        if (*p != ':') { json_free_node(key_node); json_free_node(n); return NULL; }
        p++;
        p = json_skip_ws(p);
        JsonNode* val = json_parse_value(&p);
        if (!val) { json_free_node(key_node); json_free_node(n); return NULL; }
        val->key = strdup(key_node->str_val);
        json_add_child(n, val);
        json_free_node(key_node);
        p = json_skip_ws(p);
        if (*p == '}') break;
        if (*p != ',') { json_free_node(n); return NULL; }
        p++;
    }
    *pp = p + 1;
    return n;
}

static JsonNode* json_parse_value(const char** pp) {
    const char* p = json_skip_ws(*pp);
    if (!*p) return NULL;
    JsonNode* n = NULL;
    if (*p == '"') n = json_parse_string(&p);
    else if (*p == '{') n = json_parse_object(&p);
    else if (*p == '[') n = json_parse_array(&p);
    else if (*p == 't' && strncmp(p, "true", 4) == 0) {
        n = json_new_node(JSON_BOOL, NULL); n->bool_val = 1; p += 4;
    }
    else if (*p == 'f' && strncmp(p, "false", 5) == 0) {
        n = json_new_node(JSON_BOOL, NULL); n->bool_val = 0; p += 5;
    }
    else if (*p == 'n' && strncmp(p, "null", 4) == 0) {
        n = json_new_node(JSON_NULL, NULL); p += 4;
    }
    else n = json_parse_number(&p);
    *pp = p;
    return n;
}

// Public API
int64_t myp_json_parse(const char* json_str) {
    if (!json_str) return 0;
    const char* p = json_str;
    JsonNode* root = json_parse_value(&p);
    return (int64_t)(intptr_t)root;
}

static JsonNode* json_resolve_path(JsonNode* root, const char* path) {
    if (!root || !path || !*path) return root;
    if (root->type != JSON_OBJECT && root->type != JSON_ARRAY) return NULL;

    // Split path by '.'
    char path_copy[256];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    JsonNode* current = root;
    char* token = strtok(path_copy, ".");
    while (token) {
        if (!current) return NULL;
        if (current->type == JSON_OBJECT) {
            int found = 0;
            for (int i = 0; i < current->child_count; i++) {
                if (current->children[i]->key && strcmp(current->children[i]->key, token) == 0) {
                    current = current->children[i];
                    found = 1;
                    break;
                }
            }
            if (!found) return NULL;
        } else if (current->type == JSON_ARRAY) {
            char* end = NULL;
            long idx = strtol(token, &end, 10);
            if (end == token || idx < 0 || idx >= current->child_count) return NULL;
            current = current->children[idx];
        } else {
            return NULL;
        }
        token = strtok(NULL, ".");
    }
    return current;
}

int32_t myp_json_get_type(int64_t handle, const char* path) {
    JsonNode* root = (JsonNode*)(intptr_t)handle;
    JsonNode* n = json_resolve_path(root, path);
    if (!n) return -1;
    return n->type;
}

const char* myp_json_get_string(int64_t handle, const char* path) {
    JsonNode* root = (JsonNode*)(intptr_t)handle;
    JsonNode* n = json_resolve_path(root, path);
    if (!n || (n->type != JSON_STRING && n->type != JSON_INT && n->type != JSON_DOUBLE)) return NULL;
    // M8: return a COUNTED copy — the node's str_val is malloc'd by the JSON
    // parser without an ARC header, so handing it out directly would make
    // myp_retain/myp_release read a bogus header.
    if (n->str_val) return myp_strdup(n->str_val);
    return myp_strdup("");
}

double myp_json_get_number(int64_t handle, const char* path) {
    JsonNode* root = (JsonNode*)(intptr_t)handle;
    JsonNode* n = json_resolve_path(root, path);
    if (!n) return 0.0;
    return n->num_val;
}

int32_t myp_json_get_bool(int64_t handle, const char* path) {
    JsonNode* root = (JsonNode*)(intptr_t)handle;
    JsonNode* n = json_resolve_path(root, path);
    if (!n || n->type != JSON_BOOL) return 0;
    return n->bool_val;
}

int32_t myp_json_array_length(int64_t handle, const char* path) {
    JsonNode* root = (JsonNode*)(intptr_t)handle;
    JsonNode* n = json_resolve_path(root, path);
    if (!n || n->type != JSON_ARRAY) return 0;
    return n->child_count;
}

void myp_json_free(int64_t handle) {
    json_free_node((JsonNode*)(intptr_t)handle);
}

// ======================
// String Hash (for HashMap<string, V>)
// ======================

int32_t myp_str_hash(const char* s) {
    if (!s) return 0;
    uint32_t hash = 5381;
    int c;
    while ((c = (unsigned char)*s++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return (int32_t)(hash & 0x7FFFFFFF);
}

// ======================
// File System Utilities
// ======================

#include <dirent.h>
#include <sys/stat.h>

int32_t myp_fs_exists(const char* path) {
    if (!path) return 0;
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
}

int32_t myp_fs_is_dir(const char* path) {
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int32_t myp_fs_is_file(const char* path) {
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

int64_t myp_fs_file_size(const char* path) {
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (int64_t)st.st_size;
}

int64_t myp_fs_modified_time(const char* path) {
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (int64_t)st.st_mtime * 1000;
}

int32_t myp_fs_list_count(const char* path) {
    if (!path) return 0;
    DIR* dir = opendir(path);
    if (!dir) return 0;
    int32_t count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        count++;
    }
    closedir(dir);
    return count;
}

char* myp_fs_list_get(const char* path, int32_t index) {
    if (!path || index < 0) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
    DIR* dir = opendir(path);
    if (!dir) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
    struct dirent* entry;
    int32_t cur = 0;
    char* result = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (cur == index) {
            result = myp_strdup(entry->d_name);
            break;
        }
        cur++;
    }
    closedir(dir);
    if (!result) { result = (char*)myp_alloc(1); result[0] = '\0'; }
    return result;
}

char* myp_fs_dirname(const char* path) {
    if (!path) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
    const char* slash = strrchr(path, '/');
    if (!slash) { char* r = myp_strdup("."); return r; }
    size_t len = (size_t)(slash - path);
    if (len == 0) { char* r = myp_strdup("/"); return r; }
    char* r = (char*)myp_alloc(len + 1);
    memcpy(r, path, len);
    r[len] = '\0';
    return r;
}

char* myp_fs_basename(const char* path) {
    if (!path) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
    const char* slash = strrchr(path, '/');
    if (!slash) return myp_strdup(path);
    return myp_strdup(slash + 1);
}

char* myp_fs_join(const char* dir, const char* file) {
    if (!dir && !file) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
    if (!dir) return myp_strdup(file);
    if (!file) return myp_strdup(dir);
    size_t dl = strlen(dir);
    size_t fl = strlen(file);
    int need_sep = (dl > 0 && dir[dl-1] != '/') ? 1 : 0;
    char* r = (char*)myp_alloc(dl + (size_t)need_sep + fl + 1);
    memcpy(r, dir, dl);
    if (need_sep) r[dl] = '/';
    memcpy(r + dl + need_sep, file, fl + 1);
    return r;
}

// 递归创建目录（mkdir -p）。返回 0 成功，-1 失败。
int32_t myp_fs_mkdir_p(const char* path) {
    if (!path || !path[0]) return -1;
    char* tmp = strdup(path);
    if (!tmp) return -1;
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len-1] == '/') tmp[--len] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return -1; }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return -1; }
    free(tmp);
    return 0;
}

// 递归删除文件/目录（rm -rf 语义）。返回 0 成功，-1 失败；不存在视为成功。
int32_t myp_fs_remove_recursive(const char* path) {
    if (!path || !path[0]) return -1;
    struct stat st;
    if (lstat(path, &st) != 0) return (errno == ENOENT) ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(path);
        if (!dir) return -1;
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char* child = myp_fs_join(path, entry->d_name);
            if (myp_fs_remove_recursive(child) != 0) { closedir(dir); return -1; }
        }
        closedir(dir);
        if (rmdir(path) != 0) return -1;
        return 0;
    }
    return remove(path) == 0 ? 0 : -1;
}

// ======================
// Networking (TCP Sockets)
// ======================

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

// Returns: server socket fd on success, -1 on error
int32_t myp_net_server(int32_t port) {
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
    socklen_t addr_len = sizeof(client_addr);
    int fd = (int)accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
    return fd;
}

// Connect to a TCP server
// Returns: socket fd on success, -1 on error
int32_t myp_net_connect(const char* host, int32_t port) {
    if (!host) return -1;
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
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

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

// ======================
// Command-Line Arguments
// ======================

// Globals set before main() via constructor
static int myp_saved_argc = 0;
static char** myp_saved_argv = NULL;

// Constructor runs before main() to capture command-line arguments.
// Reads /proc/self/cmdline (Linux-specific, no external dependencies).
__attribute__((constructor))
static void myp_capture_args(void) {
    FILE* fp = fopen("/proc/self/cmdline", "rb");
    if (!fp) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    int argc = 0;
    for (size_t i = 0; i < n; i++) { if (buf[i] == '\0') argc++; }
    if (argc == 0) return;
    myp_saved_argc = argc;
    myp_saved_argv = (char**)calloc((size_t)argc + 1, sizeof(char*));
    int idx = 0;
    size_t start = 0;
    for (size_t i = 0; i <= n && idx < argc; i++) {
        if (buf[i] == '\0') {
            myp_saved_argv[idx++] = strdup(buf + start);
            start = i + 1;
        }
    }
}

int32_t myp_args_count(void) {
    return myp_saved_argc;
}

char* myp_args_get(int32_t index) {
    if (index < 0 || index >= myp_saved_argc || !myp_saved_argv)
        { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    return myp_strdup(myp_saved_argv[index]);
}

// ======================
// Environment Variables
// ======================

#include <stdlib.h>

char* myp_env_get(const char* name) {
    if (!name) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    const char* val = getenv(name);
    if (!val) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    return myp_strdup(val);
}

int32_t myp_env_set(const char* name, const char* value) {
    if (!name || !value) return -1;
    return setenv(name, value, 1);
}

int32_t myp_env_unset(const char* name) {
    if (!name) return -1;
    return unsetenv(name);
}

// ======================
// String Enhancements
// ======================

char* myp_str_repeat(const char* s, int32_t count) {
    if (!s || count <= 0) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    size_t slen = strlen(s);
    size_t total = slen * (size_t)count;
    char* r = (char*)myp_alloc(total + 1);
    if (!r) return NULL;
    char* p = r;
    for (int32_t i = 0; i < count; i++) {
        memcpy(p, s, slen);
        p += slen;
    }
    *p = '\0';
    return r;
}

char* myp_str_pad_left(const char* s, int32_t total_len, int32_t pad_char) {
    if (!s) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    size_t slen = strlen(s);
    if ((int32_t)slen >= total_len) return myp_strdup(s);
    size_t pad_count = (size_t)(total_len - (int32_t)slen);
    size_t total = (size_t)total_len;
    char* r = (char*)myp_alloc(total + 1);
    if (!r) return NULL;
    memset(r, (char)pad_char, pad_count);
    memcpy(r + pad_count, s, slen);
    r[total] = '\0';
    return r;
}

char* myp_str_pad_right(const char* s, int32_t total_len, int32_t pad_char) {
    if (!s) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    size_t slen = strlen(s);
    if ((int32_t)slen >= total_len) return myp_strdup(s);
    size_t total = (size_t)total_len;
    char* r = (char*)myp_alloc(total + 1);
    if (!r) return NULL;
    memcpy(r, s, slen);
    memset(r + slen, (char)pad_char, total - slen);
    r[total] = '\0';
    return r;
}

char* myp_str_reverse(const char* s) {
    if (!s) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    size_t len = strlen(s);
    char* r = (char*)myp_alloc(len + 1);
    if (!r) return NULL;
    for (size_t i = 0; i < len; i++) r[i] = s[len - 1 - i];
    r[len] = '\0';
    return r;
}

char* myp_str_replace_all(const char* s, const char* old_str, const char* new_str) {
    if (!s || !old_str || !new_str || !*old_str) {
        if (!s) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
        return myp_strdup(s);
    }
    // First pass: count occurrences
    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    int count = 0;
    const char* p = s;
    while ((p = strstr(p, old_str)) != NULL) { count++; p += old_len; }
    if (count == 0) return myp_strdup(s);
    // Second pass: build result
    size_t total = strlen(s) + (size_t)count * (new_len - old_len);
    char* r = (char*)myp_alloc(total + 1);
    if (!r) return NULL;
    char* w = r;
    p = s;
    while (*p) {
        const char* found = strstr(p, old_str);
        if (found) {
            size_t copy_len = (size_t)(found - p);
            memcpy(w, p, copy_len); w += copy_len;
            memcpy(w, new_str, new_len); w += new_len;
            p = found + old_len;
        } else {
            size_t remaining = strlen(p);
            memcpy(w, p, remaining); w += remaining;
            break;
        }
    }
    *w = '\0';
    return r;
}

// ======================
// POSIX Regular Expressions
// ======================

#include <regex.h>

// Compile a regex pattern. Returns handle (>0) or 0 on error.
int64_t myp_regex_compile(const char* pattern) {
    if (!pattern) return 0;
    regex_t* re = (regex_t*)malloc(sizeof(regex_t));
    if (!re) return 0;
    int ret = regcomp(re, pattern, REG_EXTENDED);
    if (ret != 0) { free(re); return 0; }
    return (int64_t)(intptr_t)re;
}

// Test if string matches regex. Returns 1 if match, 0 otherwise.
int32_t myp_regex_match(int64_t handle, const char* s) {
    if (!handle || !s) return 0;
    regex_t* re = (regex_t*)(intptr_t)handle;
    return regexec(re, s, 0, NULL, 0) == 0 ? 1 : 0;
}

// Free compiled regex
void myp_regex_free(int64_t handle) {
    if (!handle) return;
    regex_t* re = (regex_t*)(intptr_t)handle;
    regfree(re);
    free(re);
}

// ======================
// Base64 Encoding
// ======================

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char* myp_base64_encode(const char* data) {
    if (!data) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    size_t len = strlen(data);
    size_t out_len = ((len + 2) / 3) * 4;
    char* r = (char*)myp_alloc(out_len + 1);
    if (!r) return NULL;
    size_t i = 0, o = 0;
    while (i < len) {
        unsigned char b0 = (unsigned char)data[i++];
        int have_b1 = (i < len) ? 1 : 0;
        unsigned char b1 = have_b1 ? (unsigned char)data[i++] : 0;
        int have_b2 = (i < len) ? 1 : 0;
        unsigned char b2 = have_b2 ? (unsigned char)data[i++] : 0;
        r[o++] = b64_table[b0 >> 2];
        r[o++] = b64_table[((b0 & 0x03) << 4) | (b1 >> 4)];
        r[o++] = have_b1 ? b64_table[((b1 & 0x0F) << 2) | (b2 >> 6)] : '=';
        r[o++] = have_b2 ? b64_table[b2 & 0x3F] : '=';
    }
    r[out_len] = '\0';
    return r;
}

static int b64_index(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return 0;
}

char* myp_base64_decode(const char* data) {
    if (!data) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    size_t len = strlen(data);
    if (len == 0) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    // Count padding
    size_t pad = 0;
    if (len > 0 && data[len-1] == '=') pad++;
    if (len > 1 && data[len-2] == '=') pad++;
    size_t out_len = (len / 4) * 3 - pad;
    char* r = (char*)myp_alloc(out_len + 1);
    if (!r) return NULL;
    size_t i = 0, o = 0;
    while (i < len) {
        unsigned char c[4];
        int nc = 0;
        while (nc < 4 && i < len && data[i] != '=') {
            c[nc++] = (unsigned char)b64_index(data[i++]);
        }
        if (nc < 4) i = len; // skip rest on padding
        if (nc >= 2) r[o++] = (unsigned char)((c[0] << 2) | (c[1] >> 4));
        if (nc >= 3) r[o++] = (unsigned char)(((c[1] & 0x0F) << 4) | (c[2] >> 2));
        if (nc >= 4) r[o++] = (unsigned char)(((c[2] & 0x03) << 6) | c[3]);
    }
    r[out_len] = '\0';
    return r;
}

typedef struct myp_alloc_node {
    struct myp_alloc_node* next;
    struct myp_alloc_node* prev;
} myp_alloc_node_t;

// M6: PROCESS-GLOBAL alloc list. ARC blocks (class instances, counted strings,
// arrays, slice backings) are all tracked here so leaked / program-lifetime
// objects can be freed once at process exit. A node is pushed/removed under a
// single spinlock — the critical section is two pointer writes — so an object
// allocated on thread A and freed (rc→0) on thread B is safe. (The old TLS
// list was thread-local: a foreign node's prev/next point into another
// thread's list, so cross-thread free corrupted the list and double-freed at
// the allocating thread's exit.) The list is freed ONLY at process exit (after
// all @thread workers are joined), never per-thread.
static pthread_spinlock_t myp_alloc_lock;
static pthread_once_t myp_alloc_lock_once = PTHREAD_ONCE_INIT;
static void myp_alloc_lock_init(void) {
    pthread_spin_init(&myp_alloc_lock, PTHREAD_PROCESS_PRIVATE);
}
static void myp_alloc_lock_ensure(void) {
    pthread_once(&myp_alloc_lock_once, myp_alloc_lock_init);
}
static myp_alloc_node_t* myp_alloc_head = NULL;

static void myp_alloc_list_push(myp_alloc_node_t* node) {
    myp_alloc_lock_ensure();
    pthread_spin_lock(&myp_alloc_lock);
    node->next = myp_alloc_head;
    node->prev = NULL;
    if (myp_alloc_head) myp_alloc_head->prev = node;
    myp_alloc_head = node;
    pthread_spin_unlock(&myp_alloc_lock);
}

static void myp_alloc_list_remove(myp_alloc_node_t* node) {
    myp_alloc_lock_ensure();
    pthread_spin_lock(&myp_alloc_lock);
    if (node->prev) node->prev->next = node->next;
    else myp_alloc_head = node->next;
    if (node->next) node->next->prev = node->prev;
    pthread_spin_unlock(&myp_alloc_lock);
}

// Free every tracked block still alive at process exit (leaked or
// program-lifetime objects). Runs on the main thread after all workers are
// joined, so no other thread can be using them.
static void myp_free_alloc_list_global(void) {
    myp_alloc_node_t* node;
    myp_alloc_lock_ensure();
    pthread_spin_lock(&myp_alloc_lock);
    node = myp_alloc_head;
    myp_alloc_head = NULL;
    pthread_spin_unlock(&myp_alloc_lock);
    while (node) {
        myp_alloc_node_t* next = node->next;
        free(node);
        node = next;
    }
}

// Forward decl — chunked bump arena defined below (after myp_free).
static void* myp_arena_bump_alloc(size_t size);
// Forward decl — counted-string allocator defined with the ARC machinery
// (needs myp_obj_header_t / MYP_STR_TYPE_ID, below).
static void* myp_alloc_str(size_t size);

void* myp_alloc(size_t size) {
    // Strings are ref-counted (§五-A M8): myp_alloc is now string-only
    // (slices/arrays use myp_alloc_slice_backing), so every allocation hands
    // out a counted {rc=1, type_id=STR} header + bytes. The chunked bump
    // arena below remains for @region temporaries (myp_region_alloc).
    return myp_alloc_str(size);
}

void myp_free(void* ptr) {
    // Individual free is optional — myp_free_all() at exit handles bulk cleanup.
    // This is provided for cases where deterministic early release is needed.
    // NOTE: only valid for pointers NOT from the bump arena (arena blocks are
    // released in bulk; myp_free is not emitted by codegen for arena data).
    free(ptr);
}

// ======================
// Chunked bump arena — for non-ARC allocations (strings, slices, T[]).
// ======================
// Strings/slices/arrays are never individually freed (only bulk at exit, or
// @region release). The old myp_alloc did malloc(size) + malloc(16-byte node)
// and pushed the node onto a TLS list — 2 mallocs per allocation, plus malloc
// header overhead on every tiny object. Long-running allocation-heavy code
// (servers, loops creating slices/strings) wasted 2-3x the data size in
// bookkeeping + fragmentation, and RSS grew accordingly.
//
// This chunked bump allocator hands out memory from large chunks (zero
// per-allocation malloc), tracks only the chunks, and bulk-frees them at exit.
// Thread-local (per-thread arena), non-atomic — same model as the old lists.
#define MYP_ARENA_CHUNK_SIZE (64 * 1024)
#define MYP_ARENA_ALIGN 16

typedef struct myp_arena_chunk {
    struct myp_arena_chunk* next;
    size_t used;      // bump offset within this chunk
    size_t cap;       // capacity of data area (sizeof(header) + cap bytes)
    /* data follows immediately after the header */
} myp_arena_chunk_t;

static pthread_key_t myp_arena_key;
static pthread_once_t myp_arena_key_once = PTHREAD_ONCE_INIT;
static __thread myp_arena_chunk_t* myp_arena_cur = NULL;  // head = current chunk

static void myp_free_arena_chunks(void* ptr) {
    myp_arena_chunk_t* c = (myp_arena_chunk_t*)ptr;
    while (c) {
        myp_arena_chunk_t* next = c->next;
        free(c);
        c = next;
    }
}

static void myp_make_arena_key(void) {
    pthread_key_create(&myp_arena_key, myp_free_arena_chunks);
}

static void* myp_arena_bump_alloc(size_t size) {
    pthread_once(&myp_arena_key_once, myp_make_arena_key);
    if (size > SIZE_MAX - (MYP_ARENA_ALIGN - 1)) myp_oom(size);
    size = (size + MYP_ARENA_ALIGN - 1) & ~(size_t)(MYP_ARENA_ALIGN - 1);
    if (!myp_arena_cur || size > myp_arena_cur->cap - myp_arena_cur->used) {
        // Need a fresh chunk. Oversized allocations get a dedicated chunk.
        size_t chunk_cap = size > MYP_ARENA_CHUNK_SIZE ? size : MYP_ARENA_CHUNK_SIZE;
        size_t chunk_total;
        if (myp_add_overflow(sizeof(myp_arena_chunk_t), chunk_cap, &chunk_total))
            myp_oom(chunk_cap);
        myp_arena_chunk_t* c = (myp_arena_chunk_t*)myp_xmalloc(chunk_total);
        c->next = myp_arena_cur;
        c->used = 0;
        c->cap = chunk_cap;
        myp_arena_cur = c;
        // Keep the head in the pthread key so the TLS destructor frees it all.
        pthread_setspecific(myp_arena_key, myp_arena_cur);
    }
    void* p = (char*)myp_arena_cur + sizeof(myp_arena_chunk_t) + myp_arena_cur->used;
    myp_arena_cur->used += size;
    return p;
}

// Free all bump-arena chunks for this thread (used by myp_free_all).
static void myp_arena_free_all(void) {
    pthread_once(&myp_arena_key_once, myp_make_arena_key);
    myp_arena_chunk_t* head = (myp_arena_chunk_t*)pthread_getspecific(myp_arena_key);
    if (head) {
        myp_free_arena_chunks(head);
        pthread_setspecific(myp_arena_key, NULL);
        myp_arena_cur = NULL;
    }
}

// ---- M9: arena byte accounting (payload reserved/used, thread-local) ----
// Walks the current thread's chunk chain; diagnostics are called rarely so
// O(chunks) is fine. "reserved" = total payload capacity, "used" = bump offset.
int64_t myp_diag_arena_reserved(void) {
    int64_t n = 0;
    for (myp_arena_chunk_t* c = myp_arena_cur; c; c = c->next) n += (int64_t)c->cap;
    return n;
}
int64_t myp_diag_arena_used(void) {
    int64_t n = 0;
    for (myp_arena_chunk_t* c = myp_arena_cur; c; c = c->next) n += (int64_t)c->used;
    return n;
}

// ======================
// ARC — automatic reference counting on class instances (§五-1)
// ======================
// Class instances are allocated with an 8-byte header { rc:u32, type_id:u32 }
// placed immediately BEFORE the data pointer handed to generated code. All
// existing field GEPs / this / vtable accesses use the data pointer, so no
// codegen offset churn is needed; retain/release reach the header at (obj-8).
// The per-TU __myp_release_table (defined by generated code) is indexed by
// type_id and holds each class's destroy stub (cascades ref fields, then
// myp_free_object). Thread-local + non-atomic (objects don't cross threads,
// §7 of docs/arc.md).

typedef struct myp_obj_header {
    _Atomic uint32_t rc;       // M6: atomic — objects may be shared across threads
    uint32_t type_id;
} myp_obj_header_t;

#define MYP_OBJ_HEADER_SIZE ((size_t)sizeof(myp_obj_header_t))

// ---- Ref-counted class arrays (§五-1) ----
// `new T[n]` where T is a class allocates a 24-byte header before the
// element-data pointer handed to generated code. rc/type_id sit at the SAME
// obj-8/obj-4 offsets as the class-object header, so myp_retain (reads obj-8)
// and myp_release (reads obj-4 → detects MYP_ARR_TYPE_ID) work uniformly on
// both objects and arrays.
#define MYP_ARR_TYPE_ID 0xFFFFFFFFu
// Strings are ref-counted too (§五-A M8): myp_alloc allocates {rc=1, type_id=STR}
// before the bytes. Distinct from MYP_ARR_TYPE_ID and class type_ids.
#define MYP_STR_TYPE_ID 0xFFFFFFFEu
// Element-kind of a ref-counted array/slice backing, stored in `pad` so the
// header size stays 24 bytes. myp_release uses it to dispose elements.
#define MYP_ARR_ELEM_CLASS  0u   // elements are class refs → release each
#define MYP_ARR_ELEM_SCALAR 1u   // scalar/struct elements → just free block
#define MYP_ARR_ELEM_SLICE  2u   // elements are {data,len} fat ptrs → release data

typedef struct myp_arr_header {
    uint64_t count;
    uint32_t elem_size;
    uint32_t pad;
    _Atomic uint32_t rc;     // == myp_obj_header.rc   (at obj-8)
    uint32_t type_id;        // == MYP_ARR_TYPE_ID     (at obj-4)
} myp_arr_header_t;
#define MYP_ARR_HEADER_SIZE ((size_t)sizeof(myp_arr_header_t))

// Live class-object count (thread-local) — diagnostic aid for ARC tests.
static __thread int64_t myp_live_objects = 0;
// M8 diagnostics: live counted strings (alloc-freed balance).
static __thread int64_t myp_live_strings = 0;
// M9: live ref-counted array/slice backing count.
static __thread int64_t myp_live_arrays = 0;
int64_t myp_live_string_count(void) { return myp_live_strings; }

int64_t myp_live_object_count(void) { return myp_live_objects; }
int64_t myp_live_array_count(void) { return myp_live_arrays; }
int64_t myp_live_total_count(void) {
    return myp_live_objects + myp_live_strings + myp_live_arrays;
}

// M9: per-type live class-instance counts (type_id → live count). type_ids are
// dense 1..max from generated code; the TLS array grows on demand. Diagnostic
// only — OOM during growth just keeps the old (undersized) table. Freed at
// thread/process exit via a pthread key destructor (like the arena keys) so
// LeakSanitizer stays clean.
static __thread int64_t* myp_type_live = NULL;
static __thread int myp_type_live_cap = 0;
static pthread_key_t myp_type_live_key;
static pthread_once_t myp_type_live_key_once = PTHREAD_ONCE_INIT;
static void myp_free_type_live(void* p) { free(p); }
static void myp_make_type_live_key(void) {
    pthread_key_create(&myp_type_live_key, myp_free_type_live);
}
static void myp_type_live_inc(int tid) {
    if (tid <= 0) return;
    if (tid >= myp_type_live_cap) {
        int nc = myp_type_live_cap ? myp_type_live_cap * 2 : 16;
        while (nc <= tid) nc *= 2;
        int64_t* np = (int64_t*)realloc(myp_type_live, (size_t)nc * sizeof(int64_t));
        if (!np) return;
        for (int i = myp_type_live_cap; i < nc; i++) np[i] = 0;
        myp_type_live = np;
        myp_type_live_cap = nc;
        pthread_once(&myp_type_live_key_once, myp_make_type_live_key);
        pthread_setspecific(myp_type_live_key, myp_type_live);
    }
    myp_type_live[tid]++;
}
static void myp_type_live_dec(int tid) {
    if (tid > 0 && tid < myp_type_live_cap && myp_type_live[tid] > 0)
        myp_type_live[tid]--;
}
int64_t myp_live_object_count_by_type(int64_t type_id) {
    if (type_id <= 0 || type_id >= myp_type_live_cap) return 0;
    return myp_type_live[type_id];
}

// M9: strict header checks. In debug/ASAN builds they are ON by default and
// Memory.setStrictChecks can toggle them at runtime. Catching a release
// underflow (double free), or a header whose type_id is not STR/ARR/a valid
// class id, aborts with a stable diagnostic instead of corrupting.
// __myp_max_type_id is emitted by generated code (weak extern so a standalone
// runtime TU that omits it degrades to 0 = only STR/ARR headers validate).
extern const int __myp_max_type_id __attribute__((weak));
static __thread int myp_strict_checks =
#ifdef MYP_SANITIZE
    1
#else
    0
#endif
    ;
void myp_diag_set_strict(int64_t on) { myp_strict_checks = on ? 1 : 0; }
int64_t myp_diag_get_strict(void) { return myp_strict_checks; }
static int myp_header_type_id_ok(uint32_t tid) {
    if (tid == MYP_STR_TYPE_ID || tid == MYP_ARR_TYPE_ID) return 1;
    if (tid > 0 && &__myp_max_type_id && (int)tid <= __myp_max_type_id) return 1;
    return 0;
}
static void myp_strict_abort_header(void* obj, uint32_t tid) {
    fprintf(stderr, "MYP runtime [strict]: corrupted object header at %p "
                    "(illegal type_id 0x%x)\n", obj, (unsigned)tid);
    abort();
}
static void myp_strict_abort_underflow(void* obj) {
    fprintf(stderr, "MYP runtime [strict]: release underflow / double free "
                    "on object %p\n", obj);
    abort();
}

void* myp_alloc_object(size_t size, uint32_t type_id) {
    size_t total;
    if (myp_add_overflow(sizeof(myp_alloc_node_t) + MYP_OBJ_HEADER_SIZE, size, &total))
        myp_oom(size);
    myp_alloc_node_t* node = (myp_alloc_node_t*)myp_xmalloc(total);
    char* base = (char*)node + sizeof(myp_alloc_node_t);
    myp_obj_header_t* h = (myp_obj_header_t*)base;
    h->rc = 1;
    h->type_id = type_id;
    myp_alloc_list_push(node);
    myp_live_objects++;
    myp_type_live_inc((int)type_id);   // M9
    return base + MYP_OBJ_HEADER_SIZE;  // data pointer
}

static void* myp_alloc_str(size_t size) {
    // Counted string: {node, {rc=1, type_id=STR}, bytes}. Same 8-byte header
    // layout as class objects, so myp_retain (reads data-8) and myp_release
    // (reads data-4 -> MYP_STR_TYPE_ID) work uniformly on strings, objects,
    // and arrays. Strings are immutable and shared by value; the count is a
    // handle count, and myp_release frees the block at zero (medium lifetime
    // is now reclaimed, not held to process exit).
    size_t total;
    if (myp_add_overflow(sizeof(myp_alloc_node_t) + MYP_OBJ_HEADER_SIZE, size, &total))
        myp_oom(size);
    myp_alloc_node_t* node = (myp_alloc_node_t*)myp_xmalloc(total);
    char* base = (char*)node + sizeof(myp_alloc_node_t);
    myp_obj_header_t* h = (myp_obj_header_t*)base;
    h->rc = 1;
    h->type_id = MYP_STR_TYPE_ID;
    myp_alloc_list_push(node);
    myp_live_strings++;
    return base + MYP_OBJ_HEADER_SIZE;  // data bytes
}

void myp_retain(void* obj) {
    if (!obj) return;
    myp_obj_header_t* h = (myp_obj_header_t*)((char*)obj - MYP_OBJ_HEADER_SIZE);
    if (myp_strict_checks && !myp_header_type_id_ok(h->type_id))
        myp_strict_abort_header(obj, h->type_id);
    // M6: relaxed atomic increment — safe for concurrent retain across threads.
    atomic_fetch_add_explicit(&h->rc, 1, memory_order_relaxed);
}

// ---- M7: weak references ----
// A `@weak` class field stores a plain pointer to its target (no retain). A
// global chained-hash registry maps target object → the set of weak-slot
// ADDRESSES pointing at it. When an object's rc hits 0 it nulls every
// registered slot first (so no reader ever sees a dangling weak), drops the
// registry entry, then frees. Weak holders unregister their slots when THEY are
// destroyed (their destroy stub calls myp_weak_clear), so a target's list never
// holds addresses into freed holder memory.
// All weak ops share ONE global spinlock; weak fields are rare so it is
// effectively uncontended. The lock also makes myp_weak_load's weak→strong
// upgrade race-free against destruction: while the lock is held the target
// cannot be freed, so the retain inside the critical section is safe.
typedef struct myp_weak_entry {
    struct myp_weak_entry* next;   // hash-bucket chain
    void* target;                  // the weak-referenced object
    void** slots;                  // dynamic array of weak slot addresses
    size_t count, cap;
} myp_weak_entry_t;

#define MYP_WEAK_BUCKETS 64
static myp_weak_entry_t* myp_weak_table[MYP_WEAK_BUCKETS];
static pthread_spinlock_t myp_weak_lock;
static pthread_once_t myp_weak_lock_once = PTHREAD_ONCE_INIT;
static void myp_weak_lock_init(void) { pthread_spin_init(&myp_weak_lock, PTHREAD_PROCESS_PRIVATE); }
static void myp_weak_lock_ensure(void) { pthread_once(&myp_weak_lock_once, myp_weak_lock_init); }

static unsigned myp_weak_hash(void* p) {
    uintptr_t v = (uintptr_t)p;
    return (unsigned)((v >> 4) ^ (v >> 12) ^ (v >> 20)) % MYP_WEAK_BUCKETS;
}
static myp_weak_entry_t* myp_weak_find_locked(void* target) {
    unsigned b = myp_weak_hash(target);
    for (myp_weak_entry_t* e = myp_weak_table[b]; e; e = e->next)
        if (e->target == target) return e;
    return NULL;
}
static void myp_weak_remove_entry_locked(myp_weak_entry_t* e) {
    unsigned b = myp_weak_hash(e->target);
    myp_weak_entry_t** link = &myp_weak_table[b];
    while (*link && *link != e) link = &(*link)->next;
    if (*link) *link = e->next;
    free(e->slots);
    free(e);
}
static void myp_weak_add_locked(void* target, void** slot) {
    myp_weak_entry_t* e = myp_weak_find_locked(target);
    if (!e) {
        e = (myp_weak_entry_t*)calloc(1, sizeof(myp_weak_entry_t));
        if (!e) return;
        e->target = target;
        unsigned b = myp_weak_hash(target);
        e->next = myp_weak_table[b];
        myp_weak_table[b] = e;
    }
    if (e->count >= e->cap) {
        size_t nc = e->cap ? e->cap * 2 : 4;
        void** ns = (void**)realloc(e->slots, nc * sizeof(void*));
        if (!ns) return;
        e->slots = ns;
        e->cap = nc;
    }
    e->slots[e->count++] = (void*)slot;
}
static void myp_weak_remove_locked(void* target, void** slot) {
    myp_weak_entry_t* e = myp_weak_find_locked(target);
    if (!e) return;
    for (size_t i = 0; i < e->count; i++) {
        if ((void*)e->slots[i] == slot) {
            e->slots[i] = e->slots[--e->count];
            if (e->count == 0) myp_weak_remove_entry_locked(e);
            return;
        }
    }
}

// Set a weak field to `obj` (no retain) and keep the registry consistent.
void myp_weak_store(void** slot, void* obj) {
    myp_weak_lock_ensure();
    pthread_spin_lock(&myp_weak_lock);
    void* old = *slot;
    if (old != obj) {
        if (old) myp_weak_remove_locked(old, slot);
        *slot = obj;
        if (obj) myp_weak_add_locked(obj, slot);
    }
    pthread_spin_unlock(&myp_weak_lock);
}

// The HOLDER is being destroyed: unregister this weak slot and null it. Called
// from the holder's destroy stub before the holder's memory is freed.
void myp_weak_clear(void** slot) {
    myp_weak_lock_ensure();
    pthread_spin_lock(&myp_weak_lock);
    void* old = *slot;
    *slot = NULL;
    if (old) myp_weak_remove_locked(old, slot);
    pthread_spin_unlock(&myp_weak_lock);
}

// Weak→strong upgrade: return a STRONG ref (rc+1) or NULL if the target died.
// Caller owns the returned ref (release it like a fresh value).
void* myp_weak_load(void** slot) {
    myp_weak_lock_ensure();
    pthread_spin_lock(&myp_weak_lock);
    void* obj = *slot;
    if (obj) myp_retain(obj);   // atomic; target cannot be freed while lock held
    pthread_spin_unlock(&myp_weak_lock);
    return obj;
}

// Called by myp_release when an object's rc hits 0: null all weak slots
// observing it and drop its registry entry. Returns 1 if this thread is the
// true last owner (should free); 0 if a concurrent weak_load re-bumped rc under
// the lock first (the object lives; the new owner will free it).
static int myp_weak_notify_death(void* obj) {
    myp_weak_lock_ensure();
    pthread_spin_lock(&myp_weak_lock);
    myp_obj_header_t* h = (myp_obj_header_t*)((char*)obj - MYP_OBJ_HEADER_SIZE);
    if (atomic_load_explicit(&h->rc, memory_order_relaxed) != 0) {
        pthread_spin_unlock(&myp_weak_lock);
        return 0;   // someone re-bumped rc — object lives
    }
    myp_weak_entry_t* e = myp_weak_find_locked(obj);
    if (e) {
        for (size_t i = 0; i < e->count; i++)
            *((void**)e->slots[i]) = NULL;
        myp_weak_remove_entry_locked(e);
    }
    pthread_spin_unlock(&myp_weak_lock);
    return 1;
}

// Free the whole weak registry at process exit (only entry/slot-array blocks;
// slot addresses may dangle but are never dereferenced here).
void myp_weak_free_all(void) {
    myp_weak_lock_ensure();
    pthread_spin_lock(&myp_weak_lock);
    for (int i = 0; i < MYP_WEAK_BUCKETS; i++) {
        myp_weak_entry_t* e = myp_weak_table[i];
        while (e) {
            myp_weak_entry_t* next = e->next;
            free(e->slots);
            free(e);
            e = next;
        }
        myp_weak_table[i] = NULL;
    }
    pthread_spin_unlock(&myp_weak_lock);
}

// Forward decl — defined after myp_free_object, used by myp_release below.
static void myp_free_class_array(void* data);

uint32_t myp_release(void* obj) {
    if (!obj) return 0;
    myp_obj_header_t* h = (myp_obj_header_t*)((char*)obj - MYP_OBJ_HEADER_SIZE);
    if (myp_strict_checks) {
        if (!myp_header_type_id_ok(h->type_id))
            myp_strict_abort_header(obj, h->type_id);
        if (atomic_load_explicit(&h->rc, memory_order_relaxed) == 0)
            myp_strict_abort_underflow(obj);
    }
    // M6: release-acquire on the count. The last release (rc→0) takes an
    // acquire fence so all prior writes to the object by any thread are
    // visible before it is freed. atomic_fetch_sub returns the OLD value.
    uint32_t old_rc = atomic_fetch_sub_explicit(&h->rc, 1, memory_order_release);
    if (old_rc == 0) {   // safety: never underflow (non-strict restore)
        atomic_store_explicit(&h->rc, 0, memory_order_relaxed);
        return 0;
    }
    uint32_t new_rc = old_rc - 1;
    if (new_rc == 0) {
        atomic_thread_fence(memory_order_acquire);
        // M7: null weak observers BEFORE freeing. If a concurrent weak_load
        // re-bumped rc under the weak lock, we are not the true last owner —
        // leave the object alive; the new owner frees it on its own release.
        if (!myp_weak_notify_death(obj)) {
            return atomic_load_explicit(&h->rc, memory_order_relaxed);
        }
        // Cache type_id BEFORE the destroy stub runs — the stub frees the
        // object, so reading h->* afterward would be a use-after-free.
        uint32_t tid = h->type_id;
        // Ref-counted array/slice backing: dispose elements per element-kind,
        // then free the header+data block.
        if (tid == MYP_ARR_TYPE_ID) {
            myp_arr_header_t* ah =
                (myp_arr_header_t*)((char*)obj - MYP_ARR_HEADER_SIZE);
            if (ah->pad == MYP_ARR_ELEM_SLICE) {
                // Elements are slice fat pointers {data, len} (16 bytes):
                // release each inner backing. Stride by elem_size so a
                // non-16-byte struct-of-slice layout still walks correctly.
                char* p = (char*)obj;
                for (uint64_t i = 0; i < ah->count; i++) {
                    void* inner_data = *(void**)p;
                    myp_release(inner_data);
                    p += ah->elem_size;
                }
            } else if (ah->pad == MYP_ARR_ELEM_CLASS) {
                void** elems = (void**)obj;
                for (uint64_t i = 0; i < ah->count; i++)
                    myp_release(elems[i]);
            }
            // MYP_ARR_ELEM_SCALAR: no per-element release.
            myp_free_class_array(obj);
        }
        // Counted string: free the {node, header, bytes} block.
        else if (tid == MYP_STR_TYPE_ID) {
            char* base = (char*)obj - MYP_OBJ_HEADER_SIZE;
            myp_alloc_node_t* node =
                (myp_alloc_node_t*)(base - sizeof(myp_alloc_node_t));
            myp_alloc_list_remove(node);
            if (myp_live_strings > 0) myp_live_strings--;
            free(node);
        }
        // Dispatch to the per-TU destroy stub (cascades reference fields).
        else if (tid > 0 && __myp_release_table[tid])
            __myp_release_table[tid](obj);
        else
            myp_free_object(obj);
    }
    return new_rc;
}

void myp_free_object(void* obj) {
    if (!obj) return;
    char* base = (char*)obj - MYP_OBJ_HEADER_SIZE;
    myp_obj_header_t* h = (myp_obj_header_t*)base;
    int tid = (int)h->type_id;
    myp_alloc_node_t* node =
        (myp_alloc_node_t*)(base - sizeof(myp_alloc_node_t));
    myp_alloc_list_remove(node);
    if (myp_live_objects > 0) myp_live_objects--;
    myp_type_live_dec(tid);   // M9: per-type count
    free(node);
}

// ---- Ref-counted class arrays (§五-1): allocation + element release ----
void* myp_alloc_class_array(uint64_t count, uint32_t elem_size) {
    size_t data_bytes, total;
    if (myp_mul_overflow((size_t)count, (size_t)elem_size, &data_bytes))
        myp_oom((size_t)count);
    if (myp_add_overflow(sizeof(myp_alloc_node_t) + MYP_ARR_HEADER_SIZE, data_bytes, &total))
        myp_oom(data_bytes);
    myp_alloc_node_t* node = (myp_alloc_node_t*)myp_xmalloc(total);
    char* base = (char*)node + sizeof(myp_alloc_node_t);
    myp_arr_header_t* h = (myp_arr_header_t*)base;
    h->count = count;
    h->elem_size = elem_size;
    h->pad = 0;
    h->rc = 1;
    h->type_id = MYP_ARR_TYPE_ID;
    myp_alloc_list_push(node);
    myp_live_arrays++;   // M9
    return base + MYP_ARR_HEADER_SIZE;  // element-data pointer
}

static void myp_free_class_array(void* data) {
    if (!data) return;
    char* base = (char*)data - MYP_ARR_HEADER_SIZE;
    myp_alloc_node_t* node =
        (myp_alloc_node_t*)(base - sizeof(myp_alloc_node_t));
    myp_alloc_list_remove(node);
    if (myp_live_arrays > 0) myp_live_arrays--;   // M9
    free(node);
}

// ---- Ref-counted slice backing (§五-1 M8) ----
// Same header layout as class arrays; the `pad` field records the element kind
// so myp_release knows how to dispose elements (class refs / none / nested
// slice fat pointers). This is what makes slice<T> values ARC-managed: every
// slice value holds a counted reference to this backing.
void* myp_alloc_slice_backing(uint64_t count, uint32_t elem_size, uint32_t elem_kind) {
    size_t data_bytes, total;
    if (myp_mul_overflow((size_t)count, (size_t)elem_size, &data_bytes))
        myp_oom((size_t)count);
    if (myp_add_overflow(sizeof(myp_alloc_node_t) + MYP_ARR_HEADER_SIZE, data_bytes, &total))
        myp_oom(data_bytes);
    myp_alloc_node_t* node = (myp_alloc_node_t*)myp_xmalloc(total);
    char* base = (char*)node + sizeof(myp_alloc_node_t);
    myp_arr_header_t* h = (myp_arr_header_t*)base;
    h->count = count;
    h->elem_size = elem_size;
    h->pad = elem_kind;
    h->rc = 1;
    h->type_id = MYP_ARR_TYPE_ID;
    myp_alloc_list_push(node);
    myp_live_arrays++;   // M9
    return base + MYP_ARR_HEADER_SIZE;  // element-data pointer
}

// P1（docs/type_system_design §6.3）：string ↔ ubyte[] 互转（bytes(s)/str(bytes)）。
// bytes(s)：把字符串字节拷贝进一个计数 ubyte[] backing（myp_alloc_slice_backing，
// elem_kind=scalar），返回 data 指针（与 `new ubyte[n]` 同一 ABI，MYP 动态数组槽位
// 直接持有该引用）。str(bytes)：从 ubyte[] data 指针（头部 count 为长度）构造计数
// 字符串（rc=1）。
void* myp_str_to_bytes(const char* s) {
    if (!s) s = "";
    size_t len = strlen(s);
    void* data = myp_alloc_slice_backing((uint64_t)len, 1, MYP_ARR_ELEM_SCALAR);
    if (!data) return NULL;
    memcpy(data, s, len);
    return data;
}
// bytesOf(bitvector<N>)（§5.1）：把位向量值按小端写进 nbytes 个字节的计数
// ubyte[] backing（MYP_ARR_ELEM_SCALAR），返回 data 指针（同 new ubyte[n] ABI）。
void* myp_uint_to_bytes(uint64_t v, int32_t nbytes) {
    if (nbytes <= 0) nbytes = 1;
    if (nbytes > 8) nbytes = 8;
    void* data = myp_alloc_slice_backing((uint64_t)nbytes, 1, MYP_ARR_ELEM_SCALAR);
    if (!data) return NULL;
    for (int i = 0; i < nbytes; i++)
        ((uint8_t*)data)[i] = (uint8_t)(v >> (8 * i));
    return data;
}
char* myp_bytes_to_str(const void* data) {
    if (!data) return myp_strdup("");
    myp_arr_header_t* h = (myp_arr_header_t*)((const char*)data - MYP_ARR_HEADER_SIZE);
    uint64_t n = h->count;
    char* r = (char*)myp_alloc((size_t)n + 1);
    if (!r) return NULL;
    memcpy(r, data, (size_t)n);
    r[n] = '\0';
    return r;
}

typedef struct myp_class_slice_cleanup {
    struct myp_class_slice_cleanup* next;
    void* data;
    int region_depth;
} myp_class_slice_cleanup_t;

static __thread int myp_region_depth = 0;
static __thread myp_class_slice_cleanup_t* myp_class_slice_cleanups = NULL;

static void myp_register_class_slice(void* data) {
    myp_class_slice_cleanup_t* cleanup =
        (myp_class_slice_cleanup_t*)malloc(sizeof(myp_class_slice_cleanup_t));
    if (!cleanup) return;  // OOM: keep backing live; leak-safe, never dangling
    cleanup->data = data;
    cleanup->region_depth = myp_region_depth;
    cleanup->next = myp_class_slice_cleanups;
    myp_class_slice_cleanups = cleanup;
}

static void myp_release_class_slices_from_depth(int region_depth) {
    myp_class_slice_cleanup_t** link = &myp_class_slice_cleanups;
    while (*link) {
        myp_class_slice_cleanup_t* cleanup = *link;
        if (cleanup->region_depth >= region_depth) {
            *link = cleanup->next;
            myp_release(cleanup->data);
            free(cleanup);
        } else {
            link = &cleanup->next;
        }
    }
}

void* myp_alloc_class_slice(uint64_t count) {
    void* data = myp_alloc_slice_backing(count, (uint32_t)sizeof(void*), MYP_ARR_ELEM_CLASS);
    if (data) myp_register_class_slice(data);
    return data;
}

// Release `count` element refs of a fixed (stack) class array. Elements are
// strong class-ref slots; the backing stack buffer is NOT freed.
void myp_release_fixed_class_array(void* data, uint64_t count) {
    if (!data) return;
    void** elems = (void**)data;
    for (uint64_t i = 0; i < count; i++)
        myp_release(elems[i]);
}

// ---- RTTI (§五-4): read type info back out of the object header ----
// __myp_type_name_table (declared in mylang/runtime.h, defined by generated
// code) maps type_id → class name string; index 0 = "" (reserved for
// non-class / string messages). Callers must pass a live object.

// Runtime type id of a class instance (0 = null / non-class object).
int myp_obj_type_id(void* obj) {
    if (!obj) return 0;
    myp_obj_header_t* h = (myp_obj_header_t*)((char*)obj - MYP_OBJ_HEADER_SIZE);
    return (int)h->type_id;
}

// Class name for a type id ("" for 0 / unknown).
const char* myp_type_name(int type_id) {
    // M8: return a COUNTED copy — the table strings are static data without
    // an ARC header; reflection results flow into string slots.
    if (type_id > 0 && __myp_type_name_table[type_id])
        return myp_strdup(__myp_type_name_table[type_id]);
    return myp_strdup("");
}

// Runtime type name of a class instance ("" if null / non-class).
const char* myp_obj_type_name(void* obj) {
    return myp_type_name(myp_obj_type_id(obj));
}

// Forward declaration — region arena is defined below (after myp_free_all).
void myp_region_free_all(void);

void myp_free_all(void) {
    // Restore terminal if we changed it to raw mode
    myp_restore_term();
    // @test 输出捕获缓冲（阶段 1）：进程退出时释放，保持 LSan 干净
    if (myp_capture_buf) {
        free(myp_capture_buf);
        myp_capture_buf = NULL;
        myp_capture_cap = 0;
        myp_capture_len = 0;
    }
    // slice<class> backing retains its elements. Release those arrays so the
    // process-exit raw free (myp_free_alloc_list_global via atexit) sees no
    // still-live element refs dangling past their backing.
    myp_release_class_slices_from_depth(0);
    // M6: the alloc list is PROCESS-GLOBAL (see above) and freed only at
    // process exit — never here. A worker thread may exit while its objects
    // are still referenced by other threads, so per-thread list free would
    // double-free / UAF. TLS resources (region/bump arenas) are still freed.
    myp_region_free_all();
    // Also free the bump arena chunks (non-ARC allocations: strings/slices/arrays)
    myp_arena_free_all();
}

// Slice subscript bounds check: reports the error and aborts.
void myp_bounds_error(int64_t idx, int64_t len) {
    fprintf(stderr, "MYP runtime error: slice index %lld out of bounds (length %lld)\n",
            (long long)idx, (long long)len);
    abort();
}

// ======================
// Region arena (two-tier memory: process-level + region-level)
// ======================
// @region functions allocate temporaries via myp_region_alloc into a
// thread-local bump arena. myp_arena_release(mark) frees newer chunks and
// rewinds the marked chunk — i.e. releases the region's temporaries —
// while process-level allocations (myp_alloc) survive until myp_free_all.
static pthread_key_t myp_region_key;
static pthread_once_t myp_region_key_once = PTHREAD_ONCE_INIT;
static __thread myp_arena_chunk_t* myp_region_cur = NULL;
// Region nesting depth (thread-local). >0 means we are inside an @region
// function's dynamic call scope, so myp_region_alloc uses the region arena
// (dynamic extent — even temporaries allocated by plain callees are reclaimed).

static void myp_make_region_key(void) {
    pthread_key_create(&myp_region_key, myp_free_arena_chunks);
}

static void* myp_region_bump_alloc(size_t size) {
    pthread_once(&myp_region_key_once, myp_make_region_key);
    if (size > SIZE_MAX - (MYP_ARENA_ALIGN - 1)) myp_oom(size);
    size = (size + MYP_ARENA_ALIGN - 1) & ~(size_t)(MYP_ARENA_ALIGN - 1);
    if (!myp_region_cur || size > myp_region_cur->cap - myp_region_cur->used) {
        size_t chunk_cap = size > MYP_ARENA_CHUNK_SIZE ? size : MYP_ARENA_CHUNK_SIZE;
        size_t chunk_total;
        if (myp_add_overflow(sizeof(myp_arena_chunk_t), chunk_cap, &chunk_total))
            myp_oom(chunk_cap);
        myp_arena_chunk_t* chunk = (myp_arena_chunk_t*)myp_xmalloc(chunk_total);
        chunk->next = myp_region_cur;
        chunk->used = 0;
        chunk->cap = chunk_cap;
        myp_region_cur = chunk;
        pthread_setspecific(myp_region_key, myp_region_cur);
    }
    void* ptr = (char*)myp_region_cur + sizeof(myp_arena_chunk_t) + myp_region_cur->used;
    myp_region_cur->used += size;
    return ptr;
}

void* myp_region_alloc(size_t size) {
    if (myp_region_depth > 0) {
        return myp_region_bump_alloc(size);
    }
    // Not inside an @region — same lifetime as process-level data (freed at
    // exit), served from the chunked bump arena (no per-allocation malloc).
    return myp_arena_bump_alloc(size);
}

// Enters a region scope and returns its exact bump pointer as the watermark.
// The pointer encodes both the current chunk and its used offset, so nested
// regions need no separately allocated marker object.
void* myp_arena_mark(void) {
    if (!myp_region_cur && !myp_region_bump_alloc(0)) return NULL;
    myp_region_depth++;
    return (char*)myp_region_cur + sizeof(myp_arena_chunk_t) + myp_region_cur->used;
}

// Leaves a region scope: frees chunks newer than the marked chunk and rewinds
// that chunk to its marked offset.
void myp_arena_release(void* mark) {
    pthread_once(&myp_region_key_once, myp_make_region_key);
    // Backings are separate counted allocations, so cascade their element
    // releases before rolling arena storage back to this mark.
    myp_release_class_slices_from_depth(myp_region_depth);
    uintptr_t target = (uintptr_t)mark;
    myp_arena_chunk_t* chunk =
        (myp_arena_chunk_t*)pthread_getspecific(myp_region_key);
    while (chunk) {
        uintptr_t begin = (uintptr_t)((char*)chunk + sizeof(myp_arena_chunk_t));
        uintptr_t end = begin + chunk->cap;
        if (target >= begin && target <= end) {
            myp_arena_chunk_t* head =
                (myp_arena_chunk_t*)pthread_getspecific(myp_region_key);
            while (head != chunk) {
                myp_arena_chunk_t* next = head->next;
                free(head);
                head = next;
            }
            chunk->used = (size_t)(target - begin);
            myp_region_cur = chunk;
            pthread_setspecific(myp_region_key, chunk);
            break;
        }
        chunk = chunk->next;
    }
    if (myp_region_depth > 0) myp_region_depth--;
}

void myp_region_free_all(void) {
    pthread_once(&myp_region_key_once, myp_make_region_key);
    myp_arena_chunk_t* head =
        (myp_arena_chunk_t*)pthread_getspecific(myp_region_key);
    if (head) {
        myp_free_arena_chunks(head);
        pthread_setspecific(myp_region_key, NULL);
    }
    myp_region_cur = NULL;
    myp_region_depth = 0;
}

// ---- M9: region arena byte accounting (thread-local) ----
int64_t myp_diag_region_reserved(void) {
    int64_t n = 0;
    for (myp_arena_chunk_t* c = myp_region_cur; c; c = c->next) n += (int64_t)c->cap;
    return n;
}
int64_t myp_diag_region_used(void) {
    int64_t n = 0;
    for (myp_arena_chunk_t* c = myp_region_cur; c; c = c->next) n += (int64_t)c->used;
    return n;
}

// ======================
// Timeline
// ======================

int64_t myp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

int64_t myp_now_realtime_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

void myp_sleep_ms(int64_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

// ======================
// Date / Time Formatting
// ======================

static time_t myp_ms_to_time_t(int64_t ms) {
    return (time_t)(ms / 1000);
}

char* myp_date_format(const char* fmt) {
    if (!fmt) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (!tm_info) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
    char buf[128];
    strftime(buf, sizeof(buf), fmt, tm_info);
    return myp_strdup(buf);
}

char* myp_date_format_ms(int64_t ms, const char* fmt) {
    if (!fmt) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
    time_t t = myp_ms_to_time_t(ms);
    struct tm* tm_info = localtime(&t);
    if (!tm_info) { char* r = (char*)myp_alloc(1); r[0] = '\0'; return r; }
    char buf[128];
    strftime(buf, sizeof(buf), fmt, tm_info);
    return myp_strdup(buf);
}

int32_t myp_date_field(int64_t ms, int32_t field) {
    time_t t = myp_ms_to_time_t(ms);
    struct tm* tm_info = localtime(&t);
    if (!tm_info) return 0;
    switch (field) {
        case 0: return tm_info->tm_year + 1900;  // year
        case 1: return tm_info->tm_mon + 1;      // month (1-12)
        case 2: return tm_info->tm_mday;          // day
        case 3: return tm_info->tm_hour;          // hour
        case 4: return tm_info->tm_min;           // minute
        case 5: return tm_info->tm_sec;           // second
        case 6: return tm_info->tm_wday;          // weekday (0=Sun)
        case 7: return tm_info->tm_yday;          // day of year
        default: return 0;
    }
}

// ======================
// String concatenation
// ======================

#include <string.h>

char* myp_strcat(const char* a, const char* b) {
    if (!a && !b) return NULL;
    if (!a) return myp_strdup(b);
    if (!b) return myp_strdup(a);
    size_t la = strlen(a), lb = strlen(b);
    char* result = (char*)myp_alloc(la + lb + 1);
    if (result) {
        memcpy(result, a, la);
        memcpy(result + la, b, lb);
        result[la + lb] = '\0';
    }
    return result;
}

// In-place string append (`s = s + x` fast path, M4 post-M8). CONSUMES `s`:
// if `s` is a unique counted string (rc==1, not an immortal literal and not
// shared) its {node,header,bytes} block is realloc'd in place and extended —
// O(1) amortized, no full copy per append. Otherwise falls back to
// myp_strcat(s,x) and releases `s`. Either way the returned string is fresh
// (rc=1) and owned by the caller, and `s` must not be released again.
// `x` may alias `s` (self-append) — handled by falling back to strcat.
char* myp_str_append(char* s, const char* x) {
    if (!s) return myp_strdup(x);
    if (!x) return s;
    if (x == s) {   // reading x while reallocating s would be a use-after-free
        char* r = myp_strcat(s, x);
        myp_release(s);
        return r;
    }
    char* base = (char*)s - MYP_OBJ_HEADER_SIZE;
    myp_obj_header_t* h = (myp_obj_header_t*)((char*)s - MYP_OBJ_HEADER_SIZE);
    size_t la = strlen(s), lb = strlen(x);
    // M6: atomic relaxed load of rc (unique counted string fast path).
    if (h->type_id == MYP_STR_TYPE_ID &&
        atomic_load_explicit(&h->rc, memory_order_relaxed) == 1) {
        // Unique counted string → in-place: unlink the intrusive alloc-list
        // node (realloc may move the block), realloc, relink, extend.
        char* base = (char*)s - MYP_OBJ_HEADER_SIZE;
        myp_alloc_node_t* node = (myp_alloc_node_t*)(base - sizeof(myp_alloc_node_t));
        size_t new_len, new_total;
        if (!myp_add_overflow(la, lb, &new_len) &&
            !myp_add_overflow(new_len, 1, &new_len) &&
            !myp_add_overflow(sizeof(myp_alloc_node_t) + MYP_OBJ_HEADER_SIZE,
                              new_len, &new_total)) {
            myp_alloc_list_remove(node);
            void* nb = realloc(node, new_total);
            if (nb) {
                myp_alloc_node_t* nn = (myp_alloc_node_t*)nb;
                myp_alloc_list_push(nn);
                char* nd = (char*)nn + sizeof(myp_alloc_node_t) + MYP_OBJ_HEADER_SIZE;
                memcpy(nd + la, x, lb);
                nd[la + lb] = '\0';
                return nd;
            }
            myp_alloc_list_push(node);   // realloc failed → restore, fall back
        }
    }
    char* r = myp_strcat(s, x);
    myp_release(s);
    return r;
}

char* myp_strdup(const char* s) {
    if (!s) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    size_t len = strlen(s);
    char* r = (char*)myp_alloc(len + 1);
    if (r) { memcpy(r, s, len + 1); }
    return r;
}

// Convert a value to string (for string concatenation with non-strings)
// 快速整数→字符串：避免 snprintf（glibc 通用格式化 + 内部 malloc + locale）。
// 2 位查表法（每步 %100 反向写两数字）：int64 至多 10 次除法；直接精确分配结果串，
// 无中间缓冲/strlen/二次拷贝。perf：arc 压力 31% 的 myp_to_string_i64 → 个位数%。
static char* myp_itoa_common(uint64_t u, int neg) {
    char tmp[24];
    int pos = 24;
    int n = 0;
    do {
        uint64_t d = u % 100;
        u /= 100;
        tmp[--pos] = (char)('0' + (d % 10));
        tmp[--pos] = (char)('0' + (d / 10));
        n += 2;
    } while (u);
    if (tmp[pos] == '0' && n > 1) { pos++; n--; }   // 去掉最高对的前导 0
    if (neg) { tmp[--pos] = '-'; n++; }
    char* r = (char*)myp_alloc((size_t)n + 1);
    if (!r) return NULL;
    memcpy(r, tmp + pos, (size_t)n);
    r[n] = '\0';
    return r;
}
char* myp_to_string_i32(int32_t val) {
    int64_t v64 = (int64_t)val;
    int neg = v64 < 0;
    uint64_t u = neg ? (uint64_t)(-(v64 + 1)) + 1 : (uint64_t)v64;  // 负 i32 取负（勿符号扩展成巨正数）
    return myp_itoa_common(u, neg);
}
char* myp_to_string_i64(int64_t val) {
    int neg = val < 0;
    uint64_t u = neg ? (uint64_t)(-(val + 1)) + 1 : (uint64_t)val;  // INT64_MIN 安全
    return myp_itoa_common(u, neg);
}
char* myp_to_string_double(double val) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", val);
    return myp_strcat(buf, "");
}
char* myp_to_string_bool(int32_t val) {
    return myp_strcat(val ? "true" : "false", "");
}
// P1（docs/type_system_design §6.1）：无符号十进制 + f32 格式化（修 D3 无符号拼成
// 有符号、D2 f32 拼接编译崩溃）。u32/u64 直接 myp_itoa_common(neg=0)；float 用 %g。
char* myp_to_string_u32(uint32_t val) {
    return myp_itoa_common((uint64_t)val, 0);
}
char* myp_to_string_u64(uint64_t val) {
    return myp_itoa_common(val, 0);
}
char* myp_to_string_float(float val) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", (double)val);
    return myp_strcat(buf, "");
}

// ======================
// printf-style formatting (stdlib/fmt.myp)
// ======================

// Format v's 32-bit bit pattern as an UNSIGNED integer in the given base
// (2..16). upper != 0 selects uppercase hex digits. Returns an owned string.
// (Fmt.x/o/b display int as unsigned, so 0xFFFFFFFF -> "ffffffff".)
char* myp_fmt_u64_base(int32_t v, int32_t base, int32_t upper) {
    if (base < 2 || base > 16) base = 10;
    uint32_t u = (uint32_t)v;
    char tmp[80];
    char buf[80];
    int i = 0, j = 0;
    if (u == 0) { buf[0] = '0'; buf[1] = '\0'; return myp_strcat(buf, ""); }
    while (u > 0) {
        int d = (int)(u % (uint32_t)base);
        tmp[i++] = (d < 10) ? (char)('0' + d) : (char)((upper ? 'A' : 'a') + d - 10);
        u /= (uint32_t)base;
    }
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return myp_strcat(buf, "");
}

char* myp_fmt_double_f(double v, int32_t prec) {
    char buf[160];
    if (prec < 0) prec = 0;
    if (prec > 60) prec = 60;
    snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return myp_strcat(buf, "");
}
char* myp_fmt_double_e(double v, int32_t prec) {
    char buf[160];
    if (prec < 0) prec = 0;
    if (prec > 60) prec = 60;
    snprintf(buf, sizeof(buf), "%.*e", prec, v);
    return myp_strcat(buf, "");
}
// IEEE 754 位型十六进制（LLVM 文本 IR 浮点常量格式 0x + 16 hex，与
// LLVM ConstantFP 打印一致）。float 先精确拓宽为 double，再取 64 位位型。
char* myp_f64_bits_hex(double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)bits);
    return myp_strcat(buf, "");
}
char* myp_f32_bits_hex(float v) {
    return myp_f64_bits_hex((double)v);
}
char* myp_fmt_double_g(double v, int32_t prec) {
    char buf[160];
    if (prec < 0) prec = 0;
    if (prec > 60) prec = 60;
    snprintf(buf, sizeof(buf), "%.*g", prec, v);
    return myp_strcat(buf, "");
}

// ======================
// Hashing (stdlib/crypto.myp)
// ======================

static char myp_hex_digit(int v) {
    return (v < 10) ? (char)('0' + v) : (char)('a' + v - 10);
}
static char* myp_bytes_to_hex(const uint8_t* p, size_t n) {
    char* out = (char*)myp_alloc(n * 2 + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = myp_hex_digit(p[i] >> 4);
        out[i * 2 + 1] = myp_hex_digit(p[i] & 0x0F);
    }
    out[n * 2] = '\0';
    return out;
}

static uint32_t myp_crc32_table[256];
static int myp_crc32_table_ready = 0;
static void myp_crc32_build_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        myp_crc32_table[i] = c;
    }
    myp_crc32_table_ready = 1;
}

// CRC-32 (IEEE 802.3). Returns the raw 32-bit value as int32_t; the high bit
// may be set, so display via Fmt.x / Crc32.crc32Hex.
int32_t myp_crc32(const char* msg) {
    if (!myp_crc32_table_ready) myp_crc32_build_table();
    uint32_t crc = 0xFFFFFFFFu;
    if (msg) {
        size_t len = strlen(msg);
        for (size_t i = 0; i < len; i++)
            crc = myp_crc32_table[(crc ^ (uint8_t)msg[i]) & 0xFFu] ^ (crc >> 8);
    }
    return (int32_t)(crc ^ 0xFFFFFFFFu);
}

// MD5 (RFC 1321). Returns lowercase hex digest.
char* myp_hash_md5(const char* msg) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const int S[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };
    size_t len = msg ? strlen(msg) : 0;
    size_t newlen = ((len + 8) / 64 + 1) * 64;
    // 中间缓冲用裸 malloc/free（不经 myp_alloc 跟踪链表，避免退出时双重释放）
    uint8_t* buf = (uint8_t*)malloc(newlen);
    if (!buf) return myp_strcat("", "");
    memset(buf, 0, newlen);
    if (msg) memcpy(buf, msg, len);
    buf[len] = 0x80;
    uint64_t bitlen = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[newlen - 8 + i] = (uint8_t)(bitlen >> (8 * i));

    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    for (size_t off = 0; off < newlen; off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++)
            M[i] = (uint32_t)buf[off + i*4] | ((uint32_t)buf[off + i*4+1] << 8) |
                   ((uint32_t)buf[off + i*4+2] << 16) | ((uint32_t)buf[off + i*4+3] << 24);
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16)      { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;          g = (3*i + 5) % 16; }
            else             { F = C ^ (B | ~D);       g = (7*i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + ((F << S[i]) | (F >> (32 - S[i])));
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    uint8_t out[16];
    for (int i = 0; i < 4; i++) {
        out[i]     = (a0 >> (8*i)) & 0xFF;
        out[4+i]   = (b0 >> (8*i)) & 0xFF;
        out[8+i]   = (c0 >> (8*i)) & 0xFF;
        out[12+i]  = (d0 >> (8*i)) & 0xFF;
    }
    char* hex = myp_bytes_to_hex(out, 16);
    free(buf);
    return hex;
}

// SHA-1 (FIPS 180-1). Returns lowercase hex digest.
char* myp_hash_sha1(const char* msg) {
    size_t len = msg ? strlen(msg) : 0;
    size_t newlen = ((len + 8) / 64 + 1) * 64;
    // 中间缓冲用裸 malloc/free（不经 myp_alloc 跟踪链表，避免退出时双重释放）
    uint8_t* buf = (uint8_t*)malloc(newlen);
    if (!buf) return myp_strcat("", "");
    memset(buf, 0, newlen);
    if (msg) memcpy(buf, msg, len);
    buf[len] = 0x80;
    uint64_t bitlen = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[newlen - 1 - i] = (uint8_t)(bitlen >> (8 * i));

    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    for (size_t off = 0; off < newlen; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)buf[off + i*4] << 24) | ((uint32_t)buf[off + i*4+1] << 16) |
                   ((uint32_t)buf[off + i*4+2] << 8) | (uint32_t)buf[off + i*4+3];
        for (int i = 16; i < 80; i++) {
            uint32_t x = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (x << 1) | (x >> 31);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;            k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;            k = 0xCA62C1D6; }
            uint32_t tmp = (((a << 5) | (a >> 27)) + f + e + k + w[i]);
            e = d; d = c; c = ((b << 30) | (b >> 2)); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    uint8_t out[20];
    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int j = 0; j < 5; j++)
        for (int i = 0; i < 4; i++)
            out[j*4+i] = (hs[j] >> (24 - 8*i)) & 0xFF;
    char* hex = myp_bytes_to_hex(out, 20);
    free(buf);
    return hex;
}

// SHA-256 (FIPS 180-2). Returns lowercase hex digest.
static const uint32_t myp_sha256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
char* myp_hash_sha256(const char* msg) {
    size_t len = msg ? strlen(msg) : 0;
    size_t newlen = ((len + 8) / 64 + 1) * 64;
    // 中间缓冲用裸 malloc/free（不经 myp_alloc 跟踪链表，避免退出时双重释放）
    uint8_t* buf = (uint8_t*)malloc(newlen);
    if (!buf) return myp_strcat("", "");
    memset(buf, 0, newlen);
    if (msg) memcpy(buf, msg, len);
    buf[len] = 0x80;
    uint64_t bitlen = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[newlen - 1 - i] = (uint8_t)(bitlen >> (8 * i));

    uint32_t h0=0x6a09e667,h1=0xbb67ae85,h2=0x3c6ef372,h3=0xa54ff53a,h4=0x510e527f,h5=0x9b05688c,h6=0x1f83d9ab,h7=0x5be0cd19;
    for (size_t off = 0; off < newlen; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)buf[off + i*4] << 24) | ((uint32_t)buf[off + i*4+1] << 16) |
                   ((uint32_t)buf[off + i*4+2] << 8) | (uint32_t)buf[off + i*4+3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ((w[i-15] >> 7) | (w[i-15] << 25)) ^ ((w[i-15] >> 18) | (w[i-15] << 14)) ^ (w[i-15] >> 3);
            uint32_t s1 = ((w[i-2] >> 17) | (w[i-2] << 15)) ^ ((w[i-2] >> 19) | (w[i-2] << 13)) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4,f=h5,g=h6,h=h7;
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^ ((e >> 25) | (e << 7));
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t temp1 = h + S1 + ch + myp_sha256_K[i] + w[i];
            uint32_t S0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^ ((a >> 22) | (a << 10));
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        h0+=a;h1+=b;h2+=c;h3+=d;h4+=e;h5+=f;h6+=g;h7+=h;
    }
    uint8_t out[32];
    uint32_t hs[8] = {h0,h1,h2,h3,h4,h5,h6,h7};
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 4; i++)
            out[j*4+i] = (hs[j] >> (24 - 8*i)) & 0xFF;
    char* hex = myp_bytes_to_hex(out, 32);
    free(buf);
    return hex;
}

// ======================
// Timer System
// ======================

#define MYP_MAX_TIMERS 64

typedef struct {
    int event_id;
    void* instance;
    int64_t fire_time;   // absolute time (ms) when timer should fire
    int64_t param;       // parameter to pass to the event
    int64_t interval;    // repeat interval (ms); 0 = one-shot
    int active;
} myp_timer_entry_t;

static myp_timer_entry_t myp_timers[MYP_MAX_TIMERS];
static int myp_timer_count = 0;
static pthread_mutex_t myp_timer_mutex = PTHREAD_MUTEX_INITIALIZER;
static void myp_timer_wake_target(void* instance);

// Create a timer that fires event_id on instance after delay_ms.
// If interval_ms > 0, the timer repeats every interval_ms.
// param is passed as event data when the timer fires.
// Returns 0 on success, -1 on failure (no timer slots available).
int32_t myp_timer_create(int event_id, void* instance, int64_t delay_ms,
                         int64_t param, int64_t interval_ms) {
    pthread_mutex_lock(&myp_timer_mutex);
    if (myp_timer_count >= MYP_MAX_TIMERS) {
        pthread_mutex_unlock(&myp_timer_mutex);
        return -1;
    }
    int64_t now = myp_now_ms();
    myp_timers[myp_timer_count].event_id = event_id;
    myp_timers[myp_timer_count].instance = instance;
    myp_timers[myp_timer_count].fire_time = now + delay_ms;
    myp_timers[myp_timer_count].param = param;
    myp_timers[myp_timer_count].interval = interval_ms;
    myp_timers[myp_timer_count].active = 1;
    myp_timer_count++;
    pthread_mutex_unlock(&myp_timer_mutex);
    myp_timer_wake_target(instance);
    return 0;
}

static int64_t myp_timer_next_delay_ms(void) {
    int64_t next = -1;
    int64_t now = myp_now_ms();
    pthread_mutex_lock(&myp_timer_mutex);
    for (int i = 0; i < myp_timer_count; i++) {
        if (!myp_timers[i].active) continue;
        int64_t delay = myp_timers[i].fire_time - now;
        if (delay < 0) delay = 0;
        if (next < 0 || delay < next) next = delay;
    }
    pthread_mutex_unlock(&myp_timer_mutex);
    return next;
}

// Look up an event id by its name at runtime (for timers created with a
// dynamic event name). names/ids/count is the compiler-generated table of all
// events in the translation unit. Returns -1 if not found.
int32_t myp_event_id_by_name(const char* name, char* const* names,
                             const int32_t* ids, int32_t count) {
    if (!name || !names || !ids) return -1;
    for (int32_t i = 0; i < count; i++) {
        if (names[i] && strcmp(name, names[i]) == 0)
            return ids[i];
    }
    return -1;
}

// Cancel all timers for a given instance (e.g., when instance is destroyed)
void myp_timer_cancel_all(void* instance) {
    pthread_mutex_lock(&myp_timer_mutex);
    for (int i = 0; i < myp_timer_count; i++) {
        if (myp_timers[i].active && myp_timers[i].instance == instance)
            myp_timers[i].active = 0;
    }
    pthread_mutex_unlock(&myp_timer_mutex);
}

// Check and fire expired timers. Called from the event loop.
// Returns the number of timers fired.
int myp_timer_check(void) {
    int fired = 0;
    int64_t now = myp_now_ms();
    pthread_mutex_lock(&myp_timer_mutex);
    for (int i = 0; i < myp_timer_count; i++) {
        if (!myp_timers[i].active) continue;
        if (now >= myp_timers[i].fire_time) {
            // Fire the event on the timer's instance
            int eid = myp_timers[i].event_id;
            void* inst = myp_timers[i].instance;
            int64_t pval = myp_timers[i].param;

            if (myp_timers[i].interval > 0) {
                // Repeating timer: reschedule
                myp_timers[i].fire_time = now + myp_timers[i].interval;
            } else {
                // One-shot: deactivate
                myp_timers[i].active = 0;
            }
            pthread_mutex_unlock(&myp_timer_mutex);

            // Fire the event (this may re-enter timer code, so unlock first)
            // Since param data (pval) is stack-local, process it now
            myp_event_fire(eid, inst, &pval);
            // Process this event immediately so pval is still valid
            myp_event_process_one();

            pthread_mutex_lock(&myp_timer_mutex);
            fired++;
        }
    }
    pthread_mutex_unlock(&myp_timer_mutex);
    return fired;
}

// ======================
// Event System — Per-Thread Queues
// ======================

#define MYP_EVENT_QUEUE_INIT_SIZE 1024

typedef struct {
    int event_id;
    void* sender;
    void* data;
    int has_data;
} myp_event_t;

// Event queue (dynamic ring buffer — grows on demand, no fixed 1024 cap)
typedef struct {
    myp_event_t* events;
    int capacity;
    volatile int head;
    volatile int tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} myp_event_queue_t;

// Event handler registration (global)
typedef struct {
    int event_id;
    int registered;
    void* instance;
    myp_handler_fn handler;
} myp_handler_entry_t;

static myp_handler_entry_t myp_handlers[MYP_MAX_HANDLERS];
static int myp_handler_count = 0;

// Thread-local queue key (pthread_getspecific / pthread_setspecific)
static pthread_key_t myp_queue_key;
static pthread_once_t myp_queue_key_once = PTHREAD_ONCE_INIT;

static void myp_free_queue(void* ptr) {
    if (ptr) {
        myp_event_queue_t* q = (myp_event_queue_t*)ptr;
        pthread_cond_destroy(&q->cond);
        pthread_mutex_destroy(&q->mutex);
        free(q->events);
        free(q);
    }
}

static void myp_make_queue_key(void) {
    pthread_key_create(&myp_queue_key, myp_free_queue);
}

// Create a new event queue
static myp_event_queue_t* myp_queue_create(void) {
    myp_event_queue_t* q = (myp_event_queue_t*)calloc(1, sizeof(myp_event_queue_t));
    q->capacity = MYP_EVENT_QUEUE_INIT_SIZE;
    q->events = (myp_event_t*)calloc((size_t)q->capacity, sizeof(myp_event_t));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&q->cond, &attr);
    pthread_condattr_destroy(&attr);
    return q;
}

// Set the current thread's queue
static void myp_queue_set_current(myp_event_queue_t* q) {
    pthread_once(&myp_queue_key_once, myp_make_queue_key);
    pthread_setspecific(myp_queue_key, q);
}

// Get the current thread's queue
static myp_event_queue_t* myp_queue_current(void) {
    pthread_once(&myp_queue_key_once, myp_make_queue_key);
    myp_event_queue_t* q = (myp_event_queue_t*)pthread_getspecific(myp_queue_key);
    if (!q) {
        // Main thread: create queue lazily
        q = myp_queue_create();
        myp_queue_set_current(q);
    }
    return q;
}

// Push an event to a specific queue (thread-safe); grows the buffer when full.
static void myp_queue_push(myp_event_queue_t* q, int event_id, void* sender, void* data) {
    pthread_mutex_lock(&q->mutex);
    int next = (q->head + 1) % q->capacity;
    if (next == q->tail) {
        // Full — grow (double) and linearize the circular contents.
        if (q->capacity > INT_MAX / 2) { pthread_mutex_unlock(&q->mutex); return; }
        int new_cap = q->capacity * 2;
        size_t nbytes;
        if (myp_mul_overflow((size_t)new_cap, sizeof(myp_event_t), &nbytes)) {
            pthread_mutex_unlock(&q->mutex); return;
        }
        myp_event_t* ne = (myp_event_t*)realloc(q->events, nbytes);
        if (!ne) { pthread_mutex_unlock(&q->mutex); return; }  // OOM: drop event, keep original
        int n = 0;
        for (int i = q->tail; i != q->head; i = (i + 1) % q->capacity)
            ne[n++] = q->events[i];
        q->events = ne;
        q->tail = 0;
        q->head = n;
        q->capacity = new_cap;
        next = (q->head + 1) % q->capacity;
    }
    q->events[q->head].event_id = event_id;
    q->events[q->head].sender = sender;
    q->events[q->head].data = data;
    q->events[q->head].has_data = (data != NULL);
    q->head = next;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

static void myp_queue_wake(myp_event_queue_t* q) {
    pthread_mutex_lock(&q->mutex);
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

static void myp_queue_wait(myp_event_queue_t* q, const _Atomic int* running,
                           int64_t timeout_ms) {
    pthread_mutex_lock(&q->mutex);
    if (q->tail == q->head && atomic_load(running)) {
        if (timeout_ms < 0) {
            pthread_cond_wait(&q->cond, &q->mutex);
        } else if (timeout_ms > 0) {
            struct timespec deadline;
            clock_gettime(CLOCK_MONOTONIC, &deadline);
            deadline.tv_sec += timeout_ms / 1000;
            deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
            if (deadline.tv_nsec >= 1000000000) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&q->cond, &q->mutex, &deadline);
        }
    }
    pthread_mutex_unlock(&q->mutex);
}

// Pop an event from a specific queue (thread-safe, returns 0 if empty)
static int myp_queue_pop(myp_event_queue_t* q, myp_event_t* ev) {
    pthread_mutex_lock(&q->mutex);
    if (q->tail == q->head) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }
    *ev = q->events[q->tail];
    q->tail = (q->tail + 1) % q->capacity;
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

// ---- Thread Support (@thread) ----
struct myp_thread {
    pthread_t thread;
    _Atomic int running;   // atomic: written by stop, read by event loop (data-race free)
    myp_event_queue_t* queue;
    void (*startup_fn)(void*, void*);
    void* startup_arg;
};

// ---- Instance→Thread mapping (for async cross-thread event delivery) ----
static pthread_mutex_t myp_inst_map_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MYP_INST_MAP_SIZE 128

typedef struct {
    void* instance;
    myp_thread_t* thread;
} myp_inst_map_entry_t;

static myp_inst_map_entry_t myp_inst_map[MYP_INST_MAP_SIZE];
static int myp_inst_map_count = 0;

void myp_thread_associate_instance(void* instance, myp_thread_t* thr) {
    pthread_mutex_lock(&myp_inst_map_mutex);
    if (myp_inst_map_count < MYP_INST_MAP_SIZE) {
        myp_inst_map[myp_inst_map_count].instance = instance;
        myp_inst_map[myp_inst_map_count].thread = thr;
        myp_inst_map_count++;
    }
    pthread_mutex_unlock(&myp_inst_map_mutex);
}

static myp_thread_t* myp_thread_for_instance(void* instance) {
    for (int i = 0; i < myp_inst_map_count; i++) {
        if (myp_inst_map[i].instance == instance)
            return myp_inst_map[i].thread;
    }
    return NULL;
}

static void myp_timer_wake_target(void* instance) {
    myp_thread_t* thr = myp_thread_for_instance(instance);
    if (thr) myp_queue_wake(thr->queue);
}

// ---- Public API ----

void myp_event_register(int event_id, void* instance, myp_handler_fn handler) {
    if (myp_handler_count >= MYP_MAX_HANDLERS) return;
    myp_handlers[myp_handler_count].event_id = event_id;
    myp_handlers[myp_handler_count].instance = instance;
    myp_handlers[myp_handler_count].handler = handler;
    myp_handlers[myp_handler_count].registered = 1;
    myp_handler_count++;
}

// Scope-based handler management for mapping() @scope
#define MYP_SCOPE_STACK_DEPTH 64
static int myp_scope_stack[MYP_SCOPE_STACK_DEPTH];
static int myp_scope_depth = 0;

void myp_event_push_scope(void) {
    if (myp_scope_depth >= MYP_SCOPE_STACK_DEPTH) return;
    myp_scope_stack[myp_scope_depth++] = myp_handler_count;
}

void myp_event_pop_scope(void) {
    if (myp_scope_depth <= 0) return;
    int saved = myp_scope_stack[--myp_scope_depth];
    myp_handler_count = saved;
}

void myp_event_fire(int event_id, void* sender, void* event_data) {
#ifdef TRACE_ENABLED
    fprintf(stderr, "[TRACE] event_fire(id=%d, sender=%p)\n", event_id, sender);
#endif
    // Check if sender belongs to another thread → async cross-thread delivery
    myp_thread_t* target_thr = sender ? myp_thread_for_instance(sender) : NULL;
    if (target_thr) {
        // Post to the target thread's queue (async — no process_all here)
        myp_queue_push(target_thr->queue, event_id, sender, event_data);
        return;
    }
    // Same-thread (or no thread): post to current thread's queue
    myp_event_queue_t* q = myp_queue_current();
    myp_queue_push(q, event_id, sender, event_data);
}

// Coroutine event-wait notification (defined in the coroutine section).
static void __myp_coro_event_notify(int event_id);

static void myp_event_dispatch(myp_event_t* ev) {
#ifdef TRACE_ENABLED
    fprintf(stderr, "[TRACE] dispatch(event_id=%d, sender=%p)\n", ev->event_id, ev->sender);
#endif
    for (int i = 0; i < myp_handler_count; i++) {
        if (myp_handlers[i].registered &&
            myp_handlers[i].event_id == ev->event_id) {
            myp_handlers[i].handler(myp_handlers[i].instance, ev->data);
        }
    }
    // Re-ready coroutines blocked on this event (C4)
    __myp_coro_event_notify(ev->event_id);
}

void myp_event_process_all(void) {
    myp_event_queue_t* q = myp_queue_current();
    myp_event_t ev;
    while (myp_queue_pop(q, &ev)) {
        myp_event_dispatch(&ev);
    }
    // Also check expired timers
    myp_timer_check();
}

int myp_event_process_one(void) {
    myp_event_queue_t* q = myp_queue_current();
    myp_event_t ev;
    if (!myp_queue_pop(q, &ev)) return 0;
    myp_event_dispatch(&ev);
    return 1;
}

// ---- Thread Support (@thread) ----

myp_thread_t* myp_thread_create(void (*startup)(void*, void*), void* startup_arg) {
    myp_thread_t* thr = (myp_thread_t*)calloc(1, sizeof(myp_thread_t));
    thr->running = 1;
    thr->queue = myp_queue_create();
    thr->startup_fn = startup;
    thr->startup_arg = startup_arg;
    return thr;
}

// Post an event to a specific thread's queue (thread-safe)
void myp_thread_post_event(myp_thread_t* thr, int event_id, void* sender, void* data) {
    myp_queue_push(thr->queue, event_id, sender, data);
}

// Coroutine TLS cleanup for the current thread (defined in the coroutine section).
void __myp_coro_thread_cleanup(void);

static void* myp_thread_entry(void* arg) {
    myp_thread_t* thr = (myp_thread_t*)arg;
    // Set this thread's queue
    myp_queue_set_current(thr->queue);

    // Run startup function (e.g. @startup action) on this thread
    if (thr->startup_fn) {
        thr->startup_fn(thr->startup_arg, NULL);
    }

    // Event loop — process this thread's own queue + timers
    while (atomic_load(&thr->running)) {
        myp_event_process_all();
        myp_queue_wait(thr->queue, &thr->running, myp_timer_next_delay_ms());
    }
    // Release this thread's coroutine state (TLS) so slots/stacks aren't leaked.
    __myp_coro_thread_cleanup();
    // Run ARC-aware cleanup while the TLS allocation lists are still valid;
    // pthread key destructors otherwise only know how to raw-free blocks.
    myp_free_all();
    // Clear TLS so destructor doesn't double-free
    pthread_setspecific(myp_queue_key, NULL);
    return NULL;
}

void myp_thread_run_loop(myp_thread_t* thr) {
    pthread_create(&thr->thread, NULL, myp_thread_entry, thr);
}

void myp_thread_stop(myp_thread_t* thr) {
    atomic_store(&thr->running, 0);
    myp_queue_wake(thr->queue);
}

void myp_thread_destroy(myp_thread_t* thr) {
    myp_thread_stop(thr);
    pthread_join(thr->thread, NULL);
    myp_free_queue(thr->queue);
    // ARC (§五-1): release the instance the thread ran its @startup on. The
    // instance is a header-bearing myp_alloc_object (both @thread and
    // @threadpool paths), so this decrements its rc; if nothing else holds it,
    // it is freed here instead of leaking to process exit. myp_free_object
    // marks the tracking-list node freed, so myp_free_all() won't double-free.
    if (thr->startup_arg)
        myp_release(thr->startup_arg);
    free(thr);
}

// ======================
// Work-Stealing Thread Pool (v6)
// ======================

typedef struct {
    int start, end, step;
} myp_work_chunk_t;

typedef struct myp_work_deque {
    myp_work_chunk_t* chunks;
    int cap;
    volatile int bottom;  // own thread pops here (LIFO)
    volatile int top;     // stealers take from here (FIFO)
    pthread_mutex_t mutex;
} myp_work_deque_t;

typedef struct myp_pool {
    int n_threads;
    pthread_t* threads;
    myp_work_deque_t* deques;
    volatile int running;
    volatile int done_count;
    int total_chunks;
    pthread_mutex_t barrier_mutex;
    pthread_cond_t barrier_cond;
    pthread_mutex_t work_mutex;
    pthread_cond_t work_cond;
    volatile int work_available;
    // work_fn is now a chunk loop: void(*)(start, end, step, arg). The worker
    // calls it once per chunk; the body loops over its own range. Removes one
    // call/ret per iteration and lets the compiler (LLVM in the generated
    // body) hoist loop-invariant work (e.g. worker id) and vectorize the body.
    void (*work_fn)(int, int, int, void*);
    void* work_arg;
} myp_pool_t;

myp_pool_t* myp_global_pool = NULL;
static pthread_mutex_t myp_pool_start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t myp_pool_start_cond = PTHREAD_COND_INITIALIZER;
static volatile int myp_pool_start_ok = 0;
static int myp_pool_requested_threads = 0;  // 0 = 自动（硬件并发数）

static void myp_work_deque_init(myp_work_deque_t* dq, int cap) {
    dq->chunks = (myp_work_chunk_t*)calloc(cap, sizeof(myp_work_chunk_t));
    dq->cap = cap;
    dq->bottom = 0;
    dq->top = 0;
    pthread_mutex_init(&dq->mutex, NULL);
}

static void myp_work_deque_destroy(myp_work_deque_t* dq) {
    free(dq->chunks);
    pthread_mutex_destroy(&dq->mutex);
}

static void myp_work_deque_push(myp_work_deque_t* dq, int start, int end, int step) {
    pthread_mutex_lock(&dq->mutex);
    int b = dq->bottom;
    if (b >= dq->cap) {
        if (dq->cap > INT_MAX / 2) { pthread_mutex_unlock(&dq->mutex); return; }
        dq->cap *= 2;
        size_t nbytes;
        if (myp_mul_overflow((size_t)dq->cap, sizeof(myp_work_chunk_t), &nbytes)) {
            pthread_mutex_unlock(&dq->mutex); return;
        }
        myp_work_chunk_t* nch = (myp_work_chunk_t*)realloc(dq->chunks, nbytes);
        if (!nch) { pthread_mutex_unlock(&dq->mutex); return; }  // keep original ptr
        dq->chunks = nch;
    }
    dq->chunks[b].start = start;
    dq->chunks[b].end = end;
    dq->chunks[b].step = step;
    dq->bottom = b + 1;
    pthread_mutex_unlock(&dq->mutex);
}

static int myp_work_deque_pop(myp_work_deque_t* dq, myp_work_chunk_t* chunk) {
    pthread_mutex_lock(&dq->mutex);
    int b = dq->bottom;
    if (b <= dq->top) { pthread_mutex_unlock(&dq->mutex); return 0; }
    b = b - 1;
    *chunk = dq->chunks[b];
    dq->bottom = b;
    pthread_mutex_unlock(&dq->mutex);
    return 1;
}

static int myp_work_deque_steal(myp_work_deque_t* dq, myp_work_chunk_t* chunk) {
    pthread_mutex_lock(&dq->mutex);
    int t = dq->top;
    if (t >= dq->bottom) { pthread_mutex_unlock(&dq->mutex); return 0; }
    *chunk = dq->chunks[t];
    dq->top = t + 1;
    pthread_mutex_unlock(&dq->mutex);
    return 1;
}

// 当前 @parallel for worker 的索引（TLS）：myp_pool_worker 启动时写入，
// myp_pool_worker_id() 返回它，供并行 body 检测多线程是否真正启动。
static __thread int myp_pool_worker_tid = -1;

static void* myp_pool_worker(void* arg) {
    int tid = (int)(uintptr_t)arg;
    myp_pool_worker_tid = tid;  // 记录当前 worker 索引（供 myp_pool_worker_id 查询）

    // Wait until pool is initialized
    pthread_mutex_lock(&myp_pool_start_mutex);
    while (!myp_pool_start_ok)
        pthread_cond_wait(&myp_pool_start_cond, &myp_pool_start_mutex);
    pthread_mutex_unlock(&myp_pool_start_mutex);

    myp_pool_t* pool = myp_global_pool;
    while (pool->running) {
        myp_work_chunk_t chunk;
        int got_work = 0;
        if (myp_work_deque_pop(&pool->deques[tid], &chunk)) got_work = 1;
        if (!got_work) {
            for (int i = 1; i < pool->n_threads; i++) {
                int victim = (tid + i) % pool->n_threads;
                if (myp_work_deque_steal(&pool->deques[victim], &chunk)) {
                    got_work = 1; break;
                }
            }
        }
        if (got_work) {
            // Body loops over its chunk: one call per chunk instead of one per
            // iteration.
            pool->work_fn(chunk.start, chunk.end, chunk.step, pool->work_arg);
            pthread_mutex_lock(&pool->barrier_mutex);
            pool->done_count++;
            if (pool->done_count >= pool->total_chunks)
                pthread_cond_signal(&pool->barrier_cond);
            pthread_mutex_unlock(&pool->barrier_mutex);
        } else {
            pthread_mutex_lock(&pool->work_mutex);
            if (!pool->work_available && pool->running)
                pthread_cond_wait(&pool->work_cond, &pool->work_mutex);
            pthread_mutex_unlock(&pool->work_mutex);
        }
    }
    return NULL;
}

myp_pool_t* myp_pool_create(int n_threads) {
    if (n_threads <= 0) {
        n_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (n_threads < 1) n_threads = 1;
    }
    myp_pool_t* pool = (myp_pool_t*)calloc(1, sizeof(myp_pool_t));
    pool->n_threads = n_threads;
    pool->running = 1;
    pool->deques = (myp_work_deque_t*)calloc(n_threads, sizeof(myp_work_deque_t));
    for (int i = 0; i < n_threads; i++)
        myp_work_deque_init(&pool->deques[i], 64);
    pthread_mutex_init(&pool->barrier_mutex, NULL);
    pthread_cond_init(&pool->barrier_cond, NULL);
    pthread_mutex_init(&pool->work_mutex, NULL);
    pthread_cond_init(&pool->work_cond, NULL);
    pool->work_available = 0;
    pool->threads = (pthread_t*)calloc(n_threads, sizeof(pthread_t));

    myp_global_pool = pool;
    // Publish pool under the start mutex so the worker's condvar handshake
    // establishes happens-before for myp_global_pool and start_ok (data-race free).
    pthread_mutex_lock(&myp_pool_start_mutex);
    myp_pool_start_ok = 1;
    pthread_cond_broadcast(&myp_pool_start_cond);
    pthread_mutex_unlock(&myp_pool_start_mutex);

    for (int i = 0; i < n_threads; i++)
        pthread_create(&pool->threads[i], NULL, myp_pool_worker, (void*)(uintptr_t)i);

    return pool;
}

static pthread_once_t myp_pool_once = PTHREAD_ONCE_INIT;

static void myp_pool_init_global(void) {
    // myp_pool_create() itself publishes myp_global_pool (before the start-mutex
    // handshake). Do NOT re-assign here — an unsynchronized second write of the
    // same pointer would race with workers reading myp_global_pool.
    myp_pool_create(myp_pool_requested_threads);
}

myp_pool_t* myp_pool_ensure_global(void) {
    // pthread_once: init exactly once with proper happens-before, so the
    // unsynchronized `myp_global_pool` read/write is no longer a data race.
    pthread_once(&myp_pool_once, myp_pool_init_global);
    return myp_global_pool;
}

void myp_pool_parallel_for(myp_pool_t* pool, int start, int end, int step,
                            void (*fn)(int, int, int, void*), void* arg) {
    if (!pool || !fn) return;
    int n = (end - start + step - 1) / step;
    if (n <= 0) return;
    pool->work_fn = fn;
    pool->work_arg = arg;
    int max_chunks = pool->n_threads * 4;
    if (max_chunks > n) max_chunks = n;
    if (max_chunks < 1) max_chunks = 1;
    int chunk_size = n / max_chunks;
    if (chunk_size < 1) chunk_size = 1;
    int remainder = n % max_chunks;

    // Reset the barrier counters BEFORE publishing any chunks. A worker left
    // spinning from the previous call can grab a newly-pushed chunk and bump
    // done_count before the old code reset it here — that increment was then
    // wiped, so that chunk was never counted and done_count could never reach
    // total_chunks → the main thread hung on the barrier forever (intermittent,
    // ~1/3 on loaded machines). Resetting first guarantees every increment for
    // this batch lands on the new counter and is preserved.
    pthread_mutex_lock(&pool->barrier_mutex);
    pool->done_count = 0;
    pool->total_chunks = 0;
    pthread_mutex_unlock(&pool->barrier_mutex);

    for (int t = 0; t < pool->n_threads; t++) {
        pthread_mutex_lock(&pool->deques[t].mutex);
        pool->deques[t].bottom = pool->deques[t].top = 0;
        pthread_mutex_unlock(&pool->deques[t].mutex);
    }

    int actual_chunks = 0;

    int iter = start;
    for (int c = 0; c < max_chunks && iter < end; c++) {
        int this_size = chunk_size + (c < remainder ? 1 : 0);
        int chunk_end = iter + this_size;
        if (chunk_end > end) chunk_end = end;
        int td = c % pool->n_threads;
        myp_work_deque_push(&pool->deques[td], iter, chunk_end, step);
        iter = chunk_end;
        actual_chunks++;
    }

    // Publish the true total (workers read/write done_count and total_chunks
    // under barrier_mutex). A worker incrementing while total_chunks is still 0
    // merely signals with no waiter — harmless; the wait loop's re-check under
    // the mutex covers it.
    pthread_mutex_lock(&pool->barrier_mutex);
    pool->total_chunks = actual_chunks;
    pthread_mutex_unlock(&pool->barrier_mutex);

    // Signal workers
    pthread_mutex_lock(&pool->work_mutex);
    pool->work_available = 1;
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->work_mutex);

    // Wait for completion
    pthread_mutex_lock(&pool->barrier_mutex);
    while (pool->done_count < pool->total_chunks)
        pthread_cond_wait(&pool->barrier_cond, &pool->barrier_mutex);
    pthread_mutex_unlock(&pool->barrier_mutex);

    pthread_mutex_lock(&pool->work_mutex);
    pool->work_available = 0;
    pthread_mutex_unlock(&pool->work_mutex);
}

void myp_pool_destroy(myp_pool_t* pool) {
    if (!pool) return;
    pool->running = 0;
    pthread_mutex_lock(&pool->work_mutex);
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->work_mutex);
    for (int i = 0; i < pool->n_threads; i++) {
        pthread_join(pool->threads[i], NULL);
        myp_work_deque_destroy(&pool->deques[i]);
    }
    free(pool->deques);
    free(pool->threads);
    pthread_mutex_destroy(&pool->barrier_mutex);
    pthread_cond_destroy(&pool->barrier_cond);
    pthread_mutex_destroy(&pool->work_mutex);
    pthread_cond_destroy(&pool->work_cond);
    free(pool);
    myp_global_pool = NULL;
}

int32_t myp_pool_worker_id(void) {
    return myp_pool_worker_tid;
}

int32_t myp_pool_thread_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int32_t)n : 1;
}

// 设置全局池大小（0 = 自动 = 硬件并发数）。仅在首次创建前生效；
// 池创建后调用为 no-op（池已按原大小启动）。应在程序早期调用。
void myp_pool_set_threads(int n) {
    if (n < 0) n = 0;
    if (n > 0) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpus > 0 && n > cpus) {
            // Warn once: oversubscription (more workers than cores) usually
            // hurts throughput via context-switch/cache contention. The user's
            // explicit count is still honored — no silent cap.
            static int warned = 0;
            if (!warned) {
                warned = 1;
                fprintf(stderr,
                    "MYP warning: Parallel.setThreads(%d) exceeds %ld CPU(s); "
                    "oversubscription may degrade performance\n", n, cpus);
            }
        }
    }
    myp_pool_requested_threads = n;
}

// 全局池实际 worker 线程数（0 = 尚未初始化）。
// n_threads 在创建时确定后不再改变，读取无竞争。
int32_t myp_pool_worker_count(void) {
    return myp_global_pool ? (int32_t)myp_global_pool->n_threads : 0;
}

// 全局池是否已初始化（1=是 0=否）。
int32_t myp_pool_is_active(void) {
    return myp_global_pool != NULL ? 1 : 0;
}

// ======================
// Math functions
// ======================

double myp_math_sqrt(double v)    { return sqrt(v); }
double myp_math_abs(double v)     { return fabs(v); }
double myp_math_floor(double v)   { return floor(v); }
double myp_math_ceil(double v)    { return ceil(v); }
double myp_math_sin(double v)     { return sin(v); }
double myp_math_cos(double v)     { return cos(v); }
double myp_math_tan(double v)     { return tan(v); }
double myp_math_asin(double v)    { return asin(v); }
double myp_math_acos(double v)    { return acos(v); }
double myp_math_atan(double v)    { return atan(v); }
double myp_math_atan2(double y, double x) { return atan2(y, x); }
double myp_math_sinh(double v)    { return sinh(v); }
double myp_math_cosh(double v)    { return cosh(v); }
double myp_math_tanh(double v)    { return tanh(v); }
double myp_math_exp(double v)     { return exp(v); }
double myp_math_log(double v)     { return log(v); }
double myp_math_pow(double b, double e) { return pow(b, e); }
double myp_math_trunc(double v)   { return trunc(v); }
int32_t myp_math_abs_int(int32_t v) { return v < 0 ? -v : v; }

// ======================
// Error handling (setjmp/longjmp)
// ======================

// setjmp/longjmp are called directly from LLVM IR.
// myp_setjmp is a wrapper that forwards to system setjmp,
// but setjmp must be called in the same function that branches
// on its return value, so we generate @llvm.setjmp or direct
// calls in codegen instead.
// The error message and the exception handler stack are thread-local so
// nested try blocks and concurrent threads each see their own state.
static __thread char myp_error_msg[256];
static int myp_error_active = 0;
static __thread void* myp_handler_bufs[64];
static __thread int myp_handler_depth = 0;
// Typed exception carrier (per thread): type_id 0 = string message;
// > 0 = a class instance of that class's type ID.
static __thread void* myp_current_exception = NULL;
static __thread int myp_current_exception_type = 0;

// ASan: notify before a non-returning jump (longjmp) so ASan unwinds properly
// and doesn't report false-positive frame-mismatch / __asan_handle_no_return
// warnings. Weak reference — a no-op when not built with ASan.
extern void __asan_handle_no_return(void) __attribute__((weak));

// Codegen's longjmp target (replaces the raw system longjmp): tell ASan about
// the non-returning jump first, then longjmp to the handler's jmp_buf.
// This is important for coroutines: an uncaught exception inside a coroutine
// longjmps through ucontext stacks, which ASan otherwise mis-reports.
void __myp_longjmp(void* jb, int val) {
    if (__asan_handle_no_return) __asan_handle_no_return();
    longjmp(*(jmp_buf*)jb, val);
}

// Called from generated code via intrinsic: saves error context
void myp_error_setup(void) {
    myp_error_active = 1;
}

// Called from generated code via intrinsic: records error and triggers longjmp
void myp_throw(const char* msg) {
    strncpy(myp_error_msg, msg, 255);
    myp_error_msg[255] = '\0';
    myp_current_exception = NULL;
    myp_current_exception_type = 0;  // 0 = string message exception
}

const char* myp_get_error(void) {
    return myp_error_msg;
}

int myp_error_is_active(void) {
    return myp_error_active;
}

// ---- Typed exception carrier helpers ----
void myp_throw_object(void* obj, int type_id) {
    myp_current_exception = obj;
    myp_current_exception_type = type_id;
    // Generic message so catch-all (string) handlers still get something.
    strncpy(myp_error_msg, "exception", 255);
    myp_error_msg[255] = '\0';
}

int myp_exception_get_type(void) {
    return myp_current_exception_type;
}

void* myp_exception_get_object(void) {
    return myp_current_exception;
}

// ---- Exception handler stack (per thread) ----
// Each try block pushes its own jmp_buf before setjmp and pops it when the
// try/finally structure ends. __myp_throw longjmps to the *top* handler, so
// nested tries and cross-function throws resolve to the correct (innermost
// still-active) handler instead of a stale global buffer.
void myp_exception_push(void* jmpbuf) {
    if (myp_handler_depth < 64)
        myp_handler_bufs[myp_handler_depth++] = jmpbuf;
}

void myp_exception_pop(void) {
    if (myp_handler_depth > 0) myp_handler_depth--;
}

// IR-level optimization barrier for the exception machinery (§五-3):
// codegen passes the ADDRESS of each try-inner ARC slot to this no-op so LLVM
// treats the slot as escaped memory. Without it, the -O pipeline sees the
// dispatch/propagate load of a try-inner slot as undef (its only defs live in
// try_block, which does not dominate the longjmp path) and folds myp_release to
// a garbage/undef arg → -O2 segfaults (result) / leaks (arc_throw). The C body
// is intentionally empty; the escape effect happens at the IR level.
void myp_try_escape(void* p) {
    (void)p;
}

// Release the object held by a local ARC slot, reading the slot's PHYSICAL
// memory here in C. On the exception longjmp path the slot's physical contents
// are the authoritative value (same-frame stack memory is preserved by
// longjmp), but LLVM cannot see that — its own load of the slot in the
// dispatch/propagate block is folded to undef at -O2 (the try_block defs don't
// dominate the longjmp path). Reading it in C (opaque to LLVM) yields the true
// object: the try-block null-init (pre-construction) or the constructed object
// (post-construction, the longjmp skipped its scope-exit release).
// kind: 0 = class ptr slot, 1/2 = interface / function-value fat pointer
// (object is element 0).
void myp_release_slot(void* slot_addr, int kind) {
    if (!slot_addr) return;
    void* obj = (kind == 0) ? *(void**)slot_addr : ((void**)slot_addr)[0];
    myp_release(obj);
}

void* myp_exception_get_jmpbuf(void) {
    if (myp_handler_depth > 0) return myp_handler_bufs[myp_handler_depth - 1];
    // Unhandled exception: print a clear message, then abort (design: abort +
    // explicit message, not a silent segfault).
    if (myp_current_exception_type > 0) {
        fprintf(stderr, "uncaught exception (object, type %d)\n",
                myp_current_exception_type);
    } else if (myp_error_msg[0]) {
        fprintf(stderr, "uncaught exception: %s\n", myp_error_msg);
    } else {
        fprintf(stderr, "uncaught exception\n");
    }
    abort();
    return NULL;
}

void myp_error_clear(void) {
    myp_error_active = 0;
}

// ======================
// Test framework
// ======================
#include <stdio.h>

static int myp_test_pass_count = 0;
static int myp_test_fail_count = 0;

// Optional per-assertion user message (set by Test.assertXxx(a,b,msg) via
// __myp_test_set_msg). Consumed by the NEXT assertion failure: if set, the
// failure prints the message instead of the default value detail. Cleared
// regardless of pass/fail so one message is consumed by exactly one assertion.
static const char* myp_test_cur_msg = NULL;
void myp_test_set_msg(const char* m) { myp_test_cur_msg = m; }

// Record one failed assertion. Prints the pending user message if set,
// otherwise the default detail (e.g. "1 != 2").
static void myp_test_fail(const char* fmt, ...) {
    if (myp_test_cur_msg) {
        fprintf(stderr, "  ASSERTION FAILED: %s\n", myp_test_cur_msg);
        myp_test_cur_msg = NULL;
    } else {
        fprintf(stderr, "  ASSERTION FAILED: ");
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fprintf(stderr, "\n");
    }
    myp_test_fail_count++;
}

void myp_assert(int cond) {
    if (!cond) {
        myp_test_fail("");
    } else {
        myp_test_pass_count++;
    }
}

// Assertion with a user-provided message: on failure print the message instead
// of the generic "ASSERTION FAILED" line (Test.assert(cond, msg)).
void myp_assert_msg(int cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "  ASSERTION FAILED: %s\n", msg ? msg : "");
        myp_test_fail_count++;
    } else {
        myp_test_pass_count++;
    }
}

void myp_assert_eq(int a, int b) {
    if (a != b) {
        myp_test_fail("%d != %d", a, b);
    } else {
        myp_test_pass_count++;
    }
}

void myp_assert_str_eq(const char* a, const char* b) {
    int eq = (a == b) || (a && b && strcmp(a, b) == 0);
    if (!eq) {
        myp_test_fail("\"%s\" != \"%s\"", a ? a : "null", b ? b : "null");
    } else {
        myp_test_pass_count++;
    }
}

void myp_assert_neq(int a, int b) {
    if (a == b) {
        myp_test_fail("%d == %d (expected not equal)", a, b);
    } else {
        myp_test_pass_count++;
    }
}

void myp_assert_long_eq(int64_t a, int64_t b) {
    if (a != b) {
        myp_test_fail("%ld != %ld", (long)a, (long)b);
    } else {
        myp_test_pass_count++;
    }
}

void myp_assert_str_neq(const char* a, const char* b) {
    int eq = (a == b) || (a && b && strcmp(a, b) == 0);
    if (eq) {
        myp_test_fail("\"%s\" == \"%s\" (expected not equal)", a ? a : "null", b ? b : "null");
    } else {
        myp_test_pass_count++;
    }
}

void myp_test_report(const char* name, int passed) {
    if (passed) {
        printf("  PASS: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
    }
}

// Hard failure with a message (Test.fail / user-facing): record a failure and
// print the reason. Previously Test.fail abused an empty-string assert, which
// double-counted and produced a confusing "ASSERTION FAILED" message.
void myp_test_fail_msg(const char* msg) {
    fprintf(stderr, "  FAILED: %s\n", msg ? msg : "(no message)");
    myp_test_fail_count++;
}

// Print assertion totals and return the process exit code for the test runner:
// 0 = all assertions passed, 1 = at least one failed. Without this the runner
// always exited 0, so a failing test suite was indistinguishable from a green
// one (CI / scripts could not detect failures).
int myp_test_summary(int test_count) {
    printf("  tests: %d, assertions: %d passed, %d failed\n",
           test_count, myp_test_pass_count, myp_test_fail_count);
    return myp_test_fail_count > 0 ? 1 : 0;
}

// -- Extended assertion helpers (Test.assertLongNeq / assertFloatNeq) --

void myp_assert_long_neq(int64_t a, int64_t b) {
    if (a == b) {
        fprintf(stderr, "  ASSERTION FAILED: %ld == %ld (expected not equal)\n",
                (long)a, (long)b);
        myp_test_fail_count++;
    } else {
        myp_test_pass_count++;
    }
}

void myp_assert_float_neq(double a, double b, double eps) {
    double diff = a - b;
    if (diff < 0.0) diff = -diff;
    if (diff <= eps) {
        fprintf(stderr, "  ASSERTION FAILED: %g == %g (expected not equal)\n", a, b);
        myp_test_fail_count++;
    } else {
        myp_test_pass_count++;
    }
}

// -- Null pointer assertions (Test.assertNull / assertNotNull) --

void myp_assert_null(const void* p) {
    if (p != NULL) {
        fprintf(stderr, "  ASSERTION FAILED: expected null\n");
        myp_test_fail_count++;
    } else {
        myp_test_pass_count++;
    }
}

void myp_assert_not_null(const void* p) {
    if (p == NULL) {
        fprintf(stderr, "  ASSERTION FAILED: expected non-null\n");
        myp_test_fail_count++;
    } else {
        myp_test_pass_count++;
    }
}

// ======================
// Barrier 同步 (pthread_barrier 封装)
// ======================

#define MYP_MAX_BARRIERS 64
static pthread_barrier_t myp_barriers[MYP_MAX_BARRIERS];
static int myp_barrier_used[MYP_MAX_BARRIERS] = {0};
static pthread_mutex_t myp_barrier_mutex = PTHREAD_MUTEX_INITIALIZER;

int32_t myp_barrier_create(int32_t count) {
    pthread_mutex_lock(&myp_barrier_mutex);
    for (int32_t i = 0; i < MYP_MAX_BARRIERS; i++) {
        if (!myp_barrier_used[i]) {
            if (pthread_barrier_init(&myp_barriers[i], NULL, (unsigned int)count) != 0) {
                pthread_mutex_unlock(&myp_barrier_mutex);
                return -1;
            }
            myp_barrier_used[i] = 1;
            pthread_mutex_unlock(&myp_barrier_mutex);
            return i; // return handle
        }
    }
    pthread_mutex_unlock(&myp_barrier_mutex);
    return -1; // no free slot
}

int32_t myp_barrier_wait(int32_t handle) {
    if (handle < 0 || handle >= MYP_MAX_BARRIERS || !myp_barrier_used[handle])
        return -1;
    int32_t ret = pthread_barrier_wait(&myp_barriers[handle]);
    return (ret == 0 || ret == PTHREAD_BARRIER_SERIAL_THREAD) ? 0 : -1;
}

void myp_barrier_destroy(int32_t handle) {
    if (handle >= 0 && handle < MYP_MAX_BARRIERS && myp_barrier_used[handle]) {
        pthread_barrier_destroy(&myp_barriers[handle]);
        myp_barrier_used[handle] = 0;
    }
}

// ======================
// 同步原语（§五-2，pthread 封装）：Mutex / RWLock / CondVar / Semaphore / Once
// 全部采用 handle 模式（同 Barrier）：固定数组 + 分配表 + 互斥保护槽位分配。
// ======================

#define MYP_MAX_MUTEX 64
static pthread_mutex_t myp_mutexes[MYP_MAX_MUTEX];
static int myp_mutex_used[MYP_MAX_MUTEX] = {0};
static pthread_mutex_t myp_mutex_slot = PTHREAD_MUTEX_INITIALIZER;

int32_t myp_mutex_create(void) {
    pthread_mutex_lock(&myp_mutex_slot);
    for (int32_t i = 0; i < MYP_MAX_MUTEX; i++) {
        if (!myp_mutex_used[i]) {
            if (pthread_mutex_init(&myp_mutexes[i], NULL) != 0) {
                pthread_mutex_unlock(&myp_mutex_slot);
                return -1;
            }
            myp_mutex_used[i] = 1;
            pthread_mutex_unlock(&myp_mutex_slot);
            return i;
        }
    }
    pthread_mutex_unlock(&myp_mutex_slot);
    return -1;
}

int32_t myp_mutex_create_recursive(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_lock(&myp_mutex_slot);
    for (int32_t i = 0; i < MYP_MAX_MUTEX; i++) {
        if (!myp_mutex_used[i]) {
            if (pthread_mutex_init(&myp_mutexes[i], &attr) != 0) {
                pthread_mutexattr_destroy(&attr);
                pthread_mutex_unlock(&myp_mutex_slot);
                return -1;
            }
            pthread_mutexattr_destroy(&attr);
            myp_mutex_used[i] = 1;
            pthread_mutex_unlock(&myp_mutex_slot);
            return i;
        }
    }
    pthread_mutexattr_destroy(&attr);
    pthread_mutex_unlock(&myp_mutex_slot);
    return -1;
}

static int myp_mutex_valid(int32_t h) {
    return h >= 0 && h < MYP_MAX_MUTEX && myp_mutex_used[h];
}

void myp_mutex_lock(int32_t h) {
    if (myp_mutex_valid(h)) pthread_mutex_lock(&myp_mutexes[h]);
}
int32_t myp_mutex_trylock(int32_t h) {
    if (!myp_mutex_valid(h)) return -1;
    return pthread_mutex_trylock(&myp_mutexes[h]) == 0 ? 1 : 0;
}
void myp_mutex_unlock(int32_t h) {
    if (myp_mutex_valid(h)) pthread_mutex_unlock(&myp_mutexes[h]);
}
void myp_mutex_destroy(int32_t h) {
    if (myp_mutex_valid(h)) {
        pthread_mutex_destroy(&myp_mutexes[h]);
        myp_mutex_used[h] = 0;
    }
}

// ---- RWLock ----
#define MYP_MAX_RWLOCK 64
static pthread_rwlock_t myp_rwlocks[MYP_MAX_RWLOCK];
static int myp_rwlock_used[MYP_MAX_RWLOCK] = {0};
static pthread_mutex_t myp_rwlock_slot = PTHREAD_MUTEX_INITIALIZER;

int32_t myp_rwlock_create(void) {
    pthread_mutex_lock(&myp_rwlock_slot);
    for (int32_t i = 0; i < MYP_MAX_RWLOCK; i++) {
        if (!myp_rwlock_used[i]) {
            if (pthread_rwlock_init(&myp_rwlocks[i], NULL) != 0) {
                pthread_mutex_unlock(&myp_rwlock_slot);
                return -1;
            }
            myp_rwlock_used[i] = 1;
            pthread_mutex_unlock(&myp_rwlock_slot);
            return i;
        }
    }
    pthread_mutex_unlock(&myp_rwlock_slot);
    return -1;
}
static int myp_rwlock_valid(int32_t h) {
    return h >= 0 && h < MYP_MAX_RWLOCK && myp_rwlock_used[h];
}
void myp_rwlock_rdlock(int32_t h) {
    if (myp_rwlock_valid(h)) pthread_rwlock_rdlock(&myp_rwlocks[h]);
}
void myp_rwlock_wrlock(int32_t h) {
    if (myp_rwlock_valid(h)) pthread_rwlock_wrlock(&myp_rwlocks[h]);
}
int32_t myp_rwlock_tryrdlock(int32_t h) {
    if (!myp_rwlock_valid(h)) return -1;
    return pthread_rwlock_tryrdlock(&myp_rwlocks[h]) == 0 ? 1 : 0;
}
int32_t myp_rwlock_trywrlock(int32_t h) {
    if (!myp_rwlock_valid(h)) return -1;
    return pthread_rwlock_trywrlock(&myp_rwlocks[h]) == 0 ? 1 : 0;
}
void myp_rwlock_unlock(int32_t h) {
    if (myp_rwlock_valid(h)) pthread_rwlock_unlock(&myp_rwlocks[h]);
}
void myp_rwlock_destroy(int32_t h) {
    if (myp_rwlock_valid(h)) {
        pthread_rwlock_destroy(&myp_rwlocks[h]);
        myp_rwlock_used[h] = 0;
    }
}

// ---- CondVar（wait 需关联一个 Mutex handle）----
#define MYP_MAX_COND 64
static pthread_cond_t myp_conds[MYP_MAX_COND];
static int myp_cond_used[MYP_MAX_COND] = {0};
static pthread_mutex_t myp_cond_slot = PTHREAD_MUTEX_INITIALIZER;

int32_t myp_cond_create(void) {
    pthread_mutex_lock(&myp_cond_slot);
    for (int32_t i = 0; i < MYP_MAX_COND; i++) {
        if (!myp_cond_used[i]) {
            if (pthread_cond_init(&myp_conds[i], NULL) != 0) {
                pthread_mutex_unlock(&myp_cond_slot);
                return -1;
            }
            myp_cond_used[i] = 1;
            pthread_mutex_unlock(&myp_cond_slot);
            return i;
        }
    }
    pthread_mutex_unlock(&myp_cond_slot);
    return -1;
}
static int myp_cond_valid(int32_t h) {
    return h >= 0 && h < MYP_MAX_COND && myp_cond_used[h];
}
void myp_cond_wait(int32_t ch, int32_t mh) {
    if (myp_cond_valid(ch) && myp_mutex_valid(mh))
        pthread_cond_wait(&myp_conds[ch], &myp_mutexes[mh]);
}
void myp_cond_signal(int32_t ch) {
    if (myp_cond_valid(ch)) pthread_cond_signal(&myp_conds[ch]);
}
void myp_cond_broadcast(int32_t ch) {
    if (myp_cond_valid(ch)) pthread_cond_broadcast(&myp_conds[ch]);
}
void myp_cond_destroy(int32_t ch) {
    if (myp_cond_valid(ch)) {
        pthread_cond_destroy(&myp_conds[ch]);
        myp_cond_used[ch] = 0;
    }
}

// ---- Semaphore（POSIX 无名信号量）----
#define MYP_MAX_SEM 64
static sem_t myp_sems[MYP_MAX_SEM];
static int myp_sem_used[MYP_MAX_SEM] = {0};
static pthread_mutex_t myp_sem_slot = PTHREAD_MUTEX_INITIALIZER;

int32_t myp_sem_create(int32_t initial) {
    pthread_mutex_lock(&myp_sem_slot);
    for (int32_t i = 0; i < MYP_MAX_SEM; i++) {
        if (!myp_sem_used[i]) {
            if (sem_init(&myp_sems[i], 0, (unsigned int)initial) != 0) {
                pthread_mutex_unlock(&myp_sem_slot);
                return -1;
            }
            myp_sem_used[i] = 1;
            pthread_mutex_unlock(&myp_sem_slot);
            return i;
        }
    }
    pthread_mutex_unlock(&myp_sem_slot);
    return -1;
}
static int myp_sem_valid(int32_t h) {
    return h >= 0 && h < MYP_MAX_SEM && myp_sem_used[h];
}
void myp_sem_wait(int32_t h) {
    if (myp_sem_valid(h)) sem_wait(&myp_sems[h]);
}
int32_t myp_sem_trywait(int32_t h) {
    if (!myp_sem_valid(h)) return -1;
    return sem_trywait(&myp_sems[h]) == 0 ? 1 : 0;
}
void myp_sem_post(int32_t h) {
    if (myp_sem_valid(h)) sem_post(&myp_sems[h]);
}
void myp_sem_destroy(int32_t h) {
    if (myp_sem_valid(h)) {
        sem_destroy(&myp_sems[h]);
        myp_sem_used[h] = 0;
    }
}

// ---- Once（call-once：enter 返回 1 表示首个调用者应执行初始化，done 标记完成）----
#define MYP_MAX_ONCE 64
typedef struct {
    pthread_mutex_t mutex;
    int32_t done;
    int32_t used;
} myp_once_t;
static myp_once_t myp_onces[MYP_MAX_ONCE];
static pthread_mutex_t myp_once_slot = PTHREAD_MUTEX_INITIALIZER;

int32_t myp_once_create(void) {
    pthread_mutex_lock(&myp_once_slot);
    for (int32_t i = 0; i < MYP_MAX_ONCE; i++) {
        if (!myp_onces[i].used) {
            pthread_mutex_init(&myp_onces[i].mutex, NULL);
            myp_onces[i].done = 0;
            myp_onces[i].used = 1;
            pthread_mutex_unlock(&myp_once_slot);
            return i;
        }
    }
    pthread_mutex_unlock(&myp_once_slot);
    return -1;
}
static int myp_once_valid(int32_t h) {
    return h >= 0 && h < MYP_MAX_ONCE && myp_onces[h].used;
}
// Returns 1 if this is the first caller (and the lock is held — caller must run
// the init body then call myp_once_done); 0 if already done (lock released).
int32_t myp_once_enter(int32_t h) {
    if (!myp_once_valid(h)) return 0;
    pthread_mutex_lock(&myp_onces[h].mutex);
    if (myp_onces[h].done) {
        pthread_mutex_unlock(&myp_onces[h].mutex);
        return 0;
    }
    return 1;   // lock stays held; caller runs init, then myp_once_done
}
void myp_once_done(int32_t h) {
    if (myp_once_valid(h)) {
        myp_onces[h].done = 1;
        pthread_mutex_unlock(&myp_onces[h].mutex);
    }
}
void myp_once_destroy(int32_t h) {
    if (myp_once_valid(h)) {
        pthread_mutex_destroy(&myp_onces[h].mutex);
        myp_onces[h].used = 0;
    }
}

// ======================
// Future/Promise (条件变量封装)
// ======================

#define MYP_MAX_FUTURES 64
#define MYP_FUTURE_MAX_CORO_WAITERS 64
typedef struct {
    int32_t value;
    int32_t ready;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int32_t used;
    // Coroutine waiters (same-thread only): a coroutine that gets a not-ready
    // future parks here instead of pthread_cond_wait (which would block the
    // whole thread). Coroutine state is thread-local, so this only works when
    // the future is set on the same thread.
    int64_t coro_waiters[MYP_FUTURE_MAX_CORO_WAITERS];
    int32_t coro_wait_count;
} myp_future_t;

static myp_future_t myp_futures[MYP_MAX_FUTURES];
static pthread_mutex_t myp_future_mutex = PTHREAD_MUTEX_INITIALIZER;

// Coroutine-aware helpers (defined in the coroutine section below).
int myp_coro_am_i_coro(void);                    // 1 if currently inside a coroutine
void myp_coro_wait_future(int32_t handle);       // suspend current coroutine until future ready
void myp_coro_wake_future(int32_t handle);       // re-ready same-thread coroutines waiting on this future

int32_t myp_future_create(void) {
    pthread_mutex_lock(&myp_future_mutex);
    for (int32_t i = 0; i < MYP_MAX_FUTURES; i++) {
        if (!myp_futures[i].used) {
            myp_futures[i].used = 1;
            myp_futures[i].ready = 0;
            myp_futures[i].value = 0;
            pthread_mutex_init(&myp_futures[i].mutex, NULL);
            pthread_cond_init(&myp_futures[i].cond, NULL);
            pthread_mutex_unlock(&myp_future_mutex);
            return i;
        }
    }
    pthread_mutex_unlock(&myp_future_mutex);
    return -1;
}

void myp_future_set(int32_t handle, int32_t value) {
    if (handle < 0 || handle >= MYP_MAX_FUTURES || !myp_futures[handle].used) return;
    pthread_mutex_lock(&myp_futures[handle].mutex);
    myp_futures[handle].value = value;
    myp_futures[handle].ready = 1;
    pthread_cond_broadcast(&myp_futures[handle].cond);
    pthread_mutex_unlock(&myp_futures[handle].mutex);
    myp_coro_wake_future(handle);   // re-ready same-thread coroutine waiters
}

int32_t myp_future_get(int32_t handle) {
    if (handle < 0 || handle >= MYP_MAX_FUTURES || !myp_futures[handle].used) return 0;
    pthread_mutex_lock(&myp_futures[handle].mutex);
    // Coroutine + not ready: park the coroutine (no thread-blocking wait).
    // Must not hold the mutex across the coroutine suspend (other threads /
    // coroutines need it to set).
    if (!myp_futures[handle].ready && myp_coro_am_i_coro()) {
        pthread_mutex_unlock(&myp_futures[handle].mutex);
        myp_coro_wait_future(handle);
        pthread_mutex_lock(&myp_futures[handle].mutex);
    }
    while (!myp_futures[handle].ready) {
        pthread_cond_wait(&myp_futures[handle].cond, &myp_futures[handle].mutex);
    }
    int32_t val = myp_futures[handle].value;
    pthread_mutex_unlock(&myp_futures[handle].mutex);
    return val;
}

void myp_future_destroy(int32_t handle) {
    if (handle >= 0 && handle < MYP_MAX_FUTURES && myp_futures[handle].used) {
        pthread_mutex_destroy(&myp_futures[handle].mutex);
        pthread_cond_destroy(&myp_futures[handle].cond);
        myp_futures[handle].used = 0;
    }
}

// ======================
// Coroutine (用户态纤程)
// ======================
// 上下文切换：x86-64 用寄存器级汇编（coro_ctx.S，无 syscall，~20-40ns）；
// 其他平台回退 ucontext swapcontext（~200ns）。见下方 myp_ctx_*。

#if defined(__x86_64__)
// 快速路径：无需 ucontext 头。
#else
#include <ucontext.h>
#endif
#include <stdint.h>

#define MYP_CORO_STACK_SIZE (128 * 1024)      // 128KB per coroutine stack
#define MYP_CORO_INITIAL_CAPACITY 64

// ---- 上下文切换抽象 ----
// myp_ctx_t 保存单份"执行上下文"。x86-64 下是一个栈指针（指向保存块：
// [rip, r15, r14, r13, r12, rbx, rbp]，见 coro_ctx.S）；其他平台用 ucontext_t。
#if defined(__x86_64__)
typedef struct { void* rsp; } myp_ctx_t;
extern void myp_ctx_switch(myp_ctx_t* save, myp_ctx_t* load);
#else
typedef struct { ucontext_t u; } myp_ctx_t;
static void myp_ctx_switch(myp_ctx_t* save, myp_ctx_t* load) {
    swapcontext(&save->u, &load->u);
}
#endif

// ASan 对 ucontext 有内建 fiber 支持；自研切换绕过它，需显式通知 sanitizer 正在
// 切换栈（否则 asan 构建产生假阳性栈不匹配）。**只在 ASan 编译下启用**：gcc 编译
// runtime.c 带 -fsanitize=address 时定义 __SANITIZE_ADDRESS__。普通/TSan 构建里
// 这些符号恒为 NULL，保留运行时检查会让 resume/yield 每次切换付一次 GOT 加载 +
// 分支（perf：非 ASan cpp_long 的 __sanitizer_finish_switch_fiber NULL 检查占
// resume/yield 自样本 64~80%），故编译期整体剔除。
#if defined(__SANITIZE_ADDRESS__)
extern void __sanitizer_start_switch_fiber(void** fake_stack_save,
                                           const void* bottom, size_t size);
extern void __sanitizer_finish_switch_fiber(void* fake_stack_save,
                                            const void** bottom_old,
                                            size_t* size_old);
static __thread void* myp_asan_fake_stack = NULL;
static void myp_asan_start_switch(const void* bottom, size_t size) {
    __sanitizer_start_switch_fiber(&myp_asan_fake_stack, bottom, size);
}
static void myp_asan_finish_switch(void) {
    __sanitizer_finish_switch_fiber(myp_asan_fake_stack, NULL, NULL);
}
#else
static inline void myp_asan_start_switch(const void* bottom, size_t size) {
    (void)bottom; (void)size;
}
static inline void myp_asan_finish_switch(void) {}
#endif

// §五-1 收尾: one entry in a coroutine's frame ARC registry — the object a
// local ARC slot currently holds (slot_id = alloca address, used only as a
// lookup key and never dereferenced).
typedef struct {
    int64_t slot_id;
    int64_t obj;
} myp_frame_slot_t;

typedef struct {
    myp_ctx_t ctx;
    myp_ctx_t ret_ctx;   // caller context (who resumed/created this coroutine)
    char* stack;
    size_t stack_size;    // bytes allocated for this coroutine's stack
    int active;
    int ready;      // in the ready queue (C3 scheduler); 0 while blocked on an event (C4)
    int wait_timeout; // set when an event-wait with a deadline expires (C10)
    int64_t last_wait_event_id; // event id that woke us (C10 waitAny)
    int64_t last_wait_index; // §五-5 P4: spec index that woke a waitAnyOf (-1 = none)
    int cancel_requested; // cooperative-cancel flag (C10)
    // §五-1 收尾: coroutine-frame ARC registry — mirrors the object each local
    // ARC slot currently holds (set at store, cleared at every normal release).
    // On Coro.destroy or an uncaught exception the runtime releases each still-
    // registered object. Objects are heap pointers, so release is safe even
    // after the coroutine's stack was unwound/reused by the exception path.
    myp_frame_slot_t* frame_slots;
    int frame_slots_count;
    int frame_slots_cap;
    void (*fn)(void); // entry function for this coroutine
    int64_t result;   // return value slot (C2)
    int64_t exec_result; // EXEC：worker 交付的文件读结果（char*，本线程 tracked）
    // M1 句柄代际化 + 槽位复用：generation 在槽被复用（create）时递增；句柄编码
    // {generation<<32 | slot}，消费句柄时校验代际，旧句柄稳定失效、绝不操作新协程。
    uint32_t generation;
    int result_pending;  // 1 = 已完成且结果未读；result() 读取后清 0 → 槽可复用
    int on_free_list;    // 1 = 该槽已在空闲复用列表
    int discard_result;  // 1 = 协程被 destroy（无结果可读），完成后直接回收槽
} myp_coro_t;

// Dynamic slot array (no hard cap — grows on demand, limited only by memory).
// Each myp_coro_t (which holds the ucontext) is allocated individually and
// stays at a stable address; only the pointer array may move on grow (realloc
// must never relocate a live ucontext_t).
//
// All coroutine state is THREAD-LOCAL (__thread): a coroutine belongs to the
// thread that created it. Each thread has its own slot array, scheduler
// context and event-wait table, so coroutines can run independently inside
// multiple @thread threads (coroutine + thread 并用) without locking.
static __thread myp_coro_t** myp_coros = NULL;
static __thread int myp_coro_count = 0;      // number of slots in use (includes inactive/reused)
static __thread int myp_coro_capacity = 0;
static __thread int myp_coro_current = -1;
// M1: 空闲槽复用列表 + 句柄编解码。句柄 = {generation<<32 | slot}（用户面）；
// 内部机制（等待表/调度器快照/exec pump/myp_coro_current）仍用槽号。
static __thread int* myp_coro_free_slots = NULL;
static __thread int myp_coro_free_count = 0;
static __thread int myp_coro_free_cap = 0;

static inline int64_t myp_coro_make_handle(int slot) {
    if (slot < 0 || slot >= myp_coro_count || !myp_coros[slot]) return -1;
    return ((int64_t)myp_coros[slot]->generation << 32) | (int64_t)(uint32_t)slot;
}
static inline int myp_coro_handle_slot(int64_t h) { return (int)(uint32_t)h; }
static inline int myp_coro_handle_valid(int64_t h) {
    int slot = (int)(uint32_t)h;
    uint32_t gen = (uint32_t)((uint64_t)h >> 32);
    return slot >= 0 && slot < myp_coro_count && myp_coros[slot] &&
           myp_coros[slot]->generation == gen;
}
static void myp_coro_push_free(int slot) {
    if (slot < 0 || slot >= myp_coro_count || !myp_coros[slot]) return;
    if (myp_coros[slot]->on_free_list) return;   // 已在列表，防双入
    if (myp_coro_free_count >= myp_coro_free_cap) {
        if (myp_coro_free_cap > INT_MAX / 2) return;
        int nc = myp_coro_free_cap ? myp_coro_free_cap * 2 : 16;
        size_t nbytes;
        if (myp_mul_overflow((size_t)nc, sizeof(int), &nbytes)) return;
        int* np = (int*)realloc(myp_coro_free_slots, nbytes);
        if (!np) return;
        myp_coro_free_slots = np;
        myp_coro_free_cap = nc;
    }
    myp_coros[slot]->on_free_list = 1;
    myp_coro_free_slots[myp_coro_free_count++] = slot;
}
static int myp_coro_pop_free(void) {
    if (myp_coro_free_count <= 0) return -1;
    int s = myp_coro_free_slots[--myp_coro_free_count];
    if (s >= 0 && s < myp_coro_count && myp_coros[s])
        myp_coros[s]->on_free_list = 0;
    return s;
}
// 槽可复用条件：不活跃 且 无未读结果 且 未在列表。满足则入空闲列表。
static void myp_coro_try_recycle(int slot) {
    if (slot < 0 || slot >= myp_coro_count || !myp_coros[slot]) return;
    myp_coro_t* c = myp_coros[slot];
    if (!c->active && !c->result_pending && !c->on_free_list)
        myp_coro_push_free(slot);
}
// Value passing between coroutine and scheduler (thread-local; only one
// coroutine of a thread runs at a time):
//   myp_coro_yield_val  — coroutine → scheduler (value passed out by await)
//   myp_coro_resume_val — scheduler → coroutine (value passed in by resume)
static __thread int64_t myp_coro_yield_val = 0;
static __thread int64_t myp_coro_resume_val = 0;

// Each coroutine keeps its OWN return context (ret_ctx): whoever resumes/creates
// it saves their context there before switching in, and the coroutine switches
// back to it on yield or completion (explicitly in the trampoline). This makes
// NESTED resume correct — a coroutine that resumes another coroutine is that
// child's caller, and the child returns to it, while the parent in turn returns
// to its own caller. A single shared scheduler context cannot express this chain.

// Grow the pointer array (at least doubling). Returns 0 on success.
static int myp_coro_grow(void) {
    int new_cap = myp_coro_capacity ? myp_coro_capacity : MYP_CORO_INITIAL_CAPACITY;
    if (myp_coro_count >= new_cap) {
        if (new_cap > INT_MAX / 2) return -1;
        new_cap *= 2;
    }
    size_t nbytes;
    if (myp_mul_overflow((size_t)new_cap, sizeof(myp_coro_t*), &nbytes)) return -1;
    myp_coro_t** np = (myp_coro_t**)realloc(myp_coros, nbytes);
    if (!np) return -1;
    memset(np + myp_coro_capacity, 0,
           (size_t)(new_cap - myp_coro_capacity) * sizeof(myp_coro_t*));
    myp_coros = np;
    myp_coro_capacity = new_cap;
    return 0;
}

// ---- Stack pool (per-thread) ----
// Reuse freed coroutine stacks to avoid repeated malloc/free on spawn/destroy
// churn. Stacks come in different sizes (@coro(stack=N)); we keep the size with
// each slot and hand out the best match. The pool is bounded so it can never
// grow unboundedly in long-running programs.
#define MYP_CORO_STACK_POOL_MAX 128   // 栈池缓存个数上限
#define MYP_CORO_STACK_POOL_MAX_BYTES (16u * 1024 * 1024)  // M2: 栈池总字节上限/线程
#define MYP_CORO_STACK_BIG (1024u * 1024)                  // M2: ≥1MiB 大栈直接归还系统
typedef struct {
    char* ptr;
    size_t size;
} myp_coro_stack_slot_t;
static __thread myp_coro_stack_slot_t* myp_coro_stack_pool = NULL;
static __thread int myp_coro_stack_pool_count = 0;
static __thread int myp_coro_stack_pool_capacity = 0;
static __thread size_t myp_coro_stack_pool_bytes = 0;   // M2: 池内总字节数

// Add a stack to the pool (free it if the pool is full / over byte cap / too big).
static void myp_coro_stack_pool_add(char* ptr, size_t size) {
    if (!ptr) return;
    // M2: 大栈直接归还系统；个数或总字节上限达到也直接 free，避免多线程/突发
    // 协程把每线程缓存堆到 ~16MiB+，峰值 RSS 受控。
    if (size >= MYP_CORO_STACK_BIG ||
        myp_coro_stack_pool_count >= MYP_CORO_STACK_POOL_MAX ||
        (size > MYP_CORO_STACK_POOL_MAX_BYTES - myp_coro_stack_pool_bytes)) {
        free(ptr);
        return;
    }
    if (myp_coro_stack_pool_count >= myp_coro_stack_pool_capacity) {
        if (myp_coro_stack_pool_capacity > INT_MAX / 2) { free(ptr); return; }
        int nc = myp_coro_stack_pool_capacity ? myp_coro_stack_pool_capacity * 2 : 16;
        size_t nbytes;
        if (myp_mul_overflow((size_t)nc, sizeof(myp_coro_stack_slot_t), &nbytes)) { free(ptr); return; }
        myp_coro_stack_slot_t* np = (myp_coro_stack_slot_t*)realloc(myp_coro_stack_pool, nbytes);
        if (!np) { free(ptr); return; }
        myp_coro_stack_pool = np;
        myp_coro_stack_pool_capacity = nc;
    }
    myp_coro_stack_pool[myp_coro_stack_pool_count].ptr = ptr;
    myp_coro_stack_pool[myp_coro_stack_pool_count].size = size;
    myp_coro_stack_pool_count++;
    myp_coro_stack_pool_bytes += size;
}

// Take a stack of the requested size from the pool: exact size match preferred,
// otherwise the smallest slot large enough. Returns NULL if none fits.
static char* myp_coro_stack_pool_take(size_t want) {
    int best = -1;
    for (int i = 0; i < myp_coro_stack_pool_count; i++) {
        if (myp_coro_stack_pool[i].size == want) { best = i; break; }
        if (myp_coro_stack_pool[i].size >= want &&
            (best < 0 || myp_coro_stack_pool[i].size < myp_coro_stack_pool[best].size))
            best = i;
    }
    if (best < 0) return NULL;
    char* p = myp_coro_stack_pool[best].ptr;
    size_t sz = myp_coro_stack_pool[best].size;
    myp_coro_stack_pool[best] = myp_coro_stack_pool[--myp_coro_stack_pool_count];
    myp_coro_stack_pool_bytes -= sz;   // M2: 扣除已取出的字节数
    return p;
}

static void myp_coro_stack_pool_free_all(void) {
    for (int i = 0; i < myp_coro_stack_pool_count; i++)
        free(myp_coro_stack_pool[i].ptr);
    free(myp_coro_stack_pool);
    myp_coro_stack_pool = NULL;
    myp_coro_stack_pool_count = 0;
    myp_coro_stack_pool_capacity = 0;
    myp_coro_stack_pool_bytes = 0;
}

// ---- Retired-stack reclamation ----
// 句柄槽位不复用（避免已存句柄被新协程别名/结果串位），故已完成协程的栈不能靠
// 槽位复用回收。trampoline 运行在自己的栈上无法就地 free，所以把 {ptr,size} 记入
// 线程本地 retired 列表，在下一个安全点（__myp_coro_create 或调度器——都不在
// 退役栈上运行）移回栈池复用，避免大量短协程累积栈内存（如 coro_spawn 2万×64KB）。
static __thread myp_coro_stack_slot_t* myp_coro_retired = NULL;
static __thread int myp_coro_retired_count = 0;
static __thread int myp_coro_retired_cap = 0;
static __thread size_t myp_coro_retired_bytes = 0;   // M9: retired stack bytes

static void myp_coro_retired_add(char* ptr, size_t size) {
    if (!ptr) return;
    if (myp_coro_retired_count >= myp_coro_retired_cap) {
        if (myp_coro_retired_cap > INT_MAX / 2) { free(ptr); return; }
        int nc = myp_coro_retired_cap ? myp_coro_retired_cap * 2 : 16;
        size_t nbytes;
        if (myp_mul_overflow((size_t)nc, sizeof(myp_coro_stack_slot_t), &nbytes)) { free(ptr); return; }
        myp_coro_stack_slot_t* np = (myp_coro_stack_slot_t*)realloc(myp_coro_retired, nbytes);
        if (!np) { free(ptr); return; }
        myp_coro_retired = np;
        myp_coro_retired_cap = nc;
    }
    myp_coro_retired[myp_coro_retired_count].ptr = ptr;
    myp_coro_retired[myp_coro_retired_count].size = size;
    myp_coro_retired_count++;
    myp_coro_retired_bytes += size;   // M9
}

static void myp_coro_retired_drain(void) {
    for (int i = 0; i < myp_coro_retired_count; i++)
        myp_coro_stack_pool_add(myp_coro_retired[i].ptr, myp_coro_retired[i].size);
    myp_coro_retired_count = 0;
    myp_coro_retired_bytes = 0;   // M9
}

static void myp_coro_retired_free_all(void) {
    for (int i = 0; i < myp_coro_retired_count; i++)
        free(myp_coro_retired[i].ptr);
    free(myp_coro_retired);
    myp_coro_retired = NULL;
    myp_coro_retired_count = 0;
    myp_coro_retired_cap = 0;
    myp_coro_retired_bytes = 0;   // M9
}

// Trampoline: entered on the coroutine's first resume (myp_coro_current already
// set to the handle). Calls the coroutine's entry function, then deactivates it
// and switches back to whoever resumed/created it (ret_ctx).
// It also installs a coroutine-level exception boundary: an exception thrown
// inside the coroutine that no inner catch handles longjmps back HERE (rather
// than to the main thread's top handler), so the coroutine ends cleanly
// (active=0) and the rest of the process keeps running instead of aborting.
// §五-1 收尾: defined below the wait table (needs myp_release); forward-declared
// so the trampoline can release a frame abandoned by an uncaught exception.
static void __myp_coro_release_frame(myp_coro_t* c);
static void __myp_coro_trampoline(void) {
    int id = myp_coro_current;
    // ASan：完成"调用者→本协程"的 fiber 切换（配对 __myp_coro_resume 里的
    // start_switch_fiber）；否则首次 entry 后第一个 yield 会报
    // "starting fiber switch while in fiber switch"。
    myp_asan_finish_switch();
    jmp_buf jb;
    if (setjmp(jb) == 0) {
        myp_exception_push(&jb);
        if (id >= 0 && id < myp_coro_count && myp_coros[id] && myp_coros[id]->fn) {
            myp_coros[id]->fn();
        }
        myp_exception_pop();
    } else {
        // Uncaught exception inside this coroutine: pop our boundary handler
        // and report, then finish the coroutine normally (no process abort).
        myp_exception_pop();
        if (myp_current_exception_type > 0) {
            fprintf(stderr, "uncaught exception in coroutine (object, type %d)\n",
                    myp_current_exception_type);
        } else if (myp_error_msg[0]) {
            fprintf(stderr, "uncaught exception in coroutine: %s\n", myp_error_msg);
        } else {
            fprintf(stderr, "uncaught exception in coroutine\n");
        }
    }
    if (id >= 0 && id < myp_coro_count && myp_coros[id]) {
        // §五-1 收尾: release any frame slots still live at completion (normally
        // the epilogue already released+del'd them, so this is a no-op; on an
        // uncaught exception it recovers the frame's objects). We are ON this
        // coroutine's stack, so reading the slot addresses is safe.
        __myp_coro_release_frame(myp_coros[id]);
        // 退役本协程的栈（延迟回收）：不能就地 free（正运行其上），记入列表由
        // 下一次 create/调度器移回池。
        myp_coro_retired_add(myp_coros[id]->stack, myp_coros[id]->stack_size);
        myp_coros[id]->stack = NULL;
        myp_coros[id]->active = 0;
        myp_coros[id]->ready = 0;
        // M1: 正常完成 → 结果待读（Coro.result 完成后仍可读），槽在结果被读或
        // destroy 后回收；被 destroy（discard_result）→ 无结果可读，直接回收。
        if (myp_coros[id]->discard_result) {
            myp_coros[id]->discard_result = 0;
            myp_coros[id]->result_pending = 0;
        } else {
            myp_coros[id]->result_pending = 1;
        }
        myp_coro_try_recycle(id);
        // 完成：切回创建/恢复我们的调用者（ret_ctx）。此函数不返回。
        myp_coro_current = -1;
        myp_asan_start_switch(NULL, 0);
        myp_ctx_switch(&myp_coros[id]->ctx, &myp_coros[id]->ret_ctx);
        myp_asan_finish_switch();
    }
}

// Set up a fresh coroutine context so the first resume jumps into `entry` with
// standard x86-64 function-entry stack alignment (rsp%16 == 8).
#if defined(__x86_64__)
static void myp_ctx_init(myp_ctx_t* ctx, char* stack, size_t stack_size,
                         void (*entry)(void)) {
    // 保存块 = 7 槽 [rsp..rsp+56]（rax=入口, r15..rbp=0）。加载时按序 pop：
    // 入口 rsp = rsp+64，需 %16==8（标准函数入口对齐）。令 base=top-64：
    //   块 [top-64, top-8] 在分配区内；入口 rsp=top-8，%16==8。
    uintptr_t top = (uintptr_t)(stack + stack_size);
    top &= ~(uintptr_t)15;              // 16-align the top
    uintptr_t rsp = top - 64;           // 块基址（16 对齐；base%16==0）
    void** frame = (void**)rsp;
    frame[0] = (void*)entry;            // -> rax（jmp 目标）
    frame[1] = 0; frame[2] = 0; frame[3] = 0;
    frame[4] = 0; frame[5] = 0; frame[6] = 0;  // r15..rbp（入口会立即改写）
    ctx->rsp = (void*)rsp;
}
#else
static void myp_ctx_init(myp_ctx_t* ctx, char* stack, size_t stack_size,
                         void (*entry)(void)) {
    getcontext(&ctx->u);
    ctx->u.uc_link = NULL;              // 完成由 trampoline 显式切回 ret_ctx
    ctx->u.uc_stack.ss_sp = stack;
    ctx->u.uc_stack.ss_size = stack_size;
    makecontext(&ctx->u, (void(*)())entry, 0);
}
#endif

int64_t __myp_coro_create(int64_t stack_bytes) {
    // stack_bytes: requested stack size in bytes (<=0 → default MYP_CORO_STACK_SIZE).
    size_t stack_size = (stack_bytes > 0) ? (size_t)stack_bytes : (size_t)MYP_CORO_STACK_SIZE;
    // 先回收已完成协程的退役栈（安全点：不在退役栈上运行）。
    myp_coro_retired_drain();
    // M1: 优先复用空闲槽（generation+1 → 旧句柄代际过期、稳定失效）；无空闲才扩容。
    int idx = myp_coro_pop_free();
    if (idx < 0) {
        if (myp_coro_grow() != 0) return -1;
        idx = myp_coro_count;
        myp_coro_count++;
        if (!myp_coros[idx]) {
            myp_coro_t* nc = (myp_coro_t*)calloc(1, sizeof(myp_coro_t));
            if (!nc) return -1;
            myp_coros[idx] = nc;
        }
    }
    myp_coro_t* c = myp_coros[idx];
    c->generation++;            // 新鲜槽 calloc 为 0 → 1；复用槽递增使旧句柄失效
    c->stack = myp_coro_stack_pool_take(stack_size);   // reuse a pooled stack if possible
    if (!c->stack) c->stack = (char*)malloc(stack_size);
    if (!c->stack) return -1;
    c->stack_size = stack_size;
    myp_ctx_init(&c->ctx, c->stack, c->stack_size, __myp_coro_trampoline);
    c->active = 1;
    c->ready = 1;
    c->result = 0;
    c->exec_result = 0;
    c->wait_timeout = 0;
    c->cancel_requested = 0;
    c->frame_slots_count = 0;   // §五-1 收尾: fresh frame (slot reuse is clean)
    c->result_pending = 0;
    c->on_free_list = 0;
    c->discard_result = 0;
    return myp_coro_make_handle(idx);
}

// fn_ptr is a 64-bit function pointer on LP64 — must NOT be int32 (truncation bug)
void __myp_coro_set_entry(int64_t handle, int64_t fn_ptr) {
    if (!myp_coro_handle_valid(handle)) return;
    int idx = myp_coro_handle_slot(handle);
    if (idx >= 0 && idx < myp_coro_count && myp_coros[idx])
        myp_coros[idx]->fn = (void (*)(void))(uintptr_t)fn_ptr;
}

// Suspend the current coroutine, passing `val` out to the caller (the scheduler
// or a parent coroutine that resumed us). When resumed, returns the value
// passed in by __myp_coro_resume.
int64_t __myp_coro_yield(int64_t val) {
    if (myp_coro_current < 0) return 0;
    myp_coro_yield_val = val;
    int saved = myp_coro_current;
    myp_coro_current = -1;
    // Save our suspend point into our own ctx, then switch back to the caller.
    myp_asan_start_switch(NULL, 0);   // 切到非纤程（调用者）
    myp_ctx_switch(&myp_coros[saved]->ctx, &myp_coros[saved]->ret_ctx);
    myp_asan_finish_switch();         // 回到本协程栈
    myp_coro_current = saved;
    return myp_coro_resume_val;
}

// Resume a coroutine, passing `val` in. Returns the value the coroutine
// passed out at its await (0 if the coroutine finished without yielding).
// The caller's context is saved into the callee's ret_ctx, enabling nested
// resume (a coroutine resuming another coroutine).
int64_t __myp_coro_resume(int64_t handle, int64_t val) {
    if (!myp_coro_handle_valid(handle)) return -1;
    int idx = myp_coro_handle_slot(handle);
    if (idx < 0 || idx >= myp_coro_count || !myp_coros[idx]) return -1;
    if (!myp_coros[idx]->active) return -1;
    myp_coro_resume_val = val;
    int saved = myp_coro_current;
    myp_coro_current = idx;
    myp_coro_t* c = myp_coros[idx];
    myp_asan_start_switch(c->stack, c->stack_size);   // 切到协程栈
    myp_ctx_switch(&c->ret_ctx, &c->ctx);
    myp_asan_finish_switch();                         // 回到调用者栈
    myp_coro_current = saved;
    return myp_coro_yield_val;
}

// Store the coroutine's return value into its result slot (called by the
// @coro method's return, via codegen).
void __myp_coro_set_result(int64_t val) {
    if (myp_coro_current >= 0 && myp_coro_current < myp_coro_count &&
        myp_coros[myp_coro_current])
        myp_coros[myp_coro_current]->result = val;
}

int64_t __myp_coro_result(int64_t handle) {
    if (!myp_coro_handle_valid(handle)) return 0;
    int idx = myp_coro_handle_slot(handle);
    if (idx < 0 || idx >= myp_coro_count || !myp_coros[idx]) return 0;
    int64_t r = myp_coros[idx]->result;
    // M1: 结果被消费 → 若无未读结果且不活跃，槽可复用（循环 create→complete→result
    // 场景下槽容量与 RSS 达到平台后保持稳定）。
    myp_coros[idx]->result_pending = 0;
    myp_coro_try_recycle(idx);
    return r;
}

int64_t __myp_coro_is_active(int64_t handle) {
    if (!myp_coro_handle_valid(handle)) return 0;
    int idx = myp_coro_handle_slot(handle);
    if (idx < 0 || idx >= myp_coro_count || !myp_coros[idx]) return 0;
    return myp_coros[idx]->active ? 1 : 0;
}

// ---- Cooperative cancellation (C10) ----
// Unlike destroy (which forcibly frees the stack), these only set/read a flag
// so a coroutine can shut itself down at a safe point (after an await/yield)
// and run cleanup.
void __myp_coro_request_cancel(int64_t handle) {
    if (!myp_coro_handle_valid(handle)) return;
    int idx = myp_coro_handle_slot(handle);
    if (idx >= 0 && idx < myp_coro_count && myp_coros[idx])
        myp_coros[idx]->cancel_requested = 1;
}

// 1 if the CURRENT coroutine has a pending cancel request, else 0.
int64_t __myp_coro_cancel_requested(void) {
    if (myp_coro_current >= 0 && myp_coro_current < myp_coro_count &&
        myp_coros[myp_coro_current])
        return myp_coros[myp_coro_current]->cancel_requested ? 1 : 0;
    return 0;
}

// Clear the current coroutine's cancel request (after handling it).
void __myp_coro_cancel_clear(void) {
    if (myp_coro_current >= 0 && myp_coro_current < myp_coro_count &&
        myp_coros[myp_coro_current])
        myp_coros[myp_coro_current]->cancel_requested = 0;
}

// ---- C4: event waiters (type + state declared here so __myp_coro_destroy can
// drop a destroyed coroutine's pending wait records) ----
// A coroutine that awaits an event is removed from the ready queue and parked
// here. When the event is dispatched, matching waiters are re-readied (and
// resumed if the event is being processed synchronously).
// Dynamic table (grows on demand) — a fixed-size table would silently deadlock
// the coroutine when it is full (parked but never registered).
typedef struct {
    int kind;           // 等待来源：0=EVENT（现有 C4），1=TIMER（sleep），2=FD，3=EXEC
    int event_id;       // EVENT
    int fd;             // FD：等待就绪的文件描述符
    short fd_events;    // FD：POLLIN / POLLOUT
    int64_t handle;
    int active;
    int64_t deadline_ms;  // absolute deadline for timed waits (0 = none)
    int expired;          // 1 if this record was cleared by a timeout
    int64_t exec_result;  // EXEC：worker 完成后交付的结果（char* 指针，本线程 myp_strdup）
    int wait_index;     // §五-5 P4 waitAnyOf：此记录对应的 spec 下标（>=0）；
                        // -1 = 普通等待（非 waitAnyOf）；-2 = waitAnyOf 的总体超时记录
} myp_coro_wait_t;
static __thread myp_coro_wait_t* myp_coro_waits = NULL;
static __thread int myp_coro_wait_count = 0;
static __thread int myp_coro_wait_capacity = 0;

// Grow the wait table (at least doubling, starting at 64). Returns 0 on success.
static int myp_coro_wait_reserve(void) {
    if (myp_coro_wait_count >= myp_coro_wait_capacity) {
        if (myp_coro_wait_capacity > INT_MAX / 2) return -1;
        int new_cap = myp_coro_wait_capacity ? myp_coro_wait_capacity * 2 : 64;
        size_t nbytes;
        if (myp_mul_overflow((size_t)new_cap, sizeof(myp_coro_wait_t), &nbytes)) return -1;
        myp_coro_wait_t* np = (myp_coro_wait_t*)realloc(myp_coro_waits, nbytes);
        if (!np) return -1;
        myp_coro_waits = np;
        myp_coro_wait_capacity = new_cap;
    }
    return 0;
}

// ---- §五-1 收尾: coroutine-frame ARC registry ----
// A coroutine's local class/interface/closure slots live on its ucontext stack.
// Normal completion releases them via the function epilogue (codegen pairs each
// release with __myp_coro_frame_clear). But a FORCE destroy (Coro.destroy) or an
// uncaught exception longjmps past those releases → the frame's objects leak.
// So every live ARC slot value inside a @coro body is mirrored into the
// coroutine's frame list at STORE time (as the OBJECT pointer, on the heap) and
// removed at every normal release; on destroy / abnormal exit the runtime
// releases each still-registered object. We track OBJECT pointers (not stack
// slot addresses) so release is safe even after the exception has unwound and
// reused the coroutine's stack. Coroutines are thread-local and destroy runs on
// the owning thread, so the per-coroutine list needs no locking.
void __myp_coro_frame_set(int64_t slot_id, int64_t obj) {
    if (myp_coro_current < 0 || myp_coro_current >= myp_coro_count) return;
    myp_coro_t* c = myp_coros[myp_coro_current];
    if (!c || !slot_id) return;
    for (int i = 0; i < c->frame_slots_count; i++) {
        if (c->frame_slots[i].slot_id == slot_id) {
            c->frame_slots[i].obj = obj;   // update in place (no duplicates)
            return;
        }
    }
    if (c->frame_slots_count >= c->frame_slots_cap) {
        if (c->frame_slots_cap > INT_MAX / 2) return;   // OOM: leave untracked
        int nc = c->frame_slots_cap ? c->frame_slots_cap * 2 : 16;
        size_t nbytes;
        if (myp_mul_overflow((size_t)nc, sizeof(myp_frame_slot_t), &nbytes)) return;
        myp_frame_slot_t* np = (myp_frame_slot_t*)realloc(c->frame_slots, nbytes);
        if (!np) return;   // OOM: leave untracked (leak on destroy, no UAF)
        c->frame_slots = np;
        c->frame_slots_cap = nc;
    }
    c->frame_slots[c->frame_slots_count].slot_id = slot_id;
    c->frame_slots[c->frame_slots_count].obj = obj;
    c->frame_slots_count++;
}
void __myp_coro_frame_clear(int64_t slot_id) {
    if (myp_coro_current < 0 || myp_coro_current >= myp_coro_count) return;
    myp_coro_t* c = myp_coros[myp_coro_current];
    if (!c || !slot_id) return;
    for (int i = 0; i < c->frame_slots_count; i++) {
        if (c->frame_slots[i].slot_id == slot_id) {
            c->frame_slots[i] = c->frame_slots[c->frame_slots_count - 1];
            c->frame_slots_count--;
            return;   // one entry per slot_id
        }
    }
}
// Release every still-live frame slot's OBJECT. The objects are heap pointers
// mirrored at store time, so this is safe even after the coroutine's stack has
// been unwound/reused (unlike reading stack slot contents).
static void __myp_coro_release_frame(myp_coro_t* c) {
    if (!c) return;
    for (int i = 0; i < c->frame_slots_count; i++) {
        void* p = (void*)(uintptr_t)c->frame_slots[i].obj;
        if (p) myp_release(p);
    }
    c->frame_slots_count = 0;
}

void __myp_coro_destroy(int64_t handle) {
    // M1: 旧句柄（代际过期）直接无效，绝不操作新协程。
    if (!myp_coro_handle_valid(handle)) return;
    int idx = myp_coro_handle_slot(handle);
    if (idx < 0 || idx >= myp_coro_count || !myp_coros[idx]) return;
    myp_coro_t* c = myp_coros[idx];
    if (!c->stack) {
        // 已完成（栈已退役）：丢弃结果，使槽可复用。
        c->result_pending = 0;
        myp_coro_try_recycle(idx);
        return;
    }
    if (idx == myp_coro_current) {
        // Safety: never free the stack of the coroutine that is CURRENTLY
        // executing (destroying itself) — that would free live stack memory
        // and corrupt execution. Mark it inactive; the trampoline (which
        // runs after the body returns) discards the result and recycles.
        __myp_coro_release_frame(c);
        c->active = 0;
        c->ready = 0;
        c->discard_result = 1;
        return;
    }
    // Force-destroy of a parked/live coroutine: release its frame's
    // still-live ARC slots BEFORE returning the stack to the pool (the
    // slot addresses point into that stack, which is still allocated).
    __myp_coro_release_frame(c);
    c->active = 0;
    c->ready = 0;
    // Drop any pending event-wait records for this coroutine so the wait
    // table never accumulates dead entries (e.g. destroyed while blocked
    // on an event that is never fired).
    for (int i = 0; i < myp_coro_wait_count; i++) {
        if (myp_coro_waits[i].active && myp_coro_waits[i].handle == idx)
            myp_coro_waits[i].active = 0;
    }
    myp_coro_stack_pool_add(c->stack, c->stack_size);
    c->stack = NULL;
    c->stack_size = 0;
    c->result_pending = 0;   // destroy 丢弃结果 → 槽可复用
    myp_coro_try_recycle(idx);
}

// Handle of the coroutine currently executing on this thread (-1 if none).
// M1: 返回编码句柄（与 create 返回一致），而非内部槽号。
int64_t __myp_coro_current_handle(void) {
    return myp_coro_make_handle(myp_coro_current);
}

// Number of active (live) coroutines on this thread.
int64_t __myp_coro_count(void) {
    int n = 0;
    for (int i = 0; i < myp_coro_count; i++)
        if (myp_coros[i] && myp_coros[i]->active) n++;
    return n;
}

// ---- M9: coroutine resource diagnostics (thread-local) ----
int64_t myp_diag_coro_slots(void) { return myp_coro_count; }
int64_t myp_diag_coro_slot_capacity(void) { return myp_coro_capacity; }
int64_t myp_diag_coro_free_slots(void) { return myp_coro_free_count; }
int64_t myp_diag_stack_pool_count(void) { return myp_coro_stack_pool_count; }
int64_t myp_diag_stack_pool_capacity(void) { return myp_coro_stack_pool_capacity; }
int64_t myp_diag_stack_pool_bytes(void) { return (int64_t)myp_coro_stack_pool_bytes; }
int64_t myp_diag_stack_pool_max_bytes(void) { return (int64_t)MYP_CORO_STACK_POOL_MAX_BYTES; }
int64_t myp_diag_retired_count(void) { return myp_coro_retired_count; }
int64_t myp_diag_retired_bytes(void) { return (int64_t)myp_coro_retired_bytes; }

// Coroutine status: -1 invalid handle, 0 inactive/finished, 1 ready/running,
// 2 blocked (waiting on an event).
int64_t __myp_coro_status(int64_t handle) {
    if (!myp_coro_handle_valid(handle)) return -1;
    int idx = myp_coro_handle_slot(handle);
    if (idx < 0 || idx >= myp_coro_count || !myp_coros[idx]) return -1;
    myp_coro_t* c = myp_coros[idx];
    if (!c->active) return 0;
    return c->ready ? 1 : 2;
}

// ---- C3: automatic scheduler (ready queue) ----
// Runs each ready coroutine exactly one step (until it yields or finishes).
// Coroutines that yield with a plain `await` stay ready, so each call to the
// scheduler advances every live coroutine by one await. Blocked (event-waiting)
// coroutines are skipped until the event arrives and re-readies them.
void myp_exec_pump_results(void);   // §五-5 P3: deliver completed file-exec results
void __myp_coro_scheduler(void) {
    // 安全点：回收已完成协程的退役栈（池满则 free）。调度器不在任何协程栈上运行。
    myp_coro_retired_drain();
    // Compact the wait table: inactive records (woken by fd-poll / expiry /
    // event) were never removed — they accumulate one per waitFd/await round,
    // so myp_coro_wait_count grows linearly with the number of waits and every
    // scheduler call (expiry scan + fd-poll setup + pump scan) becomes O(N) →
    // total O(N²). Dropping inactive entries keeps the table ~constant-size.
    // Safe: no code holds a table index across a scheduler call (wake paths
    // reference handles / spec indices, not table positions); parked-and-still-
    // waiting coroutines have active=1 and are preserved.
    if (myp_coro_wait_count > 0) {
        int w = 0;
        for (int i = 0; i < myp_coro_wait_count; i++) {
            if (myp_coro_waits[i].active) {
                if (w != i) myp_coro_waits[w] = myp_coro_waits[i];
                w++;
            }
        }
        myp_coro_wait_count = w;
    }
    // Process pending events first so event-waiting coroutines (C4) that were
    // re-readied by __myp_coro_event_notify become runnable this round.
    myp_event_process_all();
    // Expire event-waits whose deadline has passed (C10: waitEventTimeout).
    // The waiter is re-readied and its wait_timeout flag set so it can tell
    // "timeout" apart from "event arrived" when it resumes.
    int64_t now = myp_now_ms();
    for (int i = 0; i < myp_coro_wait_count; i++) {
        myp_coro_wait_t* w = &myp_coro_waits[i];
        if (w->active && w->deadline_ms > 0 && now >= w->deadline_ms) {
            w->active = 0;
            int64_t h = w->handle;
            if (h >= 0 && h < myp_coro_count && myp_coros[h] && myp_coros[h]->active) {
                if (w->wait_index >= 0 && w->kind == 1) {
                    // §五-5 P4 waitAnyOf TIMER spec: its OWN deadline fired — a
                    // specific spec (not the overall timeout), so report index.
                    myp_coros[h]->last_wait_index = w->wait_index;
                    myp_coros[h]->ready = 1;
                } else {
                    // Overall timeout: the waitAnyOf -2 marker, a normal timed
                    // wait (wait_index==-1), or an EVENT/FD spec whose deadline
                    // is just the overall timeout — all mean "timed out".
                    myp_coros[h]->wait_timeout = 1;
                    myp_coros[h]->ready = 1;
                }
            }
        }
    }
    // §五-5 P2: poll registered fd waits (kind=FD) once and re-ready the
    // coroutines whose fd became ready. Batch poll of all active fd waits.
    {
        int nfd = 0;
        for (int i = 0; i < myp_coro_wait_count; i++)
            if (myp_coro_waits[i].active && myp_coro_waits[i].kind == 2) nfd++;
        if (nfd > 0) {
            // Reusable thread-local buffers (grow on demand) instead of
            // malloc/free per scheduler call: io_socket polls every round, so
            // per-call heap traffic is pure overhead. (Same rationale as the
            // ready-set snapshot buffer below.)
            static __thread struct pollfd* s_pfds = NULL;
            static __thread int* s_widx = NULL;
            static __thread int s_fd_cap = 0;
            if (nfd > s_fd_cap) {
                int nc = s_fd_cap ? s_fd_cap * 2 : 16;
                while (nc < nfd) {
                    if (nc > INT_MAX / 2) goto fd_poll_done;
                    nc *= 2;
                }
                size_t pfds_bytes, widx_bytes;
                if (myp_mul_overflow((size_t)nc, sizeof(struct pollfd), &pfds_bytes) ||
                    myp_mul_overflow((size_t)nc, sizeof(int), &widx_bytes))
                    goto fd_poll_done;
                struct pollfd* np = (struct pollfd*)realloc(s_pfds, pfds_bytes);
                int* nw = (int*)realloc(s_widx, widx_bytes);
                if (!np || !nw) {
                    // Adopt whichever succeeded (realloc keeps the original on
                    // failure, so neither pointer ever dangles); skip this round.
                    if (np) s_pfds = np;
                    if (nw) s_widx = nw;
                    goto fd_poll_done;
                }
                s_pfds = np;
                s_widx = nw;
                s_fd_cap = nc;
            }
            struct pollfd* pfds = s_pfds;
            int* widx = s_widx;
            int k = 0;
            for (int i = 0; i < myp_coro_wait_count; i++) {
                if (myp_coro_waits[i].active && myp_coro_waits[i].kind == 2) {
                    pfds[k].fd = myp_coro_waits[i].fd;
                    pfds[k].events = myp_coro_waits[i].fd_events;
                    pfds[k].revents = 0;
                    widx[k] = i;
                    k++;
                }
            }
            if (poll(pfds, (nfds_t)nfd, 0) > 0) {
                for (int j = 0; j < nfd; j++) {
                    if (pfds[j].revents & (POLLIN | POLLOUT | POLLERR | POLLHUP)) {
                        int wi = widx[j];
                        int64_t h = myp_coro_waits[wi].handle;
                        myp_coro_waits[wi].active = 0;
                        if (h >= 0 && h < myp_coro_count && myp_coros[h] &&
                            myp_coros[h]->active) {
                            if (myp_coro_waits[wi].wait_index >= 0)  // §五-5 P4 waitAnyOf
                                myp_coros[h]->last_wait_index = myp_coro_waits[wi].wait_index;
                            myp_coros[h]->ready = 1;
                        }
                    }
                }
            }
        }
        fd_poll_done: ;
    }
    // §五-5 P3: deliver completed file-executor results to this thread's
    // waiting coroutines (worker threads posted them to a global result list).
    myp_exec_pump_results();
    if (myp_coro_count == 0) return;
    // Snapshot the ready set first — a coroutine may yield (stay ready) while
    // we are running; we must not re-enter it in the same round.
    // Reusable thread-local buffer (grows on demand) instead of malloc/free per
    // scheduler call — scheduler-driven workloads (channel ping-pong, io_socket)
    // call Coro.scheduler() once per round, so per-call heap traffic is pure
    // overhead (measured: ping-pong 54→~44ms, io_socket 89→~82ms).
    static __thread int64_t* s_snapshot = NULL;
    static __thread size_t s_snapshot_cap = 0;
    size_t need;
    if (myp_mul_overflow((size_t)myp_coro_count, sizeof(int64_t), &need)) return;
    if (need > s_snapshot_cap) {
        int64_t* np = (int64_t*)realloc(s_snapshot, need);
        if (!np) return;
        s_snapshot = np;
        s_snapshot_cap = need;
    }
    int64_t* snapshot = s_snapshot;
    int n = 0;
    for (int i = 0; i < myp_coro_count; i++) {
        if (myp_coros[i] && myp_coros[i]->active && myp_coros[i]->ready)
            snapshot[n++] = i;
    }
    for (int k = 0; k < n; k++) {
        int64_t h = snapshot[k];
        if (h >= 0 && h < myp_coro_count && myp_coros[h] &&
            myp_coros[h]->active && myp_coros[h]->ready)
            __myp_coro_resume(myp_coro_make_handle(h), 0);
    }
}

// ---- C4: event waiters (type/state/reserve declared above, before destroy) ----

// Block until `event_id` is fired, or `timeout_ms` elapses (0 = no timeout).
// Returns the value passed in by the resume that woke us (event arrived), or
// -1 if the wait timed out. The scheduler drives timeout expiry; without the
// scheduler being called, a timed wait behaves like an untimed one.
int64_t __myp_coro_wait_event_timeout(int64_t event_id, int64_t timeout_ms, int64_t val) {
    int64_t deadline = 0;
    if (timeout_ms > 0) deadline = myp_now_ms() + timeout_ms;
    if (myp_coro_current >= 0 && myp_coro_current < myp_coro_count &&
        myp_coros[myp_coro_current]) {
        if (myp_coro_wait_reserve() == 0) {
            myp_coro_waits[myp_coro_wait_count].kind = 0;  // EVENT
            myp_coro_waits[myp_coro_wait_count].event_id = (int)event_id;
            myp_coro_waits[myp_coro_wait_count].handle = myp_coro_current;
            myp_coro_waits[myp_coro_wait_count].active = 1;
            myp_coro_waits[myp_coro_wait_count].deadline_ms = deadline;
            myp_coro_waits[myp_coro_wait_count].expired = 0;
            myp_coro_waits[myp_coro_wait_count].wait_index = -1;
            myp_coro_wait_count++;
            myp_coros[myp_coro_current]->ready = 0;  // blocked, not ready
        }
        // On allocation failure: leave the coroutine READY (do not park it)
        // so it can never be silently lost / deadlocked.
    }
    myp_coro_t* c = (myp_coro_current >= 0 && myp_coro_current < myp_coro_count)
        ? myp_coros[myp_coro_current] : NULL;
    if (c) c->wait_timeout = 0;
    // Suspend; when the event arrives (or we time out) we are re-readied.
    int64_t r = __myp_coro_yield(val);
    if (c && c->wait_timeout) { c->wait_timeout = 0; return -1; }
    return r;
}

// Block until `event_id` is fired (no timeout). Returns the resume-passed value.
int64_t __myp_coro_wait_event(int64_t event_id, int64_t val) {
    return __myp_coro_wait_event_timeout(event_id, 0, val);
}

// Block until ANY of the given event ids fires (or `timeout_ms` elapses).
// Returns the event id that woke us, or -1 on timeout.
int64_t __myp_coro_wait_any(const int64_t* ids, int64_t count, int64_t timeout_ms,
                            int64_t val) {
    int64_t deadline = 0;
    if (timeout_ms > 0) deadline = myp_now_ms() + timeout_ms;
    if (myp_coro_current >= 0 && myp_coro_current < myp_coro_count &&
        myp_coros[myp_coro_current] && ids && count > 0) {
        for (int64_t i = 0; i < count; i++) {
            if (myp_coro_wait_reserve() == 0) {
                myp_coro_waits[myp_coro_wait_count].kind = 0;  // EVENT
                myp_coro_waits[myp_coro_wait_count].event_id = (int)ids[i];
                myp_coro_waits[myp_coro_wait_count].handle = myp_coro_current;
                myp_coro_waits[myp_coro_wait_count].active = 1;
                myp_coro_waits[myp_coro_wait_count].deadline_ms = deadline;
                myp_coro_waits[myp_coro_wait_count].expired = 0;
                myp_coro_waits[myp_coro_wait_count].wait_index = -1;
                myp_coro_wait_count++;
            }
        }
        myp_coros[myp_coro_current]->ready = 0;
        myp_coros[myp_coro_current]->last_wait_event_id = -1;
    }
    myp_coro_t* c = (myp_coro_current >= 0 && myp_coro_current < myp_coro_count)
        ? myp_coros[myp_coro_current] : NULL;
    if (c) c->wait_timeout = 0;
    int64_t r = __myp_coro_yield(val);
    // Clear any remaining wait records for this coroutine — only one event may
    // wake a waitAny (later fires of other listed events must not re-ready it).
    if (c) {
        for (int i = 0; i < myp_coro_wait_count; i++) {
            if (myp_coro_waits[i].active &&
                myp_coro_waits[i].handle == myp_coro_current)
                myp_coro_waits[i].active = 0;
        }
    }
    if (c && c->wait_timeout) { c->wait_timeout = 0; return -1; }
    return (c && c->last_wait_event_id >= 0) ? c->last_wait_event_id : r;
}

// ---- §五-5 P4: unified waitAny — mix EVENT / TIMER / FD specs in ONE wait ----
// spec is a flat long[] of `count * 3` entries, each spec = 3 consecutive longs:
//   [i*3+0] kind : 0=EVENT, 1=TIMER, 2=FD
//   [i*3+1] id   : event_id (EVENT) / fd (FD) / -1 (TIMER)
//   [i*3+2] flag : FD 1=wantRead 2=wantWrite 3=both; TIMER ms (relative deadline);
//                  EVENT 0
// Returns the 0-based index of the spec that fired, -1 on overall timeout
// (`timeout_ms`, ms), -2 if not in a coroutine / bad args.
int64_t __myp_coro_wait_any_of(const int64_t* spec, int64_t count, int64_t timeout_ms,
                               int64_t val) {
    if (!spec || count <= 0) return -2;
    if (myp_coro_current < 0 || myp_coro_current >= myp_coro_count ||
        !myp_coros[myp_coro_current])
        return -2;   // not in a coroutine
    int64_t now0 = myp_now_ms();
    int64_t deadline = (timeout_ms > 0) ? now0 + timeout_ms : 0;
    for (int64_t i = 0; i < count; i++) {
        int64_t kind = spec[i * 3];
        int64_t id = spec[i * 3 + 1];
        int64_t flag = spec[i * 3 + 2];
        if (myp_coro_wait_reserve() != 0) break;
        myp_coro_wait_t* w = &myp_coro_waits[myp_coro_wait_count];
        w->kind = (int)kind;
        w->event_id = (kind == 0) ? (int)id : -1;
        w->fd = (kind == 2) ? (int)id : -1;
        w->fd_events = (kind == 2)
            ? (short)(((flag & 1) ? POLLIN : 0) | ((flag & 2) ? POLLOUT : 0)) : 0;
        w->handle = myp_coro_current;
        w->active = 1;
        w->deadline_ms = (kind == 1) ? ((flag > 0) ? now0 + flag : 0) : deadline;
        w->expired = 0;
        w->exec_result = 0;
        w->wait_index = (int)i;
        myp_coro_wait_count++;
    }
    // Overall-timeout marker (kind=TIMER, wait_index=-2), registered LAST so at a
    // tie its wait_timeout wins the deadline loop (specs' overall deadlines expire
    // before it in table order but only set last_wait_index when kind==1).
    if (deadline > 0 && myp_coro_wait_reserve() == 0) {
        myp_coro_wait_t* w = &myp_coro_waits[myp_coro_wait_count];
        w->kind = 1;
        w->event_id = -1;
        w->fd = -1;
        w->fd_events = 0;
        w->handle = myp_coro_current;
        w->active = 1;
        w->deadline_ms = deadline;
        w->expired = 0;
        w->exec_result = 0;
        w->wait_index = -2;
        myp_coro_wait_count++;
    }
    myp_coro_t* c = myp_coros[myp_coro_current];
    c->ready = 0;
    c->last_wait_index = -1;
    c->wait_timeout = 0;
    int64_t r = __myp_coro_yield(val);
    // Only one spec may wake a waitAnyOf — clear the rest so later fires of
    // other listed specs must not re-ready this coroutine.
    for (int i = 0; i < myp_coro_wait_count; i++) {
        if (myp_coro_waits[i].active && myp_coro_waits[i].handle == myp_coro_current)
            myp_coro_waits[i].active = 0;
    }
    if (c && c->last_wait_index >= 0) return c->last_wait_index;
    if (c && c->wait_timeout) { c->wait_timeout = 0; return -1; }
    return r;
}

// ---- §五-5 P1: coroutine timer (await sleep) ----
// Sleep the current coroutine for `ms` milliseconds WITHOUT blocking the thread:
// register a TIMER wait (deadline = now+ms, no event — event_id = -1 never
// matches a real event), park; the scheduler's deadline-expiry loop re-readies
// it when the deadline passes. Outside a coroutine this falls back to a real
// blocking sleep (myp_sleep_ms) so Async.sleep / Coro.sleep is safe anywhere.
int64_t __myp_coro_sleep(int64_t ms) {
    if (ms <= 0) return 0;
    if (myp_coro_current < 0 || myp_coro_current >= myp_coro_count ||
        !myp_coros[myp_coro_current]) {
        myp_sleep_ms(ms);   // not in a coroutine → blocking fallback
        return 0;
    }
    int64_t deadline = myp_now_ms() + ms;
    if (myp_coro_wait_reserve() == 0) {
        myp_coro_waits[myp_coro_wait_count].kind = 1;      // TIMER
        myp_coro_waits[myp_coro_wait_count].event_id = -1; // never matches an event
        myp_coro_waits[myp_coro_wait_count].handle = myp_coro_current;
        myp_coro_waits[myp_coro_wait_count].active = 1;
        myp_coro_waits[myp_coro_wait_count].deadline_ms = deadline;
        myp_coro_waits[myp_coro_wait_count].expired = 0;
        myp_coro_waits[myp_coro_wait_count].wait_index = -1;
        myp_coro_wait_count++;
        myp_coros[myp_coro_current]->ready = 0;   // park
        myp_coros[myp_coro_current]->wait_timeout = 0;
    }
    // Suspend; the scheduler re-readies us when the deadline passes. A
    // wait_timeout flag set by the expiry path is the NORMAL wake here → 0.
    __myp_coro_yield(0);
    return 0;
}

// ---- §五-5 P2: fd readiness (await socket IO) ----
// Park the current coroutine until `fd` is ready for read/write (poll), or
// `timeout_ms` elapses (0 = no timeout). The scheduler polls registered FD
// waits each round and re-readies ready ones. Returns 1 on ready, -1 on
// timeout. Outside a coroutine returns 0 (cannot wait — caller decides).
int64_t __myp_coro_wait_fd(int64_t fd, int64_t want_read, int64_t want_write,
                           int64_t timeout_ms) {
    if (myp_coro_current < 0 || myp_coro_current >= myp_coro_count ||
        !myp_coros[myp_coro_current]) {
        return 0;   // not in a coroutine
    }
    int64_t deadline = 0;
    if (timeout_ms > 0) deadline = myp_now_ms() + timeout_ms;
    short events = 0;
    if (want_read)  events |= POLLIN;
    if (want_write) events |= POLLOUT;
    if (myp_coro_wait_reserve() == 0) {
        myp_coro_waits[myp_coro_wait_count].kind = 2;        // FD
        myp_coro_waits[myp_coro_wait_count].event_id = -1;   // not event-based
        myp_coro_waits[myp_coro_wait_count].fd = (int)fd;
        myp_coro_waits[myp_coro_wait_count].fd_events = events;
        myp_coro_waits[myp_coro_wait_count].handle = myp_coro_current;
        myp_coro_waits[myp_coro_wait_count].active = 1;
        myp_coro_waits[myp_coro_wait_count].deadline_ms = deadline;
        myp_coro_waits[myp_coro_wait_count].expired = 0;
        myp_coro_waits[myp_coro_wait_count].wait_index = -1;
        myp_coro_wait_count++;
        myp_coros[myp_coro_current]->ready = 0;   // park
        myp_coros[myp_coro_current]->wait_timeout = 0;
    }
    myp_coro_t* c = (myp_coro_current >= 0 && myp_coro_current < myp_coro_count)
        ? myp_coros[myp_coro_current] : NULL;
    if (c) c->wait_timeout = 0;
    __myp_coro_yield(0);
    if (c && c->wait_timeout) { c->wait_timeout = 0; return -1; }  // timeout
    return 1;   // fd ready (or manual resume)
}

// ======================
// §五-5 P3: file IO executor (bounded worker pool + cross-thread result delivery)
// ======================
// Blocking file reads (fgets / read-all) run on a small worker pool so the
// coroutine's thread is never blocked. A worker reads the file's FILE* directly
// from the io handle table, then posts the malloc'd result to a global pending
// list; the owning thread's scheduler (myp_exec_pump_results) strdups it onto
// the coroutine's thread, stashes it in the coroutine's EXEC wait record and
// re-readies it. Coroutines are thread-local, so delivery is one-way (worker →
// owning thread); no cross-thread coroutine mutation.

#define MYP_EXEC_WORKERS 4
#define MYP_EXEC_MAX_TASKS 256

typedef struct {
    int io_handle;
    int op;              // 0 = readLine, 1 = readAll
    pthread_t target;    // owning thread (the coroutine's thread)
    int64_t coro_handle;
} myp_exec_task_t;
static myp_exec_task_t myp_exec_tasks[MYP_EXEC_MAX_TASKS];
static int myp_exec_head = 0, myp_exec_tail = 0, myp_exec_count = 0;
static pthread_mutex_t myp_exec_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t myp_exec_cond = PTHREAD_COND_INITIALIZER;
static pthread_t myp_exec_workers[MYP_EXEC_WORKERS];
static int myp_exec_started = 0;

typedef struct {
    pthread_t target;
    int64_t coro_handle;
    char* result;   // malloc'd (NOT myp_alloc'd — freed after delivery)
    int active;
} myp_exec_result_t;
static myp_exec_result_t* myp_exec_results = NULL;
static int myp_exec_results_count = 0, myp_exec_results_cap = 0;

static void myp_exec_push_result(pthread_t t, int64_t h, char* r) {
    if (myp_exec_results_count >= myp_exec_results_cap) {
        if (myp_exec_results_cap > INT_MAX / 2) { free(r); return; }
        int nc = myp_exec_results_cap ? myp_exec_results_cap * 2 : 64;
        size_t nbytes;
        if (myp_mul_overflow((size_t)nc, sizeof(myp_exec_result_t), &nbytes)) { free(r); return; }
        myp_exec_result_t* np = (myp_exec_result_t*)realloc(myp_exec_results, nbytes);
        if (!np) { free(r); return; }
        myp_exec_results = np;
        myp_exec_results_cap = nc;
    }
    myp_exec_results[myp_exec_results_count].target = t;
    myp_exec_results[myp_exec_results_count].coro_handle = h;
    myp_exec_results[myp_exec_results_count].result = r;
    myp_exec_results[myp_exec_results_count].active = 1;
    myp_exec_results_count++;
}

static char* myp_exec_read_line_from(FILE* fp) {
    // INTERNAL helper: returns a plain malloc'd buffer (callers wrap it in
    // myp_strdup for the counted-string path, then free() this buffer).
    char* line = (char*)malloc(4096);
    if (!line) return strdup("");
    if (fgets(line, 4096, fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        return line;
    }
    free(line);
    return strdup("");
}

static char* myp_exec_read_all_from(FILE* fp) {
    // INTERNAL helper: returns a plain malloc'd buffer (callers wrap it in
    // myp_strdup for the counted-string path, then free() this buffer).
    size_t cap = 4096, len = 0;
    size_t init;
    if (myp_add_overflow(cap, 1, &init)) return strdup("");
    char* buf = (char*)malloc(init);
    if (!buf) return strdup("");
    char tmp[4096];
    while (fgets(tmp, sizeof(tmp), fp)) {
        size_t n = strlen(tmp);
        size_t need;
        if (myp_add_overflow(len, n, &need) || myp_add_overflow(need, 1, &need)) {
            free(buf); return strdup("");
        }
        if (need > cap) {
            size_t ncap;
            if (myp_add_overflow(cap, n, &ncap) || ncap > SIZE_MAX / 2) {
                free(buf); return strdup("");
            }
            ncap = ncap * 2;
            size_t ncap1;
            if (myp_add_overflow(ncap, 1, &ncap1)) { free(buf); return strdup(""); }
            char* nb = (char*)realloc(buf, ncap1);
            if (!nb) { free(buf); return strdup(""); }
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    buf[len] = '\0';
    return buf;
}

static void* myp_exec_worker_loop(void* arg) {
    for (;;) {
        myp_exec_task_t task;
        pthread_mutex_lock(&myp_exec_mutex);
        while (myp_exec_count == 0)
            pthread_cond_wait(&myp_exec_cond, &myp_exec_mutex);
        task = myp_exec_tasks[myp_exec_head];
        myp_exec_head = (myp_exec_head + 1) % MYP_EXEC_MAX_TASKS;
        myp_exec_count--;
        pthread_mutex_unlock(&myp_exec_mutex);

        char* out = NULL;
        FILE* fp = myp_io_lock_handle(task.io_handle);
        if (fp) {
            out = (task.op == 0) ? myp_exec_read_line_from(fp) : myp_exec_read_all_from(fp);
            myp_io_unlock_handle(task.io_handle);
        } else {
            out = strdup("");
        }

        pthread_mutex_lock(&myp_exec_mutex);
        myp_exec_push_result(task.target, task.coro_handle, out);
        pthread_mutex_unlock(&myp_exec_mutex);
    }
    return NULL;
}

static void myp_exec_ensure_started(void) {
    pthread_mutex_lock(&myp_exec_mutex);
    if (!myp_exec_started) {
        for (int i = 0; i < MYP_EXEC_WORKERS; i++)
            pthread_create(&myp_exec_workers[i], NULL, myp_exec_worker_loop, NULL);
        myp_exec_started = 1;
    }
    pthread_mutex_unlock(&myp_exec_mutex);
}

// Deliver completed file-exec results to the CURRENT thread's waiting
// coroutines (called by the scheduler each round). Runs on the coroutine's
// owning thread; copies each result into the coroutine's EXEC wait record
// (myp_strdup → tracked on this thread) and re-readies it.
void myp_exec_pump_results(void) {
    pthread_t me = pthread_self();
    pthread_mutex_lock(&myp_exec_mutex);
    for (int i = 0; i < myp_exec_results_count; i++) {
        myp_exec_result_t* r = &myp_exec_results[i];
        if (!r->active || !pthread_equal(r->target, me)) continue;
        r->active = 0;
        int64_t h = r->coro_handle;
        if (h >= 0 && h < myp_coro_count && myp_coros[h] && myp_coros[h]->active) {
            char* copy = myp_strdup(r->result);   // tracked on THIS thread
            for (int j = 0; j < myp_coro_wait_count; j++) {
                if (myp_coro_waits[j].active && myp_coro_waits[j].kind == 3 &&
                    myp_coro_waits[j].handle == h) {
                    myp_coro_waits[j].active = 0;
                    myp_coro_waits[j].exec_result = (int64_t)copy;
                    break;
                }
            }
            myp_coros[h]->exec_result = (int64_t)copy; // 存进协程体（地址稳定，压缩无关）
            myp_coros[h]->ready = 1;
        }
        free(r->result);
        r->result = NULL;
    }
    pthread_mutex_unlock(&myp_exec_mutex);
}

// Park the current coroutine until a worker completes a blocking read of the
// file referenced by `io_handle`. Returns the read string (tracked on the
// coroutine's thread). Outside a coroutine → synchronous fallback.
static char* myp_coro_file_read(int io_handle, int op) {
    if (!myp_coro_am_i_coro()) {
        FILE* fp = myp_io_lock_handle(io_handle);
        if (!fp) return myp_strdup("");
        char* r = (op == 0) ? myp_exec_read_line_from(fp) : myp_exec_read_all_from(fp);
        myp_io_unlock_handle(io_handle);
        char* tracked = myp_strdup(r);
        free(r);
        return tracked;
    }
    myp_exec_ensure_started();
    int64_t h = myp_coro_current;
    int idx = -1;
    if (myp_coro_wait_reserve() == 0) {
        idx = myp_coro_wait_count;
        myp_coro_waits[idx].kind = 3;            // EXEC
        myp_coro_waits[idx].event_id = -1;
        myp_coro_waits[idx].fd = -1;
        myp_coro_waits[idx].fd_events = 0;
        myp_coro_waits[idx].handle = h;
        myp_coro_waits[idx].active = 1;
        myp_coro_waits[idx].deadline_ms = 0;
        myp_coro_waits[idx].expired = 0;
        myp_coro_waits[idx].exec_result = 0;
        myp_coro_waits[idx].wait_index = -1;
        myp_coro_wait_count++;
        myp_coros[h]->ready = 0;                 // park
        myp_coros[h]->wait_timeout = 0;
    }
    pthread_mutex_lock(&myp_exec_mutex);
    if (myp_exec_count < MYP_EXEC_MAX_TASKS) {
        myp_exec_tasks[myp_exec_tail].io_handle = io_handle;
        myp_exec_tasks[myp_exec_tail].op = op;
        myp_exec_tasks[myp_exec_tail].target = pthread_self();
        myp_exec_tasks[myp_exec_tail].coro_handle = h;
        myp_exec_tail = (myp_exec_tail + 1) % MYP_EXEC_MAX_TASKS;
        myp_exec_count++;
        pthread_cond_signal(&myp_exec_cond);
    }
    pthread_mutex_unlock(&myp_exec_mutex);
    myp_coros[h]->exec_result = 0;   // 挂起前清掉上次结果
    __myp_coro_yield(0);   // park until the worker delivers the result
    // 结果存在协程体（地址稳定），不依赖 wait 表索引——压缩安全。
    char* res = (char*)myp_coros[h]->exec_result;
    return res ? res : myp_strdup("");
}

// Async readLine on a File handle (§五-5 P3). Returns the line ('' at EOF).
char* myp_coro_file_read_line(int io_handle) {
    return myp_coro_file_read(io_handle, 0);
}
// Async read-all of a File handle (§五-5 P3).
char* myp_coro_file_read_all(int io_handle) {
    return myp_coro_file_read(io_handle, 1);
}

// Re-ready (and resume if currently dispatching) waiters for an event.
static void __myp_coro_event_notify(int event_id) {
    for (int i = 0; i < myp_coro_wait_count; i++) {
        if (myp_coro_waits[i].active && myp_coro_waits[i].event_id == event_id) {
            int64_t h = myp_coro_waits[i].handle;
            myp_coro_waits[i].active = 0;
            if (h >= 0 && h < myp_coro_count && myp_coros[h] && myp_coros[h]->active) {
                myp_coros[h]->last_wait_event_id = event_id;  // for waitAny
                if (myp_coro_waits[i].wait_index >= 0)        // §五-5 P4 waitAnyOf
                    myp_coros[h]->last_wait_index = myp_coro_waits[i].wait_index;
                myp_coros[h]->ready = 1;
            }
        }
    }
}

// Free all remaining coroutine stacks at process exit (avoids leaks)
static void __myp_coro_cleanup_all(void) {
    for (int i = 0; i < myp_coro_count; i++) {
        if (myp_coros[i]) {
            if (myp_coros[i]->stack) {
                free(myp_coros[i]->stack);
                myp_coros[i]->stack = NULL;
            }
            free(myp_coros[i]->frame_slots);   // §五-1 收尾
            free(myp_coros[i]);
            myp_coros[i] = NULL;
        }
    }
    free(myp_coros);
    myp_coros = NULL;
    myp_coro_count = 0;
    myp_coro_capacity = 0;
    free(myp_coro_free_slots);   // M1: 空闲槽列表
    myp_coro_free_slots = NULL;
    myp_coro_free_count = 0;
    myp_coro_free_cap = 0;
    free(myp_coro_waits);
    myp_coro_waits = NULL;
    myp_coro_wait_count = 0;
    myp_coro_wait_capacity = 0;
    myp_coro_stack_pool_free_all();
    myp_coro_retired_free_all();
}

// Release the current thread's coroutine state (called when a @thread thread
// exits, so its TLS coroutine slots/stacks are not leaked).
static void myp_channel_cleanup_all(void);   // defined below (channel section)
void __myp_coro_thread_cleanup(void) {
    __myp_coro_cleanup_all();
    myp_coro_current = -1;
    myp_coro_wait_count = 0;
    myp_channel_cleanup_all();
}

__attribute__((constructor))
static void __myp_coro_register_cleanup(void) {
    // M6: free the process-global ARC tracking list at exit. atexit handlers
    // run LIFO, so register this FIRST and the coroutine cleanup SECOND: the
    // coroutine cleanup runs first (releasing frame-slot objects → removed
    // from the tracking list at rc==0), then the raw free handles whatever is
    // still live (leaked / program-lifetime). Runs after main returns when all
    // @thread workers have been joined, so no thread is still using blocks.
    atexit(myp_free_alloc_list_global);
    // M7: free the weak registry (entry/slot-array blocks only).
    atexit(myp_weak_free_all);
    atexit(__myp_coro_cleanup_all);
}

// ---- Coroutine entry arguments (thread-local) ----
// The coroutine entry wrapper reads 'this' (slot 0) and params (slot 1..N)
// from here; spawn stores them before the first resume. Thread-local is fine
// because only one coroutine of a thread runs at a time.
#define MYP_CORO_MAX_ENTRY_ARGS 16
static __thread uint64_t myp_coro_entry_args[MYP_CORO_MAX_ENTRY_ARGS];
void __myp_coro_set_entry_arg(int64_t idx, int64_t val) {
    if (idx >= 0 && idx < MYP_CORO_MAX_ENTRY_ARGS)
        myp_coro_entry_args[idx] = (uint64_t)val;
}
int64_t __myp_coro_get_entry_arg(int64_t idx) {
    if (idx >= 0 && idx < MYP_CORO_MAX_ENTRY_ARGS)
        return (int64_t)myp_coro_entry_args[idx];
    return 0;
}

// ---- Channel (协程间通信, Go-style buffered channel) ----
// A bounded ring buffer with coroutine-aware blocking send/recv:
//   - a coroutine that sends into a full buffer parks itself (ready=0 + yield)
//     and is re-readied when a recv frees a slot;
//   - a coroutine that recvs from an empty buffer parks and is re-readied when
//     a send delivers data.
// Non-coroutine callers (main / @thread) get a non-blocking -1 when the buffer
// is full/empty instead of parking.
// Channels are thread-local (a channel belongs to the thread that created it),
// matching the coroutine TLS model.
#define MYP_CHANNEL_MAX_WAITERS 256
typedef struct {
    int64_t* buf;
    int capacity;
    int head;
    int count;
    int closed;
    int64_t recv_waiters[MYP_CHANNEL_MAX_WAITERS]; // coroutines waiting for data
    int recv_wait_count;
    int64_t send_waiters[MYP_CHANNEL_MAX_WAITERS]; // coroutines waiting for space
    int send_wait_count;
} myp_channel_t;
#define MYP_MAX_CHANNELS 256
static __thread myp_channel_t* myp_channels = NULL; // allocated per-thread lazily
static __thread int myp_channel_count = 0;

// Pop the FIRST waiter (FIFO). Returns handle or -1.
static int64_t myp_channel_pop_first(int64_t* waiters, int* wcount) {
    if (*wcount <= 0) return -1;
    int64_t h = waiters[0];
    for (int i = 1; i < *wcount; i++) waiters[i - 1] = waiters[i];
    (*wcount)--;
    return h;
}

// ---- Synchronous handoff (Go-style rendezvous) ----
// send/recv 完成缓冲操作后唤醒对端等待者时，若调用方本身是协程，**立即** resume
// 对端（__myp_coro_resume 内联运行它一步），省掉一轮 Coro.scheduler() 往返。
// 深度守卫防跨多协程链式唤醒时 C 级递归失控；到上限/非协程上下文回退 ready=1。
// 前置正确性修复（count 循环校验 + 句柄唯一）已保证：内联 resume 的对端若发现
// 无值/无空间会重新挂起，不会下溢/越界；句柄唯一使 Coro.result 不串位。
static __thread int myp_channel_wake_depth = 0;
#define MYP_CHANNEL_WAKE_DEPTH_MAX 64
static int64_t myp_channel_wake_one(int64_t* waiters, int* wcount) {
    int64_t h = myp_channel_pop_first(waiters, wcount);
    if (h < 0) return -1;
    if (h < 0 || h >= myp_coro_count || !myp_coros[h] || !myp_coros[h]->active) return h;
    if (myp_coro_current >= 0 && myp_channel_wake_depth < MYP_CHANNEL_WAKE_DEPTH_MAX) {
        myp_channel_wake_depth++;
        __myp_coro_resume(myp_coro_make_handle(h), 0);   // 同步交接：现在就跑对端一步
        myp_channel_wake_depth--;
    } else {
        myp_coros[h]->ready = 1;   // 回退：等下一轮调度
    }
    return h;
}

// Ready-only wake: mark for the scheduler, never inline-resume. Used by close()
// (broadcast) and try_* — inline-resuming there could run a woken coroutine's
// unbounded send/recv loop on a closed channel, hanging the caller.
static int64_t myp_channel_wake_ready(int64_t* waiters, int* wcount) {
    int64_t h = myp_channel_pop_first(waiters, wcount);
    if (h >= 0 && h < myp_coro_count && myp_coros[h] && myp_coros[h]->active)
        myp_coros[h]->ready = 1;
    return h;
}

static myp_channel_t* myp_channel_get(int64_t handle) {
    if (handle < 0 || handle >= myp_channel_count) return NULL;
    return &myp_channels[handle];
}

int64_t myp_channel_create(int64_t capacity) {
    if (!myp_channels) {
        myp_channels = (myp_channel_t*)calloc((size_t)MYP_MAX_CHANNELS, sizeof(myp_channel_t));
        if (!myp_channels) return -1;
    }
    for (int i = 0; i < MYP_MAX_CHANNELS; i++) {
        if (!myp_channels[i].buf) {
            int cap = (capacity > 0) ? (int)capacity : 1;
            myp_channels[i].buf = (int64_t*)calloc((size_t)cap, sizeof(int64_t));
            if (!myp_channels[i].buf) return -1;
            myp_channels[i].capacity = cap;
            myp_channels[i].head = 0;
            myp_channels[i].count = 0;
            myp_channels[i].closed = 0;
            myp_channels[i].recv_wait_count = 0;
            myp_channels[i].send_wait_count = 0;
            if (i >= myp_channel_count) myp_channel_count = i + 1;
            return i;
        }
    }
    return -1; // too many channels
}

void myp_channel_destroy(int64_t handle) {
    myp_channel_t* c = myp_channel_get(handle);
    if (!c) return;
    free(c->buf);
    c->buf = NULL;
    c->capacity = 0;
    c->count = 0;
    c->closed = 1;
    c->recv_wait_count = 0;
    c->send_wait_count = 0;
}

// Send. Returns 0 on success. A coroutine parks if the buffer is full;
// a non-coroutine caller returns -1 instead of parking.
// 修复：park-resume 后重新校验 count<capacity——多生产者下，唤醒我们的槽位可能
// 已被另一个先 resume 的生产者占用，必须重新挂起而非无守卫写缓冲（曾致 count 上溢
// + 环形缓冲越界，见 multi-consumer 崩溃）。
int64_t myp_channel_send(int64_t handle, int64_t val) {
    myp_channel_t* c = myp_channel_get(handle);
    if (!c || c->closed) return -1;
    for (;;) {
        if (c->count < c->capacity) {
            // head/count bounded: head+count < 2*cap, one wrap suffices (no idiv).
            int wpos = c->head + c->count;
            if (wpos >= c->capacity) wpos -= c->capacity;
            c->buf[wpos] = val;
            c->count++;
            myp_channel_wake_one(c->recv_waiters, &c->recv_wait_count);
            return 0;
        }
        // Buffer full — park as a sender (if a coroutine).
        if (myp_coro_current >= 0 && myp_coro_current < myp_coro_count &&
            myp_coros[myp_coro_current]) {
            if (c->send_wait_count < MYP_CHANNEL_MAX_WAITERS) {
                c->send_waiters[c->send_wait_count++] = myp_coro_current;
                myp_coros[myp_coro_current]->ready = 0;   // parked
            }
            __myp_coro_yield(val);   // suspend until a slot frees up
            if (c->closed) return -1;
            // Loop: 重新校验。可能别的发送者已先占用了空位。
        } else {
            return -1; // non-coroutine, buffer full
        }
    }
}

// Recv. Returns the value. A coroutine parks if the buffer is empty;
// a non-coroutine caller returns -1 instead of parking.
// 修复：park-resume 后重新校验 count>0——多消费者下，唤醒我们的值可能已被另一个
// 先 resume 的消费者取走，必须重新挂起而非无守卫读+count--（曾致 count 下溢 -1 +
// 环形缓冲越界，见 multi-consumer 崩溃）。
int64_t myp_channel_recv(int64_t handle) {
    myp_channel_t* c = myp_channel_get(handle);
    if (!c) return -1;
    for (;;) {
        if (c->count > 0) {
            int64_t v = c->buf[c->head];
            // head < cap: head+1 <= cap, wrap once (no idiv).
            int nh = c->head + 1;
            if (nh >= c->capacity) nh = 0;
            c->head = nh;
            c->count--;
            myp_channel_wake_one(c->send_waiters, &c->send_wait_count);
            return v;
        }
        if (c->closed) return -1;
        if (myp_coro_current >= 0 && myp_coro_current < myp_coro_count &&
            myp_coros[myp_coro_current]) {
            if (c->recv_wait_count < MYP_CHANNEL_MAX_WAITERS) {
                c->recv_waiters[c->recv_wait_count++] = myp_coro_current;
                myp_coros[myp_coro_current]->ready = 0;   // parked
            }
            __myp_coro_yield(0);   // suspend until data arrives
            if (c->closed) return -1;
            // Loop: 重新校验。可能别的消费者已先取走了值。
        } else {
            return -1; // non-coroutine, empty
        }
    }
}

// Non-blocking variants (never park).
int64_t myp_channel_try_send(int64_t handle, int64_t val) {
    myp_channel_t* c = myp_channel_get(handle);
    if (!c || c->closed || c->count >= c->capacity) return -1;
    int wpos = c->head + c->count;
    if (wpos >= c->capacity) wpos -= c->capacity;
    c->buf[wpos] = val;
    c->count++;
    myp_channel_wake_ready(c->recv_waiters, &c->recv_wait_count);
    return 0;
}

int64_t myp_channel_try_recv(int64_t handle) {
    myp_channel_t* c = myp_channel_get(handle);
    if (!c || c->count <= 0) return -1;
    int64_t v = c->buf[c->head];
    int nh = c->head + 1;
    if (nh >= c->capacity) nh = 0;
    c->head = nh;
    c->count--;
    myp_channel_wake_ready(c->send_waiters, &c->send_wait_count);
    return v;
}

int64_t myp_channel_size(int64_t handle) {
    myp_channel_t* c = myp_channel_get(handle);
    return c ? c->count : -1;
}

void myp_channel_close(int64_t handle) {
    myp_channel_t* c = myp_channel_get(handle);
    if (!c) return;
    c->closed = 1;
    // Wake all parked senders/recvers so they see the closed state. Ready-only
    // (not synchronous): inline-resuming here could run a woken coroutine's
    // unbounded send/recv loop on a closed channel and hang close() itself.
    while (c->send_wait_count > 0)
        myp_channel_wake_ready(c->send_waiters, &c->send_wait_count);
    while (c->recv_wait_count > 0)
        myp_channel_wake_ready(c->recv_waiters, &c->recv_wait_count);
}

// Free all channels at thread exit (avoid leaks).
static void myp_channel_cleanup_all(void) {
    if (!myp_channels) return;
    for (int i = 0; i < myp_channel_count; i++)
        if (myp_channels[i].buf) free(myp_channels[i].buf);
    free(myp_channels);
    myp_channels = NULL;
    myp_channel_count = 0;
}

// ---- Coroutine-aware Future helpers ----
// A coroutine calling Future.get() on a not-ready future parks (ready=0 + yield)
// instead of pthread_cond_wait, so it does NOT block the whole thread. Only
// same-thread sets can wake it (coroutine state is thread-local).
int myp_coro_am_i_coro(void) {
    return (myp_coro_current >= 0) ? 1 : 0;
}

void myp_coro_wait_future(int32_t fh) {
    if (myp_coro_current < 0 || fh < 0 || fh >= MYP_MAX_FUTURES ||
        !myp_futures[fh].used) return;
    myp_future_t* f = &myp_futures[fh];
    pthread_mutex_lock(&f->mutex);
    if (f->ready) { pthread_mutex_unlock(&f->mutex); return; }
    if (f->coro_wait_count < MYP_FUTURE_MAX_CORO_WAITERS) {
        f->coro_waiters[f->coro_wait_count++] = myp_coro_current;
        myp_coros[myp_coro_current]->ready = 0;   // parked
    }
    pthread_mutex_unlock(&f->mutex);
    __myp_coro_yield(0);   // suspend until set() wakes us
}

void myp_coro_wake_future(int32_t fh) {
    if (fh < 0 || fh >= MYP_MAX_FUTURES) return;
    myp_future_t* f = &myp_futures[fh];
    pthread_mutex_lock(&f->mutex);
    for (int i = 0; i < f->coro_wait_count; i++) {
        int64_t h = f->coro_waiters[i];
        if (h >= 0 && h < myp_coro_count && myp_coros[h] && myp_coros[h]->active)
            myp_coros[h]->ready = 1;
    }
    f->coro_wait_count = 0;
    pthread_mutex_unlock(&f->mutex);
}
