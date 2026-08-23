#!/usr/bin/env python3
"""extract_sd15_vae.py — D4：SD1.5 VAE（AutoencoderKL）decoder 权重 → MYP .bin + 参考输出。

架构（block_out=[128,256,512,512]，decoder 反序）：
  post_quant_conv(4→4,1×1) → conv_in(4→512,3×3 pad1) → mid(resnet512,attn512,resnet512)
  → up0(3×resnet512+us) → up1(3×resnet512+us) → up2(3×resnet:512→256,256,256+us)
  → up3(3×resnet:256→128,128,128，无 us) → conv_norm_out(GroupNorm32,eps=1e-6)
  → conv_out(128→3,3×3 pad1)。输出 [3,512,512]。
  **无 skip 连接**；resnet 无 time_emb；attention 旧式（group_norm eps=1e-6 +
  query/key/value/proj_attn 全带 bias，heads=1）；GroupNorm eps=1e-6。

bases.bin（22 逻辑块）：
  post_quant_conv, conv_in, mid_r0, mid_a0, mid_r1,
  up0_r0,r1,r2,up0_us, up1_r0,r1,r2,up1_us, up2_r0,r1,r2,up2_us, up3_r0,r1,r2,
  conv_norm_out, conv_out

ResBlock(in→out)：norm1(w,b),conv1(w,b),norm2(w,b),conv2(w,b),(in!=out: conv_shortcut(w,b))
Attention(dim)：group_norm(w,b), to_q(w,b), to_k(w,b), to_v(w,b), proj_attn(w,b)
Upsample(out)：conv(w,b)

输出（data/diffusion/vae/）：vae_weights.bin, bases.bin, latent_in.bin（=UNet latent 复用，
  进 decoder 前 ×1/0.18215）, ref_out.f32 [3,512,512], stage_<block>.f32。
用法：onnxvenv/bin/python deeplearning/diffusion/extract_sd15_vae.py
"""
import os
import numpy as np
import torch
from diffusers import AutoencoderKL
from safetensors import safe_open

DL = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SD = os.path.join(DL, "data", "diffusion", "sd15")
OUT = os.path.join(DL, "data", "diffusion", "vae")
os.makedirs(OUT, exist_ok=True)

print("loading AutoencoderKL (fp32)...")
m = AutoencoderKL.from_pretrained(os.path.join(SD, "vae"), torch_dtype=torch.float32).to("cpu")
m.eval()
with safe_open(os.path.join(SD, "vae", "diffusion_pytorch_model.safetensors"), framework="np") as f:
    sf = dict((k, f.get_tensor(k).astype(np.float32)) for k in f.keys())
W = lambda k: sf[k]

def resnet_block(prefix, inC, outC):
    blk = [W(f"{prefix}.norm1.weight"), W(f"{prefix}.norm1.bias"),
           W(f"{prefix}.conv1.weight"), W(f"{prefix}.conv1.bias"),
           W(f"{prefix}.norm2.weight"), W(f"{prefix}.norm2.bias"),
           W(f"{prefix}.conv2.weight"), W(f"{prefix}.conv2.bias")]
    if inC != outC:
        blk += [W(f"{prefix}.conv_shortcut.weight"), W(f"{prefix}.conv_shortcut.bias")]
    return blk

def attention_block(prefix, dim):
    # 旧式 Attention：group_norm + query/key/value/proj_attn（全带 bias），heads=1
    return [W(f"{prefix}.group_norm.weight"), W(f"{prefix}.group_norm.bias"),
            W(f"{prefix}.query.weight"), W(f"{prefix}.query.bias"),
            W(f"{prefix}.key.weight"), W(f"{prefix}.key.bias"),
            W(f"{prefix}.value.weight"), W(f"{prefix}.value.bias"),
            W(f"{prefix}.proj_attn.weight"), W(f"{prefix}.proj_attn.bias")]

def upsampler(prefix, outC):
    return [W(f"{prefix}.conv.weight"), W(f"{prefix}.conv.bias")]

# up 块通道（含 r0 的 inC）；返回【子块列表】（每个 resnet 一个列表，勿用 * 展开成张量）
def up_resnets(prefix, inC, outs):
    blk = []
    for ri, outC in enumerate(outs):
        blk.append(resnet_block(f"{prefix}.resnets.{ri}", inC if ri == 0 else outC, outC))
        if ri == 0:
            inC = outC
    return blk

blocks = [
    [W("post_quant_conv.weight"), W("post_quant_conv.bias")],
    [W("decoder.conv_in.weight"), W("decoder.conv_in.bias")],
    resnet_block("decoder.mid_block.resnets.0", 512, 512),
    attention_block("decoder.mid_block.attentions.0", 512),
    resnet_block("decoder.mid_block.resnets.1", 512, 512),
] + up_resnets("decoder.up_blocks.0", 512, [512, 512, 512]) + [
    upsampler("decoder.up_blocks.0.upsamplers.0", 512),
] + up_resnets("decoder.up_blocks.1", 512, [512, 512, 512]) + [
    upsampler("decoder.up_blocks.1.upsamplers.0", 512),
] + up_resnets("decoder.up_blocks.2", 512, [256, 256, 256]) + [
    upsampler("decoder.up_blocks.2.upsamplers.0", 256),
] + up_resnets("decoder.up_blocks.3", 256, [128, 128, 128]) + [
    [W("decoder.conv_norm_out.weight"), W("decoder.conv_norm_out.bias")],
    [W("decoder.conv_out.weight"), W("decoder.conv_out.bias")],
]

bases, flat, off = [], [], 0
for blk in blocks:
    bases.append(off)
    for t in blk:
        flat.append(t.reshape(-1)); off += t.size
weights = np.concatenate(flat)
weights.astype(np.float32).tofile(os.path.join(OUT, "vae_weights.bin"))
np.array(bases, dtype=np.int32).tofile(os.path.join(OUT, "bases.bin"))
print(f"VAE weights: {weights.size} floats ({weights.size*4/1e6:.0f}MB), {len(bases)} blocks")

# 输入 latent（复用 UNet latent；进 decoder 前 ×1/scaling_factor）
lat = np.fromfile(os.path.join(DL, "data", "diffusion", "unet", "latent_in.bin"), dtype=np.float32).reshape(1, 4, 64, 64)
lat.astype(np.float32).tofile(os.path.join(OUT, "latent_in.bin"))
scaling = m.config.scaling_factor  # 0.18215
zin = torch.from_numpy(lat.astype(np.float32)) / scaling

stage_refs = {}
def hook(name):
    def fn(mod, inp, out):
        o = out[0] if isinstance(out, tuple) else out
        stage_refs[name] = o.detach().cpu().numpy().astype(np.float32)
    return fn
m.post_quant_conv.register_forward_hook(hook("post_quant"))
m.decoder.conv_in.register_forward_hook(hook("conv_in"))
for i, r in enumerate(m.decoder.mid_block.resnets):
    r.register_forward_hook(hook(f"mid_r{i}"))
m.decoder.mid_block.attentions[0].register_forward_hook(hook("mid_a0"))
for bi, u in enumerate(m.decoder.up_blocks):
    for ri, r in enumerate(u.resnets):
        r.register_forward_hook(hook(f"u{bi}r{ri}"))
    if u.upsamplers:
        u.upsamplers[0].register_forward_hook(hook(f"u{bi}us"))
m.decoder.conv_norm_out.register_forward_hook(hook("conv_norm_out"))

with torch.no_grad():
    dec = m.decode(zin).sample
out = dec.numpy()[0].astype(np.float32)  # [3,512,512]
out.reshape(-1).tofile(os.path.join(OUT, "ref_out.f32"))
for k, v in stage_refs.items():
    v.reshape(-1).tofile(os.path.join(OUT, f"stage_{k}.f32"))
    print(f"stage_{k}: {tuple(v.shape)}")
print("ref_out [3,512,512] written; sample:", out[:, 0, :4].tolist())
