// date_bridge.c — Date / Time Formatting（从 runtime.c 分离，按需链接）
// ---------------------------------------------------------------------------
// 仅用到 date 的程序链接（bridge 符号匹配，同 sdl/ttf/json）。
// ---------------------------------------------------------------------------
#include "mylang/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

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

