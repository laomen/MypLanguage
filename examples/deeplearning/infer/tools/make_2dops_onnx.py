#!/usr/bin/env python3
"""F8：生成合成 ONNX（Sub/Div/Mul/Sqrt/ReduceMean/InstanceNorm/Resize）+ ORT 交叉校验。

用法（仓库根，venv 解释器）:
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_2dops_onnx.py
  生成:
    deeplearning/data/onnx/ops2d_test.onnx
    deeplearning/data/onnx/ops2d_test.f32
    deeplearning/data/onnx/ops2d_test_ort.bin

模型（opset14）:
  x[1,3,8,8] → InstanceNorm(scale,bias) → [1,3,8,8]
            → Resize(nearest, asymmetric, scale 2) → [1,3,16,16]
            → ReduceMean(axes=[2,3], keepdims=1) → m[1,3,1,1]
            → Sub(x2, m) → Sub[1,3,16,16]   （标量广播：m 为 [1,3,1,1]）
            → Mul(s, s) → sq[1,3,16,16]
            → ReduceMean(sq, all) → v[1]     （全局均值）
            → Sqrt(v) → std[1]
            → Div(s, std) → y[1,3,16,16]     （标量广播）
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/ops2d_test.onnx"
IN_F32 = "deeplearning/data/onnx/ops2d_test.f32"
ORT_BIN = "deeplearning/data/onnx/ops2d_test_ort.bin"

np.random.seed(31)
x = np.random.randn(1, 3, 8, 8).astype(np.float32)
scale = np.array([1.2, 0.8, 1.5], dtype=np.float32)
bias = np.array([0.1, -0.2, 0.3], dtype=np.float32)
scales = np.array([1.0, 1.0, 2.0, 2.0], dtype=np.float32)

def i64(name, arr):
    return numpy_helper.from_array(np.array(arr, dtype=np.int64), name=name)

nodes = [
    helper.make_node("InstanceNormalization", ["x", "in_scale", "in_bias"], ["y1"], epsilon=1e-5, name="in"),
    helper.make_node("Resize", ["y1", "", "rs_scales"], ["y2"],
                     mode="nearest", coordinate_transformation_mode="asymmetric", nearest_mode="floor", name="rs"),
    helper.make_node("ReduceMean", ["y2"], ["m"], axes=[2, 3], keepdims=1, name="rm1"),
    helper.make_node("Sub", ["y2", "m"], ["s"], name="sub"),
    helper.make_node("Mul", ["s", "s"], ["sq"], name="mul"),
    helper.make_node("ReduceMean", ["sq"], ["v"], keepdims=0, name="rm2"),
    helper.make_node("Sqrt", ["v"], ["std"], name="sqrt"),
    helper.make_node("Div", ["s", "std"], ["y"], name="div"),
]

graph = helper.make_graph(
    nodes, "ops2d_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 3, 8, 8])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 3, 16, 16])],
    initializer=[
        numpy_helper.from_array(scale, name="in_scale"),
        numpy_helper.from_array(bias, name="in_bias"),
        numpy_helper.from_array(scales, name="rs_scales"),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 14)])
model.ir_version = 8
onnx.checker.check_model(model)
onnx.save(model, OUT)
print("wrote", OUT)

import onnxruntime as ort
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y = sess.run(["y"], {"x": x})[0]
print("ORT y shape:", y.shape, "sum:", float(y.sum()))

with open(IN_F32, "wb") as f:
    f.write(x.tobytes())
with open(ORT_BIN, "wb") as f:
    f.write(y.astype(np.float32).tobytes())
print("wrote", IN_F32, "and", ORT_BIN)
