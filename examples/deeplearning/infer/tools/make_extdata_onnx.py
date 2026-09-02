#!/usr/bin/env python3
"""阶段七：ONNX external data（外置权重）合成模型 + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_extdata_onnx.py
生成:
  deeplearning/data/onnx/extdata_test.onnx        （权重外置）
  deeplearning/data/onnx/extdata_test.bin         （external data：w/b 字节）
  deeplearning/data/onnx/extdata_in.f32
  deeplearning/data/onnx/extdata_test_ort.bin

模型（opset13）：x[1,1,5,5] → Conv(w[1,1,3,3], b[1]) → y[1,1,3,3]
用 convert_model_to_external_data 把 w/b 移到独立 .bin 文件。
验证：loader 识别 external data、读外部文件追加、输出 vs ORT 一致。
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

np.random.seed(71)
x = np.random.randn(1, 1, 5, 5).astype(np.float32)
w = np.random.randn(1, 1, 3, 3).astype(np.float32)
b = np.random.randn(1).astype(np.float32)

node = helper.make_node("Conv", ["x", "w", "b"], ["y"], kernel_shape=[3, 3], pads=[0, 0, 0, 0])
graph = helper.make_graph(
    [node], "extdata_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 1, 5, 5])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 1, 3, 3])],
    initializer=[
        numpy_helper.from_array(w, name="w"),
        numpy_helper.from_array(b, name="b"),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
# onnx 1.22 的 save_model 会把 external tensor 写回内联（reload 后 data_location=0），
# write_external_data_tensors 又有路径解析问题。因此完全手动：
#  1) 自写外部数据文件 extdata_test.bin（w 前 36B + b 后 4B = 40B）
#  2) 手动填 external_data（location/offset/length）+ data_location=EXTERNAL + 清 raw_data
#  3) SerializeToString 手写 onnx 文件，确保 EXTERNAL 落盘。
from onnx.external_data_helper import set_external_data
bin_path = "deeplearning/data/onnx/extdata_test.bin"
with open(bin_path, "wb") as f:
    f.write(w.tobytes())
    f.write(b.tobytes())
exts = []          # (tensor, offset, length)
off = 0
for t in model.graph.initializer:
    n = int(np.prod(t.dims)) * 4
    exts.append((t, off, n))
    off += n
for t, o, ln in exts:
    set_external_data(t, "extdata_test.bin", offset=o, length=ln)
    t.data_location = TensorProto.EXTERNAL
    t.ClearField("raw_data")
with open("deeplearning/data/onnx/extdata_test.onnx", "wb") as f:
    f.write(model.SerializeToString())

import onnxruntime as ort
x.tofile("deeplearning/data/onnx/extdata_in.f32")
sess = ort.InferenceSession("deeplearning/data/onnx/extdata_test.onnx", providers=["CPUExecutionProvider"])
y = sess.run(None, {"x": x})[0]
y.tofile("deeplearning/data/onnx/extdata_test_ort.bin")
print("wrote extdata_test.onnx + extdata_test.bin, y[0,0,0,0]", y[0, 0, 0, 0])
