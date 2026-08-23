#!/usr/bin/env python3
"""extract_d5_ref.py — 阶段 D5：端到端 DDIM 采样参考（diffusers 真值）+ 解码参考图。

输入（复用既有抽取产物）：
  data/diffusion/unet/weights.bin, bases.bin, time_emb.bin（MYP 用）
  data/diffusion/unet/latent_in.bin [4,64,64]（作 x_T，rng(7) 固定）
  data/diffusion/clip/ref_emb.bin   [77,768]（文本嵌入，作 cross-attn context）
输出（data/diffusion/d5/）：
  d5_tsteps.bin    i32×N        DDIM 时间步（降序，diffusers 0.39 整数步长）
  d5_alphas.bin    fp32×1000    alphas_cumprod
  d5_eps_<i>.bin   fp32×16384   第 i 步 UNet 输出（噪声预测，MYP 逐步对拍）
  d5_latent_<i>.bin fp32×16384  第 i 步后的 latent
  d5_final.bin     fp32×16384   最终 latent
  d5_ref_image.f32 fp32×[3,512,512] 最终 latent 解码图（原始值）
  d5_image.ppm     解码图（P6，像素 = clamp((v/2+0.5)*255)）
用法：onnxvenv/bin/python deeplearning/diffusion/extract_d5_ref.py [N]
  N 缺省 50；传小 N（如 5）用于逐步对拍验证。
"""
import os
import sys
import numpy as np
import torch
from diffusers import UNet2DConditionModel, AutoencoderKL, DDIMScheduler

DL = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SD = os.path.join(DL, "data", "diffusion", "sd15")
UOUT = os.path.join(DL, "data", "diffusion", "unet")
DOUT = os.path.join(DL, "data", "diffusion", "d5")
os.makedirs(DOUT, exist_ok=True)

N = int(sys.argv[1]) if len(sys.argv) > 1 else 50
L = 16384  # 4*64*64

print(f"loading UNet2DConditionModel (fp32) ...")
unet = UNet2DConditionModel.from_pretrained(os.path.join(SD, "unet"), torch_dtype=torch.float32).to("cpu")
unet.eval()
print("loading AutoencoderKL ...")
vae = AutoencoderKL.from_pretrained(os.path.join(SD, "vae"), torch_dtype=torch.float32).to("cpu")
vae.eval()

# 调度器（SD1.5 默认 beta 调度，与 D1 一致）
sched = DDIMScheduler.from_pretrained(os.path.join(SD, "scheduler"))
sched.set_timesteps(N)
tsteps = sched.timesteps.numpy().astype(np.int32)          # [980,960,...]
alpha_cp = sched.alphas_cumprod.numpy().astype(np.float32)  # [1000]
alpha_cp.tofile(os.path.join(DOUT, "d5_alphas.bin"))
# 时间步文件带计数前缀（i32 count + i32[N]，与 D1 sched_tsteps.bin 同格式）
with open(os.path.join(DOUT, "d5_tsteps.bin"), "wb") as f:
    f.write(np.int32(len(tsteps)).tobytes())
    f.write(tsteps.tobytes())
print(f"timesteps={tsteps.tolist()}")

# 初始 latent x_T = latent_in.bin（rng(7) 固定随机）
x = torch.from_numpy(np.fromfile(os.path.join(UOUT, "latent_in.bin"), dtype=np.float32)).view(1, 4, 64, 64)

# 文本嵌入 [1,77,768]（位置主序；来自 CLIP ref_emb）
clip_emb = np.fromfile(os.path.join(DL, "data", "diffusion", "clip", "ref_emb.bin"), dtype=np.float32).reshape(77, 768)
txt = torch.from_numpy(clip_emb.astype(np.float32)).unsqueeze(0)  # [1,77,768]

with torch.no_grad():
    for i, t in enumerate(tsteps):
        tts = torch.tensor([int(t)], dtype=torch.long)
        eps = unet(x, tts, encoder_hidden_states=txt).sample            # [1,4,64,64]
        eps.numpy()[0].reshape(-1).tofile(os.path.join(DOUT, f"d5_eps_{i}.bin"))
        x = sched.step(eps, int(t), x).prev_sample                     # DDIM η=0
        x.numpy()[0].reshape(-1).tofile(os.path.join(DOUT, f"d5_latent_{i}.bin"))
        print(f"step {i}: t={int(t)} latent_std={x.std().item():.4f}")
    x.numpy()[0].reshape(-1).tofile(os.path.join(DOUT, "d5_final.bin"))

# 解码最终 latent → 图像（参考：VAE decode，无 extra scaling）
with torch.no_grad():
    dec = vae.decode(x / vae.config.scaling_factor).sample             # [1,3,512,512]
img = dec.numpy()[0].astype(np.float32)                                # [3,512,512]
img.reshape(-1).tofile(os.path.join(DOUT, "d5_ref_image.f32"))

# PPM P6：像素 = clamp((v/2+0.5)*255)
chw = img
rgb = np.clip((chw / 2.0 + 0.5) * 255.0, 0, 255).astype(np.uint8)      # [3,512,512]
hwc = rgb.transpose(1, 2, 0)                                           # [512,512,3]
with open(os.path.join(DOUT, "d5_image.ppm"), "wb") as f:
    f.write(b"P6\n512 512\n255\n")
    f.write(hwc.tobytes())
print(f"d5_image.ppm written; pixel[0,0]={hwc[0,0].tolist()}")
print("D5 reference done.")
