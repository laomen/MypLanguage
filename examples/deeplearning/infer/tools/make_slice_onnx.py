#!/usr/bin/env python3
"""F8：生成合成 ONNX（Slice）+ ORT 交叉校验。

用法（仓库根，venv 解释器）:
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_slice_onnx.py
  生成:
    deeplearning/data/onnx/slice_test.onnx   # 单输入 x[2,3,4,5] → 三路 Slice 拼接
    deeplearning/data/onnx/slice_test.f32    # 随机输入（小端 f32）
    deeplearning/data/onnx/slice_test_ort.bin # ORT 输出（小端 f32）

模型结构（验证 Slice 各形态，opset14）:
  x[2,3,4,5] → s1 = Slice(axis=1, 1:3)            → [2,2,4,5]   （正区间）
            → s2 = Slice(axes=[1,3], [-3,-5]:[-1,MAX]) → [2,2,4,5]（负索引，end 开区间）
            → s3 = Slice(axis=3, -1:-6, step=-1) → [2,3,4,5]   （负 step 全反）
            → s4 = Slice(axis=1, 0:2)            → [2,2,4,5]   （普通区间）
  y = Concat([s1,s2,s3], axis=1) → [2,7,4,5] → Concat(..,s4) → [2,9,4,5]
  （框架 3 输入上限 → 拆两步）
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/slice_test.onnx"
IN_F32 = "deeplearning/data/onnx/slice_test.f32"
ORT_BIN = "deeplearning/data/onnx/slice_test_ort.bin"

np.random.seed(23)
x = np.random.randn(2, 3, 4, 5).astype(np.float32)

INT64_MAX = np.iinfo(np.int64).max

def i64(name, arr):
    return numpy_helper.from_array(np.array(arr, dtype=np.int64), name=name)

nodes = [
    # s1: axis=1, [1:3]
    helper.make_node("Slice", ["x", "s1_st", "s1_en", "s1_ax"], ["s1"], name="s1"),
    # s2: axes=[1,3], starts=[-3,-5], ends=[-1,INT64_MAX]
    helper.make_node("Slice", ["x", "s2_st", "s2_en", "s2_ax"], ["s2"], name="s2"),
    # s3: axis=3（W）全反向：starts=-1, ends=-6, steps=-1 → [2,3,4,5]
    helper.make_node("Slice", ["x", "s3_st", "s3_en", "s3_ax", "s3_sp"], ["s3"], name="s3"),
    # s4: axis=1, [0:2]
    helper.make_node("Slice", ["x", "s4_st", "s4_en", "s4_ax"], ["s4"], name="s4"),
    # 拼接（框架 3 输入上限，拆两步）
    helper.make_node("Concat", ["s1", "s2", "s3"], ["c1"], axis=1, name="cat1"),
    helper.make_node("Concat", ["c1", "s4"], ["y"], axis=1, name="cat2"),
]

inits = [
    i64("s1_st", [1]), i64("s1_en", [3]), i64("s1_ax", [1]),
    i64("s2_st", [-3, -5]), i64("s2_en", [-1, INT64_MAX]), i64("s2_ax", [1, 3]),
    i64("s3_st", [-1]), i64("s3_en", [-6]), i64("s3_ax", [3]), i64("s3_sp", [-1]),
    i64("s4_st", [0]), i64("s4_en", [2]), i64("s4_ax", [1]),
]

graph = helper.make_graph(
    nodes, "slice_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [2, 3, 4, 5])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [2, 9, 4, 5])],
    initializer=inits,
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
