#!/usr/bin/env bash
# run_onnx.sh — 通用 ONNX 运行器脚本（F7）
# 自动定位 mypc、编译 run_onnx.myp（缓存到 /tmp，源码更新自动重编）、透传参数运行。
#
# 用法（仓库任意目录）:
#   bash scripts/run_onnx.sh <model.onnx> <输入名> <输出名> <input.f32> [--topk N] [-o out.bin]
#   MYP_GPU=1 bash scripts/run_onnx.sh ...
#   MYP_CC=/path/to/mypc bash scripts/run_onnx.sh ...     # 指定编译器
#   MYP_RUN_ONNX_BIN=/path/to/bin bash scripts/run_onnx.sh ...   # 指定缓存二进制
#
# 示例:
#   bash scripts/run_onnx.sh deeplearning/data/onnx/resnet18_v1_7.onnx \
#       data resnetv15_dense0_fwd deeplearning/data/onnx/resnet_input.f32 --topk 5
#   MYP_GPU=1 bash scripts/run_onnx.sh <同上> -o /tmp/out.bin

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---- 定位 mypc ----
MYPCC="${MYP_CC:-$ROOT/build/mypc}"
if [ ! -x "$MYPCC" ]; then
  echo "error: 找不到 mypc: $MYPCC" >&2
  echo "  请先构建: cmake -B build && cmake --build build --target mypc -j" >&2
  exit 1
fi

# ---- 编译 run_onnx（缓存到 /tmp，源码更新自动重编）----
BIN="${MYP_RUN_ONNX_BIN:-/tmp/myp_run_onnx}"
SRC="$ROOT/deeplearning/infer_tests/run_onnx.myp"
if [ ! -f "$BIN" ] || [ "$SRC" -nt "$BIN" ] || [ "$ROOT/deeplearning/infer/pb.myp" -nt "$BIN" ] \
   || [ "$ROOT/deeplearning/infer/runtime.myp" -nt "$BIN" ] \
   || [ "$ROOT/deeplearning/infer/onnx_loader.myp" -nt "$BIN" ] \
   || [ "$ROOT/deeplearning/infer/graph.myp" -nt "$BIN" ]; then
  "$MYPCC" "$SRC" -o "$BIN" --stdlib "$ROOT/stdlib" >/dev/null 2>&1 \
    || { echo "error: 编译 run_onnx.myp 失败" >&2; exit 1; }
fi

# ---- 参数不足则打印用法 ----
if [ "$#" -lt 4 ]; then
  echo "用法: run_onnx <model.onnx> <输入名> <输出名> <input.f32> [--topk N] [-o out.bin]"
  echo "  查张量名: deeplearning/infer/tools/onnxvenv/bin/python -c \"import onnx; m=onnx.load('模型'); print([i.name for i in m.graph.input], [o.name for o in m.graph.output])\""
  exit 1
fi

exec "$BIN" "$@"
