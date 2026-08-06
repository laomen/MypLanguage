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
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/ioctl.h>
#include <sys/select.h>

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
static FILE* myp_io_fp = NULL;             // 当前活动文件
static FILE* myp_io_table[MYP_IO_MAX_FILES] = {0};
static int32_t myp_io_cur = 0;             // 当前句柄（0=无）

int32_t myp_io_fopen(const char* path, const char* mode) {
    FILE* fp = fopen(path, mode);
    if (!fp) return -1;
    for (int i = 1; i < MYP_IO_MAX_FILES; i++) {
        if (!myp_io_table[i]) {
            myp_io_table[i] = fp;
            myp_io_fp = fp;
            myp_io_cur = i;
            return 0;
        }
    }
    fclose(fp);
    return -1;  // 句柄表满
}

// 当前活动句柄（File.open 成功后读取存入 handle_）
int32_t myp_io_current_handle(void) { return myp_io_cur; }

// 切换当前活动文件（多文件交替读写）
void myp_io_select(int32_t handle) {
    if (handle >= 1 && handle < MYP_IO_MAX_FILES && myp_io_table[handle]) {
        myp_io_fp = myp_io_table[handle];
        myp_io_cur = handle;
    }
}

void myp_io_fclose(void) {
    if (myp_io_cur >= 1 && myp_io_cur < MYP_IO_MAX_FILES && myp_io_table[myp_io_cur]) {
        fclose(myp_io_table[myp_io_cur]);
        myp_io_table[myp_io_cur] = NULL;
    }
    myp_io_fp = NULL;
    myp_io_cur = 0;
}

// 读取一行并返回**新分配的**字符串（修复：原 static buf 共享缓冲，
// 多次 readLine 结果存数组会全部指向最后一行）。EOF 返回空串（文档契约）。
const char* myp_io_read_line(void) {
    if (!myp_io_fp) return myp_strdup("");
    static char buf[4096];
    if (!fgets(buf, sizeof(buf), myp_io_fp)) return myp_strdup("");
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    return myp_strdup(buf);
}

void myp_io_write(const char* text) {
    if (!myp_io_fp) return;
    fputs(text, myp_io_fp);
}

void myp_io_write_line(const char* text) {
    if (!myp_io_fp) return;
    fprintf(myp_io_fp, "%s\n", text);
}

int32_t myp_io_has_next(void) {
    if (!myp_io_fp) return 0;
    return !feof(myp_io_fp);
}

// Binary file I/O for IDX parsing
#include <stdint.h>
int32_t myp_io_read_byte(void) {
    if (!myp_io_fp) return -1;
    unsigned char c;
    if (fread(&c, 1, 1, myp_io_fp) != 1) return -1;
    return (int32_t)c;
}

int32_t myp_io_read_i32be(void) {
    if (!myp_io_fp) return -1;
    unsigned char buf[4];
    if (fread(buf, 1, 4, myp_io_fp) != 4) return -1;
    return ((int32_t)buf[0] << 24) | ((int32_t)buf[1] << 16) |
           ((int32_t)buf[2] << 8) | (int32_t)buf[3];
}

int32_t myp_io_seek(int32_t offset, int32_t whence) {
    if (!myp_io_fp) return -1;
    return fseek(myp_io_fp, offset, whence) == 0 ? 0 : -1;
}

// Binary file I/O for weight save/load
int32_t myp_io_write_byte(int32_t c) {
    if (!myp_io_fp) return -1;
    unsigned char byte = (unsigned char)(c & 0xFF);
    if (fwrite(&byte, 1, 1, myp_io_fp) != 1) return -1;
    return 0;
}

int32_t myp_io_write_i32be(int32_t val) {
    if (!myp_io_fp) return -1;
    unsigned char buf[4];
    buf[0] = (unsigned char)((val >> 24) & 0xFF);
    buf[1] = (unsigned char)((val >> 16) & 0xFF);
    buf[2] = (unsigned char)((val >> 8) & 0xFF);
    buf[3] = (unsigned char)(val & 0xFF);
    if (fwrite(buf, 1, 4, myp_io_fp) != 4) return -1;
    return 0;
}

int32_t myp_io_write_double(double val) {
    if (!myp_io_fp) return -1;
    if (fwrite(&val, sizeof(double), 1, myp_io_fp) != 1) return -1;
    return 0;
}

double myp_io_read_double(void) {
    if (!myp_io_fp) return 0.0;
    double val;
    if (fread(&val, sizeof(double), 1, myp_io_fp) != 1) return 0.0;
    return val;
}

// ======================
// Basic I/O
// ======================

void myp_print(const char* str) { printf("%s", str); fflush(stdout); }
void myp_println(const char* str) { printf("%s\n", str); fflush(stdout); }
void myp_print_int(int32_t val) { printf("%d\n", val); fflush(stdout); }
void myp_print_long(int64_t val) { printf("%ld\n", val); fflush(stdout); }
void myp_print_float(double val) { printf("%g", val); fflush(stdout); }
void myp_print_bool(int32_t val) { printf(val ? "true" : "false"); fflush(stdout); }

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
    return buf;
}

// String length
int32_t myp_strlen(const char* s) {
    if (!s) return 0;
    return (int32_t)strlen(s);
}

// Convert a character code to a single-character string
const char* myp_chr(int32_t code) {
    static char buf[2];
    buf[0] = (char)(code & 0xFF);
    buf[1] = '\0';
    return buf;
}

// ASCII code of the first character (0 if empty)
int32_t myp_ord(const char* s) {
    if (!s || !s[0]) return 0;
    return (int32_t)(unsigned char)s[0];
}

// String equality comparison (content, not pointer)
int32_t myp_str_eq(const char* a, const char* b) {
    if (!a || !b) return a == b ? 1 : 0;
    return strcmp(a, b) == 0 ? 1 : 0;
}

// String to double (for parsing data files)
double myp_atof(const char* s) {
    if (!s) return 0.0;
    return atof(s);
}

// String to int (decimal; 非数字前缀解析失败返回 0)
int32_t myp_str_to_int(const char* s) {
    if (!s) return 0;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return 0;   // 无有效数字
    return (int32_t)v;
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
    if (n->str_val) return n->str_val;
    return "";
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
    void* ptr;
    struct myp_alloc_node* next;
} myp_alloc_node_t;

static pthread_key_t myp_alloc_key;
static pthread_once_t myp_alloc_key_once = PTHREAD_ONCE_INIT;

static void myp_free_alloc_list(void* ptr) {
    myp_alloc_node_t* node = (myp_alloc_node_t*)ptr;
    while (node) {
        if (node->ptr) free(node->ptr);
        myp_alloc_node_t* next = node->next;
        free(node);
        node = next;
    }
}

static void myp_make_alloc_key(void) {
    pthread_key_create(&myp_alloc_key, myp_free_alloc_list);
}

static myp_alloc_node_t* myp_alloc_list_head(void) {
    pthread_once(&myp_alloc_key_once, myp_make_alloc_key);
    return (myp_alloc_node_t*)pthread_getspecific(myp_alloc_key);
}

static void myp_alloc_list_push(void* p) {
    pthread_once(&myp_alloc_key_once, myp_make_alloc_key);
    myp_alloc_node_t* node = (myp_alloc_node_t*)malloc(sizeof(myp_alloc_node_t));
    if (!node) return;
    node->ptr = p;
    node->next = (myp_alloc_node_t*)pthread_getspecific(myp_alloc_key);
    pthread_setspecific(myp_alloc_key, node);
}

void* myp_alloc(size_t size) {
    void* ptr = malloc(size);
    if (ptr) myp_alloc_list_push(ptr);
    return ptr;
}

void myp_free(void* ptr) {
    // Individual free is optional — myp_free_all() at exit handles bulk cleanup.
    // This is provided for cases where deterministic early release is needed.
    free(ptr);
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
    uint32_t rc;
    uint32_t type_id;
} myp_obj_header_t;

#define MYP_OBJ_HEADER_SIZE ((size_t)sizeof(myp_obj_header_t))

// Live class-object count (thread-local) — diagnostic aid for ARC tests.
static __thread int64_t myp_live_objects = 0;

int64_t myp_live_object_count(void) { return myp_live_objects; }

// Mark the tracking-list node for `base` as freed so myp_free_all() at exit
// does not double-free an object ARC already released.
static void myp_alloc_list_mark_freed(void* base) {
    pthread_once(&myp_alloc_key_once, myp_make_alloc_key);
    myp_alloc_node_t* node = (myp_alloc_node_t*)pthread_getspecific(myp_alloc_key);
    while (node) {
        if (node->ptr == base) { node->ptr = NULL; return; }
        node = node->next;
    }
}

void* myp_alloc_object(size_t size, uint32_t type_id) {
    size_t total = size + MYP_OBJ_HEADER_SIZE;
    char* base = (char*)malloc(total);
    if (!base) return NULL;
    myp_obj_header_t* h = (myp_obj_header_t*)base;
    h->rc = 1;
    h->type_id = type_id;
    myp_alloc_list_push(base);   // track base for exit cleanup
    myp_live_objects++;
    return base + MYP_OBJ_HEADER_SIZE;  // data pointer
}

void myp_retain(void* obj) {
    if (!obj) return;
    myp_obj_header_t* h = (myp_obj_header_t*)((char*)obj - MYP_OBJ_HEADER_SIZE);
    h->rc++;
}

uint32_t myp_release(void* obj) {
    if (!obj) return 0;
    myp_obj_header_t* h = (myp_obj_header_t*)((char*)obj - MYP_OBJ_HEADER_SIZE);
    if (h->rc == 0) return 0;          // safety: never underflow
    h->rc--;
    uint32_t new_rc = h->rc;
    if (new_rc == 0) {
        // Cache type_id BEFORE the destroy stub runs — the stub frees the
        // object, so reading h->* afterward would be a use-after-free.
        uint32_t tid = h->type_id;
        // Dispatch to the per-TU destroy stub (cascades reference fields).
        if (tid > 0 && __myp_release_table[tid])
            __myp_release_table[tid](obj);
        else
            myp_free_object(obj);
    }
    return new_rc;
}

void myp_free_object(void* obj) {
    if (!obj) return;
    char* base = (char*)obj - MYP_OBJ_HEADER_SIZE;
    myp_alloc_list_mark_freed(base);
    if (myp_live_objects > 0) myp_live_objects--;
    free(base);
}

// Forward declaration — region arena is defined below (after myp_free_all).
void myp_region_free_all(void);

void myp_free_all(void) {
    // Restore terminal if we changed it to raw mode
    myp_restore_term();
    // Trigger the pthread key destructor which calls myp_free_alloc_list
    pthread_once(&myp_alloc_key_once, myp_make_alloc_key);
    myp_alloc_node_t* head = (myp_alloc_node_t*)pthread_getspecific(myp_alloc_key);
    if (head) {
        myp_free_alloc_list(head);
        pthread_setspecific(myp_alloc_key, NULL);
    }
    // Also free the region arena (thread-local)
    myp_region_free_all();
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
// thread-local region list. myp_arena_release(mark) frees everything
// allocated after myp_arena_mark() — i.e. the region's temporaries —
// while process-level allocations (myp_alloc) survive until myp_free_all.
static pthread_key_t myp_region_key;
static pthread_once_t myp_region_key_once = PTHREAD_ONCE_INIT;
// Region nesting depth (thread-local). >0 means we are inside an @region
// function's dynamic call scope, so myp_region_alloc pushes to the region list
// (dynamic extent — even temporaries allocated by plain callees are reclaimed).
static __thread int myp_region_depth = 0;

static void myp_free_region_list(void* ptr) {
    myp_alloc_node_t* node = (myp_alloc_node_t*)ptr;
    while (node) {
        if (node->ptr) free(node->ptr);
        myp_alloc_node_t* next = node->next;
        free(node);
        node = next;
    }
}

static void myp_make_region_key(void) {
    pthread_key_create(&myp_region_key, myp_free_region_list);
}

void* myp_region_alloc(size_t size) {
    void* ptr = malloc(size);
    if (!ptr) return ptr;
    if (myp_region_depth > 0) {
        pthread_once(&myp_region_key_once, myp_make_region_key);
        myp_alloc_node_t* node = (myp_alloc_node_t*)malloc(sizeof(myp_alloc_node_t));
        if (node) {
            node->ptr = ptr;
            node->next = (myp_alloc_node_t*)pthread_getspecific(myp_region_key);
            pthread_setspecific(myp_region_key, node);
        }
    } else {
        // Not inside an @region — behave exactly like the process-level allocator.
        myp_alloc_list_push(ptr);
    }
    return ptr;
}

// Enters a region scope: bumps the depth and returns the current watermark
// (head of the region list) for the matching myp_arena_release.
void* myp_arena_mark(void) {
    pthread_once(&myp_region_key_once, myp_make_region_key);
    myp_region_depth++;
    return pthread_getspecific(myp_region_key);
}

// Leaves a region scope: frees all region allocations newer than mark
// (those made since the mark) and restores the depth.
void myp_arena_release(void* mark) {
    pthread_once(&myp_region_key_once, myp_make_region_key);
    myp_alloc_node_t* node = (myp_alloc_node_t*)pthread_getspecific(myp_region_key);
    while (node && node != (myp_alloc_node_t*)mark) {
        myp_alloc_node_t* next = node->next;
        if (node->ptr) free(node->ptr);
        free(node);
        node = next;
    }
    pthread_setspecific(myp_region_key, node);
    if (myp_region_depth > 0) myp_region_depth--;
}

void myp_region_free_all(void) {
    pthread_once(&myp_region_key_once, myp_make_region_key);
    myp_alloc_node_t* head = (myp_alloc_node_t*)pthread_getspecific(myp_region_key);
    if (head) {
        myp_free_region_list(head);
        pthread_setspecific(myp_region_key, NULL);
    }
    myp_region_depth = 0;
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

char* myp_strdup(const char* s) {
    if (!s) { char* r = (char*)myp_alloc(1); if (r) r[0] = '\0'; return r; }
    size_t len = strlen(s);
    char* r = (char*)myp_alloc(len + 1);
    if (r) { memcpy(r, s, len + 1); }
    return r;
}

// Convert a value to string (for string concatenation with non-strings)
char* myp_to_string_i32(int32_t val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    return myp_strcat(buf, "");
}
char* myp_to_string_i64(int64_t val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", val);
    return myp_strcat(buf, "");
}
char* myp_to_string_double(double val) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", val);
    return myp_strcat(buf, "");
}
char* myp_to_string_bool(int32_t val) {
    return myp_strcat(val ? "true" : "false", "");
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
    return 0;
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
        int new_cap = q->capacity * 2;
        myp_event_t* ne = (myp_event_t*)realloc(q->events,
                                                (size_t)new_cap * sizeof(myp_event_t));
        if (!ne) { pthread_mutex_unlock(&q->mutex); return; }  // OOM: drop event
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
    while (thr->running) {
        myp_event_process_all();
        myp_timer_check();  // fire expired timers
        // Brief sleep to avoid busy-waiting
        struct timespec ts = {0, 1000000}; // 1ms
        nanosleep(&ts, NULL);
    }
    // Release this thread's coroutine state (TLS) so slots/stacks aren't leaked.
    __myp_coro_thread_cleanup();
    // Clear TLS so destructor doesn't double-free
    pthread_setspecific(myp_queue_key, NULL);
    return NULL;
}

void myp_thread_run_loop(myp_thread_t* thr) {
    pthread_create(&thr->thread, NULL, myp_thread_entry, thr);
}

void myp_thread_stop(myp_thread_t* thr) {
    thr->running = 0;
}

void myp_thread_destroy(myp_thread_t* thr) {
    myp_thread_stop(thr);
    pthread_join(thr->thread, NULL);
    free(thr->queue);
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
    void (*work_fn)(int, void*);
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
        dq->cap *= 2;
        dq->chunks = (myp_work_chunk_t*)realloc(dq->chunks, dq->cap * sizeof(myp_work_chunk_t));
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
            for (int i = chunk.start; i < chunk.end; i += chunk.step)
                pool->work_fn(i, pool->work_arg);
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
                            void (*fn)(int, void*), void* arg) {
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

    // Publish total_chunks under barrier_mutex (workers read/write done_count
    // and total_chunks under this mutex) — avoids the reset/publish data race.
    pthread_mutex_lock(&pool->barrier_mutex);
    pool->done_count = 0;
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

void myp_assert(int cond) {
    if (!cond) {
        fprintf(stderr, "  ASSERTION FAILED\n");
        myp_test_fail_count++;
    } else {
        myp_test_pass_count++;
    }
}

void myp_assert_eq(int a, int b) {
    if (a != b) {
        fprintf(stderr, "  ASSERTION FAILED: %d != %d\n", a, b);
        myp_test_fail_count++;
    } else {
        myp_test_pass_count++;
    }
}

void myp_assert_str_eq(const char* a, const char* b) {
    int eq = (a == b) || (a && b && strcmp(a, b) == 0);
    if (!eq) {
        fprintf(stderr, "  ASSERTION FAILED: \"%s\" != \"%s\"\n", a ? a : "null", b ? b : "null");
        myp_test_fail_count++;
    } else {
        myp_test_pass_count++;
    }
}

void myp_assert_neq(int a, int b) {
    if (a == b) {
        fprintf(stderr, "  ASSERTION FAILED: %d == %d (expected not equal)\n", a, b);
        myp_test_fail_count++;
    } else {
        myp_test_pass_count++;
    }
}

void myp_assert_long_eq(int64_t a, int64_t b) {
    if (a != b) {
        fprintf(stderr, "  ASSERTION FAILED: %ld != %ld\n", (long)a, (long)b);
        myp_test_fail_count++;
    } else {
        myp_test_pass_count++;
    }
}

void myp_assert_str_neq(const char* a, const char* b) {
    int eq = (a == b) || (a && b && strcmp(a, b) == 0);
    if (eq) {
        fprintf(stderr, "  ASSERTION FAILED: \"%s\" == \"%s\" (expected not equal)\n", a ? a : "null", b ? b : "null");
        myp_test_fail_count++;
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
// Coroutine (基于 ucontext 的用户态纤程)
// ======================

#include <ucontext.h>
#include <stdint.h>

#define MYP_CORO_STACK_SIZE (128 * 1024)      // 128KB per coroutine stack
#define MYP_CORO_INITIAL_CAPACITY 64

typedef struct {
    ucontext_t ctx;
    ucontext_t ret_ctx;   // caller context (who resumed/created this coroutine)
    char* stack;
    size_t stack_size;    // bytes allocated for this coroutine's stack
    int active;
    int ready;      // in the ready queue (C3 scheduler); 0 while blocked on an event (C4)
    int wait_timeout; // set when an event-wait with a deadline expires (C10)
    int64_t last_wait_event_id; // event id that woke us (C10 waitAny)
    int cancel_requested; // cooperative-cancel flag (C10)
    void (*fn)(void); // entry function for this coroutine
    int64_t result;   // return value slot (C2)
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
// Value passing between coroutine and scheduler (thread-local; only one
// coroutine of a thread runs at a time):
//   myp_coro_yield_val  — coroutine → scheduler (value passed out by await)
//   myp_coro_resume_val — scheduler → coroutine (value passed in by resume)
static __thread int64_t myp_coro_yield_val = 0;
static __thread int64_t myp_coro_resume_val = 0;

// Each coroutine keeps its OWN return context (ret_ctx): whoever resumes/creates
// it saves their context there before switching in, and the coroutine switches
// back to it on yield or completion (via uc_link). This makes NESTED resume
// correct — a coroutine that resumes another coroutine is that child's caller,
// and the child returns to it, while the parent in turn returns to its own
// caller. A single shared scheduler context cannot express this chain.

// Grow the pointer array (at least doubling). Returns 0 on success.
static int myp_coro_grow(void) {
    int new_cap = myp_coro_capacity ? myp_coro_capacity : MYP_CORO_INITIAL_CAPACITY;
    if (myp_coro_count >= new_cap) new_cap *= 2;
    myp_coro_t** np = (myp_coro_t**)realloc(myp_coros,
                                            (size_t)new_cap * sizeof(myp_coro_t*));
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
#define MYP_CORO_STACK_POOL_MAX 128
typedef struct {
    char* ptr;
    size_t size;
} myp_coro_stack_slot_t;
static __thread myp_coro_stack_slot_t* myp_coro_stack_pool = NULL;
static __thread int myp_coro_stack_pool_count = 0;
static __thread int myp_coro_stack_pool_capacity = 0;

// Add a stack to the pool (free it if the pool is full or allocation fails).
static void myp_coro_stack_pool_add(char* ptr, size_t size) {
    if (!ptr) return;
    if (myp_coro_stack_pool_count >= MYP_CORO_STACK_POOL_MAX) { free(ptr); return; }
    if (myp_coro_stack_pool_count >= myp_coro_stack_pool_capacity) {
        int nc = myp_coro_stack_pool_capacity ? myp_coro_stack_pool_capacity * 2 : 16;
        myp_coro_stack_slot_t* np = (myp_coro_stack_slot_t*)realloc(
            myp_coro_stack_pool, (size_t)nc * sizeof(myp_coro_stack_slot_t));
        if (!np) { free(ptr); return; }
        myp_coro_stack_pool = np;
        myp_coro_stack_pool_capacity = nc;
    }
    myp_coro_stack_pool[myp_coro_stack_pool_count].ptr = ptr;
    myp_coro_stack_pool[myp_coro_stack_pool_count].size = size;
    myp_coro_stack_pool_count++;
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
    myp_coro_stack_pool[best] = myp_coro_stack_pool[--myp_coro_stack_pool_count];
    return p;
}

static void myp_coro_stack_pool_free_all(void) {
    for (int i = 0; i < myp_coro_stack_pool_count; i++)
        free(myp_coro_stack_pool[i].ptr);
    free(myp_coro_stack_pool);
    myp_coro_stack_pool = NULL;
    myp_coro_stack_pool_count = 0;
    myp_coro_stack_pool_capacity = 0;
}

// Trampoline: called by makecontext with coroutine index as arg
// Calls the coroutine's entry function, then deactivates it.
// It also installs a coroutine-level exception boundary: an exception thrown
// inside the coroutine that no inner catch handles longjmps back HERE (rather
// than to the main thread's top handler), so the coroutine ends cleanly
// (active=0) and the rest of the process keeps running instead of aborting.
static void __myp_coro_trampoline(int id) {
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
        myp_coros[id]->active = 0;
        myp_coros[id]->ready = 0;
    }
}

int64_t __myp_coro_create(int64_t stack_bytes) {
    // stack_bytes: requested stack size in bytes (<=0 → default MYP_CORO_STACK_SIZE).
    size_t stack_size = (stack_bytes > 0) ? (size_t)stack_bytes : (size_t)MYP_CORO_STACK_SIZE;
    // Reuse a finished coroutine slot first (its stack is returned to the
    // pool here, not in the trampoline, because the trampoline still runs on
    // its own stack).
    int idx = -1;
    for (int i = 0; i < myp_coro_count; i++) {
        if (myp_coros[i] && !myp_coros[i]->active && myp_coros[i]->stack) {
            myp_coro_stack_pool_add(myp_coros[i]->stack, myp_coros[i]->stack_size);
            myp_coros[i]->stack = NULL;
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (myp_coro_grow() != 0) return -1;
        idx = myp_coro_count;
        myp_coro_count++;
    }
    if (!myp_coros[idx]) {
        myp_coro_t* nc = (myp_coro_t*)calloc(1, sizeof(myp_coro_t));
        if (!nc) return -1;
        myp_coros[idx] = nc;
    }
    myp_coro_t* c = myp_coros[idx];
    c->stack = myp_coro_stack_pool_take(stack_size);   // reuse a pooled stack if possible
    if (!c->stack) c->stack = (char*)malloc(stack_size);
    if (!c->stack) return -1;
    c->stack_size = stack_size;
    if (getcontext(&c->ctx) == -1) { myp_coro_stack_pool_add(c->stack, stack_size); c->stack = NULL; return -1; }
    c->ctx.uc_link = &c->ret_ctx;   // on completion, return to the caller
    c->ctx.uc_stack.ss_sp = c->stack;
    c->ctx.uc_stack.ss_size = stack_size;
    makecontext(&c->ctx, (void(*)())__myp_coro_trampoline, 1, idx);
    c->active = 1;
    c->ready = 1;
    c->result = 0;
    c->wait_timeout = 0;
    c->cancel_requested = 0;
    return idx;
}

// fn_ptr is a 64-bit function pointer on LP64 — must NOT be int32 (truncation bug)
void __myp_coro_set_entry(int64_t handle, int64_t fn_ptr) {
    if (handle >= 0 && handle < myp_coro_count && myp_coros[handle]) {
        myp_coros[handle]->fn = (void (*)(void))(uintptr_t)fn_ptr;
    }
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
    swapcontext(&myp_coros[saved]->ctx, &myp_coros[saved]->ret_ctx);
    myp_coro_current = saved;
    return myp_coro_resume_val;
}

// Resume a coroutine, passing `val` in. Returns the value the coroutine
// passed out at its await (0 if the coroutine finished without yielding).
// The caller's context is saved into the callee's ret_ctx, enabling nested
// resume (a coroutine resuming another coroutine).
int64_t __myp_coro_resume(int64_t handle, int64_t val) {
    if (handle < 0 || handle >= myp_coro_count || !myp_coros[handle]) return -1;
    if (!myp_coros[handle]->active) return -1;
    myp_coro_resume_val = val;
    int saved = myp_coro_current;
    myp_coro_current = handle;
    swapcontext(&myp_coros[handle]->ret_ctx, &myp_coros[handle]->ctx);
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
    if (handle >= 0 && handle < myp_coro_count && myp_coros[handle])
        return myp_coros[handle]->result;
    return 0;
}

int64_t __myp_coro_is_active(int64_t handle) {
    if (handle < 0 || handle >= myp_coro_count || !myp_coros[handle]) return 0;
    return myp_coros[handle]->active ? 1 : 0;
}

// ---- Cooperative cancellation (C10) ----
// Unlike destroy (which forcibly frees the stack), these only set/read a flag
// so a coroutine can shut itself down at a safe point (after an await/yield)
// and run cleanup.
void __myp_coro_request_cancel(int64_t handle) {
    if (handle >= 0 && handle < myp_coro_count && myp_coros[handle])
        myp_coros[handle]->cancel_requested = 1;
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
    int event_id;
    int64_t handle;
    int active;
    int64_t deadline_ms;  // absolute deadline for timed waits (0 = none)
    int expired;          // 1 if this record was cleared by a timeout
} myp_coro_wait_t;
static __thread myp_coro_wait_t* myp_coro_waits = NULL;
static __thread int myp_coro_wait_count = 0;
static __thread int myp_coro_wait_capacity = 0;

// Grow the wait table (at least doubling, starting at 64). Returns 0 on success.
static int myp_coro_wait_reserve(void) {
    if (myp_coro_wait_count >= myp_coro_wait_capacity) {
        int new_cap = myp_coro_wait_capacity ? myp_coro_wait_capacity * 2 : 64;
        myp_coro_wait_t* np = (myp_coro_wait_t*)realloc(
            myp_coro_waits, (size_t)new_cap * sizeof(myp_coro_wait_t));
        if (!np) return -1;
        myp_coro_waits = np;
        myp_coro_wait_capacity = new_cap;
    }
    return 0;
}

void __myp_coro_destroy(int64_t handle) {
    if (handle >= 0 && handle < myp_coro_count && myp_coros[handle] &&
        myp_coros[handle]->stack) {
        // Safety: never free the stack of the coroutine that is CURRENTLY
        // executing (destroying itself) — that would free live stack memory
        // and corrupt execution. Mark it inactive; its stack is reclaimed
        // when the slot is reused by __myp_coro_create (or at process exit).
        if (handle == myp_coro_current) {
            myp_coros[handle]->active = 0;
            myp_coros[handle]->ready = 0;
            return;
        }
        myp_coros[handle]->active = 0;
        myp_coros[handle]->ready = 0;
        // Drop any pending event-wait records for this coroutine so the wait
        // table never accumulates dead entries (e.g. destroyed while blocked
        // on an event that is never fired).
        for (int i = 0; i < myp_coro_wait_count; i++) {
            if (myp_coro_waits[i].active && myp_coro_waits[i].handle == handle)
                myp_coro_waits[i].active = 0;
        }
        myp_coro_stack_pool_add(myp_coros[handle]->stack, myp_coros[handle]->stack_size);
        myp_coros[handle]->stack = NULL;
        myp_coros[handle]->stack_size = 0;
    }
}

// Handle of the coroutine currently executing on this thread (-1 if none).
int64_t __myp_coro_current_handle(void) {
    return myp_coro_current;
}

// Number of active (live) coroutines on this thread.
int64_t __myp_coro_count(void) {
    int n = 0;
    for (int i = 0; i < myp_coro_count; i++)
        if (myp_coros[i] && myp_coros[i]->active) n++;
    return n;
}

// Coroutine status: -1 invalid handle, 0 inactive/finished, 1 ready/running,
// 2 blocked (waiting on an event).
int64_t __myp_coro_status(int64_t handle) {
    if (handle < 0 || handle >= myp_coro_count || !myp_coros[handle]) return -1;
    myp_coro_t* c = myp_coros[handle];
    if (!c->active) return 0;
    return c->ready ? 1 : 2;
}

// ---- C3: automatic scheduler (ready queue) ----
// Runs each ready coroutine exactly one step (until it yields or finishes).
// Coroutines that yield with a plain `await` stay ready, so each call to the
// scheduler advances every live coroutine by one await. Blocked (event-waiting)
// coroutines are skipped until the event arrives and re-readies them.
void __myp_coro_scheduler(void) {
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
                myp_coros[h]->wait_timeout = 1;
                myp_coros[h]->ready = 1;
            }
        }
    }
    if (myp_coro_count == 0) return;
    // Snapshot the ready set first — a coroutine may yield (stay ready) while
    // we are running; we must not re-enter it in the same round.
    int64_t* snapshot = (int64_t*)malloc((size_t)myp_coro_count * sizeof(int64_t));
    if (!snapshot) return;
    int n = 0;
    for (int i = 0; i < myp_coro_count; i++) {
        if (myp_coros[i] && myp_coros[i]->active && myp_coros[i]->ready)
            snapshot[n++] = i;
    }
    for (int k = 0; k < n; k++) {
        int64_t h = snapshot[k];
        if (h >= 0 && h < myp_coro_count && myp_coros[h] &&
            myp_coros[h]->active && myp_coros[h]->ready)
            __myp_coro_resume(h, 0);
    }
    free(snapshot);
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
            myp_coro_waits[myp_coro_wait_count].event_id = (int)event_id;
            myp_coro_waits[myp_coro_wait_count].handle = myp_coro_current;
            myp_coro_waits[myp_coro_wait_count].active = 1;
            myp_coro_waits[myp_coro_wait_count].deadline_ms = deadline;
            myp_coro_waits[myp_coro_wait_count].expired = 0;
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
                myp_coro_waits[myp_coro_wait_count].event_id = (int)ids[i];
                myp_coro_waits[myp_coro_wait_count].handle = myp_coro_current;
                myp_coro_waits[myp_coro_wait_count].active = 1;
                myp_coro_waits[myp_coro_wait_count].deadline_ms = deadline;
                myp_coro_waits[myp_coro_wait_count].expired = 0;
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

// Re-ready (and resume if currently dispatching) waiters for an event.
static void __myp_coro_event_notify(int event_id) {
    for (int i = 0; i < myp_coro_wait_count; i++) {
        if (myp_coro_waits[i].active && myp_coro_waits[i].event_id == event_id) {
            int64_t h = myp_coro_waits[i].handle;
            myp_coro_waits[i].active = 0;
            if (h >= 0 && h < myp_coro_count && myp_coros[h] && myp_coros[h]->active) {
                myp_coros[h]->last_wait_event_id = event_id;  // for waitAny
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
            free(myp_coros[i]);
            myp_coros[i] = NULL;
        }
    }
    free(myp_coros);
    myp_coros = NULL;
    myp_coro_count = 0;
    myp_coro_capacity = 0;
    free(myp_coro_waits);
    myp_coro_waits = NULL;
    myp_coro_wait_count = 0;
    myp_coro_wait_capacity = 0;
    myp_coro_stack_pool_free_all();
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

static void myp_channel_wake_one(int64_t* waiters, int* wcount) {
    if (*wcount <= 0) return;
    int64_t h = waiters[0];
    for (int i = 1; i < *wcount; i++) waiters[i - 1] = waiters[i];
    (*wcount)--;
    if (h >= 0 && h < myp_coro_count && myp_coros[h] && myp_coros[h]->active)
        myp_coros[h]->ready = 1;
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
int64_t myp_channel_send(int64_t handle, int64_t val) {
    myp_channel_t* c = myp_channel_get(handle);
    if (!c || c->closed) return -1;
    if (c->count < c->capacity) {
        c->buf[(c->head + c->count) % c->capacity] = val;
        c->count++;
        myp_channel_wake_one(c->recv_waiters, &c->recv_wait_count);
        return 0;
    }
    // Buffer full.
    if (myp_coro_current >= 0 && myp_coro_current < myp_coro_count &&
        myp_coros[myp_coro_current]) {
        if (c->send_wait_count < MYP_CHANNEL_MAX_WAITERS) {
            c->send_waiters[c->send_wait_count++] = myp_coro_current;
            myp_coros[myp_coro_current]->ready = 0;   // parked
        }
        __myp_coro_yield(val);   // suspend until a slot frees up
        if (c->closed) return -1;
        c->buf[(c->head + c->count) % c->capacity] = val;
        c->count++;
        myp_channel_wake_one(c->recv_waiters, &c->recv_wait_count);
        return 0;
    }
    return -1; // non-coroutine, buffer full
}

// Recv. Returns the value. A coroutine parks if the buffer is empty;
// a non-coroutine caller returns -1 instead of parking.
int64_t myp_channel_recv(int64_t handle) {
    myp_channel_t* c = myp_channel_get(handle);
    if (!c) return -1;
    if (c->count > 0) {
        int64_t v = c->buf[c->head];
        c->head = (c->head + 1) % c->capacity;
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
        int64_t v = c->buf[c->head];
        c->head = (c->head + 1) % c->capacity;
        c->count--;
        myp_channel_wake_one(c->send_waiters, &c->send_wait_count);
        return v;
    }
    return -1; // non-coroutine, empty
}

// Non-blocking variants (never park).
int64_t myp_channel_try_send(int64_t handle, int64_t val) {
    myp_channel_t* c = myp_channel_get(handle);
    if (!c || c->closed || c->count >= c->capacity) return -1;
    c->buf[(c->head + c->count) % c->capacity] = val;
    c->count++;
    myp_channel_wake_one(c->recv_waiters, &c->recv_wait_count);
    return 0;
}

int64_t myp_channel_try_recv(int64_t handle) {
    myp_channel_t* c = myp_channel_get(handle);
    if (!c || c->count <= 0) return -1;
    int64_t v = c->buf[c->head];
    c->head = (c->head + 1) % c->capacity;
    c->count--;
    myp_channel_wake_one(c->send_waiters, &c->send_wait_count);
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
    // Wake all parked senders/recvers so they see the closed state.
    while (c->send_wait_count > 0)
        myp_channel_wake_one(c->send_waiters, &c->send_wait_count);
    while (c->recv_wait_count > 0)
        myp_channel_wake_one(c->recv_waiters, &c->recv_wait_count);
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
