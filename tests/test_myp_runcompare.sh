#!/usr/bin/env bash
# test_myp_runcompare.sh — 语料级 run-compare 差异测试（myp_self vs mypc）
#
# 目的：定位 runtime 正确性 bug——代码能编译、但 myp_self 产物运行输出/退出码
# 与 mypc 不一致（如 struct 数组元素大小导致的越界写段错误，编译覆盖率测不出）。
# 把可运行语料用两个编译器各编译一次、各跑一遍，diff stdout+stderr+退出码。
#
# 用法：
#   bash tests/test_myp_runcompare.sh
#   MYPCC=/path/to/mypc MYP_SELF=/path/to/myp_self bash tests/test_myp_runcompare.sh
#   RUN_TIMEOUT=30 bash tests/test_myp_runcompare.sh   # 单程序超时秒（默认 20）
#
# 输出分类：
#   PASS  —— mypc 与 myp_self 产物运行输出+退出码一致
#   FAIL  —— 不一致（myp_self runtime 正确性 bug，需修）
#   GAP   —— mypc 能编译但 myp_self 编译失败（缺特性或编译期 bug）
#   SKIP  —— mypc 也编译失败（坏语料/遗留样例）或库文件（无 main/@startup）
#
# 关联：design.md §7 验收 2、roadmap.md「后续计划 Phase 1」。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MYPCC="${MYPCC:-$ROOT/build/mypc}"
SELF="${MYP_SELF:-$ROOT/build/myp_self}"
STDLIB="${MYP_STDLIB:-$ROOT/stdlib}"
TIMEOUT="${RUN_TIMEOUT:-20}"

PASS=0; FAIL=0; GAP=0; SKIP=0
say() { printf '%s\n' "$*"; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 归一化计时输出（基准程序打印耗时/倍率，随运行波动）：ms/best/x倍率/轮/次/ns
# 一律塌缩数字为 N；verify 行保留精确值（正确性锚点，必须逐字一致）。
normalize() {
    sed -E \
        -e 's/ms [0-9]+/ms N/g' \
        -e 's/best [0-9]+/best N/g' \
        -e 's/x[0-9.]+/xN/g' \
        -e 's/[0-9]+ ns\//N ns\//g' \
        -e 's/[0-9]+ 次 /N 次 /g' \
        -e 's/次 = [0-9]+/次 = N/g' \
        -e 's/轮 = [0-9]+/轮 = N/g' \
        -e 's/resume\+yield = [0-9]+/resume+yield = N/g'
}

# 跑一个二进制，返回 "rc|stdout+stderr"（超时/崩溃均捕获，只留 rc 与输出）。
# 输出写临时文件而非命令替换捕获管道——被测程序 fork 的持有 stdout 的子进程
# 会让 `$(...)` 永久阻塞等 EOF；文件无此问题。
# 超时用 setsid 起独立进程组 + 轮询 + `kill -9` 整组强杀：@test 二进制单独运行
# 可能挂起（等事件），且 `timeout` 对忽略信号/阻塞态进程会无限等待，必须整组强杀。
run_bin() {
    local bin=$1 rc out pid i
    out="$TMP/run.out"
    rm -f "$out"
    setsid "$bin" </dev/null >"$out" 2>&1 &
    pid=$!
    for ((i = 0; i < TIMEOUT; i++)); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 1
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 -- -"$pid" 2>/dev/null   # 杀整个进程组（setsid 使 pid 为组长）
        kill -9 "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        rc=124
    else
        wait "$pid" 2>/dev/null
        rc=$?
    fi
    printf '%s|%s' "$rc" "$(cat "$out" 2>/dev/null)"
}

# 单个文件 run-compare。mode: normal | test（@test 框架用 --test 编译）。
check() {
    local f=$1 mode=$2 name args
    name=$(basename "$f")
    args=()
    [ "$mode" = "test" ] && args+=(--test)
    # oracle 编译失败 → 坏语料/遗留样例，跳过（不计缺口）
    if ! "$MYPCC" -O2 "${args[@]}" "$f" -o "$TMP/o_mypc" >/dev/null 2>&1; then
        SKIP=$((SKIP+1)); say "  SKIP(oracle 编译失败): $name"; return
    fi
    # myp_self 编译失败 → 编译缺口（缺特性或编译期 bug，单独统计）
    if ! "$SELF" "${args[@]}" "$f" -o "$TMP/o_self" --stdlib "$STDLIB" >/dev/null 2>&1; then
        GAP=$((GAP+1)); say "  GAP(self 编译失败): $name"; return
    fi
    local r1 r2
    r1=$(run_bin "$TMP/o_mypc" | normalize)
    r2=$(run_bin "$TMP/o_self" | normalize)
    if [ "$r1" = "$r2" ]; then
        PASS=$((PASS+1)); say "  PASS: $name"
    else
        FAIL=$((FAIL+1)); say "  FAIL: $name"
        say "    mypc : $(printf '%s' "$r1" | cut -c1-140)"
        say "    self : $(printf '%s' "$r2" | cut -c1-140)"
    fi
}

say "=== [1/2] 常规可运行语料（examples/bench/BNCT，含 main/@startup）==="
for f in "$ROOT"/examples/*.myp "$ROOT"/bench/myp/*.myp "$ROOT"/BNCTDoseEngine/*.myp; do
    [ -f "$f" ] || continue
    grep -qE "main[[:space:]]*\(|@startup" "$f" || { SKIP=$((SKIP+1)); continue; }
    check "$f" normal
done

say "=== [2/2] @test 语料（--test 框架模式）==="
for f in "$ROOT"/tests/@test/*.myp; do
    [ -f "$f" ] || continue
    check "$f" test
done

say ""
say "=== run-compare 汇总 ==="
say "PASS=$PASS  FAIL=$FAIL  GAP(self 编译失败)=$GAP  SKIP(oracle 失败/库文件)=$SKIP"
if [ "$FAIL" -gt 0 ] || [ "$GAP" -gt 0 ]; then
    say "结论：存在需要修复的差异（FAIL=运行正确性 bug，GAP=编译缺口）。"
    exit 1
fi
say "结论：全部一致。"
exit 0
