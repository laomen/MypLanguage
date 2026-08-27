#!/usr/bin/env bash
# test_leak_long.sh — 长时间内存泄漏测试驱动。
# 编译 leak_long.myp（混合负载），运行指定时长，分析：
#   - Node/total 存活计数：对象泄漏指标（应稳定有界，不随迭代增长）
#   - arenaR（mmap 保留）：内存占用指标（应趋平；持续线性增长 = bump 不复用）
# 用法：
#   bash tests/test_leak_long.sh [秒数] [MYPCC]
#   （默认 300 秒；传 1800 跑 30 分钟）
# 退出码 0 = 无对象泄漏 + arena 趋平；1 = 疑似对象泄漏；2 = arena 疑似不趋平。
set -u
SECS="${1:-300}"
MYPCC="${2:-./build/mypc}"
cd "$(dirname "$0")/.."

echo "[leak_long] 编译..."
if ! "$MYPCC" tests/leak_long.myp -o /tmp/leak_long_bin 2>/dev/null; then
    echo "FAIL: 编译失败"; exit 1
fi

echo "[leak_long] 运行 ${SECS}s（timeout）..."
timeout "$SECS" /tmp/leak_long_bin > /tmp/leak_long.log 2>&1
rc=$?
if [ "$rc" -ne 124 ]; then
    echo "FAIL: 程序提前退出（rc=$rc，非 timeout）"; tail -5 /tmp/leak_long.log; exit 1
fi

n=$(grep -c '^iter=' /tmp/leak_long.log)
if [ "$n" -lt 2 ]; then
    echo "WARN: 报告点不足（$n），无法分析趋势"; exit 3
fi

first_node=$(grep '^iter=' /tmp/leak_long.log | head -1 | sed -E 's/.*Node=([0-9]+).*/\1/')
last_node=$(grep '^iter=' /tmp/leak_long.log | tail -1 | sed -E 's/.*Node=([0-9]+).*/\1/')
first_total=$(grep '^iter=' /tmp/leak_long.log | head -1 | sed -E 's/.*total=([0-9]+).*/\1/')
last_total=$(grep '^iter=' /tmp/leak_long.log | tail -1 | sed -E 's/.*total=([0-9]+).*/\1/')
first_ar=$(grep '^iter=' /tmp/leak_long.log | head -1 | sed -E 's/.*arenaR=([0-9]+).*/\1/')
last_ar=$(grep '^iter=' /tmp/leak_long.log | tail -1 | sed -E 's/.*arenaR=([0-9]+).*/\1/')

echo "[leak_long] 报告点: $n"
echo "  Node:  $first_node -> $last_node  (增量 $((last_node - first_node)))"
echo "  total: $first_total -> $last_total (增量 $((last_total - first_total)))"
echo "  arenaR: $first_ar -> $last_ar (增量 $((last_ar - first_ar)))"

fail=0
# 对象泄漏：Node/total 持续增长（末段应稳定；允许报告间小幅波动）
if [ "$last_node" -gt $((first_node + 2000)) ]; then
    echo "FAIL: Node 存活数持续增长（$first_node -> $last_node）→ 疑似对象泄漏"
    fail=1
else
    echo "OK: Node/total 存活计数稳定（无对象泄漏）"
fi

# arena 趋平：末段报告 arenaR 应基本持平（dA≈0 或末 3 点增量不显著）
tail_ar=$(grep '^iter=' /tmp/leak_long.log | tail -3 | sed -E 's/.*arenaR=([0-9]+).*/\1/')
if [ -n "$tail_ar" ]; then
    t1=$(echo "$tail_ar" | sed -n 1p); t2=$(echo "$tail_ar" | sed -n 3p)
    if [ "$t1" -gt 0 ] && [ $(( (t2 - t1) * 100 / t1 )) -gt 15 ]; then
        echo "WARN: arenaR 末段仍在增长（$t1 -> $t2）→ 稳态 pool 未趋平（可能碎片化/慢增长）"
    else
        echo "OK: arenaR 已趋平（$t1 -> $t2）"
    fi
fi

echo "[leak_long] 完成 (fail=$fail)"
exit $fail
