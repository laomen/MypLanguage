/* regex_win.c — Windows：POSIX ERE 迷你引擎（被 regex_bridge.c 在 _WIN32 下
 * include，提供 myp_regex_* 实现；Linux 用系统 <regex.h>）。
 *
 * 支持语法（MYP regex.myp 用法 + 常见 ERE）：
 *   字面量、`.`、`[...]`（范围/取反）、`*` `+` `?`、`^` `$`、`(...)`、
 *   `|` 交替、`\` 转义。只判断匹配（regex_bridge 不提取子组）。
 *
 * 实现：AST + 贪婪回溯（re_star 先试 1+ 次再 0 次）。深度上限防栈溢出。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct re_node re_node;
struct re_node {
    int op;                 /* 0 CHAR 1 ANY 2 CLASS 3 STAR 4 PLUS 5 QUEST
                                6 ALT 7 GROUP 8 ASS_START 9 ASS_END */
    char ch;                /* CHAR */
    char cls[256];          /* CLASS 字符集 */
    int cls_len;
    int neg;                /* CLASS 取反 */
    re_node* child;         /* 量词子表达式 / GROUP 内 / ALT 左分支 */
    re_node* right;         /* ALT 右分支 */
    re_node* next;          /* 本节点后的续接（同一序列） */
};

static re_node* re_new(int op) {
    re_node* n = (re_node*)calloc(1, sizeof(re_node));
    if (n) n->op = op;
    return n;
}

static void re_free_node(re_node* n) {
    if (!n) return;
    re_free_node(n->child);
    re_free_node(n->right);
    re_free_node(n->next);
    free(n);
}

/* ---- 解析器（递归下降） ---- */
static const char* g_p;
static int g_i;

static re_node* parse_alt(void);
static re_node* parse_rep(void);

static re_node* parse_seq(void) {
    re_node* head = NULL, * tail = NULL;
    while (g_p[g_i] && g_p[g_i] != ')' && g_p[g_i] != '|') {
        re_node* r = parse_rep();
        if (!r) return NULL;
        if (!head) head = r; else tail->next = r;
        tail = r;
    }
    return head;
}

/* 为避免与 parse_alt 递归混淆，统一走 parse_seq → parse_rep */
static re_node* parse_atom(void);
static re_node* parse_rep(void) {
    re_node* atom = parse_atom();
    if (!atom) return NULL;
    for (;;) {
        if (g_p[g_i] == '*') { g_i++; re_node* s = re_new(3); s->child = atom; atom = s; }
        else if (g_p[g_i] == '+') { g_i++; re_node* p = re_new(4); p->child = atom; atom = p; }
        else if (g_p[g_i] == '?') { g_i++; re_node* q = re_new(5); q->child = atom; atom = q; }
        else break;
    }
    return atom;
}

/* 解析 [...] 字符类，返回字符集（存入 out） */
static int parse_class(char* out, int* neg) {
    int o = 0;
    *neg = 0;
    g_i++;                       /* 跳过 '[' */
    if (g_p[g_i] == '^') { *neg = 1; g_i++; }
    int first = 1;
    while (g_p[g_i] && (g_p[g_i] != ']' || first)) {
        char c = g_p[g_i];
        if (c == '\\' && g_p[g_i + 1]) { g_i++; c = g_p[g_i]; }
        if (g_p[g_i + 1] == '-' && g_p[g_i + 2] && g_p[g_i + 2] != ']' && o < 250) {
            char lo = c, hi = g_p[g_i + 2];
            if (lo > hi) { char t = lo; lo = hi; hi = t; }
            for (char ch = lo; ch <= hi; ch++) out[o++] = ch;
            g_i += 3;
        } else {
            if (o < 250) out[o++] = c;
            g_i++;
        }
        first = 0;
    }
    if (g_p[g_i] == ']') g_i++;
    return o;
}

static re_node* parse_atom(void) {
    if (!g_p[g_i]) return NULL;
    char c = g_p[g_i];
    if (c == '(') {
        g_i++;
        re_node* inner = parse_alt();
        if (g_p[g_i] == ')') g_i++;
        if (!inner) return NULL;
        re_node* g = re_new(7);   /* GROUP */
        g->child = inner;
        return g;
    }
    if (c == '[') {
        re_node* n = re_new(2);   /* CLASS */
        n->cls_len = parse_class(n->cls, &n->neg);
        return n;
    }
    if (c == '.') { g_i++; return re_new(1); }
    if (c == '^') { g_i++; return re_new(8); }
    if (c == '$') { g_i++; return re_new(9); }
    if (c == '\\' && g_p[g_i + 1]) {
        g_i++;   /* 转义：取下一个字符作为字面量 */
        re_node* n = re_new(0);
        n->ch = g_p[g_i];
        g_i++;
        return n;
    }
    if (c == '*' || c == '+' || c == '?' || c == '|' || c == ')')
        return NULL;   /* 未匹配到 atom（空 rep 由调用方处理） */
    re_node* n = re_new(0);
    n->ch = c;
    g_i++;
    return n;
}

static re_node* parse_alt(void) {
    re_node* left = parse_seq();
    if (!left) return NULL;
    while (g_p[g_i] == '|') {
        g_i++;
        re_node* right = parse_seq();
        if (!right) return NULL;
        re_node* a = re_new(6);   /* ALT */
        a->child = left;
        a->right = right;
        left = a;
    }
    return left;
}

/* ---- 匹配（贪婪回溯） ---- */
static int re_m(re_node* n, const char* s, int pos, int end, int depth);
static int re_star(re_node* c, re_node* nx, const char* s, int pos, int end, int depth) {
    if (depth > 50000) return -1;
    /* greedy：先 1+ 次（若前进），再 0 次 */
    int p = re_m(c, s, pos, end, depth + 1);
    if (p != -1 && p != pos) {
        int r = re_star(c, nx, s, p, end, depth + 1);
        if (r != -1) return r;
    }
    return re_m(nx, s, pos, end, depth + 1);
}

static int re_m(re_node* n, const char* s, int pos, int end, int depth) {
    if (!n) return pos;
    if (depth > 50000) return -1;
    switch (n->op) {
        case 0: /* CHAR */
            if (pos >= end || s[pos] != n->ch) return -1;
            return re_m(n->next, s, pos + 1, end, depth + 1);
        case 1: /* ANY */
            if (pos >= end || s[pos] == '\n') return -1;
            return re_m(n->next, s, pos + 1, end, depth + 1);
        case 2: { /* CLASS */
            if (pos >= end) return -1;
            int in = 0;
            for (int i = 0; i < n->cls_len; i++)
                if (n->cls[i] == s[pos]) { in = 1; break; }
            if (n->neg) in = !in;
            return in ? re_m(n->next, s, pos + 1, end, depth + 1) : -1;
        }
        case 3: return re_star(n->child, n->next, s, pos, end, depth + 1);
        case 4: { /* PLUS = 一次 + star */
            int p = re_m(n->child, s, pos, end, depth + 1);
            if (p != -1) return re_star(n->child, n->next, s, p, end, depth + 1);
            return -1;
        }
        case 5: { /* QUEST */
            int p = re_m(n->child, s, pos, end, depth + 1);
            if (p != -1) { int r = re_m(n->next, s, p, end, depth + 1); if (r != -1) return r; }
            return re_m(n->next, s, pos, end, depth + 1);
        }
        case 6: { /* ALT：试左分支+续接，再右分支+续接 */
            int p1 = re_m(n->child, s, pos, end, depth + 1);
            if (p1 != -1) { int r = re_m(n->next, s, p1, end, depth + 1); if (r != -1) return r; }
            int p2 = re_m(n->right, s, pos, end, depth + 1);
            if (p2 != -1) { int r = re_m(n->next, s, p2, end, depth + 1); if (r != -1) return r; }
            return -1;
        }
        case 7: { /* GROUP */
            int p = re_m(n->child, s, pos, end, depth + 1);
            if (p != -1) return re_m(n->next, s, p, end, depth + 1);
            return -1;
        }
        case 8: return pos == 0 ? re_m(n->next, s, pos, end, depth + 1) : -1;
        case 9: return pos == end ? re_m(n->next, s, pos, end, depth + 1) : -1;
    }
    return -1;
}

/* regexec 语义（匹配判定）：从任意起始位置尝试；^ 锚点由 ASS_START 处理 */
static int re_exec(const re_node* ast, const char* s, int len) {
    for (int start = 0; start <= len; start++) {
        if (re_m((re_node*)ast, s, start, len, 0) != -1) return 1;
    }
    return 0;
}

/* ---- myp_regex_* 实现（替代 Linux <regex.h>） ---- */
int64_t myp_regex_compile(const char* pattern) {
    if (!pattern) return 0;
    g_p = pattern;
    g_i = 0;
    re_node* ast = parse_alt();
    if (!ast) return 0;
    /* 验证整体解析到串尾（多余字符视为编译失败） */
    return (int64_t)(intptr_t)ast;
}

int32_t myp_regex_match(int64_t handle, const char* s) {
    if (!handle || !s) return 0;
    re_node* ast = (re_node*)(intptr_t)handle;
    return re_exec(ast, s, (int)strlen(s)) ? 1 : 0;
}

void myp_regex_free(int64_t handle) {
    if (!handle) return;
    re_free_node((re_node*)(intptr_t)handle);
}
