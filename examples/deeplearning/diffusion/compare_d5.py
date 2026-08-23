#!/usr/bin/env python3
"""compare_d5.py — D5 对拍：MYP DDIM 采样 vs diffusers 参考。
- 每步 UNet 输出（噪声预测）：d5_eps_<i>.bin（diffusers） vs d5/myp_d5_eps_<0i>.bin（MYP）
- 最终 latent：d5_final.bin vs vae/latent_in.bin（MYP 写）
- 参考解码图：d5_ref_image.f32；MYP 解码图由 vae_decode.myp 生成后另比
用法：onnxvenv/bin/python deeplearning/diffusion/compare_d5.py
"""
import os
import numpy as np

DL = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D5 = os.path.join(DL, "data", "diffusion", "d5")
VAE = os.path.join(DL, "data", "diffusion", "vae")

# 找 MYP eps dump（myp_d5_eps_<i>.bin，两位零填充）
import glob
myp_eps = sorted(glob.glob(os.path.join(D5, "myp_d5_eps_*.bin")))
n = len(myp_eps)
if n == 0:
    print("no MYP eps dumps found — run ddim_sampler.myp (MYP_D5_MODE=ddim) first")
else:
    print(f"comparing {n} steps:")
    for p in myp_eps:
        idx = int(os.path.basename(p).split("_")[-1].split(".")[0])  # "00" → 0
        ref = np.fromfile(os.path.join(D5, f"d5_eps_{idx}.bin"), dtype=np.float32)
        myp = np.fromfile(p, dtype=np.float32)
        d = np.abs(ref - myp).max()
        rms = np.sqrt(np.mean((ref - myp) ** 2))
        print(f"  step {idx}: eps maxAbsDiff={d:.6e} rms={rms:.6e}")

# 最终 latent
if os.path.exists(os.path.join(VAE, "latent_in.bin")):
    fin_ref = np.fromfile(os.path.join(D5, "d5_final.bin"), dtype=np.float32)
    fin_myp = np.fromfile(os.path.join(VAE, "latent_in.bin"), dtype=np.float32)
    d = np.abs(fin_ref - fin_myp).max()
    print(f"final latent maxAbsDiff={d:.6e} (ref_std={fin_ref.std():.4f})")
    print(f"  ref[0,:4]={fin_ref[:4].tolist()}")
    print(f"  myp[0,:4]={fin_myp[:4].tolist()}")
    if d < 5e-3:
        print("DDIM FINAL LATENT OK")
    else:
        print("DDIM FINAL LATENT FAIL")

# 解码图像（VAE 解码后 myp_image.f32 vs d5_ref_image.f32）
if os.path.exists(os.path.join(D5, "myp_image.f32")):
    img_ref = np.fromfile(os.path.join(D5, "d5_ref_image.f32"), dtype=np.float32).reshape(3, 512, 512)
    img_myp = np.fromfile(os.path.join(D5, "myp_image.f32"), dtype=np.float32).reshape(3, 512, 512)
    d = np.abs(img_ref - img_myp).max()
    # 相对误差（图像值域 ~[-1.5,1.5]）
    rel = d / max(img_ref.std(), 1e-6)
    print(f"decoded image maxAbsDiff={d:.6f} (ref_std={img_ref.std():.4f}, rel={rel:.4f})")
    print(f"  ref[0,0,:4]={img_ref[0,0,:4].tolist()}")
    print(f"  myp[0,0,:4]={img_myp[0,0,:4].tolist()}")
    if d < 0.05:
        print("D5 IMAGE OK")
    else:
        print("D5 IMAGE FAIL")
