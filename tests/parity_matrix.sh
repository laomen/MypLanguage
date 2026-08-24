#!/bin/bash
# parity_matrix.sh — 双编译器 parity 矩阵
#
# 同一套 tests/run_tests.sh 用 oracle(mypc) 与 selfhost(myp_self2) 各跑一遍，
# 输出：
#   1) 各类别 通过/失败 对照表
#   2) "selfhost 挂但 oracle 过"的测试清单（= 待补差距）
#   3) "两边都挂"的测试（= 环境/测试本身问题，非 parity 差距）
#
# 用法:
#   ./tests/parity_matrix.sh                     # 全量
#   ORACLE=./build/mypc SELF=./build/myp_self2 ./tests/parity_matrix.sh
#
set -o pipefail
cd "$(dirname "$0")/.."

ORACLE="${ORACLE:-./build/mypc}"
SELF="${SELF:-./build/myp_self2}"

echo "== 运行 oracle: $ORACLE =="
MYPCC="$ORACLE" bash tests/run_tests.sh > /tmp/parity_oracle.log 2>&1
o_exit=$?

echo "== 运行 selfhost: $SELF =="
MYPCC="$SELF" bash tests/run_tests.sh > /tmp/parity_self.log 2>&1
s_exit=$?

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
paste -d ' | ' <(summary_of /tmp/parity_oracle.log) <(summary_of /tmp/parity_self.log) \
    | awk -F' \\| ' '{ printf "%-46s oracle=%-34s selfhost=%s\n", $1, $2, $3 }'

echo
echo "===== 差距：selfhost 挂 但 oracle 过（待补） ====="
comm -13 <(failed_of /tmp/parity_oracle.log) <(failed_of /tmp/parity_self.log) \
    | sed 's/^/  - /'
if [ -z "$(comm -13 <(failed_of /tmp/parity_oracle.log) <(failed_of /tmp/parity_self.log))" ]; then
    echo "  (无)"
fi

echo
echo "===== 两边都挂（非 parity 差距，环境/测试问题） ====="
comm -12 <(failed_of /tmp/parity_oracle.log) <(failed_of /tmp/parity_self.log) \
    | sed 's/^/  - /'
if [ -z "$(comm -12 <(failed_of /tmp/parity_oracle.log) <(failed_of /tmp/parity_self.log))" ]; then
    echo "  (无)"
fi

echo
echo "oracle exit=$o_exit  selfhost exit=$s_exit"
echo "日志: /tmp/parity_oracle.log /tmp/parity_self.log"
