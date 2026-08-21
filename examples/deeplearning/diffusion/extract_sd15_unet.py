#!/usr/bin/env python3
"""extract_sd15_unet.py — 阶段 D3b：SD1.5 UNet（UNet2DConditionModel）权重 → MYP .bin + 参考输出。

架构（config：in/out=4, block=[320,640,1280,1280], 3 CrossAttnDown+1 Down, mid CrossAttn,
  up=[Up,CrossAttn×3], layers_per_block=2, cross_attention_dim=768, head_dim=8, GroupNorm32, silu）：

布局（块连续，bases.bin 存每块起始偏移，int32 LE，47 块）：
  conv_in → down0(r0,a0,r1,a1,ds) → down1 → down2 → down3(r0,r1，无ds)
  → mid(r0,a0,r1) → up0(r0,r1,r2,us) → up1(r0,a0,r1,a1,r2,a2,us) → up2 → up3(r..a..，无us)
  → conv_norm_out → conv_out
  **SD 不对称：Down 块 2 resnet+2 attn；Up 块 3 resnet+3 attn（up0 纯 3 resnet）**

ResBlock(inC→outC)：norm1_w[inC],norm1_b[inC], conv1_w[outC,inC,3,3],conv1_b[outC],
  time_emb_proj_w[outC,1280],time_emb_proj_b[outC], norm2_w[outC],norm2_b[outC],
  conv2_w[outC,outC,3,3],conv2_b[outC], (inC!=outC: conv_shortcut_w[outC,inC,1,1],b[outC])
Attention(dim)：norm_w[dim],norm_b[dim], proj_in_w[dim,dim],proj_in_b[dim],
  basic: norm1_w[dim],norm1_b[dim], attn1_q[dim,dim],attn1_k[dim,dim],attn1_v[dim,dim],
    attn1_ow[dim,dim],attn1_ob[dim], norm2_w[dim],norm2_b[dim],
    attn2_q[dim,dim],attn2_k[dim,768],attn2_v[dim,768],attn2_ow[dim,dim],attn2_ob[dim],
    norm3_w[dim],norm3_b[dim], ff_proj_w[2*ff,dim],ff_proj_b[2*ff],ff_lin_w[dim,ff],ff_lin_b[dim]
  proj_out_w[dim,dim],proj_out_b[dim]   （ff=dim*4）
Downsample/Upsample(outC)：conv_w[outC,outC,3,3],conv_b[outC]

输出（data/diffusion/unet/）：
  weights.bin   全部权重 fp32 LE
  bases.bin     41 个块起始偏移 int32 LE
  time_emb.bin  [1000,1280] 时间嵌入表（time_proj+time_embedding 预计算，MYP 直接查表）
  latent_in.bin [4,64,64] 固定随机 latent（fp32）
  text_emb.bin  [768,77] 固定文本嵌入（特征主序，来自 CLIP，作 cross-attn context）
  ref_out.bin   [4,64,64] UNet(latent, t=980, text) 参考输出（fp32）
用法：onnxvenv/bin/python deeplearning/diffusion/extract_sd15_unet.py
"""
import os
import numpy as np
import torch
from diffusers import UNet2DConditionModel
from safetensors import safe_open

DL = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SD = os.path.join(DL, "data", "diffusion", "sd15")
OUT = os.path.join(DL, "data", "diffusion", "unet")
os.makedirs(OUT, exist_ok=True)
DEV = "cpu"
TE_DIM = 1280
CROSS_DIM = 768

print("loading UNet2DConditionModel (fp32)...")
m = UNet2DConditionModel.from_pretrained(os.path.join(SD, "unet"), torch_dtype=torch.float32).to(DEV)
m.eval()
sd = m.state_dict()
W = lambda k: sd[k].detach().cpu().numpy().astype(np.float32)


# ============ 权重布局 dump ============
def conv2d_wb(prefix, cout, cin):
    return W(f"{prefix}.weight"), W(f"{prefix}.bias")

def resnet_block(prefix, inC, outC):
    blk = []
    blk.append(W(f"{prefix}.norm1.weight")); blk.append(W(f"{prefix}.norm1.bias"))
    w, b = conv2d_wb(f"{prefix}.conv1", outC, inC); blk += [w, b]
    blk.append(W(f"{prefix}.time_emb_proj.weight")); blk.append(W(f"{prefix}.time_emb_proj.bias"))
    blk.append(W(f"{prefix}.norm2.weight")); blk.append(W(f"{prefix}.norm2.bias"))
    w, b = conv2d_wb(f"{prefix}.conv2", outC, outC); blk += [w, b]
    if inC != outC:
        w, b = conv2d_wb(f"{prefix}.conv_shortcut", outC, inC); blk += [w, b]
    return blk

def attention_block(prefix, dim):
    blk = []
    blk.append(W(f"{prefix}.norm.weight")); blk.append(W(f"{prefix}.norm.bias"))
    blk.append(W(f"{prefix}.proj_in.weight")); blk.append(W(f"{prefix}.proj_in.bias"))
    tb = f"{prefix}.transformer_blocks.0"
    blk.append(W(f"{tb}.norm1.weight")); blk.append(W(f"{tb}.norm1.bias"))
    # attn1
    blk.append(W(f"{tb}.attn1.to_q.weight")); blk.append(W(f"{tb}.attn1.to_k.weight"))
    blk.append(W(f"{tb}.attn1.to_v.weight"))
    blk.append(W(f"{tb}.attn1.to_out.0.weight")); blk.append(W(f"{tb}.attn1.to_out.0.bias"))
    # norm2（attn1 与 attn2 之间的 LayerNorm）
    blk.append(W(f"{tb}.norm2.weight")); blk.append(W(f"{tb}.norm2.bias"))
    # attn2
    blk.append(W(f"{tb}.attn2.to_q.weight")); blk.append(W(f"{tb}.attn2.to_k.weight"))
    blk.append(W(f"{tb}.attn2.to_v.weight"))
    blk.append(W(f"{tb}.attn2.to_out.0.weight")); blk.append(W(f"{tb}.attn2.to_out.0.bias"))
    blk.append(W(f"{tb}.norm3.weight")); blk.append(W(f"{tb}.norm3.bias"))
    ff = dim * 4
    blk.append(W(f"{tb}.ff.net.0.proj.weight")); blk.append(W(f"{tb}.ff.net.0.proj.bias"))
    blk.append(W(f"{tb}.ff.net.2.weight")); blk.append(W(f"{tb}.ff.net.2.bias"))
    blk.append(W(f"{prefix}.proj_out.weight")); blk.append(W(f"{prefix}.proj_out.bias"))
    return blk

def downsampler(prefix, outC):
    return list(conv2d_wb(prefix, outC, outC))

def upsampler(prefix, outC):
    return list(conv2d_wb(prefix, outC, outC))

# 块顺序（bases 索引）：见文件头。SD 不对称：Down 块 2 resnet+2 attn(+ds)；Up 块 3 resnet+3 attn(+us)。
# 注意：这些函数返回【子块列表】（每个子块=张量列表），bases.bin 按逻辑子块记录偏移，
# 与 MYP 的 base[1]=resnet, base[2]=attention... 约定一致（勿用 * 展开成张量）。
def down0():  # conv_in 已 4→320
    return [resnet_block("down_blocks.0.resnets.0", 320, 320),
            attention_block("down_blocks.0.attentions.0", 320),
            resnet_block("down_blocks.0.resnets.1", 320, 320),
            attention_block("down_blocks.0.attentions.1", 320),
            downsampler("down_blocks.0.downsamplers.0.conv", 320)]

def down1():
    return [resnet_block("down_blocks.1.resnets.0", 320, 640),
            attention_block("down_blocks.1.attentions.0", 640),
            resnet_block("down_blocks.1.resnets.1", 640, 640),
            attention_block("down_blocks.1.attentions.1", 640),
            downsampler("down_blocks.1.downsamplers.0.conv", 640)]

def down2():
    return [resnet_block("down_blocks.2.resnets.0", 640, 1280),
            attention_block("down_blocks.2.attentions.0", 1280),
            resnet_block("down_blocks.2.resnets.1", 1280, 1280),
            attention_block("down_blocks.2.attentions.1", 1280),
            downsampler("down_blocks.2.downsamplers.0.conv", 1280)]

def down3():  # 无 downsampler（mid 在 8×8）
    return [resnet_block("down_blocks.3.resnets.0", 1280, 1280),
            resnet_block("down_blocks.3.resnets.1", 1280, 1280)]

def mid():
    return [resnet_block("mid_block.resnets.0", 1280, 1280),
            attention_block("mid_block.attentions.0", 1280),
            resnet_block("mid_block.resnets.1", 1280, 1280)]

def up0():  # UpBlock2D：3 resnet（无 attn）+ us
    return [resnet_block("up_blocks.0.resnets.0", 2560, 1280),
            resnet_block("up_blocks.0.resnets.1", 2560, 1280),
            resnet_block("up_blocks.0.resnets.2", 2560, 1280),
            upsampler("up_blocks.0.upsamplers.0.conv", 1280)]

def up1():  # CrossAttnUp：3 resnet + 3 attn（前向顺序 r0,a0,r1,a1,r2,a2）+ us
    return [resnet_block("up_blocks.1.resnets.0", 2560, 1280),
            attention_block("up_blocks.1.attentions.0", 1280),
            resnet_block("up_blocks.1.resnets.1", 2560, 1280),
            attention_block("up_blocks.1.attentions.1", 1280),
            resnet_block("up_blocks.1.resnets.2", 1920, 1280),
            attention_block("up_blocks.1.attentions.2", 1280),
            upsampler("up_blocks.1.upsamplers.0.conv", 1280)]

def up2():
    return [resnet_block("up_blocks.2.resnets.0", 1920, 640),
            attention_block("up_blocks.2.attentions.0", 640),
            resnet_block("up_blocks.2.resnets.1", 1280, 640),
            attention_block("up_blocks.2.attentions.1", 640),
            resnet_block("up_blocks.2.resnets.2", 960, 640),
            attention_block("up_blocks.2.attentions.2", 640),
            upsampler("up_blocks.2.upsamplers.0.conv", 640)]

def up3():  # 末块无 upsampler（输出 64×64）
    return [resnet_block("up_blocks.3.resnets.0", 960, 320),
            attention_block("up_blocks.3.attentions.0", 320),
            resnet_block("up_blocks.3.resnets.1", 640, 320),
            attention_block("up_blocks.3.attentions.1", 320),
            resnet_block("up_blocks.3.resnets.2", 640, 320),
            attention_block("up_blocks.3.attentions.2", 320)]

blocks = [list(conv2d_wb("conv_in", 320, 4))]
for f in [down0, down1, down2, down3, mid, up0, up1, up2, up3]:
    blocks += f()
blocks += [[W("conv_norm_out.weight"), W("conv_norm_out.bias")]]
blocks += [list(conv2d_wb("conv_out", 4, 320))]

# 展平 + 记录 bases
bases = []
flat = []
off = 0
for blk in blocks:
    bases.append(off)
    for t in blk:
        flat.append(t.reshape(-1))
        off += t.size
weights = np.concatenate(flat) if flat else np.zeros(0)
weights.astype(np.float32).tofile(os.path.join(OUT, "weights.bin"))
np.array(bases, dtype=np.int32).tofile(os.path.join(OUT, "bases.bin"))
print(f"UNet weights: {weights.size} floats ({weights.size*4/1e6:.0f}MB), {len(bases)} blocks")

# ============ 时间嵌入表 [1000,1280] ============
with torch.no_grad():
    te = m.time_embedding(m.time_proj(torch.arange(1000, dtype=torch.float32)))
te.numpy().astype(np.float32).tofile(os.path.join(OUT, "time_emb.bin"))
print("time_emb.bin [1000,1280] written")

# ============ 参考输出：固定 latent + t + 文本嵌入 ============
rng = np.random.default_rng(7)
latent = rng.standard_normal((1, 4, 64, 64)).astype(np.float32)
latent.tofile(os.path.join(OUT, "latent_in.bin"))
t = 980
with open(os.path.join(OUT, "timestep.txt"), "w") as f:
    f.write(f"{t}\n")
# 文本嵌入：用 CLIP 已生成的 ref_emb [77,768]（位置主序）→ 转特征主序 [768,77]
clip_emb = np.fromfile(os.path.join(DL, "data", "diffusion", "clip", "ref_emb.bin"), dtype=np.float32).reshape(77, 768)
text_feat = clip_emb.T.astype(np.float32)     # [768,77] 特征主序
text_feat.tofile(os.path.join(OUT, "text_emb.bin"))

# 各阶段参考输出（分阶段对拍用）：conv_in / down0..3 / mid / up0..3
stage_refs = {}
def hook(name):
    def fn(mod, inp, out):
        o = out[0] if isinstance(out, tuple) else out
        stage_refs[name] = o.detach().cpu().numpy().astype(np.float32)
    return fn
m.conv_in.register_forward_hook(hook("conv_in"))
for i, b in enumerate(m.down_blocks):
    b.register_forward_hook(hook(f"down{i}"))
m.mid_block.register_forward_hook(hook("mid"))
# mid 内部 resnets 输出（hook 定义在下方，此处仅 hook 类型）
m.mid_block.resnets[0].register_forward_hook(hook("mid_r0"))
m.mid_block.resnets[1].register_forward_hook(hook("mid_r1"))
for i, b in enumerate(m.up_blocks):
    b.register_forward_hook(hook(f"up{i}"))
# up 内部钩子（定位 up 路径积累误差）：resnets 输出 + upsampler 输出 + attention 输出
for bi, b in enumerate(m.up_blocks):
    if bi > 1:
        continue
    for ri, r in enumerate(b.resnets):
        r.register_forward_hook(hook(f"u{bi}r{ri}"))
    for ui, u in enumerate(b.upsamplers):
        u.register_forward_hook(hook(f"u{bi}us"))
    if hasattr(b, "attentions"):
        for ai, at in enumerate(b.attentions):
            at.register_forward_hook(hook(f"u{bi}a{ai}"))
# up3（末块，无 upsampler）内部：resnets + attentions
for ri, r in enumerate(m.up_blocks[3].resnets):
    r.register_forward_hook(hook(f"u3r{ri}"))
for ai, at in enumerate(m.up_blocks[3].attentions):
    at.register_forward_hook(hook(f"u3a{ai}"))
m.conv_norm_out.register_forward_hook(hook("conv_norm_out"))

# 细粒度子阶段参考（定位 down0/down1 内部分歧）：r=resnet 输出（post-residual），
# a=attention 输出（post-proj_out+residual），ds=downsampler 输出（=下一 block 输入）。
sub_names = {}
for bi, b in enumerate(m.down_blocks):
    if bi > 1:
        continue
    for ri, r in enumerate(b.resnets):
        r.register_forward_hook(hook(f"d{bi}r{ri}"))
        sub_names[f"d{bi}r{ri}"] = r
    for ai, at in enumerate(b.attentions):
        at.register_forward_hook(hook(f"d{bi}a{ai}"))
        sub_names[f"d{bi}a{ai}"] = at
    if b.downsamplers:
        b.downsamplers[0].register_forward_hook(hook(f"d{bi}ds"))
        sub_names[f"d{bi}ds"] = b.downsamplers[0]

# attention 内部切开（定位 attnBlock 内部分歧）：
#   a{ai}_gn=GroupNorm 输出；a{ai}_proj=proj_in 输出；a{ai}_a1=attn1 softmax@v 输出（to_out 前）；
#   a{ai}_a1o=attn1 to_out 输出；a{ai}_ffp=FFN proj 输出；a{ai}_ffo=FFN lin 输出；a{ai}_po=proj_out 输出。
# 这些输出为 [B,S,D] token 主序，转置为 [B,D,S] 后 flatten 才与 MYP [D,S] 布局一致。
def hookT(name):
    def fn(mod, inp, out):
        o = out[0] if isinstance(out, tuple) else out
        a = o.detach().cpu().numpy().astype(np.float32)
        if a.ndim == 3:          # [B,S,D] → [B,D,S]（token 主序转特征主序）
            a = a.transpose(0, 2, 1)
        # 4D [B,C,H,W] 保持 NCHW（flatten=C-major，与 MYP [C,HW] 一致）
        stage_refs[name] = a
    return fn
# mid attention 内部（hookT 需在此定义之后）
mid_at = m.mid_block.attentions[0]
mid_tb = mid_at.transformer_blocks[0]
mid_at.norm.register_forward_hook(hook("mid_gn"))
mid_at.proj_in.register_forward_hook(hookT("mid_proj"))
mid_tb.attn1.register_forward_hook(hookT("mid_a1"))
mid_tb.attn1.to_out[0].register_forward_hook(hookT("mid_a1o"))
mid_tb.attn1.to_q.register_forward_hook(hookT("mid_q1"))
mid_tb.attn1.to_k.register_forward_hook(hookT("mid_k1"))
mid_tb.attn1.to_v.register_forward_hook(hookT("mid_v1"))
mid_tb.norm2.register_forward_hook(hookT("mid_ln2"))
mid_tb.attn2.register_forward_hook(hookT("mid_a2"))
mid_tb.attn2.to_out[0].register_forward_hook(hookT("mid_a2o"))
mid_tb.attn2.to_q.register_forward_hook(hookT("mid_q2"))
mid_tb.attn2.to_k.register_forward_hook(hookT("mid_k2"))
mid_tb.attn2.to_v.register_forward_hook(hookT("mid_v2"))
mid_at.proj_out.register_forward_hook(hookT("mid_po"))
for bi, b in enumerate(m.down_blocks):
    if bi > 0:
        continue
    for ai, at in enumerate(b.attentions):
        tb = at.transformer_blocks[0]
        at.norm.register_forward_hook(hook(f"d{bi}a{ai}_gn"))
        at.proj_in.register_forward_hook(hookT(f"d{bi}a{ai}_proj"))
        tb.attn1.register_forward_hook(hookT(f"d{bi}a{ai}_a1"))
        tb.attn1.to_out[0].register_forward_hook(hookT(f"d{bi}a{ai}_a1o"))
        tb.attn2.register_forward_hook(hookT(f"d{bi}a{ai}_a2"))
        tb.ff.net[0].register_forward_hook(hookT(f"d{bi}a{ai}_ffp"))
        tb.ff.net[2].register_forward_hook(hookT(f"d{bi}a{ai}_ffo"))
        at.proj_out.register_forward_hook(hookT(f"d{bi}a{ai}_po"))

with torch.no_grad():
    out = m(torch.from_numpy(latent), torch.tensor(t), encoder_hidden_states=torch.from_numpy(text_feat.T[None])).sample
out.numpy().astype(np.float32).tofile(os.path.join(OUT, "ref_out.bin"))
for k, v in stage_refs.items():
    v.reshape(-1).tofile(os.path.join(OUT, f"stage_{k}.f32"))
    print(f"stage_{k}: {tuple(v.shape)}")
print("ref_out.bin written", tuple(out.shape))
print("ref_out[0,0,0,0:4] =", out[0,0,0,0:4].tolist())
