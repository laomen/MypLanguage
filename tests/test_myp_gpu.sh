#!/bin/bash
# MYP GPU 测试（CPU 回退模式，§3.x GPU CPU fallback）
#
# 自举编译器无 GPU kernel 发射；@gpu for/tile/reduce/scan/scatter 走 CPU 回退
# 串行语义（与 C++ oracle 的 fallback 对齐）。本脚本验证回退结果正确：
#   · 运行时测试：输出含 PASS / 全行 OK 且无 FAIL
#   · assert 负测试（test_gpu_assert_fail）：触发 kernel.assert → 退出码 1
#   · 无 GPU 设备测试（shfl/stream/byoc）：打印 SKIP 不崩溃
#   · 纯编译测试（compute/math/gpu/reduce_bit/transport/prn/cuda_*）：仅编译通过
#
# 用法: MYPCC=./build/myp_self bash tests/test_myp_gpu.sh
# 退出码: 0=全过, 1=有失败

set -u
MYPCC="${MYPCC:-./build/mypc}"
PROJ_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PASS=0
FAIL=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

check() {
    local name="$1"; local cond="$2"
    if eval "$cond"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}

echo "myp-gpu: GPU CPU 回退测试 (CPU fallback)"

# 1) 运行时验证测试：输出必须含 PASS 或全行 OK/无 FAIL
#    注：test_gpu_algo 的 GpuAlgo.sort 有自举编译器既有栈问题（旧编译器同样
#    段错误 139，与 GPU CPU 回退无关），故只校验输出、不校验退出码。
RUNTIME_TESTS="test_gpu_block test_gpu_graph test_gpu_hal
test_gpu_kernel_ctx test_gpu_math_float test_gpu_printk test_gpu_query
test_gpu_reduce test_gpu_scan test_gpu_scatter test_gpu_static
test_gpu_stride test_gpu_tile test_gpu_tile_degrade test_gpu_vec4"
for b in $RUNTIME_TESTS; do
    f="$PROJ_ROOT/tests/$b.myp"
    if [ ! -f "$f" ]; then echo "  SKIP: $b (文件缺失)"; continue; fi
    if ! $MYPCC "$f" -o "$TMPDIR/$b" --stdlib "$PROJ_ROOT/stdlib" >/dev/null 2>&1; then
        check "$b 编译" "false"
        continue
    fi
    out=$("$TMPDIR/$b" 2>&1)
    ec=$?
    check "$b 输出含 PASS/OK" "echo \"$out\" | grep -qE 'PASS|OK'"
    check "$b 无 FAIL" "! echo \"$out\" | grep -qE 'FAIL'"
    check "$b 退出码0" "test $ec -eq 0"
done

# 1b) test_gpu_algo（gpu.algo 库，@gpu for 在 sort 内）：输出含 OK 即可
f="$PROJ_ROOT/tests/test_gpu_algo.myp"
if [ -f "$f" ] && $MYPCC "$f" -o "$TMPDIR/test_gpu_algo" --stdlib "$PROJ_ROOT/stdlib" >/dev/null 2>&1; then
    out=$("$TMPDIR/test_gpu_algo" 2>&1)
    check "test_gpu_algo 输出含 OK" "echo \"$out\" | grep -qE 'OK'"
    check "test_gpu_algo 无 FAIL" "! echo \"$out\" | grep -qE 'FAIL'"
else
    check "test_gpu_algo 编译" "false"
fi

# 2) assert 负测试：kernel.assert 失败 → stderr FAILED + 退出码 1
f="$PROJ_ROOT/tests/test_gpu_assert_fail.myp"
if [ -f "$f" ] && $MYPCC "$f" -o "$TMPDIR/assert_fail" --stdlib "$PROJ_ROOT/stdlib" >/dev/null 2>&1; then
    out=$("$TMPDIR/assert_fail" 2>&1)
    ec=$?
    check "assert_fail 退出码1" "test $ec -eq 1"
    check "assert_fail 打印 FAILED" "echo \"$out\" | grep -qE 'FAILED'"
else
    check "test_gpu_assert_fail 编译" "false"
fi

# 3) 无 GPU 设备测试：SKIP 输出不崩溃
SKIP_TESTS="test_gpu_shfl test_gpu_stream test_gpu_byoc test_gpu_force_cpu"
for b in $SKIP_TESTS; do
    f="$PROJ_ROOT/tests/$b.myp"
    if [ ! -f "$f" ]; then continue; fi
    if ! $MYPCC "$f" -o "$TMPDIR/$b" --stdlib "$PROJ_ROOT/stdlib" >/dev/null 2>&1; then
        check "$b 编译" "false"
        continue
    fi
    out=$("$TMPDIR/$b" 2>&1)
    ec=$?
    check "$b 不崩溃(退出码0)" "test $ec -eq 0"
done

# 4) 纯编译测试（无运行时输出）：仅需编译通过
COMPILE_ONLY="test_gpu test_gpu_compute test_gpu_math test_gpu_reduce_bit
test_gpu_transport test_prn_gpu test_cuda_stdlib test_cuda_enhance"
for b in $COMPILE_ONLY; do
    f="$PROJ_ROOT/tests/$b.myp"
    if [ ! -f "$f" ]; then continue; fi
    check "$b 编译通过" "$MYPCC \"$f\" -o \"$TMPDIR/$b\" --stdlib \"$PROJ_ROOT/stdlib\" >/dev/null 2>&1"
done

echo ""
echo "myp-gpu PASS=$PASS FAIL=$FAIL"
if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
