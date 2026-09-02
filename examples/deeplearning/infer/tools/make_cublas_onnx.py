#!/usr/bin/env python3
"""阶段六：cuBLAS GEMM（GPU dense/matmul 厂商库加速）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_cublas_onnx.py
生成:
  deeplearning/data/onnx/cublas_test.onnx
  deeplearning/data/onnx/cublas_in.f32
  deeplearning/data/onnx/cublas_test_ort.bin

模型（opset13，全 4D BatchMatMul，batch 维全 1 降级到 2D matmul）:
  A[1,1,64,128] @ B[1,1,128,96] → Y1[1,1,64,96]   （大：64×96≥4096 → GPU cuBLAS）
  C[1,1,96,64]  @ D[1,1,64,32]  → Y2[1,1,96,32]   （小：96×32=3072<4096 → thread-per-output）

双输出（避免 4D concat 非 axis 维不等的限制）。Y1 走 cuBLAS（厂商库），Y2 走
手写 kernel；输出 vs ORT 一致。CPU（MYP_GPU=0）回退 denseTiled/thread-per-output。
"""
import numpy as np
import onnx
from onnx import helper, TensorProto

OUT = "deeplearning/data/onnx/cublas_test.onnx"
IN_F32 = "deeplearning/data/onnx/cublas_in.f32"
ORT_BIN = "deeplearning/data/onnx/cublas_test_ort.bin"

np.random.seed(68)
a = np.random.randn(1, 1, 64, 128).astype(np.float32)
b = np.random.randn(1, 1, 128, 96).astype(np.float32)
c = np.random.randn(1, 1, 96, 64).astype(np.float32)
d = np.random.randn(1, 1, 64, 32).astype(np.float32)

nodes = [
    helper.make_node("MatMul", ["a", "b"], ["y1"]),
    helper.make_node("MatMul", ["c", "d"], ["y2"]),
]
graph = helper.make_graph(
    nodes, "cublas_test",
    inputs=[
        helper.make_tensor_value_info("a", TensorProto.FLOAT, [1, 1, 64, 128]),
        helper.make_tensor_value_info("b", TensorProto.FLOAT, [1, 1, 128, 96]),
        helper.make_tensor_value_info("c", TensorProto.FLOAT, [1, 1, 96, 64]),
        helper.make_tensor_value_info("d", TensorProto.FLOAT, [1, 1, 64, 32]),
    ],
    outputs=[
        helper.make_tensor_value_info("y1", TensorProto.FLOAT, [1, 1, 64, 96]),
        helper.make_tensor_value_info("y2", TensorProto.FLOAT, [1, 1, 96, 32]),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
# 输入拼成一个文件：a(64*128) + b(128*96) + c(96*64) + d(64*32)
x = np.concatenate([a.ravel(), b.ravel(), c.ravel(), d.ravel()])
x.tofile(IN_F32)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y1, y2 = sess.run(None, {"a": a, "b": b, "c": c, "d": d})
np.concatenate([y1.ravel(), y2.ravel()]).tofile(ORT_BIN)
print("wrote", OUT, "y1.shape", y1.shape, "y2.shape", y2.shape,
      "y1[0,0,0,0]", y1[0, 0, 0, 0], "y2[0,0,0,0]", y2[0, 0, 0, 0])
