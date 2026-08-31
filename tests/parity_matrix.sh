#!/bin/bash
# parity_matrix.sh — 双编译器 parity 矩阵（种子 vs 自举）
#
# C++ mypc-seed 现在是**种子编译器**（仅用于引导自举，已冻结）；
# 交付版是 selfhost 不动点 mypc（= myp_self2）。本矩阵用同一套
# tests/run_tests.sh 各跑一遍，输出三类差异：
#   1) 类别 通过/失败 对照表
#   2) **待补差距**：selfhost 挂但 seed 过（= 自举落后，必须修）
#   3) **种子落后**：seed 挂但 selfhost 过（= 种子不支持的新特性，非 parity
#      差距——种子已冻结，仅引导自举，不要求跟进新特性）
#   4) 两边都挂（= 环境/测试本身问题，非 parity 差距）
#
# 用法:
#   ./tests/parity_matrix.sh                     # 全量
#   SEED=./build/mypc-seed SELF=./build/mypc ./tests/parity_matrix.sh
#
set -o pipefail
cd "$(dirname "$0")/.."

SEED="${SEED:-./build/mypc-seed}"
SELF="${SELF:-./build/mypc}"

echo "== 运行 seed(种子): $SEED =="
MYPCC="$SEED" bash tests/run_tests.sh > /tmp/parity_seed.log 2>&1
seed_exit=$?

echo "== 运行 selfhost(自举): $SELF =="
MYPCC="$SELF" bash tests/run_tests.sh > /tmp/parity_self.log 2>&1
self_exit=$?

# 提取"类别: X 通过, Y 失败"行
summary_of() {
    local f="$1"
    grep -E '^\s*(回归测试|负测试|测试框架|协程栈警告|无崩溃|自举包管理|自举格式化|自举可视化|mypc run|LSP|GPU 回退|总计):' "$f" \
        | sed 's/^[[:space:]]*//; s/[[:space:]]*$//'
}

# 提取失败清单
failed_of() {
    local f="$1"
    awk '/失败的测试:/{flag=1; next} /^[[:space:]]*$/{if(flag) exit} flag' "$f" \
        | grep '    - ' | sed 's/^    - //' | LC_ALL=C sort
}

echo
echo "===== 类别对照（通过, 失败） ====="
paste -d ' | ' <(summary_of /tmp/parity_seed.log) <(summary_of /tmp/parity_self.log) \
    | awk -F' \\| ' '{ printf "%-46s seed=%-34s selfhost=%s\n", $1, $2, $3 }'

echo
echo "===== 待补差距：selfhost 挂 但 seed 过（必须修） ====="
comm -13 <(failed_of /tmp/parity_seed.log) <(failed_of /tmp/parity_self.log) \
    | sed 's/^/  - /'
if [ -z "$(comm -13 <(failed_of /tmp/parity_seed.log) <(failed_of /tmp/parity_self.log))" ]; then
    echo "  (无)"
fi

echo
echo "===== 种子落后：seed 挂 但 selfhost 过（非差距，种子已冻结） ====="
comm -23 <(failed_of /tmp/parity_seed.log) <(failed_of /tmp/parity_self.log) \
    | sed 's/^/  - /'
if [ -z "$(comm -23 <(failed_of /tmp/parity_seed.log) <(failed_of /tmp/parity_self.log))" ]; then
    echo "  (无)"
fi

echo
echo "===== 两边都挂（非 parity 差距，环境/测试问题） ====="
comm -12 <(failed_of /tmp/parity_seed.log) <(failed_of /tmp/parity_self.log) \
    | sed 's/^/  - /'
if [ -z "$(comm -12 <(failed_of /tmp/parity_seed.log) <(failed_of /tmp/parity_self.log))" ]; then
    echo "  (无)"
fi

echo
echo "seed exit=$seed_exit  selfhost exit=$self_exit"
echo "日志: /tmp/parity_seed.log /tmp/parity_self.log"
