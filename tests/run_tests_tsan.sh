#!/bin/bash
# MYP ThreadSanitizer 并发测试专项
#
# 用 ThreadSanitizer 检测生成程序的数据竞争。
# 生成程序由 codegen 插桩 (MYP_SANITIZE_TSAN=1) + runtime 链接 libtsan。
#
# 用法: ./tests/run_tests_tsan.sh          # 用 ./build/mypc
#       MYPCC=./build/mypc ./tests/run_tests_tsan.sh
#
# 退出码: 0=全部无竞争, 1=发现竞争/崩溃

set -u
cd "$(dirname "$0")/.."
# 跨平台移植层：Windows Git Bash 无 GNU timeout，用 myp_timeout 等价替代
source "$(dirname "$0")/lib/portable.sh"
MYPCC="${MYPCC:-./build/mypc}"
TIMEOUT_COMPILE=60
TIMEOUT_RUN=30

# 并发相关测试 (真实并发原语 + @thread 事件流)
TESTS="atomic barrier future threadpool parallel_for io_thread mapping_chain multi_event multitarget delay_throttle scope_mapping instance_mapping lambda_mapping where_mapping"

PASS=0
FAIL=0
FAILED=""

echo "=========================================="
echo "  MYP ThreadSanitizer 并发测试"
echo "  $(date)"
echo "  Compiler: $MYPCC (MYP_SANITIZE_TSAN=1)"
echo "=========================================="
echo ""

for name in $TESTS; do
    test_file="tests/$name/test.myp"
    [ ! -f "$test_file" ] && continue
    printf "  %-22s " "$name"

    bin="/tmp/tsan_${name}.out"
    bin=$(myp_resolve_bin "$bin")
    compile_out=$(MYP_SANITIZE_TSAN=1 myp_timeout $TIMEOUT_COMPILE "$MYPCC" "$test_file" -o "$bin" 2>&1)
    if [ $? -ne 0 ]; then
        echo -e "COMPILE FAIL"
        echo "$compile_out" | head -4
        FAIL=$((FAIL + 1)); FAILED="$FAILED $name(compile)"
        continue
    fi

    run_out=$(myp_timeout $TIMEOUT_RUN "$bin" 2>&1)
    code=$?
    rm -f "$bin"

    # TSan 竞争/崩溃判定
    if echo "$run_out" | grep -qE "WARNING: ThreadSanitizer|data race|ThreadSanitizer: reported|FATAL"; then
        echo -e "DATA RACE"
        echo "$run_out" | grep -E "WARNING:|data race|SUMMARY:|#0|#1|#2" | head -8
        FAIL=$((FAIL + 1)); FAILED="$FAILED $name(race)"
    elif [ $code -lt 0 ] || [ $code -ge 128 ]; then
        echo -e "CRASH (exit=$code)"
        FAIL=$((FAIL + 1)); FAILED="$FAILED $name(crash)"
    elif [ $code -eq 124 ]; then
        echo -e "TIMEOUT"
        FAIL=$((FAIL + 1)); FAILED="$FAILED $name(timeout)"
    else
        echo -e "PASS (no races)"
        PASS=$((PASS + 1))
    fi
done

echo ""
echo "=========================================="
echo "  结果: $PASS 通过, $FAIL 失败"
[ $FAIL -gt 0 ] && echo "  失败: $FAILED"
echo "=========================================="
exit $((FAIL > 0 ? 1 : 0))
