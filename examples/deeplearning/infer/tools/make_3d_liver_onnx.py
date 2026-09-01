#!/usr/bin/env python3
"""make_3d_liver_onnx.py — 生成 16³ 带跳跃连接 3D U-Net（阶段4g：类不平衡分割）

结构同 make_3d_unet_skip_onnx.py（8³）按 16³ 放大：
  输入 data [1,1,16,16,16]（NCDHW）
  编码：Conv3D(1→8)→ReLU→r1[1,8,16³](skip1) → Conv3D(8→8)→ReLU → MaxPool3D(→8³)
        → Conv3D(8→16)→ReLU→r3[1,16,8³](skip2) → Conv3D(16→16)→ReLU → MaxPool3D(→4³)
  解码：Resize3D 4→8 → Concat(r3)axis1 → [1,32,8³] → Conv3D(32→16)→ReLU
        → Resize3D 8→16 → Concat(r1)axis1 → [1,24,16³] → Conv3D(24→8)→ReLU
        → Conv3D(8→2) → Softmax → prob [1,2,16,16,16]

权重随机小初始化（训练入口从零训练）。
用法（须在 examples/ 下）：
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_3d_liver_onnx.py
"""
import numpy as np
import onnx
from onnx import helper, TensorProto

OUT = "deeplearning/data/onnx/3d_liver.onnx"
np.random.seed(77)


def w(name, shape, scale=0.1):
    return helper.make_tensor(name, TensorProto.FLOAT, list(shape),
                              (np.random.randn(*shape) * scale).astype(np.float32).flatten().tolist())


def b(name, n, scale=0.02):
    return helper.make_tensor(name, TensorProto.FLOAT, [n],
                              (np.random.randn(n) * scale).astype(np.float32).flatten().tolist())


def sz(name, vals):
    return helper.make_tensor(name, TensorProto.INT64, [len(vals)], [int(v) for v in vals])


nodes = [
    helper.make_node("Conv", ["data", "e1_w", "e1_b"], ["e1"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="enc_conv1"),
    helper.make_node("Relu", ["e1"], ["r1"], name="enc_relu1"),
    helper.make_node("Conv", ["r1", "e2_w", "e2_b"], ["e2"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="enc_conv2"),
    helper.make_node("Relu", ["e2"], ["r2"], name="enc_relu2"),
    helper.make_node("MaxPool", ["r2"], ["p1"],
                     kernel_shape=[2, 2, 2], strides=[2, 2, 2], name="enc_pool1"),
    helper.make_node("Conv", ["p1", "e3_w", "e3_b"], ["e3"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="enc_conv3"),
    helper.make_node("Relu", ["e3"], ["r3"], name="enc_relu3"),
    helper.make_node("Conv", ["r3", "e4_w", "e4_b"], ["e4"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="enc_conv4"),
    helper.make_node("Relu", ["e4"], ["r4"], name="enc_relu4"),
    helper.make_node("MaxPool", ["r4"], ["p2"],
                     kernel_shape=[2, 2, 2], strides=[2, 2, 2], name="enc_pool2"),
    helper.make_node("Resize", ["p2", "", "", "u1_sz"], ["u1"],
                     mode="linear", coordinate_transformation_mode="asymmetric", name="dec_up1"),
    helper.make_node("Concat", ["u1", "r3"], ["cat1"], axis=1, name="dec_cat1"),
    helper.make_node("Conv", ["cat1", "d1_w", "d1_b"], ["d1"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="dec_conv1"),
    helper.make_node("Relu", ["d1"], ["rd1"], name="dec_relu1"),
    helper.make_node("Resize", ["rd1", "", "", "u2_sz"], ["u2"],
                     mode="linear", coordinate_transformation_mode="asymmetric", name="dec_up2"),
    helper.make_node("Concat", ["u2", "r1"], ["cat2"], axis=1, name="dec_cat2"),
    helper.make_node("Conv", ["cat2", "d2_w", "d2_b"], ["d2"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="dec_conv2"),
    helper.make_node("Relu", ["d2"], ["rd2"], name="dec_relu2"),
    helper.make_node("Conv", ["rd2", "d3_w", "d3_b"], ["d3"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="dec_conv3"),
    helper.make_node("Softmax", ["d3"], ["prob"], axis=1, name="softmax1"),
]

inits = [
    w("e1_w", [8, 1, 3, 3, 3], 0.1), b("e1_b", 8, 0.02),
    w("e2_w", [8, 8, 3, 3, 3], 0.1), b("e2_b", 8, 0.02),
    w("e3_w", [16, 8, 3, 3, 3], 0.1), b("e3_b", 16, 0.02),
    w("e4_w", [16, 16, 3, 3, 3], 0.1), b("e4_b", 16, 0.02),
    w("d1_w", [16, 32, 3, 3, 3], 0.1), b("d1_b", 16, 0.02),
    w("d2_w", [8, 24, 3, 3, 3], 0.1), b("d2_b", 8, 0.02),
    w("d3_w", [2, 8, 3, 3, 3], 0.1), b("d3_b", 2, 0.02),
    sz("u1_sz", [1, 16, 8, 8, 8]),
    sz("u2_sz", [1, 16, 16, 16, 16]),
]

inputs = [helper.make_tensor_value_info("data", TensorProto.FLOAT, [1, 1, 16, 16, 16])]
outputs = [helper.make_tensor_value_info("prob", TensorProto.FLOAT, [1, 2, 16, 16, 16])]

graph = helper.make_graph(nodes, "3d_liver", inputs, outputs, initializer=inits)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 7
onnx.checker.check_model(model)
onnx.save(model, OUT)
print("wrote", OUT)
