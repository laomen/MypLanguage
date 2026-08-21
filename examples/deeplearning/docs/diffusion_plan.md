# MYP 扩散模型专项（Diffusion in MYP）— 文生图 SD 1.5

> 2026-08-21 立项。目标：在 MYP infer 框架内跑通 **Stable Diffusion 1.5** 文生图
> 全流程（prompt → CLIP → UNet 去噪循环 → VAE → 图像）。硬件 RTX 2070 SUPER 8GB。

## 为什么选 SD 1.5

- 权重 fp32 约 **4.3GB**（UNet 3.4G + CLIP 0.5G + VAE 0.33G）——8GB 显存装得下，
  磁盘 333G 富余。
- 生态/验证最好：diffusers 直接可作 ground truth（torch 管道），与 Qwen2 用
  transformers 当权威同法。
- **算子缺口极小**：infer 框架已有 Conv/Attention/LayerNorm/GELU/SiLU/RoPE/
  Dense(含转置)/Resize/ConvTranspose/Sigmoid。SD1.5 只缺 **GroupNorm** 和
  **cross-attention 变体**（q=hidden，k/v=context）。

## 架构总览

```
prompt
  └─ CLIP BPE tokenizer ─ CLIP text encoder ─ c (77×768)
                                            ┌──────────────┐
x_T ~ N(0,1)  ──►  UNet(x_t, t, c) → ε  ──►│ DDIM 50 步    │
  (64×64×4)     （去噪循环，复用 UNet）      │ 调度器循环     │
                                            └──────────────┘
                                        → x_0 (64×64×4 latent)
                                          └─ VAE decoder ─► 512×512×3 图像
```

## 里程碑（每个都独立可验证，对齐 infer 既有方法论：生成参考 → 逐层对拍）

| 里程碑 | 内容 | 验证判据 | 依赖 | 状态 |
|---|---|---|---|---|
| **D1 调度器** | SD1.5 噪声调度（scaled_linear 0.00085→0.012，1000 步）+ DDIM（η=0 确定性） | MYP == numpy float64 **字节精确**（diff==0）+ diffusers 首步交叉 3.5e-7 | 无模型 | ✅ 2026-08-21 |
| **D2 CLIP 编码器** | CLIP BPE（vocab 49408，复用 bpe.myp 模式）+ 12 层 transformer（LayerNorm/GELU/MHA） | CLIP embedding vs transformers CLIPTextModel（<1e-5） | extract_sd15.py（CLIP） | 🔶 D2a 编码器 ✅ 2.7e-4；D2b 分词器待做 |
| **D3 UNet 前向** | **新算子 GroupNorm（32 组）+ cross-attention**；ResBlock、time embedding（sinusoidal+MLP+SiLU）、down/up blocks、skip | UNet 输出 vs diffusers UNet（多 timestep，<1e-4） | extract_sd15.py（UNet） | 🔶 D3a 算子 ✅；D3b 全装配待做 |
| **D4 VAE 解码** | Conv + ConvTranspose + GroupNorm + SiLU | latent→图 vs diffusers AutoencoderKL.decode（<1e-4） | extract_sd15.py（VAE） | |
| **D5 端到端** | 全流程 + 图像输出（stdlib 或 SDL saveBmp / PPM） | 生成图与 diffusers 参考结构一致（像素/结构指标） | D2-D4 | |
| **D6 GPU** | UNet conv/attention/cross-attention 走 `@gpu for`（复用 gpu_llm_ops 模式） | 逐位一致 + 提速 | D5 | |

## 资产管线（extract_sd15.py，仿 extract_qwen2.py）

- diffusers `StableDiffusionPipeline.from_pretrained("runwayml/stable-diffusion-v1-5")`
  （HF 镜像 `HF_ENDPOINT=https://hf-mirror.com`）。
- 权重顺序 dump 成 `data/diffusion/sd15_weights.bin`（fp32 或 fp16？——先 fp32 与
  torch 严格对拍；后续可 fp16 提速）。
- 三段独立偏移：UNet / CLIP / VAE，各配 layout 说明（同 qwen2 perLayer 约定）。
- 每个里程碑只提取自己需要的段，避免一次下载全部（UNet 3.4G 最大）。

## 关键算子/组件对照（infer 已有 vs 需新增）

| SD1.5 组件 | infer 现状 | 动作 |
|---|---|---|
| CLIP transformer | distilgpt2 LayerNorm/GELU/MHA 全套已有 | 复用 |
| UNet ResBlock Conv | conv2d 已有 | 复用 |
| GroupNorm（32 组） | 有 InstanceNorm/LayerNorm/RMSNorm，**无 GroupNorm** | **新增 op** |
| Cross-attention | 有 self-attention（GQA）+ AttentionCached | **新增 cross 变体**（k/v 来自 context） |
| Time embedding | sinusoidal 可内联（RoPE 表同法预生成） | 预生成表 |
| Up/Downsample | conv stride2 / Resize nearest + conv | 复用 |
| VAE decoder | conv/convtranspose/silu | 复用 + GroupNorm |
| Scheduler DDIM/Euler | 无（纯 MYP 数学） | **D1 新增** |

## 风险与对策

- **UNet 前向是最大件**（约 1100 个 op/步 × 50 步）：D3 先单步验证，再 D5 连循环。
- **8GB 显存**：UNet fp32 3.4G 单块可载；50 步循环 H2D 一次 + 原地去噪（复用 LLM
  resident 单数组模式）。
- **生成速度**：2070S 上 UNet 单步 ~1-2s → 50 步 ~1 分钟量级（CPU 更慢，D5 先 CPU
  验证正确性，D6 再 GPU）。
- **图像输出**：stdlib 无现成 PNG；可选 (a) 手写 PPM→转换，(b) SDL saveBmp（已有
  bridge），(c) MYP 写 BMP（简单格式，RGB 直接可行）。
- **float32 vs torch 累积漂移**：深层 UNet 需容差（同 3D 医学影像 rel 2e-4 教训）。

## 本专项目录

```
examples/deeplearning/diffusion/
  scheduler.myp        D1：DDIM/Euler 调度器（纯 MYP）
  verify_scheduler.py  D1：diffusers DDIMScheduler 参考 + 对拍
  extract_sd15.py      D2 起：权重提取（UNet/CLIP/VAE 分段）
  clip_encoder.myp     D2
  unet_forward.myp     D3
  vae_decode.myp       D4
  gen_image.myp        D5 端到端
```
