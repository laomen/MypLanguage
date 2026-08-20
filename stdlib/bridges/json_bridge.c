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

// ==================== 编辑支持（JsonEditor 控件用） ====================
// 在只读查询之上补充：遍历（子节点计数/键名/标量文本）、原地修改（改值/加子/
// 删除）、序列化（美化打印）。全部保持 M8 约定：返回 string 一律 myp_strdup。

// 把用户输入的原始文本解析为标量节点（数字/布尔/null/字符串）。不做容器解析
// （编辑器改值只针对标量；容器用 add_child 逐步构建）。
static JsonNode* json_parse_scalar_text(const char* raw) {
    if (!raw) return json_new_node(JSON_NULL, NULL);
    const char* p = raw;
    if (*p == 't' && strncmp(p, "true", 4) == 0 && p[4] == '\0') {
        JsonNode* n = json_new_node(JSON_BOOL, NULL); n->bool_val = 1; return n;
    }
    if (*p == 'f' && strncmp(p, "false", 5) == 0 && p[5] == '\0') {
        JsonNode* n = json_new_node(JSON_BOOL, NULL); n->bool_val = 0; return n;
    }
    if (*p == 'n' && strncmp(p, "null", 4) == 0 && p[4] == '\0') {
        return json_new_node(JSON_NULL, NULL);
    }
    // 数字：strtod 必须消费整个串
    char* end = NULL;
    double d = strtod(p, &end);
    if (end != p && *end == '\0') {
        int is_int = 1;
        for (const char* q = p; q < end; q++)
            if (*q == '.' || *q == 'e' || *q == 'E') { is_int = 0; break; }
        JsonNode* n = json_new_node(is_int ? JSON_INT : JSON_DOUBLE, NULL);
        n->num_val = d;
        n->str_val = strdup(p);
        return n;
    }
    // 其余一律当字符串
    JsonNode* n = json_new_node(JSON_STRING, NULL);
    n->str_val = strdup(raw);
    return n;
}

// 在 parent 下按 token 找子节点（对象按键名 / 数组按下标）
static JsonNode* json_find_child(JsonNode* parent, const char* token) {
    if (!parent) return NULL;
    if (parent->type == JSON_OBJECT) {
        for (int i = 0; i < parent->child_count; i++)
            if (parent->children[i]->key && strcmp(parent->children[i]->key, token) == 0)
                return parent->children[i];
        return NULL;
    }
    if (parent->type == JSON_ARRAY) {
        char* end = NULL;
        long idx = strtol(token, &end, 10);
        if (end == token || idx < 0 || idx >= parent->child_count) return NULL;
        return parent->children[idx];
    }
    return NULL;
}

// ---- 遍历 ----
int32_t myp_json_child_count(int64_t handle, const char* path) {
    JsonNode* root = (JsonNode*)(intptr_t)handle;
    JsonNode* n = json_resolve_path(root, path);
    if (!n || (n->type != JSON_OBJECT && n->type != JSON_ARRAY)) return 0;
    return n->child_count;
}

const char* myp_json_child_key(int64_t handle, const char* path, int32_t index) {
    JsonNode* root = (JsonNode*)(intptr_t)handle;
    JsonNode* n = json_resolve_path(root, path);
    if (!n || (n->type != JSON_OBJECT && n->type != JSON_ARRAY)) return myp_strdup("");
    if (index < 0 || index >= n->child_count) return myp_strdup("");
    JsonNode* c = n->children[index];
    if (c->key) return myp_strdup(c->key);
    return myp_strdup("");
}

// 标量节点 → 显示/编辑用文本（字符串原样去引号；数字用词法原文；bool/null 关键字）
const char* myp_json_scalar(int64_t handle, const char* path) {
    JsonNode* root = (JsonNode*)(intptr_t)handle;
    JsonNode* n = json_resolve_path(root, path);
    if (!n) return myp_strdup("");
    switch (n->type) {
        case JSON_STRING: return myp_strdup(n->str_val ? n->str_val : "");
        case JSON_INT:
        case JSON_DOUBLE: return myp_strdup(n->str_val ? n->str_val : "0");
        case JSON_BOOL:   return myp_strdup(n->bool_val ? "true" : "false");
        case JSON_NULL:   return myp_strdup("null");
        default:          return myp_strdup("");   // 容器
    }
}

// ---- 修改 ----
// 把 path 指向的标量节点原地改成 raw 解析出的标量值。返回 1=成功。
int32_t myp_json_set_value(int64_t handle, const char* path, const char* raw) {
    JsonNode* root = (JsonNode*)(intptr_t)handle;
    if (!root || !path || !*path) return 0;
    char pbuf[256];
    JsonNode* parent = NULL;
    const char* tok = path;
    char* lastdot = strrchr(pbuf, '.');
    // 先拷贝再定位
    strncpy(pbuf, path, sizeof(pbuf) - 1); pbuf[sizeof(pbuf) - 1] = '\0';
    lastdot = strrchr(pbuf, '.');
    if (lastdot) {
        *lastdot = '\0';
        tok = lastdot + 1;
        parent = json_resolve_path(root, pbuf);
    } else {
        parent = root;
        tok = pbuf;
    }
    if (!parent) return 0;
    JsonNode* target = json_find_child(parent, tok);
    if (!target) return 0;
    if (target->type == JSON_OBJECT || target->type == JSON_ARRAY) return 0;
    JsonNode* val = json_parse_scalar_text(raw);
    if (!val) return 0;
    free(target->str_val);
    target->type = val->type;
    target->str_val = val->str_val;   // 转移所有权
    target->num_val = val->num_val;
    target->bool_val = val->bool_val;
    free(val);
    return 1;
}

// 给容器 path 追加一个子节点：对象加 key=raw，数组追加 raw。raw 优先按完整
// JSON 值解析（对象/数组/标量——设计器注入控件节点用），裸文本（非 { [ 开头）
// 回退按标量解析（数字/布尔/null/字符串，JsonEditor 用）。返回 1=成功。
int32_t myp_json_add_child(int64_t handle, const char* path, const char* key, const char* raw) {
    JsonNode* root = (JsonNode*)(intptr_t)handle;
    JsonNode* parent = json_resolve_path(root, path);
    if (!parent || (parent->type != JSON_OBJECT && parent->type != JSON_ARRAY)) return 0;
    JsonNode* val = NULL;
    if (raw && (raw[0] == '{' || raw[0] == '[')) {
        const char* p0 = raw;
        val = json_parse_value(&p0);
        if (!val || *p0 != '\0') {
            if (val) json_free_node(val);
            val = NULL;
        }
    }
    if (!val) val = json_parse_scalar_text(raw);
    if (!val) return 0;
    if (parent->type == JSON_OBJECT) {
        val->key = strdup(key ? key : "newKey");
    }
    json_add_child(parent, val);
    return 1;
}

// 删除 path 指向的子节点（根不可删）。返回 1=成功。
int32_t myp_json_remove(int64_t handle, const char* path) {
    JsonNode* root = (JsonNode*)(intptr_t)handle;
    if (!root || !path || !*path) return 0;
    char pbuf[256];
    strncpy(pbuf, path, sizeof(pbuf) - 1); pbuf[sizeof(pbuf) - 1] = '\0';
    char* lastdot = strrchr(pbuf, '.');
    JsonNode* parent = NULL;
    const char* tok = pbuf;
    if (lastdot) {
        *lastdot = '\0';
        tok = lastdot + 1;
        parent = json_resolve_path(root, pbuf);
    } else {
        parent = root;
        tok = pbuf;
    }
    if (!parent) return 0;
    int found = -1;
    if (parent->type == JSON_OBJECT) {
        for (int i = 0; i < parent->child_count; i++)
            if (parent->children[i]->key && strcmp(parent->children[i]->key, tok) == 0) { found = i; break; }
    } else if (parent->type == JSON_ARRAY) {
        char* end = NULL;
        long idx = strtol(tok, &end, 10);
        if (end != tok && idx >= 0 && idx < parent->child_count) found = (int)idx;
    } else return 0;
    if (found < 0) return 0;
    JsonNode* victim = parent->children[found];
    for (int i = found; i < parent->child_count - 1; i++)
        parent->children[i] = parent->children[i + 1];
    parent->child_count--;
    json_free_node(victim);
    return 1;
}

// ---- 序列化（美化打印，2 空格缩进） ----
typedef struct { char* buf; size_t len; size_t cap; } JsonSb;

static void jb_init(JsonSb* b) { b->buf = NULL; b->len = 0; b->cap = 0; }
static void jb_ensure(JsonSb* b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + extra + 1) nc *= 2;
    b->buf = (char*)realloc(b->buf, nc);
    b->cap = nc;
}
static void jb_str(JsonSb* b, const char* s) {
    if (!s) return;
    size_t l = strlen(s);
    jb_ensure(b, l);
    memcpy(b->buf + b->len, s, l);
    b->len += l;
    b->buf[b->len] = '\0';
}
static void jb_char(JsonSb* b, char c) {
    jb_ensure(b, 1);
    b->buf[b->len++] = c;
    b->buf[b->len] = '\0';
}
static void jb_indent(JsonSb* b, int n) {
    for (int i = 0; i < n; i++) jb_str(b, "  ");
}

static void jb_escaped(JsonSb* b, const char* s) {
    jb_char(b, '"');
    if (s) for (const char* p = s; *p; p++) {
        switch (*p) {
            case '"':  jb_str(b, "\\\""); break;
            case '\\': jb_str(b, "\\\\"); break;
            case '\n': jb_str(b, "\\n"); break;
            case '\t': jb_str(b, "\\t"); break;
            case '\r': jb_str(b, "\\r"); break;
            default:
                if ((unsigned char)*p < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned char)*p);
                    jb_str(b, tmp);
                } else jb_char(b, *p);
        }
    }
    jb_char(b, '"');
}

static void json_serialize_node(JsonNode* n, int indent, JsonSb* b) {
    if (!n) { jb_str(b, "null"); return; }
    switch (n->type) {
        case JSON_NULL:   jb_str(b, "null"); break;
        case JSON_BOOL:   jb_str(b, n->bool_val ? "true" : "false"); break;
        case JSON_INT:
        case JSON_DOUBLE: jb_str(b, n->str_val ? n->str_val : "0"); break;
        case JSON_STRING: jb_escaped(b, n->str_val ? n->str_val : ""); break;
        case JSON_ARRAY: {
            jb_char(b, '[');
            if (n->child_count == 0) { jb_char(b, ']'); break; }
            jb_char(b, '\n');
            for (int i = 0; i < n->child_count; i++) {
                jb_indent(b, indent + 1);
                json_serialize_node(n->children[i], indent + 1, b);
                if (i < n->child_count - 1) jb_char(b, ',');
                jb_char(b, '\n');
            }
            jb_indent(b, indent);
            jb_char(b, ']');
            break;
        }
        case JSON_OBJECT: {
            jb_char(b, '{');
            if (n->child_count == 0) { jb_char(b, '}'); break; }
            jb_char(b, '\n');
            for (int i = 0; i < n->child_count; i++) {
                jb_indent(b, indent + 1);
                jb_escaped(b, n->children[i]->key ? n->children[i]->key : "");
                jb_str(b, ": ");
                json_serialize_node(n->children[i], indent + 1, b);
                if (i < n->child_count - 1) jb_char(b, ',');
                jb_char(b, '\n');
            }
            jb_indent(b, indent);
            jb_char(b, '}');
            break;
        }
    }
}

const char* myp_json_serialize(int64_t handle) {
    JsonNode* root = (JsonNode*)(intptr_t)handle;
    if (!root) return myp_strdup("null");
    JsonSb b;
    jb_init(&b);
    json_serialize_node(root, 0, &b);
    const char* out = myp_strdup(b.buf ? b.buf : "");
    free(b.buf);
    return out;
}
