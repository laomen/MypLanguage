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
    if (!s) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
    int32_t len = (int32_t)strlen(s);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) {
        char* r = (char*)malloc(1); r[0] = '\0'; return r;
    }
    int32_t new_len = end - start;
    char* result = (char*)malloc((size_t)new_len + 1);
    memcpy(result, s + start, (size_t)new_len);
    result[new_len] = '\0';
    return result;
}

char* myp_str_replace(const char* s, const char* old_str, const char* new_str) {
    if (!s || !old_str || !new_str) {
        if (!s) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
        return myp_strcat(s, "");
    }
    const char* pos = strstr(s, old_str);
    if (!pos) return myp_strcat(s, "");
    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    size_t prefix_len = (size_t)(pos - s);
    size_t total_len = strlen(s) - old_len + new_len;
    char* result = (char*)malloc(total_len + 1);
    memcpy(result, s, prefix_len);
    memcpy(result + prefix_len, new_str, new_len);
    strcpy(result + prefix_len + new_len, pos + old_len);
    return result;
}

char* myp_str_to_upper(const char* s) {
    if (!s) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
    char* r = myp_strcat(s, "");
    for (char* p = r; *p; p++) *p = (char)toupper((unsigned char)*p);
    return r;
}

char* myp_str_to_lower(const char* s) {
    if (!s) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
    char* r = myp_strcat(s, "");
    for (char* p = r; *p; p++) *p = (char)tolower((unsigned char)*p);
    return r;
}

char* myp_str_trim(const char* s) {
    if (!s) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
    while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    if (!*s) {
        char* r = (char*)malloc(1); r[0] = '\0'; return r;
    }
    const char* end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
    size_t len = (size_t)(end - s + 1);
    char* r = (char*)malloc(len + 1);
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
    if (!s || !delim || index < 0) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
    size_t dlen = strlen(delim);
    const char* p = s;
    int32_t cur = 0;
    while (*p) {
        const char* found = strstr(p, delim);
        if (found) {
            if (cur == index) {
                size_t len = (size_t)(found - p);
                char* r = (char*)malloc(len + 1);
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
    char* r = (char*)malloc(1); r[0] = '\0'; return r;
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
    char* key;        // NULL for array elements
    char* str_val;    // string value
    double num_val;   // numeric value (also used for int)
    int bool_val;     // boolean value
    int child_count;
    int child_cap;
    struct JsonNode** children;
} JsonNode;

static JsonNode* json_new_node(int type, const char* key) {
    JsonNode* n = (JsonNode*)calloc(1, sizeof(JsonNode));
    n->type = type;
    n->key = key ? strdup(key) : NULL;
    n->child_count = 0;
    n->child_cap = 0;
    n->children = NULL;
    return n;
}

static void json_add_child(JsonNode* parent, JsonNode* child) {
    if (parent->child_count >= parent->child_cap) {
        parent->child_cap = parent->child_cap ? parent->child_cap * 2 : 8;
        parent->children = (JsonNode**)realloc(parent->children,
            (size_t)parent->child_cap * sizeof(JsonNode*));
    }
    parent->children[parent->child_count++] = child;
}

static void json_free_node(JsonNode* n) {
    if (!n) return;
    free(n->key);
    free(n->str_val);
    for (int i = 0; i < n->child_count; i++)
        json_free_node(n->children[i]);
    free(n->children);
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
    // Check if it's an integer
    const char* check = p;
    int is_int = 1;
    while (check < end) {
        if (*check == '.' || *check == 'e' || *check == 'E') { is_int = 0; break; }
        check++;
    }
    if (is_int) {
        n = json_new_node(JSON_INT, NULL);
        n->num_val = d;
        n->str_val = strdup(p);
        // Trim to just the number part
        size_t nlen = (size_t)(end - p);
        char* tmp = (char*)malloc(nlen + 1);
        memcpy(tmp, p, nlen);
        tmp[nlen] = '\0';
        free(n->str_val);
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
    if (!path || index < 0) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
    DIR* dir = opendir(path);
    if (!dir) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
    struct dirent* entry;
    int32_t cur = 0;
    char* result = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (cur == index) {
            result = strdup(entry->d_name);
            break;
        }
        cur++;
    }
    closedir(dir);
    if (!result) { result = (char*)malloc(1); result[0] = '\0'; }
    return result;
}

char* myp_fs_dirname(const char* path) {
    if (!path) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
    const char* slash = strrchr(path, '/');
    if (!slash) { char* r = strdup("."); return r; }
    size_t len = (size_t)(slash - path);
    if (len == 0) { char* r = strdup("/"); return r; }
    char* r = (char*)malloc(len + 1);
    memcpy(r, path, len);
    r[len] = '\0';
    return r;
}

char* myp_fs_basename(const char* path) {
    if (!path) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
    const char* slash = strrchr(path, '/');
    if (!slash) return strdup(path);
    return strdup(slash + 1);
}

char* myp_fs_join(const char* dir, const char* file) {
    if (!dir && !file) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
    if (!dir) return strdup(file);
    if (!file) return strdup(dir);
    size_t dl = strlen(dir);
    size_t fl = strlen(file);
    int need_sep = (dl > 0 && dir[dl-1] != '/') ? 1 : 0;
    char* r = (char*)malloc(dl + (size_t)need_sep + fl + 1);
    memcpy(r, dir, dl);
    if (need_sep) r[dl] = '/';
    memcpy(r + dl + need_sep, file, fl + 1);
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
    if (!fmt) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
    time_t t = time(NULL);
    struct tm* tm_info = localtime(&t);
    if (!tm_info) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
    char buf[128];
    strftime(buf, sizeof(buf), fmt, tm_info);
    return strdup(buf);
}

char* myp_date_format_ms(int64_t ms, const char* fmt) {
    if (!fmt) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
    time_t t = myp_ms_to_time_t(ms);
    struct tm* tm_info = localtime(&t);
    if (!tm_info) { char* r = (char*)malloc(1); r[0] = '\0'; return r; }
    char buf[128];
    strftime(buf, sizeof(buf), fmt, tm_info);
    return strdup(buf);
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
    if (!a) return strdup(b);
    if (!b) return strdup(a);
    size_t la = strlen(a), lb = strlen(b);
    char* result = (char*)myp_alloc(la + lb + 1);
    if (result) {
        memcpy(result, a, la);
        memcpy(result + la, b, lb);
        result[la + lb] = '\0';
    }
    return result;
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
    volatile int running;
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
    myp_pool_start_ok = 1;
    pthread_cond_broadcast(&myp_pool_start_cond);

    for (int i = 0; i < n_threads; i++)
        pthread_create(&pool->threads[i], NULL, myp_pool_worker, (void*)(uintptr_t)i);

    return pool;
}

myp_pool_t* myp_pool_ensure_global(void) {
    if (!myp_global_pool)
        myp_global_pool = myp_pool_create(0);
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

    pool->done_count = 0;
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

    pool->total_chunks = actual_chunks;

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

    pool->work_available = 0;
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
