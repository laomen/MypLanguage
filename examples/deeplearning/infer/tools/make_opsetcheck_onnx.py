#!/usr/bin/env python3
"""阶段七：ONNX opset/version 检查合成模型。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_opsetcheck_onnx.py
生成:
  deeplearning/data/onnx/opsetcheck_v13.onnx   （标准 opset 13）
  deeplearning/data/onnx/opsetcheck_v21.onnx   （标准 opset 21）
  deeplearning/data/onnx/opsetcheck_in.f32
  deeplearning/data/onnx/opsetcheck_ort.bin    （用 v13 模型跑 ORT 参考）

模型（Conv x[1,1,5,5] → y[1,1,3,3]）：
  同一结构分别导出 opset 13 与 opset 21，验证 loader 读出的 opsetVersion/
  irVersion 与模型声明一致、opsetSupported() 判断正确。
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

np.random.seed(70)
x = np.random.randn(1, 1, 5, 5).astype(np.float32)
w = np.random.randn(1, 1, 3, 3).astype(np.float32)
b = np.random.randn(1).astype(np.float32)


def make(path, opset):
    node = helper.make_node("Conv", ["x", "w", "b"], ["y"], kernel_shape=[3, 3], pads=[0, 0, 0, 0])
    graph = helper.make_graph(
        [node], "opsetcheck",
        inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 1, 5, 5])],
        outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 1, 3, 3])],
        initializer=[
            numpy_helper.from_array(w, name="w"),
            numpy_helper.from_array(b, name="b"),
        ],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", opset)])
    onnx.checker.check_model(model)
    onnx.save(model, path)
    return model


m13 = make("deeplearning/data/onnx/opsetcheck_v13.onnx", 13)
m21 = make("deeplearning/data/onnx/opsetcheck_v21.onnx", 21)
print("wrote v13 ir_version", m13.ir_version, "opset", m13.opset_import[0].version,
      "| v21 ir_version", m21.ir_version, "opset", m21.opset_import[0].version)

x.tofile("deeplearning/data/onnx/opsetcheck_in.f32")
import onnxruntime as ort
sess = ort.InferenceSession("deeplearning/data/onnx/opsetcheck_v13.onnx", providers=["CPUExecutionProvider"])
y = sess.run(None, {"x": x})[0]
y.tofile("deeplearning/data/onnx/opsetcheck_ort.bin")
print("y[0,0,0,0]", y[0, 0, 0, 0])
