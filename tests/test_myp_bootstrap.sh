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
if ulimit -v >/dev/null 2>&1; then
    ulimit -v 8388608 || true   # 8GB
fi

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

# ---- 判定：myp_self2 与 myp_self3 前端 dump 字节一致 ----
for mode in tokens ast sema; do
    for f in "${DUMP_CORPUS[@]}"; do
        if [ ! -f "$f" ]; then bad "[$mode] 语料缺失: $f"; continue; fi
        o2="$TMP/d2_${mode}_$(basename "$f")"
        o3="$TMP/d3_${mode}_$(basename "$f")"
        if ! "$TMP/myp_self2" --frontend-dump "$mode" "$f" >"$o2" 2>/dev/null; then
            bad "[$mode] myp_self2 dump 失败: $f"; continue
        fi
        if ! "$TMP/myp_self3" --frontend-dump "$mode" "$f" >"$o3" 2>/dev/null; then
            bad "[$mode] myp_self3 dump 失败: $f"; continue
        fi
        if cmp -s "$o2" "$o3"; then
            ok "[$mode] self2/self3 字节一致: $f"
        else
            bad "[$mode] self2/self3 不一致: $f"
        fi
    done
done

# ---- 判定：myp_self2 与 myp_self3 产物运行输出/退出码一致 ----
for item in "${RUN_CORPUS[@]}"; do
    f="${item%%:*}"
    exp="${item##*:}"
    [ ! -f "$f" ] && { bad "运行语料缺失: $f"; continue; }
    if ! "$TMP/myp_self2" "$f" -o "$TMP/r2" --stdlib "$STDLIB" >/dev/null 2>&1; then
        bad "run: myp_self2 编译失败: $f"; continue
    fi
    if ! "$TMP/myp_self3" "$f" -o "$TMP/r3" --stdlib "$STDLIB" >/dev/null 2>&1; then
        bad "run: myp_self3 编译失败: $f"; continue
    fi
    "$TMP/r2"; s2=$?
    "$TMP/r3"; s3=$?
    if [ "$s2" = "$s3" ] && [ "$s2" = "$exp" ]; then
        ok "run: self2/self3 退出码=$exp 一致: $f"
    else
        bad "run: self2=$s2 self3=$s3 期望=$exp: $f"
    fi
done

say "myp-bootstrap PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
