// json_bridge.c — JSON 解析/查询 FFI（从 runtime.c 分离，2026-08-20）
// ---------------------------------------------------------------------------
// JSON 只在 import json 时按需链接（bridge 符号匹配机制，同 sdl/ttf）。
// 依赖 runtime 的 myp_strdup（runtime.h 声明）。
// ---------------------------------------------------------------------------
#include "mylang/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
