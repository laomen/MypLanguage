#!/usr/bin/env python3
"""Cross-validate the pure-MYP ONNX reader against onnxruntime on MNIST MLP.

Runs the same first-100 MNIST samples through onnxruntime and compares
accuracy + sample-0 probability vector with the MYP output (onnx_main.myp).
"""
import struct
import numpy as np
import onnxruntime as ort

ONNX = "deeplearning/data/onnx/mnist_mlp.onnx"
IMGS = "deeplearning/data/t10k-images-idx3-ubyte/t10k-images.idx3-ubyte"
LABS = "deeplearning/data/t10k-labels-idx1-ubyte/t10k-labels.idx1-ubyte"


def load_images(path, n):
    with open(path, "rb") as f:
        magic, count, rows, cols = struct.unpack(">IIII", f.read(16))
        data = np.frombuffer(f.read(), dtype=np.uint8).reshape(count, rows * cols)
    return data[:n].astype(np.float32) / 255.0


def load_labels(path, n):
    with open(path, "rb") as f:
        magic, count = struct.unpack(">II", f.read(8))
        return np.frombuffer(f.read(), dtype=np.uint8)[:n]


def main():
    imgs = load_images(IMGS, 100)
    labs = load_labels(LABS, 100)

    sess = ort.InferenceSession(ONNX, providers=["CPUExecutionProvider"])
    iname = sess.get_inputs()[0].name
    oname = sess.get_outputs()[0].name

    probs = []
    correct = 0
    for i in range(100):
        p = sess.run([oname], {iname: imgs[i : i + 1]})[0][0]
        probs.append(p)
        if int(p.argmax()) == int(labs[i]):
            correct += 1

    print(f"onnxruntime accuracy: {correct}/100")
    print("sample0 prob:", " ".join(f"{v:.5e}" for v in probs[0]))
    print("sample0 argmax:", int(probs[0].argmax()), "label:", int(labs[0]))


if __name__ == "__main__":
    main()
