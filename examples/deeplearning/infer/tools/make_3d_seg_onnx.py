#!/usr/bin/env python3
"""make_3d_seg_onnx.py — 生成小型 3D 分割 ONNX（阶段4d Dice 训练夹具）。

结构：Conv3D(1→4,k3,p1) → ReLU → Conv3D(4→2,k3,p1) → Softmax(prob)。
输入 [1,1,8,8,8]（NCDHW），2 类体素分割（中心块=前景 / 其余=背景）。
权重随机小初始化（训练入口从零训练）。

用法（须在 examples/ 下）：
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_3d_seg_onnx.py
"""
import numpy as np
import onnx
from onnx import helper, TensorProto

OUT = "deeplearning/data/onnx/3d_seg.onnx"
np.random.seed(33)


def w(name, shape, scale=0.1):
    return helper.make_tensor(name, TensorProto.FLOAT, list(shape),
                              (np.random.randn(*shape) * scale).astype(np.float32).flatten().tolist())


nodes = [
    helper.make_node("Conv", ["data", "c1_w", "c1_b"], ["c1"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="conv1"),
    helper.make_node("Relu", ["c1"], ["r1"], name="relu1"),
    helper.make_node("Conv", ["r1", "c2_w", "c2_b"], ["c2"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="conv2"),
    helper.make_node("Softmax", ["c2"], ["prob"], axis=1, name="softmax1"),
]

inits = [
    w("c1_w", [4, 1, 3, 3, 3], 0.1), w("c1_b", [4], 0.02),
    w("c2_w", [2, 4, 3, 3, 3], 0.1), w("c2_b", [2], 0.02),
]

inputs = [helper.make_tensor_value_info("data", TensorProto.FLOAT, [1, 1, 8, 8, 8])]
outputs = [helper.make_tensor_value_info("prob", TensorProto.FLOAT, [1, 2, 8, 8, 8])]

graph = helper.make_graph(nodes, "3d_seg", inputs, outputs, initializer=inits)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 7
onnx.checker.check_model(model)
onnx.save(model, OUT)
print("wrote", OUT)
