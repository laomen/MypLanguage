# MYP 扩散模型专项（SD 1.5 文生图）

在 MYP 自研 DL 框架（`examples/deeplearning/infer`，纯 MYP + LLVM 后端）中从零实现
Stable Diffusion 1.5 完整文生图管线：CLIP 文本编码 → DDIM 去噪（UNet）→ VAE 解码 → 图像。
全程**不用 Python 推理**（Python 仅用于权重抽取与数值对拍参考）。

硬件目标：RTX 2070 SUPER 8GB（sm_75）。当前 CPU 验证已全部通过，GPU 加速（D6）待做。

## 里程碑状态

| 阶段 | 内容 | 文件 | 状态 |
|------|------|------|------|
| D1 | DDIM 调度器 | `scheduler.myp` | ✅ 字节精确（vs numpy float64）；diffusers 交叉 3.5e-7 |
| D2a | CLIP 文本编码器 | `clip_encoder.myp` | ✅ 77 位置 maxAbsDiff 2.7e-4 |
| D2b | CLIP 分词器（GPT-2 BPE） | 待做 | ⏳ |
| D3a | UNet 算子 | `unet_ops.myp` | ✅ vs numpy 0~1.8e-7 |
| D3b | UNet 全装配 | `unet_forward.myp` | ✅ **UNET out maxAbsDiff 1.29e-5** |
| D4 | VAE 解码 | `vae_decode.myp` | ✅ 全阶段 ~1e-3，**VAE DECODE OK（9e-6）** |
| D5 | 端到端（出图 PPM/BMP/SDL） | 待做 | ⏳ |
| D6 | @gpu for 加速 | 待做 | ⏳ |

## 目录结构

```
examples/deeplearning/diffusion/
├── scheduler.myp / verify_scheduler.py      # D1 DDIM
├── clip_encoder.myp / extract_sd15.py       # D2a CLIP 文本编码
├── unet_ops.myp / unet_ops_test.myp         # D3a UNet 算子（GroupNorm/attention2/geglu/upsample）
├── unet_forward.myp / extract_sd15_unet.py  # D3b UNet 完整前向 + 权重抽取
├── vae_decode.myp / extract_sd15_vae.py     # D4 VAE decoder + 权重抽取
├── debug_*.py                               # 调试对拍脚本（numpy 逐步复算 / torch 真值）
└── diffusion_plan.md                        # 路线图
```

数据（git 忽略）：`examples/deeplearning/data/diffusion/`
- `sd15/{scheduler,unet,vae}/` 官方权重（HF 镜像下载，**须 `HF_HUB_DISABLE_XET=1`**）
- `unet/`：weights.bin(3.43GB)+bases.bin(47 逻辑块)+time_emb/latent/text/ref_out+stage_*.f32
- `vae/`：vae_weights.bin(198MB)+bases.bin(22 逻辑块)+latent/ref_out+stage_*.f32
- `clip/`：CLIP 权重+参考

## 运行与验证

Python 环境：`examples/deeplearning/infer/tools/onnxvenv/`（torch/diffusers/transformers/safetensors）。
缺包用 tuna 镜像：`python -m pip install -i https://pypi.tuna.tsinghua.edu.cn/simple`。

```bash
cd examples
# 1) 抽取权重 + 参考（须先下载官方权重）
./deeplearning/infer/tools/onnxvenv/bin/python deeplearning/diffusion/extract_sd15_unet.py
./deeplearning/infer/tools/onnxvenv/bin/python deeplearning/diffusion/extract_sd15_vae.py
# 2) 编译（-O3，CPU 推理快 10-20 倍）
../../build/mypc -O3 deeplearning/diffusion/unet_forward.myp -o /tmp/unetfwd --stdlib ../stdlib
../../build/mypc -O3 deeplearning/diffusion/vae_decode.myp   -o /tmp/vaedec  --stdlib ../stdlib
# 3) 运行（须在 examples/ 下，权重在 data/diffusion/）
/tmp/unetfwd    # → UNET FORWARD OK（maxAbsDiff 1.29e-5）
/tmp/vaedec     # → VAE DECODE OK
# 分阶段对拍：env MYP_UNET_STAGE=<conv_in|down0..|mid|up0..|conv_norm_out> /tmp/unetfwd
#             env MYP_VAE_STAGE=<conv_in|mid_r0|u0r0..|u3r2|conv_norm_out> /tmp/vaedec
```

## SD1.5 架构要点

- **UNet**（in/out=4，块通道 [320,640,1280,1280]）：conv_in(4→320) → 3×CrossAttnDown+1 Down
  → mid → Up×4 → conv_norm_out → conv_out(320→4)。**不对称**：Down 块 2 resnet+2 attn(+ds)；
  Up 块 3 resnet+3 attn(+us)；down3 无 ds（mid 在 8×8）；up3 无 us。
- **ResBlock**：GroupNorm32(eps=1e-5)→silu→conv3×3→+time_emb_proj(silu(time_emb))→GroupNorm→silu→conv3×3（in≠out 有 1×1 shortcut）。
- **Transformer2DModel**：GroupNorm→proj_in(Linear)→basic block(LN→self-attn→LN→cross-attn kv=768→LN→FFN GEGLU)→proj_out→残差。
  **SD1.5 attention 是 8 头 × dh=ch/8**（`attention_head_dim:8` 被 diffusers 0.39 解释为 num_attention_heads=8），scale=1/sqrt(dh)。
- **VAE**（AutoencoderKL decoder）：post_quant_conv(4→4)→conv_in(4→512)→mid(resnet512,attn512,resnet512)
  →up×4(512/512/512→256/256→128，3 resnet+us)→conv_norm_out(GroupNorm32 eps=1e-6)→silu→conv_out(128→3)。
  **VAE resnet 无 time_emb**；attention 旧式（group_norm eps=1e-6 + q/k/v/proj_attn 全带 bias，heads=1）；
  **无 skip 连接**。latent 进 decoder 前 ×1/0.18215。
- 时间嵌入：`time_emb.bin[1000,1280]` 预计算表（time_proj+MLP），ResBlock 内再 silu+proj。

## MYP 实现要点 / 惯用法

- `int main(){ X x = new X(); return 0; }`；类方法须在 `static:`/`action:` 段内；
  `ffi`/`fact`/`ref` 为保留字。
- 权重加载：`__myp_io_fopen` + `F32.toDouble(bits)` 读 fp32（LE）；`F32.toBits` + 4×write_byte 写。
  注意 **`__myp_io` 无批量读**，per-byte 读 3.4GB ≈ 34s。
- 大内存：`new float[1024*1024*1024]` 4GB arena，权重+激活+scratch 统一管理（块偏移由 bases.bin 提供）。
- 分阶段对拍：抽取脚本用 forward hook 落盘块级 + attention/up 内部参考（`[B,S,D]` 转 `[B,D,S]`），
  MYP 读 `stage_*.f32` 逐块比较；`myp_*.f32` 为 numpy/torch 内部参考（attnBlock 的 `tag` 参数 + dbgCheck 二分）。

## 调试经验（关键 bug 清单）

见 `diffusion_plan.md` 与 repo memory `diffusion-project.md`。核心教训：
1. **bases.bin 必须是逻辑块级偏移**（UNet 47 / VAE 22）——块函数返回子块列表，勿用 `*` 展开成张量。
2. **SD1.5 attention 是 8 头**（dh=ch/8），不是 40 头×8 维；scale=1/sqrt(dh)。
3. 抽取布局必须**含 norm2**（attn1↔attn2 间 LayerNorm），否则块后半错位。
4. **scratch 别名**：attnBlock 的 k/v 需 dim*kvS 元素，kvS>S 时 `at=v+hw` 溢出 → 用 `bv=max(hw,dim*kvS)` 间距。
5. **原地 resnet（x==y）残差自覆盖** → 先复制 x 到 scratch。
6. **上采样 conv 不能原地**（stride1 pad1 覆盖相邻输入）→ nearest 经 tscr 中转。
7. **resnet scratch 与 concat 临时区须分离**（up3 r0 960→320 需 5.24M scratch）。
8. **判断正确性看相对误差**：up/VAE 特征值可达 O(100-195)，绝对 diff 会随量级放大。
9. **带 bias 的 Linear 权重偏移**：q/k/v/proj_attn 的 bias 是 [dim] 不是 [dim*dim]——offset 累加要按实际张量形状。
10. GroupNorm eps：UNet=1e-5，VAE=1e-6；conv_norm_out 参考只含 GroupNorm（silu 前）。
11. **参考区不得与工作区重叠**：`refOff=latOff+16384` 恰好等于 `A0` 起点 → post_quant_conv 覆盖参考 → 仅 FULL 最终比较读到垃圾（阶段对拍读磁盘不受影响）。参考放 arena 尾部（如 900MB）。

## 性能现状

- UNet 完整前向：CPU -O3 数分钟级（权重加载 34s + 计算）。
- VAE 解码：512×512 分辨率卷积量大（完整 ~数百 GFLOPs），CPU 标量循环很慢（每阶段 1-2 分钟）。
- 优化方向：`@parallel for`（多核）、AVX 向量化 ops、D6 `@gpu for`（RTX 2070 SUPER 百倍加速）。

## 下一步

- D2b：CLIP 分词器（GPT-2 字节级 BPE，复用 bpe.myp 机制）
- D5：端到端管线（CLIP→UNet 50 步 DDIM→VAE→图像输出 PPM/BMP/SDL saveBmp）
  DDIM 时间步用 `arange(N)[::-1]*(1000//N)`（diffusers 0.39）。
- D6：@gpu for 加速（重点：conv/attention 的 CUDA kernel）
