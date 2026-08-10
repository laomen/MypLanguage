#!/usr/bin/env bash
# run_stress.sh — 运行 tests/stress/ 协程/并发压力测试套件
#
# 用法:
#   bash tests/stress/run_stress.sh          # 编译并运行全部（-O2）
#   TSAN=1 bash tests/stress/run_stress.sh   # ThreadSanitizer：检测数据竞争
#   ASAN=1 bash tests/stress/run_stress.sh   # AddressSanitizer：检测内存错误
#   bash tests/stress/run_stress.sh coro_flood   # 只跑指定项（可多个）
#
# 每个测试打印 "PASS <name>" 视为通过；退出码 0=全部通过, 1=有失败。
# 独立于 run_tests.sh（压测负载重、有时序数据，不进快速回归）。

set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"   # tests/stress
cd "$SCRIPT_DIR/../.."                        # 仓库根
STRESS_DIR="$SCRIPT_DIR"
MYPCC="${MYPCC:-./build/mypc}"
TIMEOUT_COMPILE=120
TIMEOUT_RUN=240

TESTS="coro_flood coro_switch_storm channel_stress parallel_stress async_io_stress"

# 对生成程序启用 sanitizer（codegen 插桩 + runtime 链接）
SAN_ENV=""
if [ "${TSAN:-0}" = "1" ]; then
    SAN_ENV="MYP_SANITIZE_TSAN=1"
    echo "[stress] ThreadSanitizer 模式 —— 检测数据竞争"
elif [ "${ASAN:-0}" = "1" ]; then
    SAN_ENV="MYP_SANITIZE=1"
    echo "[stress] AddressSanitizer 模式 —— 检测内存错误"
fi

if [ "$#" -gt 0 ]; then
    TESTS="$*"
fi

PASS=0; FAIL=0; FAILED=""
for t in $TESTS; do
    f="$STRESS_DIR/$t.myp"
    [ -f "$f" ] || { echo "  [SKIP] $t (无 $f)"; continue; }
    printf "  %-22s " "$t"
    if ! env $SAN_ENV "$MYPCC" -O2 "$f" -o "/tmp/stress_$t" >/tmp/stress_${t}.compile 2>&1; then
        echo "COMPILE FAIL"; FAIL=$((FAIL+1)); FAILED="$FAILED $t(compile)"; continue
    fi
    out=$(timeout $TIMEOUT_RUN env $SAN_ENV "/tmp/stress_$t" 2>&1)
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "RUNTIME FAIL (exit=$rc)"
        echo "$out" | grep -E "FAIL|error|ERROR" | head -5
        FAIL=$((FAIL+1)); FAILED="$FAILED $t(runtime)"; continue
    fi
    if echo "$out" | grep -q "^PASS "; then
        echo "PASS"; PASS=$((PASS+1))
    else
        echo "CHECK FAIL"; echo "$out" | tail -4
        FAIL=$((FAIL+1)); FAILED="$FAILED $t(check)"
    fi
done

echo "=========================================="
echo "  通过: $PASS  失败: $FAIL"
[ -n "$FAILED" ] && echo "  失败项:$FAILED"
[ "$FAIL" -eq 0 ] && echo "  全部压力测试通过!" && exit 0
exit 1
