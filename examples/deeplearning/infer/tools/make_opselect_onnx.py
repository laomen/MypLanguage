#!/usr/bin/env python3
"""阶段五：算子选择（BatchMatMul batch 维全 1 → 降级 2D matmul）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_opselect_onnx.py
生成:
  deeplearning/data/onnx/opselect_test.onnx
  deeplearning/data/onnx/opselect_in.f32
  deeplearning/data/onnx/opselect_test_ort.bin

模型（opset13）:
  A[1,1,16,24] @ B[1,1,24,8] → Y1[1,1,16,8]   （batch 全 1 → 降级 2D matmul，
    大矩阵触发 GPU denseTiled 分块 GEMM）
  C[1,1,8,8]   @ D[1,1,8,8]   → Y2[1,1,8,8]    （batch 全 1 小矩阵）

双输出（不 Concat，避免 4D concat 非 axis 维不等的限制）。

验证：batch 维全 1 的 BatchMatMul 走 matmul 降级路径（CPU @parallel / GPU
denseTiled），输出 vs ORT 一致。op 表仍是 opKind 82（addBatchMatmul），
kernel 内部分派选择。
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/opselect_test.onnx"
IN_F32 = "deeplearning/data/onnx/opselect_in.f32"
ORT_BIN = "deeplearning/data/onnx/opselect_test_ort.bin"

np.random.seed(66)
a = np.random.randn(1, 1, 16, 24).astype(np.float32)
b = np.random.randn(1, 1, 24, 8).astype(np.float32)
c = np.random.randn(1, 1, 8, 8).astype(np.float32)
d = np.random.randn(1, 1, 8, 8).astype(np.float32)

nodes = [
    helper.make_node("MatMul", ["a", "b"], ["y1"]),
    helper.make_node("MatMul", ["c", "d"], ["y2"]),
]
graph = helper.make_graph(
    nodes, "opselect_test",
    inputs=[
        helper.make_tensor_value_info("a", TensorProto.FLOAT, [1, 1, 16, 24]),
        helper.make_tensor_value_info("b", TensorProto.FLOAT, [1, 1, 24, 8]),
        helper.make_tensor_value_info("c", TensorProto.FLOAT, [1, 1, 8, 8]),
        helper.make_tensor_value_info("d", TensorProto.FLOAT, [1, 1, 8, 8]),
    ],
    outputs=[
        helper.make_tensor_value_info("y1", TensorProto.FLOAT, [1, 1, 16, 8]),
        helper.make_tensor_value_info("y2", TensorProto.FLOAT, [1, 1, 8, 8]),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
# 4 输入拼成一个文件：a(16*24) + b(24*8) + c(8*8) + d(8*8)
x = np.concatenate([a.ravel(), b.ravel(), c.ravel(), d.ravel()])
x.tofile(IN_F32)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y1, y2 = sess.run(None, {"a": a, "b": b, "c": c, "d": d})
# 输出拼成 y1 + y2 写入（测试按此布局读）
np.concatenate([y1.ravel(), y2.ravel()]).tofile(ORT_BIN)
print("wrote", OUT, "y1.shape", y1.shape, "y2.shape", y2.shape,
      "y1[0,0,0,0]", y1[0, 0, 0, 0], "y2[0,0,0,0]", y2[0, 0, 0, 0])
