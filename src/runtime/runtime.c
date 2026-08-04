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

static FILE* myp_io_fp = NULL;

int32_t myp_io_fopen(const char* path, const char* mode) {
    FILE* fp = fopen(path, mode);
    if (!fp) return -1;
    myp_io_fp = fp;
    return 0;
}

void myp_io_fclose(void) {
    if (myp_io_fp) fclose(myp_io_fp);
    myp_io_fp = NULL;
}

const char* myp_io_read_line(void) {
    if (!myp_io_fp) return NULL;
    static char buf[4096];
    if (!fgets(buf, sizeof(buf), myp_io_fp)) return NULL;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    return buf;
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

void myp_print(const char* str) { printf("%s", str); }
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

// Run a command via system(). Returns exit code (0 = success).
int32_t myp_process_run(const char* cmd) {
    if (!cmd) return -1;
    return system(cmd);
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

#define MYP_EVENT_QUEUE_SIZE 1024

typedef struct {
    int event_id;
    void* sender;
    void* data;
    int has_data;
} myp_event_t;

// Event queue (ring buffer)
typedef struct {
    myp_event_t events[MYP_EVENT_QUEUE_SIZE];
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
        free(q);
    }
}

static void myp_make_queue_key(void) {
    pthread_key_create(&myp_queue_key, myp_free_queue);
}

// Create a new event queue
static myp_event_queue_t* myp_queue_create(void) {
    myp_event_queue_t* q = (myp_event_queue_t*)calloc(1, sizeof(myp_event_queue_t));
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

// Push an event to a specific queue (thread-safe)
static void myp_queue_push(myp_event_queue_t* q, int event_id, void* sender, void* data) {
    pthread_mutex_lock(&q->mutex);
    int next = (q->head + 1) % MYP_EVENT_QUEUE_SIZE;
    if (next != q->tail) {
        q->events[q->head].event_id = event_id;
        q->events[q->head].sender = sender;
        q->events[q->head].data = data;
        q->events[q->head].has_data = (data != NULL);
        q->head = next;
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
    q->tail = (q->tail + 1) % MYP_EVENT_QUEUE_SIZE;
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
    // Note: thr->startup_arg (the instance) is tracked by myp_alloc and
    // will be freed by myp_free_all() at main exit. Do NOT free it here.
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

static void* myp_pool_worker(void* arg) {
    int tid = (int)(uintptr_t)arg;

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
    myp_pool_create(0);
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

int32_t myp_pool_thread_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int32_t)n : 1;
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
static char myp_error_msg[256];
static int myp_error_active = 0;

// Called from generated code via intrinsic: saves error context
void myp_error_setup(void) {
    myp_error_active = 1;
}

// Called from generated code via intrinsic: records error and triggers longjmp
void myp_throw(const char* msg) {
    strncpy(myp_error_msg, msg, 255);
    myp_error_msg[255] = '\0';
}

const char* myp_get_error(void) {
    return myp_error_msg;
}

int myp_error_is_active(void) {
    return myp_error_active;
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
// Future/Promise (条件变量封装)
// ======================

#define MYP_MAX_FUTURES 64
typedef struct {
    int32_t value;
    int32_t ready;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int32_t used;
} myp_future_t;

static myp_future_t myp_futures[MYP_MAX_FUTURES];
static pthread_mutex_t myp_future_mutex = PTHREAD_MUTEX_INITIALIZER;

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
}

int32_t myp_future_get(int32_t handle) {
    if (handle < 0 || handle >= MYP_MAX_FUTURES || !myp_futures[handle].used) return 0;
    pthread_mutex_lock(&myp_futures[handle].mutex);
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

#define MYP_CORO_STACK_SIZE (256 * 1024)
#define MYP_MAX_COROS 1024

typedef struct {
    ucontext_t ctx;
    char* stack;
    int active;
    void (*fn)(void); // entry function for this coroutine
} myp_coro_t;

static myp_coro_t myp_coros[MYP_MAX_COROS];
static int myp_coro_count = 0;
static int myp_coro_current = -1;
static ucontext_t myp_coro_sched_ctx;

// Trampoline: called by makecontext with coroutine index as arg
// Calls the coroutine's entry function, then deactivates it
static void __myp_coro_trampoline(int id) {
    if (id >= 0 && id < myp_coro_count && myp_coros[id].fn) {
        myp_coros[id].fn();
    }
    if (id >= 0 && id < myp_coro_count) {
        myp_coros[id].active = 0;
    }
}

int32_t __myp_coro_create(void) {
    if (myp_coro_count >= MYP_MAX_COROS) return -1;
    int idx = myp_coro_count;
    // Note: fn must be set BEFORE calling create, via __myp_coro_set_entry
    myp_coro_t* c = &myp_coros[idx];
    c->stack = (char*)malloc(MYP_CORO_STACK_SIZE);
    if (!c->stack) return -1;
    if (getcontext(&c->ctx) == -1) { free(c->stack); return -1; }
    c->ctx.uc_link = &myp_coro_sched_ctx;
    c->ctx.uc_stack.ss_sp = c->stack;
    c->ctx.uc_stack.ss_size = MYP_CORO_STACK_SIZE;
    makecontext(&c->ctx, (void(*)())__myp_coro_trampoline, 1, idx);
    c->active = 1;
    myp_coro_count++;
    return idx;
}

void __myp_coro_set_entry(int32_t handle, int32_t fn_ptr) {
    if (handle >= 0 && handle < MYP_MAX_COROS) {
        myp_coros[handle].fn = (void (*)(void))(uintptr_t)fn_ptr;
    }
}

void __myp_coro_yield(void) {
    if (myp_coro_current < 0) return;
    int saved = myp_coro_current;
    myp_coro_current = -1;
    swapcontext(&myp_coros[saved].ctx, &myp_coro_sched_ctx);
    myp_coro_current = saved;
}

int32_t __myp_coro_resume(int32_t handle) {
    if (handle < 0 || handle >= myp_coro_count) return -1;
    if (!myp_coros[handle].active) return -1;
    myp_coro_current = handle;
    swapcontext(&myp_coro_sched_ctx, &myp_coros[handle].ctx);
    return 0;
}

int32_t __myp_coro_is_active(int32_t handle) {
    if (handle < 0 || handle >= myp_coro_count) return 0;
    return myp_coros[handle].active ? 1 : 0;
}

void __myp_coro_destroy(int32_t handle) {
    if (handle >= 0 && handle < myp_coro_count && myp_coros[handle].stack) {
        myp_coros[handle].active = 0;
        free(myp_coros[handle].stack);
        myp_coros[handle].stack = NULL;
    }
}
