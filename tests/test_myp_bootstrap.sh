#!/bin/bash
# test_myp_bootstrap.sh — 全自举编译器两级自举验证（design.md D8 / roadmap H1）
#
# 验证自举成立：
#   stage0: C++ mypc 编译 tools/selfhost/src/main.myp → myp_self   （第一代）
#   stage1: myp_self 编译同样源码 → myp_self2                    （自编译）
#   stage2: myp_self2 编译同样源码 → myp_self3                    （再自编译）
#   判定：myp_self2 与 myp_self3 对同一语料行为一致
#         （--frontend-dump 三模式字节一致 + 产物运行输出/退出码一致）
#
# 用法：bash tests/test_myp_bootstrap.sh
#       MYPCC=/path/to/mypc  bash tests/test_myp_bootstrap.sh（默认 ./build/mypc）
#
# 关联：roadmap.md H1、design.md D8、docs/self_hosting.md

set -u
# 跨平台移植层：Windows Git Bash 无 ulimit -v，myp_guard_ulimit 自动跳过
source "$(dirname "$0")/lib/portable.sh"
MYPCC="${MYPCC:-./build/mypc}"
STDLIB="$(dirname "$MYPCC")/../stdlib"
PASS=0
FAIL=0

say() { printf '%s\n' "$*"; }
ok()  { say "  PASS: $*"; PASS=$((PASS+1)); }
bad() { say "  FAIL: $*"; FAIL=$((FAIL+1)); }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 防 OOM：自编译生成的 .ll 很大，llc/gcc 内存占用高（用户环境曾爆内存）。
# Windows（Git Bash）无 ulimit -v，由 myp_guard_ulimit 静默跳过。
myp_guard_ulimit 8388608

SRC="tools/selfhost/src/main.myp"

# ---- 前端 dump 对拍语料（覆盖纯函数/类/import/泛型）----
DUMP_CORPUS=(
    "examples/hello.myp"
    "examples/fib.myp"
    "tools/selfhost/src/main.myp"
)

# ---- 运行对拍语料（当前自举子集可编译运行：int 返回值样本）----
RUN_CORPUS=(
    "examples/hello.myp:42"
    "examples/fib.myp:55"
    "examples/simple.myp:0"
)

# ---- stage0：mypc 编译 myp_self ----
if ! "$MYPCC" "$SRC" -o "$TMP/myp_self" --stdlib "$STDLIB" >/dev/null 2>&1; then
    bad "stage0：mypc 编译 myp_self 失败"
    say "myp-bootstrap PASS=0 FAIL=1"
    exit 1
fi
ok "stage0：mypc → myp_self"

# ---- stage1：myp_self 自编译 → myp_self2 ----
if ! "$TMP/myp_self" "$SRC" -o "$TMP/myp_self2" --stdlib "$STDLIB" >/dev/null 2>&1; then
    bad "stage1：myp_self 自编译失败"
    say "myp-bootstrap PASS=$PASS FAIL=$((FAIL+1))"
    exit 1
fi
ok "stage1：myp_self → myp_self2"

# ---- stage2：myp_self2 自编译 → myp_self3 ----
if ! "$TMP/myp_self2" "$SRC" -o "$TMP/myp_self3" --stdlib "$STDLIB" >/dev/null 2>&1; then
    bad "stage2：myp_self2 自编译失败"
    say "myp-bootstrap PASS=$PASS FAIL=$((FAIL+1))"
    exit 1
fi
ok "stage2：myp_self2 → myp_self3"

# ---- 判定：stage0/1/2 前端 dump 三方字节一致 ----
for mode in tokens ast sema; do
    for f in "${DUMP_CORPUS[@]}"; do
        if [ ! -f "$f" ]; then bad "[$mode] 语料缺失: $f"; continue; fi
        o0="$TMP/d0_${mode}_$(basename "$f")"
        o1="$TMP/d1_${mode}_$(basename "$f")"
        o2="$TMP/d2_${mode}_$(basename "$f")"
        if ! "$TMP/myp_self"  --frontend-dump "$mode" "$f" >"$o0" 2>/dev/null; then
            bad "[$mode] myp_self  dump 失败: $f"; continue
        fi
        if ! "$TMP/myp_self2" --frontend-dump "$mode" "$f" >"$o1" 2>/dev/null; then
            bad "[$mode] myp_self2 dump 失败: $f"; continue
        fi
        if ! "$TMP/myp_self3" --frontend-dump "$mode" "$f" >"$o2" 2>/dev/null; then
            bad "[$mode] myp_self3 dump 失败: $f"; continue
        fi
        if cmp -s "$o0" "$o1" && cmp -s "$o1" "$o2"; then
            ok "[$mode] stage0/1/2 三方字节一致: $f"
        else
            bad "[$mode] stage 间不一致: $f"
            cmp -s "$o0" "$o1" || diff "$o0" "$o1" | head -3
        fi
    done
done

# ---- 判定：stage0/1/2 产物运行输出/退出码一致 ----
STAGES=("$TMP/myp_self" "$TMP/myp_self2" "$TMP/myp_self3")
for item in "${RUN_CORPUS[@]}"; do
    f="${item%%:*}"
    exp="${item##*:}"
    [ ! -f "$f" ] && { bad "运行语料缺失: $f"; continue; }
    okall=1
    for i in 0 1 2; do
        if ! "${STAGES[$i]}" "$f" -o "$TMP/r$i" --stdlib "$STDLIB" >/dev/null 2>&1; then
            bad "run: stage$i 编译失败: $f"; okall=0; break
        fi
    done
    [ "$okall" = 0 ] && continue
    "$TMP/r0"; s0=$?
    "$TMP/r1"; s1=$?
    "$TMP/r2"; s2=$?
    if [ "$s0" = "$s1" ] && [ "$s1" = "$s2" ] && [ "$s0" = "$exp" ]; then
        ok "run: stage0/1/2 退出码=$exp 一致: $f"
    else
        bad "run: stage0=$s0 stage1=$s1 stage2=$s2 期望=$exp: $f"
    fi
done

# ---- 不动点信息（H1 判据是行为一致，非二进制相等；仅供观测）----
if cmp -s "$TMP/myp_self2" "$TMP/myp_self3"; then
    ok "不动点：myp_self2 == myp_self3 字节相同（md5 $(md5sum "$TMP/myp_self2" | cut -d' ' -f1)）"
else
    say "  note: myp_self2 与 myp_self3 非字节相同（H1 不要求二进制相等）"
fi

say "myp-bootstrap PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
