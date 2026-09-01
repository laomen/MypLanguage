#!/usr/bin/env python3
"""阶段三：统一 numpy 广播（Sub/Div/Mul 4D）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_bcast_onnx.py
生成:
  deeplearning/data/onnx/bcast_test.onnx
  deeplearning/data/onnx/bcast_in_*.f32（各输入）
  deeplearning/data/onnx/bcast_test_ort.bin（out1..out6 顺序拼接）

覆盖 6 种广播（opset13，多输出）:
  out1 = a[1,2,3,4]  - b[1,2,3,4]      （同形状）
  out2 = a2[1,2,3,4] / bscalar[1]       （标量）
  out3 = a3[1,4,3,4] * bchan[1,4,1,1]   （逐通道 [1,C,1,1]）
  out4 = a4[1,2,3,4] - bw[1,1,1,4]      （W 维广播）
  out5 = a5[1,2,3,4] * bhw[1,1,3,4]     （H,W 维广播）
  out6 = a6[1,1,3,4] - b6[1,2,3,4]      （b 放大输出，a 广播）
"""
import numpy as np
import onnx
from onnx import helper, TensorProto

OUT = "deeplearning/data/onnx/bcast_test.onnx"
ORT_BIN = "deeplearning/data/onnx/bcast_test_ort.bin"

np.random.seed(58)
a = np.random.randn(1, 2, 3, 4).astype(np.float32)
b = np.random.randn(1, 2, 3, 4).astype(np.float32)
a2 = np.random.randn(1, 2, 3, 4).astype(np.float32)
bscalar = np.array([2.5], dtype=np.float32)
a3 = np.random.randn(1, 4, 3, 4).astype(np.float32)
bchan = np.random.randn(1, 4, 1, 1).astype(np.float32)
a4 = np.random.randn(1, 2, 3, 4).astype(np.float32)
bw = np.random.randn(1, 1, 1, 4).astype(np.float32)
a5 = np.random.randn(1, 2, 3, 4).astype(np.float32)
bhw = np.random.randn(1, 1, 3, 4).astype(np.float32)
a6 = np.random.randn(1, 1, 3, 4).astype(np.float32)
b6 = np.random.randn(1, 2, 3, 4).astype(np.float32)

nodes = [
    helper.make_node("Sub", ["a", "b"], ["out1"], name="s1"),
    helper.make_node("Div", ["a2", "bscalar"], ["out2"], name="d1"),
    helper.make_node("Mul", ["a3", "bchan"], ["out3"], name="m1"),
    helper.make_node("Sub", ["a4", "bw"], ["out4"], name="s2"),
    helper.make_node("Mul", ["a5", "bhw"], ["out5"], name="m2"),
    helper.make_node("Sub", ["a6", "b6"], ["out6"], name="s3"),
    helper.make_node("Add", ["a", "b"], ["out7"], name="ad1"),        # Add 同形状
    helper.make_node("Add", ["a3", "bchan"], ["out8"], name="ad2"),    # Add 逐通道 bias
]
graph = helper.make_graph(
    nodes, "bcast_test",
    inputs=[
        helper.make_tensor_value_info("a", TensorProto.FLOAT, [1, 2, 3, 4]),
        helper.make_tensor_value_info("b", TensorProto.FLOAT, [1, 2, 3, 4]),
        helper.make_tensor_value_info("a2", TensorProto.FLOAT, [1, 2, 3, 4]),
        helper.make_tensor_value_info("bscalar", TensorProto.FLOAT, [1]),
        helper.make_tensor_value_info("a3", TensorProto.FLOAT, [1, 4, 3, 4]),
        helper.make_tensor_value_info("bchan", TensorProto.FLOAT, [1, 4, 1, 1]),
        helper.make_tensor_value_info("a4", TensorProto.FLOAT, [1, 2, 3, 4]),
        helper.make_tensor_value_info("bw", TensorProto.FLOAT, [1, 1, 1, 4]),
        helper.make_tensor_value_info("a5", TensorProto.FLOAT, [1, 2, 3, 4]),
        helper.make_tensor_value_info("bhw", TensorProto.FLOAT, [1, 1, 3, 4]),
        helper.make_tensor_value_info("a6", TensorProto.FLOAT, [1, 1, 3, 4]),
        helper.make_tensor_value_info("b6", TensorProto.FLOAT, [1, 2, 3, 4]),
    ],
    outputs=[
        helper.make_tensor_value_info("out1", TensorProto.FLOAT, [1, 2, 3, 4]),
        helper.make_tensor_value_info("out2", TensorProto.FLOAT, [1, 2, 3, 4]),
        helper.make_tensor_value_info("out3", TensorProto.FLOAT, [1, 4, 3, 4]),
        helper.make_tensor_value_info("out4", TensorProto.FLOAT, [1, 2, 3, 4]),
        helper.make_tensor_value_info("out5", TensorProto.FLOAT, [1, 2, 3, 4]),
        helper.make_tensor_value_info("out6", TensorProto.FLOAT, [1, 2, 3, 4]),
        helper.make_tensor_value_info("out7", TensorProto.FLOAT, [1, 2, 3, 4]),
        helper.make_tensor_value_info("out8", TensorProto.FLOAT, [1, 4, 3, 4]),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
feed = {"a": a, "b": b, "a2": a2, "bscalar": bscalar, "a3": a3, "bchan": bchan,
        "a4": a4, "bw": bw, "a5": a5, "bhw": bhw, "a6": a6, "b6": b6}
# 写各输入 .f32
for nm, v in feed.items():
    v.tofile("deeplearning/data/onnx/bcast_in_%s.f32" % nm)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
outs = sess.run(None, feed)
# 拼接所有输出到单一 ORT bin（测试按 offset 读各输出）
blob = np.concatenate([o.flatten() for o in outs]).astype(np.float32)
blob.tofile(ORT_BIN)
print("wrote", OUT, "n_out", len(outs),
      "out1[0,0,0,0]", outs[0][0, 0, 0, 0], "out6[0,1,0,0]", outs[5][0, 1, 0, 0])
