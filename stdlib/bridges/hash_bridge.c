// hash_bridge.c — Hashing (stdlib/crypto.myp)（从 runtime.c 分离，按需链接）
// ---------------------------------------------------------------------------
// 仅用到 hash 的程序链接（bridge 符号匹配，同 sdl/ttf/json）。
// ---------------------------------------------------------------------------
#include "mylang/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

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

