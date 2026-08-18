#!/bin/bash
# run_bugs.sh — 运行 tests/bugs/ 下的 bug 复现测试（内建 @test 框架）
#
# 用法: MYPCC=./build/mypc bash tests/bugs/run_bugs.sh
#
# 每个 bug 用例：--test 编译 → 运行。未修复的 bug 会红（断言失败 / 运行时
# 崩溃 / 编译器崩溃）。修复后全部转绿，脚本退出码 0（可作 CI 门禁）。
set -u
MYPCC="${MYPCC:-./build/mypc}"
DIR="$(cd "$(dirname "$0")" && pwd)"
GREEN=0
RED=0
REDLIST=()

echo "bug-repro: tests/bugs/ ($(date '+%Y-%m-%d'))"
echo ""

for src in "$DIR"/*.myp; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .myp)
    bin="${src%.myp}.out"
    printf "  %-32s " "$name"

    # 编译（--test 内建测试框架）
    if ! cout=$($MYPCC --test "$src" 2>&1); then
        echo -e "\033[0;31mCOMPILE CRASH\033[0m"
        echo "$cout" | tail -2 | sed 's/^/      /'
        RED=$((RED + 1)); REDLIST+=("$name(compile)")
        continue
    fi

    # 运行（@test 断言失败时进程退出码非 0，故先捕获输出再按内容分类）
    rout=$(timeout 10 "$bin" 2>&1); rc=$?

    # 断言失败判定（含 @test 汇总行 "tests: N, assertions: X passed, Y failed"）
    if echo "$rout" | grep -qE "FAIL:|ASSERTION FAILED|passed, [1-9][0-9]* failed"; then
        echo -e "\033[0;31mRED (assertion)\033[0m"
        echo "$rout" | grep -E "ASSERTION FAILED|FAIL:|passed, [1-9][0-9]* failed" | head -3 | sed 's/^/      /'
        RED=$((RED + 1)); REDLIST+=("$name(assert)")
    elif [ $rc -ne 0 ]; then
        echo -e "\033[0;31mRUNTIME CRASH\033[0m"
        echo "$rout" | tail -2 | sed 's/^/      /'
        RED=$((RED + 1)); REDLIST+=("$name(runtime)")
    else
        echo -e "\033[0;32mGREEN\033[0m"
        GREEN=$((GREEN + 1))
    fi
done

echo ""
echo "bugs: $GREEN green, $RED red"
if [ $RED -gt 0 ]; then
    echo "red list: ${REDLIST[*]}"
fi
[ $RED -eq 0 ]
