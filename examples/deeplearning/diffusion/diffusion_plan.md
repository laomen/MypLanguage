# MYP 扩散模型专项（SD 1.5 文生图）路线图

目标：在 MYP 自研 DL 框架（examples/deeplearning/infer）中用纯 MYP 实现 SD1.5 文生图，
最终 @gpu 加速。数据在 examples/deeplearning/data/diffusion/（git 忽略，需自行下载）。

## 里程碑状态

| 阶段 | 内容 | 状态 |
|------|------|------|
| D1 | DDIM 调度器（scheduler.myp） | ✅ 字节精确（vs numpy float64），diffusers 交叉 3.5e-7 |
| D2a | CLIP 文本编码器（clip_encoder.myp） | ✅ 77 位置 maxAbsDiff 2.7e-4 |
| D2b | CLIP 分词器（GPT-2 BPE） | 待做 |
| D3a | UNet 算子（unet_ops.myp：GroupNorm/attention2/geglu/nearestUpsample2x） | ✅ vs numpy 0~1.8e-7 |
| D3b | UNet 全装配（unet_forward.myp） | ✅ **UNET out maxAbsDiff 1.29e-5（UNET FORWARD OK）** |
| D4 | VAE 解码 | 待做（权重已下载 335MB） |
| D5 | 端到端（latent→图像输出 PPM/BMP/SDL saveBmp） | 待做 |
| D6 | @gpu for 加速 | 待做 |

## D3b 关键文件
- `extract_sd15_unet.py`：diffusers UNet 权重 → weights.bin(3.43GB) + bases.bin(47 逻辑块)
  + time_emb.bin + latent_in/text_emb/ref_out + stage_<block>.f32 分阶段参考
  （含 forward hook 落盘 attention/up 内部 myp_* 参考）。
- `unet_forward.myp`：完整前向。分阶段对拍：`MYP_UNET_STAGE=<名> /tmp/unetfwd`。
  -O3 编译：`mypc -O3 unet_forward.myp -o /tmp/unetfwd --stdlib ../stdlib`
- 权重加载 ~34s（__myp_io 无批量读，per-byte）；调试期每阶段独立运行重载较慢。

## D3b 调试修掉的 8 个关键 bug（详见 repo memory diffusion-project.md）
1. bases.bin 张量级偏移（应逻辑块 47）
2. attention 头数 40→8（dh=ch/8，scale=1/sqrt(dh)）
3. 抽取漏 norm2（attn1↔attn2 间 LayerNorm）
4. attnBlock scratch 别名（kvS>S 时 at 覆盖 v）→ bv=max(hw,dim*kvS)
5. 原地 resnet（x==y）残差自覆盖 → 复制 x
6. 上采样器原地卷积 → nearest 经 tscr 中转
7. resnet scratch 与 tscr 重叠（up3 r0 960→320 需 5.24M）→ tscr 移到 rscr+6M
8. 相对误差判断（up 特征 O(100)），conv_norm_out 参考只含 GroupNorm

## 下一步（D4/D5）
- D4：VAE decoder 权重抽取（已下载），VAEDecoder（groupnorm→conv→残差 upsample×3→conv_norm→conv）。
- D5：CLIP 文本 → UNet 去噪 50 步（DDIM，D1 已备）→ VAE 解码 → 图像；DDIM 时间步用
  `arange(N)[::-1]*(1000//N)`（diffusers 0.39）。
- 运行：onnxvenv/bin/python（HF 镜像下载须 `HF_HUB_DISABLE_XET=1`）。
