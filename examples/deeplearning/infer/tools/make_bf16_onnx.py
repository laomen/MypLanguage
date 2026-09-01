#!/usr/bin/env python3
"""阶段三：BF16 权重 dtype 转换（ONNX BFLOAT16 → MYP float32）合成模型 + numpy 参考。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_bf16_onnx.py
生成:
  deeplearning/data/onnx/bf16_test.onnx
  deeplearning/data/onnx/bf16_in.f32
  deeplearning/data/onnx/bf16_test_ort.bin

模型（opset13）: x[1,1,5,5] → Conv(W[1,1,3,3] BFLOAT16, B[1] BFLOAT16) → y[1,1,3,3]
  BF16 权重 = f32 高 16 位（raw_data 2 字节/元素）。ORT CPU 不支持 BF16 →
  参考用 numpy 手算（BF16 权重转 f32 后做卷积）。
"""
import numpy as np
import onnx
from onnx import helper, TensorProto

OUT = "deeplearning/data/onnx/bf16_test.onnx"
IN_F32 = "deeplearning/data/onnx/bf16_in.f32"
ORT_BIN = "deeplearning/data/onnx/bf16_test_ort.bin"

np.random.seed(60)
x = np.random.randn(1, 1, 5, 5).astype(np.float32)
w = np.random.randn(1, 1, 3, 3).astype(np.float32)
b = np.random.randn(1).astype(np.float32)


def to_bf16_raw(a):
    # BF16 = f32 高 16 位
    return (a.astype(np.float32).view(np.uint32) >> 16).astype(np.uint16).tobytes()


wt = TensorProto(); wt.name = "w"; wt.data_type = TensorProto.BFLOAT16; wt.dims.extend([1, 1, 3, 3]); wt.raw_data = to_bf16_raw(w)
bt = TensorProto(); bt.name = "b"; bt.data_type = TensorProto.BFLOAT16; bt.dims.extend([1]); bt.raw_data = to_bf16_raw(b)

node = helper.make_node("Conv", ["x", "w", "b"], ["y"], kernel_shape=[3, 3])
graph = helper.make_graph(
    [node], "bf16_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 1, 5, 5])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 1, 3, 3])],
    initializer=[wt, bt],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

x.tofile(IN_F32)
# numpy 参考（BF16 权重 → f32）
wf = ((w.view(np.uint32) >> 16).astype(np.uint32) << 16).view(np.float32)
bf = ((b.view(np.uint32) >> 16).astype(np.uint32) << 16).view(np.float32)
y = np.zeros((1, 1, 3, 3), np.float32)
for i in range(3):
    for j in range(3):
        y[0, 0, i, j] = np.sum(x[0, 0, i:i + 3, j:j + 3] * wf[0, 0]) + bf[0]
y.tofile(ORT_BIN)
print("wrote", OUT, "y.shape", y.shape, "y[0,0,0,0]", y[0, 0, 0, 0], "y[0,0,2,2]", y[0, 0, 2, 2])
