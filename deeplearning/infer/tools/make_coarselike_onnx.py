#!/usr/bin/env python3
"""P3/P4：合成「类 coarse」3D 模型 —— 复刻 coarse_model 预处理 + 归一化 + 小卷积块。
验证：Shape→Slice→Concat→Resize(sizes) 折叠、Clip、ReduceMean(all)、
Sub/Mul/Div/Sqrt/Add 标量广播、Cast(int64→float)、Conv3D、InstanceNorm、MaxPool3D。
"""
import numpy as np, onnx, os
from onnx import helper, TensorProto, numpy_helper
import onnxruntime as ort

# 尺寸参数：IN_D=输入边长，RES_D=Resize 目标边长（默认 4→8，可设 16→32 测中尺度）
IN_D = int(os.environ.get("COARSELIKE_IN_D", "4"))
RES_D = int(os.environ.get("COARSELIKE_RES_D", "8"))
TAG = os.environ.get("COARSELIKE_TAG", "")

OUT = f"deeplearning/data/onnx/coarselike{TAG}.onnx"
IN_F32 = f"deeplearning/data/onnx/coarselike{TAG}_in.f32"
ORT_BIN = f"deeplearning/data/onnx/coarselike{TAG}_ort.bin"

np.random.seed(11)
x = np.random.randn(1, 1, IN_D, IN_D, IN_D).astype(np.float32)
w = (np.random.randn(2, 1, 3, 3, 3) * 0.3).astype(np.float32)
b = np.array([0.1, -0.2], dtype=np.float32)
scale = np.array([1.3, 0.8], dtype=np.float32)
bias = np.array([0.05, -0.1], dtype=np.float32)

def const_i64(name, arr):
    return numpy_helper.from_array(np.array(arr, dtype=np.int64), name=name)

def const_f32(name, arr):
    return numpy_helper.from_array(np.array(arr, dtype=np.float32), name=name)

nodes = [
    # Shape→Slice→Concat → Resize sizes [1,1,8,8,8]
    helper.make_node("Shape", ["x"], ["shp"]),
    helper.make_node("Slice", ["shp", "st", "en", "ax"], ["slc"]),
    helper.make_node("Concat", ["slc", "dst"], ["sz"], axis=0),
    helper.make_node("Resize", ["x", "roi", "scales", "sz"], ["r"], mode="linear", coordinate_transformation_mode="align_corners"),
    # Clip
    helper.make_node("Clip", ["r", "minv", "maxv"], ["c"]),
    # 归一化（复刻 coarse 节点 10-26）
    helper.make_node("ReduceMean", ["c"], ["m1"], keepdims=0),
    helper.make_node("Sub", ["c", "m1"], ["ctr"]),
    helper.make_node("Shape", ["c"], ["shp2"]),
    helper.make_node("ReduceProd", ["shp2"], ["cnt"]),
    helper.make_node("Cast", ["cnt"], ["cntf"], to=1),
    helper.make_node("Mul", ["ctr", "ctr"], ["sq"]),
    helper.make_node("ReduceMean", ["sq"], ["var"], keepdims=0),
    helper.make_node("Mul", ["var", "cntf"], ["ssq"]),
    helper.make_node("Sub", ["cntf", "one"], ["cm1"]),
    helper.make_node("Div", ["ssq", "cm1"], ["svar"]),
    helper.make_node("Sqrt", ["svar"], ["std"]),
    helper.make_node("Add", ["std", "eps"], ["den"]),
    helper.make_node("Div", ["ctr", "den"], ["norm"]),
    # 小卷积块
    helper.make_node("Conv", ["norm", "w3d", "b3d"], ["cv"], strides=[1,1,1], pads=[1,1,1,1,1,1]),
    helper.make_node("InstanceNormalization", ["cv", "in_scale", "in_bias"], ["in3"]),
    helper.make_node("Relu", ["in3"], ["rl"]),
    helper.make_node("MaxPool", ["rl"], ["y"], kernel_shape=[2,2,2], strides=[2,2,2]),
]

graph = helper.make_graph(
    nodes, "coarselike",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 1, IN_D, IN_D, IN_D])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 2, RES_D // 2, RES_D // 2, RES_D // 2])],
    initializer=[
        const_i64("st", [0]), const_i64("en", [2]), const_i64("ax", [0]),
        const_i64("dst", [RES_D, RES_D, RES_D]),
        const_f32("roi", []), const_f32("scales", []),
        const_f32("minv", [0.0]), const_f32("maxv", [6.0]),
        const_f32("one", [1.0]), const_f32("eps", [1.0e-5]),
        numpy_helper.from_array(w, name="w3d"), numpy_helper.from_array(b, name="b3d"),
        numpy_helper.from_array(scale, name="in_scale"), numpy_helper.from_array(bias, name="in_bias"),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 12)])
model.ir_version = 8
onnx.checker.check_model(model)
onnx.save(model, OUT)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y = sess.run(["y"], {"x": x})[0]
print("ORT y shape:", y.shape, "sum:", float(y.sum()))
with open(IN_F32, "wb") as f: f.write(x.tobytes())
with open(ORT_BIN, "wb") as f: f.write(y.astype(np.float32).tobytes())
print("wrote refs")
