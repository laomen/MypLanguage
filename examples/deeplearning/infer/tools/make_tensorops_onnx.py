#!/usr/bin/env python3
"""F8：生成合成 ONNX（Concat / Reshape / Transpose）+ ORT 交叉校验。

用法（仓库根，venv 解释器）:
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_tensorops_onnx.py
  生成:
    deeplearning/data/onnx/tensorops_test.onnx    # 4D NCHW 路径
    deeplearning/data/onnx/tensorops_test.f32     # 随机输入（小端 f32）
    deeplearning/data/onnx/tensorops_test_ort.bin # ORT 输出（小端 f32）

模型结构（验证 F8 三算子，全部 4D NCHW）:
  x[1,2,4,4] + x2[1,2,4,4] → Concat(axis=1) → [1,4,4,4]
             → Reshape [1,16,2,2]    （含 -1：总元素 64 → [1,16,2,2]）
             → Transpose perm=[0,2,3,1] → [1,2,2,16]
             → Reshape [1,0,0,-1]    （0=复制维[2,2]，-1=推断 16）→ [1,2,2,16]
             → 输出 y[1,2,2,16]
"""
import struct
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/tensorops_test.onnx"
IN_F32 = "deeplearning/data/onnx/tensorops_test.f32"
ORT_BIN = "deeplearning/data/onnx/tensorops_test_ort.bin"

np.random.seed(42)

# ---- 输入张量 ----
x = np.random.randn(1, 2, 4, 4).astype(np.float32)     # 主输入
x2 = np.random.randn(1, 2, 4, 4).astype(np.float32)    # concat 第二输入（用 Constant）

def f32_tensor(name, arr):
    return numpy_helper.from_array(arr, name=name)

# ---- 节点 ----
nodes = [
    helper.make_node("Concat", ["x", "x2"], ["c1"], axis=1, name="concat1"),
    # Reshape 1：shape 初始器 [1,16,2,2]
    helper.make_node("Reshape", ["c1", "shape1"], ["r1"], name="reshape1"),
    # Transpose：perm=[0,2,3,1]
    helper.make_node("Transpose", ["r1"], ["t1"], perm=[0, 2, 3, 1], name="transpose1"),
    # Reshape 2：shape 初始器 [1,0,0,-1]（0=复制输入维[2,2]，-1=推断 16）
    helper.make_node("Reshape", ["t1", "shape2"], ["y"], name="reshape2"),
]

# ---- 初始器：Reshape 的 shape 是 int64 初始器；x2 为 4D Constant（测试常量拼接路径）----
shape1 = numpy_helper.from_array(np.array([1, 16, 2, 2], dtype=np.int64), name="shape1")
shape2 = numpy_helper.from_array(np.array([1, 0, 0, -1], dtype=np.int64), name="shape2")
const_x2 = numpy_helper.from_array(x2, name="x2")

graph = helper.make_graph(
    nodes, "tensorops_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2, 4, 4])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 2, 2, 16])],
    initializer=[shape1, shape2, const_x2],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 14)])
model.ir_version = 8
onnx.checker.check_model(model)
onnx.save(model, OUT)
print("wrote", OUT)

# ---- ORT 参考输出 ----
import onnxruntime as ort
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y = sess.run(["y"], {"x": x, "x2": x2})[0]
print("ORT y shape:", y.shape)
print("ORT y sum:", float(y.sum()), "y[0,0,0,:4]:", y[0, 0, 0, :4])

# ---- 写输入 .f32 与 ORT 输出 .bin ----
with open(IN_F32, "wb") as f:
    f.write(x.tobytes())
with open(ORT_BIN, "wb") as f:
    f.write(y.astype(np.float32).tobytes())
print("wrote", IN_F32, "and", ORT_BIN)
