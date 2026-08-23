#!/usr/bin/env python3
"""analyze_d5.py — 定位 DDIM latent 分歧来源。
- 用 MYP 每步 eps dump + diffusers alphas/tsteps 重建 latent 轨迹（纯 DDIM 数学）
- 与 MYP 最终 latent（vae/latent_in.bin）对比 → 验证 MYP DDIM 更新自洽
- 与 diffusers 每步 latent（d5_latent_<i>.bin）对比 → 观察分歧增长
- 打印每步放大因子 sqrt(1-α)/sqrt(α)（低 α 步放大 eps 误差）
"""
import os
import numpy as np
import glob

DL = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D5 = os.path.join(DL, "data", "diffusion", "d5")
VAE = os.path.join(DL, "data", "diffusion", "vae")

alpha = np.fromfile(os.path.join(D5, "d5_alphas.bin"), dtype=np.float32)
with open(os.path.join(D5, "d5_tsteps.bin"), "rb") as f:
    n = np.frombuffer(f.read(4), dtype=np.int32)[0]
    ts = np.frombuffer(f.read(4 * n), dtype=np.int32)

L = 16384
x = np.fromfile(os.path.join(DL, "data", "diffusion", "unet", "latent_in.bin"), dtype=np.float32).copy().astype(np.float64)
myp_eps = sorted(glob.glob(os.path.join(D5, "myp_d5_eps_*.bin")))
print(f"steps={ts.tolist()}  alpha_cumprod[t]={[f'{alpha[t]:.6f}' for t in ts]}")
print(f"amplification sqrt(1-a)/sqrt(a) per step: {[f'{np.sqrt(1-alpha[t])/np.sqrt(alpha[t]):.3f}' for t in ts]}")

for i, t in enumerate(ts):
    prev_t = ts[i + 1] if i + 1 < len(ts) else -1
    at = float(alpha[t])
    # 末步 prev_t=-1：α_prev 用 alphas_cumprod[0]（SD1.5 set_alpha_to_one=false）
    aPrev = float(alpha[prev_t]) if prev_t >= 0 else float(alpha[0])
    eps = np.fromfile(myp_eps[i], dtype=np.float32).astype(np.float64)
    x0 = (x - np.sqrt(1 - at) * eps) / np.sqrt(at)
    xn = np.sqrt(aPrev) * x0 + np.sqrt(1 - aPrev) * eps
    # 与 diffusers 该步 latent 对比（divergence 从哪步开始）
    ref_lat = np.fromfile(os.path.join(D5, f"d5_latent_{i}.bin"), dtype=np.float32)
    d = np.abs(xn - ref_lat).max()
    print(f"step {i} t={t}: reconstructed-vs-diffusers maxAbsDiff={d:.6e}  latent_std={xn.std():.4f}")
    x = xn

# 重建的最终 vs MYP 实际写的最终（自洽性：应为 ~0）
myp_final = np.fromfile(os.path.join(VAE, "latent_in.bin"), dtype=np.float32).astype(np.float64)
print(f"reconstructed-final vs MYP vae/latent_in.bin maxAbsDiff={np.abs(x - myp_final).max():.6e}  (应~0 → MYP DDIM 更新自洽)")
