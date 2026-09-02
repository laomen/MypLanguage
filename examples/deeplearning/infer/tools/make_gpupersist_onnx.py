#!/usr/bin/env python3
"""阶段六：GPU arena 常驻与增量同步（P5b 多帧）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_gpupersist_onnx.py
生成:
  deeplearning/data/onnx/gpupersist_test.onnx
  deeplearning/data/onnx/gpupersist_in.f32      （帧1 两输入 + 帧2 两输入）
  deeplearning/data/onnx/gpupersist_test_ort.bin（帧1 y + 帧2 y）

模型（opset13）:
  x1[1,1,5,5], x2[1,1,5,5] → Sub → y[1,1,5,5]

帧语义（测试驱动 gpuInferStart 持久化）:
  帧1: setFlat x1=x1a, x2=x2a → runGpu → y1a = x1a - x2a
  帧2: setFlat x1=x1b（仅改 x1，x2 保持 x2a）→ runGpu → y1b = x1b - x2a
  断言: 帧2 增量 H2D 只上传 1 个张量（x1），输出 vs ORT 一致。
"""
import numpy as np
import onnx
from onnx import helper, TensorProto

OUT = "deeplearning/data/onnx/gpupersist_test.onnx"
IN_F32 = "deeplearning/data/onnx/gpupersist_in.f32"
ORT_BIN = "deeplearning/data/onnx/gpupersist_test_ort.bin"

np.random.seed(67)
x1a = np.random.randn(1, 1, 5, 5).astype(np.float32)
x2a = np.random.randn(1, 1, 5, 5).astype(np.float32)
x1b = np.random.randn(1, 1, 5, 5).astype(np.float32)

nodes = [helper.make_node("Sub", ["x1", "x2"], ["y"])]
graph = helper.make_graph(
    nodes, "gpupersist_test",
    inputs=[
        helper.make_tensor_value_info("x1", TensorProto.FLOAT, [1, 1, 5, 5]),
        helper.make_tensor_value_info("x2", TensorProto.FLOAT, [1, 1, 5, 5]),
    ],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 1, 5, 5])],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
# 输入：帧1(x1a,x2a) + 帧2(x1b,x2a)
np.concatenate([x1a.ravel(), x2a.ravel(), x1b.ravel(), x2a.ravel()]).tofile(IN_F32)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y1a = sess.run(None, {"x1": x1a, "x2": x2a})[0]
y1b = sess.run(None, {"x1": x1b, "x2": x2a})[0]
np.concatenate([y1a.ravel(), y1b.ravel()]).tofile(ORT_BIN)
print("wrote", OUT, "y1a[0,0,0,0]", y1a[0, 0, 0, 0], "y1b[0,0,0,0]", y1b[0, 0, 0, 0])
