#!/usr/bin/env bash
# run_torture.sh — 编译并运行 Torture Test 套件（仿 GCC torture 运行器）
#
# 1) compile/  所有测试仅编译（-O0/-O1/-O2 三档，编译器不得崩溃/挂死）
# 2) execute/  编译 + 运行，检查退出码 0 且含 "PASS "（自验证）
# 汇总：编译通过数、运行通过数、失败清单、总耗时。
#
# 用法: bash tests/torture/run_torture.sh [输出目录]
set -u
cd "$(dirname "$0")/../.."
GEN="${1:-tests/torture/generated}"
MYPCC="${MYPCC:-./build/mypc}"
TIMEOUT_CC=60
TIMEOUT_RUN=60
# 编译压测内存上限（KB）：自举/大枚举等病态输入曾 OOM 崩溃系统，必须限内存。
# 超限时 mypc/opt/llc 以 SIGSEGV 死掉而非把系统打到 swap——用 MEM_LIMIT_KB=0 禁用。
MEM_LIMIT_KB="${MEM_LIMIT_KB:-8388608}"

# 便携 timeout
source "$(dirname "$0")/../lib/portable.sh"

# 受限子 shell 编译：ulimit -v 作用于 mypc + opt + llc + ld 全链路。
cc_limited() {
    if [ "$MEM_LIMIT_KB" = "0" ]; then
        myp_timeout $TIMEOUT_CC "$@"
    else
        ( ulimit -v "$MEM_LIMIT_KB"; myp_timeout $TIMEOUT_CC "$@" )
    fi
}

[ -d "$GEN/compile" ] || { echo "[run_torture] 未找到 $GEN/compile——先跑 gen_torture.sh"; exit 1; }

PASS=0; FAIL=0; FAILED=""
total_s=$(date +%s)

echo "=== compile/ 压力（-O0/-O1/-O2 三档，仅编译） ==="
for f in "$GEN"/compile/*.myp; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .myp)
    for lvl in 0 1 2; do
        if ! cc_limited "$MYPCC" "-O$lvl" "$f" -o "/tmp/torture_cc_$name" >/dev/null 2>&1; then
            echo "  [FAIL] $name (-O$lvl) 编译失败/崩溃"
            FAIL=$((FAIL+1)); FAILED="$FAILED $name(O$lvl)"
            continue 2
        fi
    done
    PASS=$((PASS+1))
done
echo "  compile: $PASS 通过, $FAIL 失败"

echo "=== deep/ 递归深度压测（编译不得崩溃：干净报错 OK，SIGSEGV/abort 判失败） ==="
DP=0; DF=0
for f in "$GEN"/deep/*.myp; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .myp)
    if cc_limited "$MYPCC" -O2 "$f" -o "/tmp/torture_d_$name" >/tmp/d_cc.out 2>&1; then
        DP=$((DP+1))   # 低于守卫阈值编译成功也可
    else
        rc=$?
        if [ $rc -eq 139 ] || [ $rc -eq 134 ] || echo "$rc" | grep -qE "Segmentation|core dumped|SIGSEGV|SIGABRT"; then
            echo "  [FAIL] $name (编译器崩溃 rc=$rc)"
            DF=$((DF+1)); FAILED="$FAILED $name(crash)"
        elif grep -q "nested too deeply" /tmp/d_cc.out; then
            DP=$((DP+1))   # 干净报错 = 守卫生效
        else
            # rc 非零但非崩溃：正常编译错误也算干净拒绝
            DP=$((DP+1))
        fi
    fi
done
echo "  deep: $DP 通过, $DF 失败"

echo "=== execute/ 自验证（编译 + 运行，exit 0 + PASS） ==="
EP=0; EF=0
for f in "$GEN"/execute/*.myp; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .myp)
    if ! cc_limited "$MYPCC" -O2 "$f" -o "/tmp/torture_$name" >/dev/null 2>&1; then
        echo "  [FAIL] $name 编译失败"
        EF=$((EF+1)); FAILED="$FAILED $name(cc)"; continue
    fi
    out=$(myp_timeout $TIMEOUT_RUN "$(myp_resolve_bin "/tmp/torture_$name")" 2>&1)
    rc=$?
    if [ $rc -eq 0 ] && echo "$out" | grep -q "PASS "; then
        EP=$((EP+1))
    else
        echo "  [FAIL] $name (exit=$rc)"; echo "$out" | tail -2
        EF=$((EF+1)); FAILED="$FAILED $name(run)"
    fi
done
echo "  execute: $EP 通过, $EF 失败"

end_s=$(date +%s)
echo "=========================================="
echo "  总计: compile $PASS + deep $DP + execute $EP 通过, 失败 $((FAIL+DF+EF))"
[ -n "$FAILED" ] && echo "  失败项:$FAILED"
echo "  总耗时: $((end_s-total_s))s"
[ $((FAIL+DF+EF)) -eq 0 ] && echo "  全部 torture 测试通过!" && exit 0
exit 1
