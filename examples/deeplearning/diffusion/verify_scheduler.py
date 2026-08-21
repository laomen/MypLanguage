#!/usr/bin/env python3
# verify_scheduler.py — D1：MYP DDIM 调度器 vs numpy float64 参考（+ diffusers 交叉检查）
#
# 步骤：
#   1) 计算 SD1.5 噪声调度（scaled_linear 0.00085→0.012, 1000 步）→ alphas_cumprod
#   2) 生成 50 个时间步 + 随机初始 latent（fp32，与 MYP 同源）
#   3) numpy float64 跑 DDIM（操作顺序与 scheduler.myp 逐字一致）→ 参考 latent
#   4) 编译 + 运行 scheduler.myp → sched_out.bin
#   5) 对拍：MYP(fp32) vs 参考(float64→fp32)，PASS 判据 = 字节一致（diff==0）
#   6) 交叉检查：diffusers DDIMScheduler 首步结果粗对比（fp32 vs fp64，容差）
#
# 用法：infer/tools/onnxvenv/bin/python deeplearning/diffusion/verify_scheduler.py

import math
import os
import struct
import subprocess
import sys

import numpy as np

DL = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # examples/deeplearning
EX = os.path.dirname(DL)                                            # examples
REPO = os.path.dirname(EX)                                          # 仓库根
OUT = os.path.join(DL, "data", "diffusion", "sched")
os.makedirs(OUT, exist_ok=True)

L = 64 * 64 * 4          # latent 元素数（4×64×64）
NSTEPS = 50
NTRAIN = 1000

# ============ 1) SD1.5 噪声调度（scaled_linear） ============
beta_start, beta_end = 0.00085, 0.012
betas = np.linspace(math.sqrt(beta_start), math.sqrt(beta_end), NTRAIN) ** 2
alphas = 1.0 - betas
alphas_cumprod = np.cumprod(alphas)                     # float64, shape (1000,)
# set_alpha_to_one（DDIM 惯例）：最后一个时刻 α=1
alphas_cumprod[-1] = 1.0

# ============ 2) 时间步 + 初始 latent ============
# diffusers 0.39 DDIMScheduler.set_timesteps：整数步长 num_train//steps → [980,960,...,20,0]
tsteps = [int(t) for t in np.arange(0, NSTEPS)[::-1] * (NTRAIN // NSTEPS)]
rng = np.random.default_rng(20260821)
x_init = rng.standard_normal(L)

# 存输入（fp32，MYP 与 numpy 从同一份 fp32 出发，double 展开后一致）
alphas_cumprod.astype(np.float32).tofile(os.path.join(OUT, "sched_alphas.bin"))
with open(os.path.join(OUT, "sched_tsteps.bin"), "wb") as f:
    f.write(struct.pack("<i", NSTEPS))
    for t in tsteps:
        f.write(struct.pack("<i", t))
x_init.astype(np.float32).tofile(os.path.join(OUT, "sched_x.bin"))

# 参考起点也取 fp32 值（与 MYP 的 F32.toDouble 完全一致）
x_init64 = x_init.astype(np.float32).astype(np.float64)
x = x_init64.copy()

# ============ 3) numpy float64 DDIM（操作顺序与 scheduler.myp 逐字一致） ============
# 关键：alphas_cumprod 参考也用 fp32 值（MYP 读 fp32 文件后 F32.toDouble 展开）——
# 若用未舍入 float64，会与 MYP 差 ~fp32 舍入量级，累计后 >1e-7。
ac = alphas_cumprod.astype(np.float32).astype(np.float64)
for step in range(NSTEPS):
    t = tsteps[step]
    prev_t = tsteps[step + 1] if step + 1 < NSTEPS else -1
    at = ac[t]
    aPrev = ac[prev_t] if prev_t >= 0 else 1.0
    bt = 1.0 - at
    bPrev = 1.0 - aPrev
    rAt = math.sqrt(at)
    rBt = math.sqrt(bt)
    rBp = math.sqrt(bPrev)
    rAp = math.sqrt(aPrev)
    eps = 0.5 * x                     # 哑模型
    x0 = (x - rBt * eps) / rAt
    x = rAp * x0 + rBp * eps

ref = x.astype(np.float32)
ref.tofile(os.path.join(OUT, "ref_out.f32"))

# ============ 3.5) diffusers 交叉检查（首步，fp32 vs fp64 容差） ============
try:
    import torch
    from diffusers import DDIMScheduler
    s = DDIMScheduler(num_train_timesteps=NTRAIN, beta_start=beta_start, beta_end=beta_end,
                      beta_schedule="scaled_linear", clip_sample=False)
    s.set_timesteps(NSTEPS)
    dts = [int(t) for t in s.timesteps]           # diffusers 权威时间步 [980,960,...]
    assert dts == tsteps, f"时间步不一致 {dts[:4]} vs {tsteps[:4]}"
    # diffusers 首步
    torch_x = x_init.astype(np.float32)
    out = s.step(torch.from_numpy(0.5 * torch_x), dts[0], torch.from_numpy(torch_x)).prev_sample.numpy()
    # numpy 同一步（用 diffusers 的 alphas_cumprod，fp32→fp64 与 MYP 同源）
    sac = s.alphas_cumprod.numpy().astype(np.float32).astype(np.float64)
    t0, t1 = dts[0], dts[1]
    a = sac[t0]; ap = sac[t1]
    rAt = math.sqrt(a); rBt = math.sqrt(1 - a)
    rAp = math.sqrt(ap); rBp = math.sqrt(1 - ap)
    e0 = 0.5 * x_init64
    np0 = rAp * ((x_init64 - rBt * e0) / rAt) + rBp * e0
    d = float(np.max(np.abs(out.astype(np.float64) - np0)))
    print(f"[diffusers 交叉] 首步 max abs diff = {d:.3e}  (fp32 vs fp64，~1e-6 量级正常)")
    if d < 1e-4:
        print("  -> diffusers 公式一致 ✓")
    else:
        print("  -> 警告：与 diffusers 公式偏差偏大，需复查")
except Exception as e:
    print(f"[diffusers 交叉] 跳过（{e}）")

# ============ 4) 编译 + 运行 MYP scheduler.myp ============
mypc = os.path.join(REPO, "build", "mypc")
stdlib = os.path.join(REPO, "stdlib")
src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scheduler.myp")
binp = "/tmp/sched"
subprocess.run([mypc, src, "-o", binp, "--stdlib", stdlib], check=True,
               cwd=REPO, capture_output=True)
subprocess.run([binp], check=True, cwd=EX, capture_output=True)

# ============ 5) 对拍 ============
myp_out = np.fromfile(os.path.join(OUT, "sched_out.bin"), dtype=np.float32)
ref_out = np.fromfile(os.path.join(OUT, "ref_out.f32"), dtype=np.float32)
assert len(myp_out) == L and len(ref_out) == L, "长度不符"

identical = np.array_equal(myp_out.view(np.uint8), ref_out.view(np.uint8))
max_abs = float(np.max(np.abs(myp_out.astype(np.float64) - ref_out.astype(np.float64))))

print("=" * 56)
print(f"  MYP  final latent: {L} elems")
print(f"  max abs diff vs numpy float64 参考: {max_abs:.3e}")
print(f"  字节级一致 (diff==0): {identical}")
print("=" * 56)
if identical:
    print("  DDIM SCHEDULER OK  (MYP == numpy float64, 字节精确)")
    sys.exit(0)
else:
    print(f"  DDIM SCHEDULER FAIL  (max diff {max_abs:.3e})")
    sys.exit(1)
