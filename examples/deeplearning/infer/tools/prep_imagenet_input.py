#!/usr/bin/env python3
"""prep_imagenet_input.py — 把真实图片预处理成 ResNet50 的 f32 输入。

流程（ImageNet 标准）：resize 短边=256 → 中心裁剪 224x224 → RGB → [0,1] →
按 mean/std 归一化 → 写 NCHW float32 到 resnet_input.f32。

用法：
  python prep_imagenet_input.py <image> [out.f32]
"""
import sys
import numpy as np
from PIL import Image

MEAN = np.array([0.485, 0.456, 0.406], dtype=np.float32).reshape(3, 1, 1)
STD = np.array([0.229, 0.224, 0.225], dtype=np.float32).reshape(3, 1, 1)


def preprocess(path):
    img = Image.open(path).convert("RGB")
    w, h = img.size
    # 短边缩放到 256（保持宽高比）
    if h < w:
        nh, nw = 256, int(256 * w / h)
    else:
        nw, nh = 256, int(256 * h / w)
    img = img.resize((nw, nh), Image.BILINEAR)
    # 中心裁剪 224x224
    left = (nw - 224) // 2
    top = (nh - 224) // 2
    img = img.crop((left, top, left + 224, top + 224))
    arr = np.asarray(img, dtype=np.float32).transpose(2, 0, 1)  # CHW
    arr = arr / 255.0
    arr = (arr - MEAN) / STD
    return arr[None]  # NCHW, [1,3,224,224]


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "deeplearning/data/imagenet/picsum.jpg"
    dst = sys.argv[2] if len(sys.argv) > 2 else "deeplearning/data/onnx/resnet_input.f32"
    x = preprocess(src)
    x.tofile(dst)
    print(f"wrote {dst} {x.shape} ({x.nbytes} bytes) from {src}")


if __name__ == "__main__":
    main()
