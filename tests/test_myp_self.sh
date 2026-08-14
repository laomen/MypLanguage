#!/bin/bash
# test_myp_self.sh — 全自举编译器（tools/selfhost）F0 骨架测试
#
# F0 范围（oracle + 骨架）验证：
#   1) tools/selfhost/src/main.myp 可编译（myp_self 构建）
#   2) myp_self CLI 骨架可用（--version / --help）
#   3) C++ oracle `mypc --frontend-dump {tokens,ast,sema}` 在采样语料上：
#      - 输出带版本头 + 模式 + Result 尾部（格式契约）
#      - 确定性：同一输入两次运行字节级一致
#      - 退出码：成功=0、有诊断错误=1
#   4) 负例（tests/negative）诊断段非空且 Result ok=0
#
# 用法：bash tests/test_myp_self.sh
#       MYPCC=/path/to/mypc  bash tests/test_myp_self.sh (默认 ./build/mypc)
#       MYP_SELF=/path/to/myp_self bash tests/test_myp_self.sh (默认 ./build/myp_self)
#
# 关联：tools/selfhost/format.md（契约）、roadmap.md F0、docs/self_hosting.md T5

set -u
MYPCC="${MYPCC:-./build/mypc}"
MYP_SELF="${MYP_SELF:-./build/myp_self}"
PASS=0
FAIL=0

say() { printf '%s\n' "$*"; }
ok()  { say "  PASS: $*"; PASS=$((PASS+1)); }
bad() { say "  FAIL: $*"; FAIL=$((FAIL+1)); }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---- 采样语料（F0-6）：覆盖纯函数/类/import/负例 ----
SAMPLES_OK=(
    "examples/hello.myp"
    "examples/showcase.myp"
    "BNCTDoseEngine/physics.myp"
    "tools/fmt/fmt.myp"
    "stdlib/env.myp"
)
SAMPLE_NEG="tests/negative/call_non_function.myp"

# ---- 1) myp_self 构建与 CLI 骨架 ----
if ! $MYPCC tools/selfhost/src/main.myp -o "$TMP/myp_self" --stdlib "$(dirname "$MYPCC")/../stdlib" >/dev/null 2>&1; then
    bad "tools/selfhost/src/main.myp 编译失败"
    say "myp-self PASS=0 FAIL=1"
    exit 1
fi
ok "tools/selfhost/src/main.myp 编译"

if ! "$TMP/myp_self" --version | grep -q "MYP Self-Hosted Compiler"; then
    bad "myp_self --version 输出不符"
else
    ok "myp_self --version"
fi

if ! "$TMP/myp_self" --help | grep -q "Usage: myp_self"; then
    bad "myp_self --help 输出不符"
else
    ok "myp_self --help"
fi

if "$TMP/myp_self" fmt --check tools/selfhost/src/main.myp >/dev/null 2>&1; then
    bad "myp_self fmt --check 期望返回 1（文件需要重排版）"
else
    ok "myp_self fmt --check"
fi

out_run="$TMP/myp_self_run.txt"
if "$TMP/myp_self" run examples/hello.myp >"$out_run" 2>&1; then
    grep -q "hello" "$out_run" && ok "myp_self run" || bad "myp_self run 输出不符"
else
    status=$?
    if [ "$status" -eq 42 ]; then
        grep -q "hello" "$out_run" && ok "myp_self run" || bad "myp_self run 输出不符"
    else
        bad "myp_self run 退出码应透传为 42，实际 $status"
    fi
fi

# ---- 2) C++ oracle：三模式 × 采样语料 ----
for mode in tokens ast sema; do
    for f in "${SAMPLES_OK[@]}"; do
        if [ ! -f "$f" ]; then
            bad "[$mode] 语料缺失: $f"
            continue
        fi
        out="$TMP/dump_${mode}_$(basename "$f").txt"
        if ! $MYPCC --frontend-dump "$mode" "$f" >"$out" 2>/dev/null; then
            bad "[$mode] $f 退出码≠0"
            continue
        fi
        # 契约头/尾
        head -1 "$out" | grep -q "^MYP_FRONTEND_DUMP v1$" \
            && ok "[$mode] $f 版本头" || bad "[$mode] $f 版本头缺失"
        grep -q "^(Mode $mode)$" "$out" \
            && ok "[$mode] $f 模式标记" || bad "[$mode] $f 模式标记缺失"
        grep -q "^(Result ok=1 " "$out" \
            && ok "[$mode] $f Result ok=1" || bad "[$mode] $f Result 异常"
        # 确定性
        if $MYPCC --frontend-dump "$mode" "$f" 2>/dev/null | cmp -s - "$out"; then
            ok "[$mode] $f 确定性一致"
        else
            bad "[$mode] $f 两次运行不一致（非确定性！）"
        fi
    done
done

# ---- 3) 负例：诊断非空 + Result ok=0 + 退出码 1 ----
if [ -f "$SAMPLE_NEG" ]; then
    out="$TMP/dump_neg.txt"
    if $MYPCC --frontend-dump sema "$SAMPLE_NEG" >"$out" 2>/dev/null; then
        bad "[sema] $SAMPLE_NEG 期望退出码 1，实际 0"
    else
        ok "[sema] $SAMPLE_NEG 退出码 1"
    fi
    grep -q "^(Diagnostics)$" "$out" && ok "[sema] 负例 Diagnostics 段存在" \
        || bad "[sema] 负例 Diagnostics 段缺失"
    grep -q "(Diag " "$out" && ok "[sema] 负例有诊断" \
        || bad "[sema] 负例无诊断条目"
    grep -q "^(Result ok=0 " "$out" && ok "[sema] 负例 Result ok=0" \
        || bad "[sema] 负例 Result ok 非 0"
else
    bad "负例缺失: $SAMPLE_NEG"
fi

# ---- 4) F1：myp_self tokens vs mypc oracle 字节对拍（含边界关键字/错误路径）----
TOK_SAMPLES=(
    "examples/hello.myp"
    "examples/showcase.myp"
    "BNCTDoseEngine/physics.myp"
    "tools/fmt/fmt.myp"
    "stdlib/env.myp"
    "tests/bitfield/test.myp"
    "tests/bitvector/test.myp"
    "tests/@test/nonlocal.myp"
    "tests/@test/man_or_boy.myp"
    "tools/fmt/lexer.myp"
)
for f in "${TOK_SAMPLES[@]}"; do
    if [ ! -f "$f" ]; then bad "[tokens对拍] 语料缺失: $f"; continue; fi
    if $MYPCC --frontend-dump tokens "$f" >"$TMP/cpp_tok.txt" 2>/dev/null \
        && "$TMP/myp_self" --frontend-dump tokens "$f" >"$TMP/myp_tok.txt" 2>/dev/null; then
        if cmp -s "$TMP/cpp_tok.txt" "$TMP/myp_tok.txt"; then
            ok "[tokens对拍] $f 字节一致"
        else
            bad "[tokens对拍] $f 不一致"
        fi
    else
        bad "[tokens对拍] $f 执行失败"
    fi
done

# 词法错误路径（未闭合串/注释、非法字符、未知转义）诊断对拍（退出码 1 属预期）
LEX_ERR="$TMP/lex_err.myp"
printf 'string s = "unterminated\nint x = '\''A\nint y = /* unclosed\nint z = '\''\\q'\''\n' > "$LEX_ERR"
$MYPCC --frontend-dump tokens "$LEX_ERR" >"$TMP/cpp_tok.txt" 2>/dev/null
"$TMP/myp_self" --frontend-dump tokens "$LEX_ERR" >"$TMP/myp_tok.txt" 2>/dev/null
if cmp -s "$TMP/cpp_tok.txt" "$TMP/myp_tok.txt"; then
    ok "[tokens对拍] 词法错误诊断一致"
else
    bad "[tokens对拍] 词法错误诊断不一致"
fi

# ---- 5) F4：myp_self sema vs mypc oracle 字节对拍（正语料全绿样本）----
# 覆盖泛型实例/泛型静态方法/lambda 捕获/GPU 发散告警/宏调用等曾出过差异的路径。
SEMA_SAMPLES=(
    "examples/hello.myp"
    "examples/showcase.myp"
    "BNCTDoseEngine/physics.myp"
    "tools/fmt/fmt.myp"
    "stdlib/env.myp"
    "tests/mega/test.myp"
    "bench/conv3d_gen_main.myp"
    "bench/gpu_reduce_scan.myp"
    "tests/macro/test.myp"
    "tools/selfhost/src/diag.myp"
    "tools/selfhost/src/token.myp"
    "tests/@test/function.myp"
    "tests/@test/man_or_boy.myp"
    "tests/@test/nonlocal.myp"
)
for f in "${SEMA_SAMPLES[@]}"; do
    if [ ! -f "$f" ]; then bad "[sema对拍] 语料缺失: $f"; continue; fi
    c_rc=0; m_rc=0
    $MYPCC --frontend-dump sema "$f" >"$TMP/cpp_sema.txt" 2>/dev/null || c_rc=$?
    "$TMP/myp_self" --frontend-dump sema "$f" >"$TMP/myp_sema.txt" 2>/dev/null || m_rc=$?
    if [ "$c_rc" -eq "$m_rc" ] && cmp -s "$TMP/cpp_sema.txt" "$TMP/myp_sema.txt"; then
        ok "[sema对拍] $f 字节一致"
    else
        bad "[sema对拍] $f 不一致 (cpp_rc=$c_rc myp_rc=$m_rc)"
    fi
done

# ---- 汇总 ----
say "myp-self PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
