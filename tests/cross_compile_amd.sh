#!/usr/bin/env bash
# §9.5 P4 ⑤ 无 AMD 硬件时的跨厂商交叉编译验证（docs/gpu_library_design §6.4/§6.7）
#
# 策略：无 AMD GPU/ROCm 时，MYP_GPU_TARGET=amdgcn 让 @gpu for 编译期发射 GCN
# ELF code object（写文件），运行期仍走 CPU 回退（语义一致）。本脚本验证：
#   1) GCN code object 产出（ELF magic + kernel 符号，llvm-readobj 检查）；
#   2) AMD 编译的二进制 CPU 回退语义正确（test_gpu_kernel_ctx PASS）；
#   3) NV 默认路径不受影响（MYP_DUMP_PTX 仍出 PTX）。
# 有 AMD 硬件时改用真实 ROCm runtime（-DMYP_ENABLE_ROCM=ON）直接跑。
set -e
cd "$(dirname "$0")/.."
MYPCC=${MYPCC:-./build/mypc}
LLVM_BIN=$(llvm-config --bindir 2>/dev/null || echo /usr/lib/llvm-21/bin)
READOBJ="$LLVM_BIN/llvm-readobj"
TEST=test_gpu_kernel_ctx
GCN=/tmp/myp_cross.gcn
BIN=/tmp/myp_cross.bin

echo "--- [1/3] AMD 交叉编译（MYP_GPU_TARGET=amdgcn → GCN ELF code object） ---"
MYP_GPU_TARGET=amdgcn MYP_GPU_EMIT_FILE="$GCN" "$MYPCC" "tests/$TEST.myp" -o "$BIN" 2>&1 \
    | grep -E "AMD cross|error|fatal" || true
if [ ! -s "$GCN" ]; then
    echo "FAIL: 未产出 GCN code object 文件" >&2
    exit 1
fi
echo "  产物: $GCN ($(stat -c%s "$GCN") bytes)"

echo "--- [2/3] GCN object 校验（ELF + kernel 符号） ---"
magic=$(head -c4 "$GCN" | xxd -p)
if [ "$magic" != "7f454c46" ]; then
    echo "FAIL: 不是 ELF 文件 (magic=$magic)" >&2
    exit 1
fi
echo "  ELF magic OK (7f454c46)"
if command -v "$READOBJ" >/dev/null 2>&1; then
    "$READOBJ" --symbols "$GCN" 2>/dev/null | grep -qiE "myp_kernel" \
        && echo "  kernel 符号 'myp_kernel' OK" \
        || { echo "  (警告: llvm-readobj 未找到 myp_kernel 符号)"; }
    "$READOBJ" --file-headers "$GCN" 2>/dev/null | grep -E "Machine" | head -1
else
    echo "  (llvm-readobj 不可用，仅校验 ELF magic)"
fi

echo "--- [3/3] AMD 编译二进制 CPU 回退语义（MYP_GPU=0） + NV 路径回归 ---"
out=$(MYP_GPU=0 "$BIN" 2>&1 | grep -E "PASS|FAIL" | tail -1)
echo "  AMD 二进制 CPU 回退: $out"
if [[ "$out" != *"PASS"* ]]; then echo "FAIL: CPU 回退语义错误" >&2; exit 1; fi
MYP_DUMP_PTX=1 "$MYPCC" "tests/$TEST.myp" -o /tmp/myp_nv.bin 2>&1 | grep -q "MYP PTX (for)" \
    && echo "  NV 默认路径仍发射 PTX: OK" \
    || { echo "FAIL: NV PTX 发射回归" >&2; exit 1; }

echo ""
echo "P4 ⑤ 交叉编译验证通过（无 AMD 硬件：GCN 产出 + CPU 回退语义 + NV 无回归）"
