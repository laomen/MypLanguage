#!/usr/bin/env python3
"""阶段四：BatchMatMul（4D batch matmul + batch 广播）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_bmm_onnx.py
生成:
  deeplearning/data/onnx/bmm_test.onnx
  deeplearning/data/onnx/bmm_in_*.f32（各输入）
  deeplearning/data/onnx/bmm_test_ort.bin（out1+out2 拼接）

模型（opset13，多输出）:
  out1 = A[1,2,3,4] @ B[1,2,4,5] → [1,2,3,5]（batch 对齐）
  out2 = A2[1,1,3,4] @ B2[1,2,4,5] → [1,2,3,5]（A batch=1 广播）
"""
import numpy as np
import onnx
from onnx import helper, TensorProto

OUT = "deeplearning/data/onnx/bmm_test.onnx"
ORT_BIN = "deeplearning/data/onnx/bmm_test_ort.bin"

np.random.seed(61)
A = np.random.randn(1, 2, 3, 4).astype(np.float32)
B = np.random.randn(1, 2, 4, 5).astype(np.float32)
A2 = np.random.randn(1, 1, 3, 4).astype(np.float32)
B2 = np.random.randn(1, 2, 4, 5).astype(np.float32)

nodes = [
    helper.make_node("MatMul", ["A", "B"], ["out1"], name="mm1"),
    helper.make_node("MatMul", ["A2", "B2"], ["out2"], name="mm2"),
]
graph = helper.make_graph(
    nodes, "bmm_test",
    inputs=[
        helper.make_tensor_value_info("A", TensorProto.FLOAT, [1, 2, 3, 4]),
        helper.make_tensor_value_info("B", TensorProto.FLOAT, [1, 2, 4, 5]),
        helper.make_tensor_value_info("A2", TensorProto.FLOAT, [1, 1, 3, 4]),
        helper.make_tensor_value_info("B2", TensorProto.FLOAT, [1, 2, 4, 5]),
    ],
    outputs=[
        helper.make_tensor_value_info("out1", TensorProto.FLOAT, [1, 2, 3, 5]),
        helper.make_tensor_value_info("out2", TensorProto.FLOAT, [1, 2, 3, 5]),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
feed = {"A": A, "B": B, "A2": A2, "B2": B2}
for nm, v in feed.items():
    v.tofile("deeplearning/data/onnx/bmm_in_%s.f32" % nm)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
outs = sess.run(None, feed)
blob = np.concatenate([o.flatten() for o in outs]).astype(np.float32)
blob.tofile(ORT_BIN)
print("wrote", OUT, "out1.shape", outs[0].shape, "out1[0,0,0,0]", outs[0][0, 0, 0, 0], "out2[0,1,2,4]", outs[1][0, 1, 2, 4])
