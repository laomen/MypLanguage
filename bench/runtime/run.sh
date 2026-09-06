#!/usr/bin/env bash
# bench/runtime/run.sh — 运行时资源基准（roadmap §六 R1–R5，v1: R1–R3）
#
# R1 ARC 短命对象吞吐 + live 归零；R2 @region 分配吞吐 + reserved 回落；
# R3 协程 spawn 时间/槽/栈池曲线（1k..20k）。
# 每个基准二进制打印 "R<n> key=val ..."（机器可解析）；此处补峰值 RSS。
#
# 用法：bash bench/runtime/run.sh [iters]     iters=重复取最小（默认 1）
# 环境：MYPCC / STDLIB 可覆盖（默认 ../../build/mypc + ../../stdlib）
set -uo pipefail
cd "$(dirname "$0")"
MYPCC=${MYPCC:-../../build/mypc}
STDLIB=${STDLIB:-../../stdlib}
ITERS=${1:-1}
[ -x "$MYPCC" ] || { echo "缺 mypc: $MYPCC"; exit 2; }

echo "== bench/runtime（R1 ARC / R2 region / R3 coro spawn）=="
for f in r1_arc r2_region r3_coro; do
    "$MYPCC" -O2 --stdlib "$STDLIB" -o "/tmp/${f}_bench" "$f.myp" || { echo "编译失败: $f"; exit 1; }
done

for f in r1_arc r2_region r3_coro; do
    echo "--- $f ---"
    best_ms=999999999
    best_line=""
    for ((i = 0; i < ITERS; i++)); do
        out=$(/usr/bin/time -v "/tmp/${f}_bench" 2>&1)
        line=$(echo "$out" | grep -E "^R[0-9] ")
        rss=$(echo "$out" | grep "Maximum resident" | grep -oE "[0-9]+" | head -1)
        ms=$(echo "$line" | grep -oE "ms=[0-9]+" | head -1 | grep -oE "[0-9]+")
        if [ -n "$ms" ] && [ "$ms" -lt "$best_ms" ]; then best_ms=$ms; best_line="$line"; best_rss=$rss; fi
    done
    echo "${best_line}  peak_rss_kb=${best_rss}"
done
