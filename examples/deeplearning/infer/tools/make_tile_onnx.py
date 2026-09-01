#!/usr/bin/env python3
"""G5：Tile（沿各维按倍数复制）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_tile_onnx.py
生成:
  deeplearning/data/onnx/tile_test.onnx
  deeplearning/data/onnx/tile_in.f32
  deeplearning/data/onnx/tile_test_ort.bin

模型（opset13）: in[1,2,3,4] → Tile(repeats=[1,2,2,3]) → out[1,4,6,12]
  （H 3×2、W 4×3 复制；repeats 是 int64 初始器）
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/tile_test.onnx"
IN_F32 = "deeplearning/data/onnx/tile_in.f32"
ORT_BIN = "deeplearning/data/onnx/tile_test_ort.bin"

np.random.seed(53)
x = np.random.randn(1, 2, 3, 4).astype(np.float32)
repeats = np.array([1, 2, 2, 3], dtype=np.int64)

nodes = [
    helper.make_node("Tile", ["x", "repeats"], ["y"], name="tile1"),
]
graph = helper.make_graph(
    nodes, "tile_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2, 3, 4])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 4, 6, 12])],
    initializer=[numpy_helper.from_array(repeats, name="repeats")],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
x.tofile(IN_F32)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y = sess.run(None, {"x": x})[0]
y.tofile(ORT_BIN)
print("wrote", OUT, "y.shape", y.shape, "y[0,0,0,0]", y[0, 0, 0, 0],
      "y[0,1,5,11]", y[0, 1, 5, 11], "y[0,1,0,0]", y[0, 1, 0, 0])
