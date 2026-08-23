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
| D4 | VAE 解码 | ✅ 全阶段 ~1e-3，**VAE DECODE OK（9e-6）** |
| D5 | 端到端 DDIM 采样出图 | ✅ N=5 逐步 eps ~1e-4、最终 latent 2.2e-4、**D5 IMAGE OK（0.28%）**；N=50 采样中 |
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

## D4 关键文件
- `extract_sd15_vae.py`：AutoencoderKL decoder 权重 → vae_weights.bin(198MB) + bases.bin(22 逻辑块)
  + latent_in/ref_out + stage_<block>.f32（forward hook 落盘；resnet 用 up_resnets() 子块列表，勿 * 展开）。
- `vae_decode.myp`：完整 decoder。分阶段对拍：`MYP_VAE_STAGE=<名> /tmp/vaedec`。
  -O3 编译：`mypc -O3 vae_decode.myp -o /tmp/vaedec --stdlib ../stdlib`。权重加载 ~2s。
- `debug_vae_attn.py`：mid attention numpy 逐步对拍；`verify_vae_tail.py` 验证尾部参考一致性。

## D4 调试修掉的 3 个关键 bug（详见 repo memory diffusion-project.md）
1. attnVae q/k/v bias 偏移错误（qb=qw+dim*dim 应为 qw+dim）→ vae_q diff 12.07。
2. **qw=gnw+dim*dim**：gnw 本就是 query_w 起始，再加 dim*dim 跳到 query_b 垃圾 → qw=gnw。
3. **refOff 与 A0 重叠**：refOff=latOff+16384 恰好是 A0 起点 → post_quant_conv 覆盖参考 →
   仅 FULL 最终比较读垃圾（14.23 FAIL），阶段对拍读磁盘不受影响。参考移到 arena 尾部 900MB。

## D5 关键文件
- `extract_d5_ref.py`：diffusers DDIM N 步参考（d5_tsteps/alphas + 每步 eps/latent + d5_final
  + d5_ref_image.f32 + d5_image.ppm）。
- `ddim_sampler.myp`：可复用 `unetForward(xOff,tOff,yOff)` + DDIM η=0 循环，写 vae/latent_in.bin。
- `vae_decode.myp`（MYP_VAE_PPM=1）：解码出 myp_image.ppm + myp_image.f32。
- `compare_d5.py` / `analyze_d5.py`：逐步 eps/latent/图像对拍。

## D5 调试修掉的 4 个关键 bug（详见 repo memory diffusion-project.md）
1. **d5_tsteps.bin 缺计数前缀** → MYP 把首时间步当 n 读 → ts[] 溢出崩溃。加 i32 count 前缀。
2. **末步 α_prev=1.0**（SD1.5 set_alpha_to_one=false → 应 alphas_cumprod[0]=0.99915）→
   末步 latent 错 4.2e-2。修复：prev_t=-1 时 aPrev=alpha[0]。
3. **vae/latent_in.bin 被 DDIM 覆盖** → D4 阶段对拍假报 9.6。D4 对拍前恢复原 latent。
4. **dumpBuf 强制加 vae/ 前缀** → myp_image.f32 写到错路径。加 dumpAbs 直接写。

## 并行化（@parallel for，~14x）
- 热点算子就地加 `@parallel for`：ops.myp 的 dense/matmul（输出行）/2D conv（空间 p，oc 内层）/
silu；unet_ops.myp 的 attention2（查询 i + 输出 (d,i2)）/groupNorm（组 g）/nearestUpsample2x。
- @parallel 体只能访问函数参数（规避 BUG-023 class 属性）；unet_ops.myp 需 import pool。
- 验证：D4 全部阶段数值与串行一致；512×512 阶段 ~14.5x（user/real）。

## 下一步（D5 后）
- D4：✅ 完成。VAE 解码全链路数值验证通过（silu+conv_out 一并验证）。
- D5：CLIP 文本 → UNet 去噪 50 步（DDIM，D1 已备）→ VAE 解码 → 图像；DDIM 时间步用
  `arange(N)[::-1]*(1000//N)`（diffusers 0.39）。
- 运行：onnxvenv/bin/python（HF 镜像下载须 `HF_HUB_DISABLE_XET=1`）。
