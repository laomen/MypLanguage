// regex_bridge.c — POSIX Regular Expressions（从 runtime.c 分离，按需链接）
// ---------------------------------------------------------------------------
// 仅用到 regex 的程序链接（bridge 符号匹配，同 sdl/ttf/json）。
// ---------------------------------------------------------------------------
#include "mylang/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

// ======================
// POSIX Regular Expressions
// ======================

#if defined(_WIN32)
/* Windows：POSIX regex 暂不支持（后续换 PCRE 或移植 mini 引擎）。stub 全部失败。 */
int64_t myp_regex_compile(const char* pattern) { (void)pattern; return 0; }
int32_t myp_regex_match(int64_t handle, const char* s) { (void)handle; (void)s; return 0; }
void myp_regex_free(int64_t handle) { (void)handle; }
#else
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
#endif /* !defined(_WIN32) */

