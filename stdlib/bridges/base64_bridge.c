// base64_bridge.c — Base64 Encoding（从 runtime.c 分离，按需链接）
// ---------------------------------------------------------------------------
// 仅用到 base64 的程序链接（bridge 符号匹配，同 sdl/ttf/json）。
// ---------------------------------------------------------------------------
#include "mylang/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

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
