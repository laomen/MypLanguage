#!/usr/bin/env python3
"""debug_attn_torch3.py — 加载真实模型，直接运行 down0 attentions[0]，对拍我的 numpy 与 stage 参考。"""
import numpy as np
import torch
from diffusers import UNet2DConditionModel

DL = "/home/xlkj/code/MYPLanguage/examples/deeplearning/data/diffusion/unet"
SD = "/home/xlkj/code/MYPLanguage/examples/deeplearning/data/diffusion/sd15"
def rd(name): return np.fromfile(f"{DL}/{name}", dtype=np.float32)
w = rd("weights.bin")
dim=320; S=4096; heads=40; dh=8
base = 2266880
def sl(off, n): return w[base+off:base+off+n].copy()
proj_w=sl(640,dim*dim).reshape(dim,dim); proj_b=sl(640+dim*dim,dim)
n1_w=sl(640+dim*dim+dim,dim); n1_b=sl(640+dim*dim+2*dim,dim)
p=640+dim*dim+3*dim
a1q=sl(p,dim*dim).reshape(dim,dim); a1k=sl(p+dim*dim,dim*dim).reshape(dim,dim)
a1v=sl(p+2*dim*dim,dim*dim).reshape(dim,dim); a1o=sl(p+3*dim*dim,dim*dim).reshape(dim,dim)
a1ob=sl(p+4*dim*dim,dim)

gn = rd("stage_d0a0_gn.f32").reshape(dim, S)
proj=(proj_w@gn)+proj_b[:,None]
mu=proj.mean(0,keepdims=True); var=((proj-mu)**2).mean(0,keepdims=True)
ln1=(proj-mu)/np.sqrt(var+1e-5)*n1_w[:,None]+n1_b[:,None]

# 加载真实模型，跑 attentions[0] 的 transformer_blocks[0]
print("loading model...")
m = UNet2DConditionModel.from_pretrained(f"{SD}/unet", torch_dtype=torch.float32).to("cpu")
m.eval()
at = m.down_blocks[0].attentions[0]
tb = at.transformer_blocks[0]
with torch.no_grad():
    # 输入 [1, S, D] token 主序
    h = torch.from_numpy(ln1.T.astype(np.float32)).unsqueeze(0)   # [1,4096,320]
    # norm1
    hn = tb.norm1(h)
    # attn1（自注意力，无 context）
    o1 = tb.attn1(hn)
    o1_t = o1[0] if isinstance(o1, tuple) else o1
    print("attn1 out shape:", tuple(o1_t.shape))
    # to_out.0
    o_out = tb.attn1.to_out[0](hn)
    print("to_out[0] shape:", tuple(o_out.shape))
    # 直接手动 qkv
    q = tb.attn1.to_q(hn); k = tb.attn1.to_k(hn); v = tb.attn1.to_v(hn)
    print("q shape:", tuple(q.shape))

# 参考
r_a1 = rd("stage_d0a0_a1.f32").reshape(dim, S)
r_a1o = rd("stage_d0a0_a1o.f32").reshape(dim, S)
print("a1 vs a1o ref  diff =", np.abs(r_a1-r_a1o).max())

o1_np = o1_t.detach().numpy().transpose(0,2,1)[0]   # [D,S]
print("torch attn1 vs ref a1  diff =", np.abs(o1_np-r_a1).max())
print("torch attn1 vs ref a1o diff =", np.abs(o1_np-r_a1o).max())

# 我的 numpy 完整 attn1
def raw_attn(qn,kn,vn):
    qs=qn.reshape(heads,dh,-1); ks=kn.reshape(heads,dh,-1); vs=vn.reshape(heads,dh,-1)
    sc=np.einsum('hds,hdt->hst',qs,ks)/np.sqrt(dh)
    mx=sc.max(-1,keepdims=True); e=np.exp(sc-mx); pr=e/e.sum(-1,keepdims=True)
    return np.einsum('hst,hdt->hds',pr,vs).reshape(dim,-1)
q_np = tb.attn1.to_q(torch.from_numpy(ln1.T.astype(np.float32)).unsqueeze(0)).detach().numpy().transpose(0,2,1)[0]
k_np = tb.attn1.to_k(torch.from_numpy(ln1.T.astype(np.float32)).unsqueeze(0)).detach().numpy().transpose(0,2,1)[0]
v_np = tb.attn1.to_v(torch.from_numpy(ln1.T.astype(np.float32)).unsqueeze(0)).detach().numpy().transpose(0,2,1)[0]
my_raw = raw_attn(q_np, k_np, v_np)
my_full = (a1o @ my_raw) + a1ob[:,None]
print("my full attn1 (from torch qkv) vs ref a1 diff =", np.abs(my_full-r_a1).max())
print("my raw (from torch qkv) vs torch-attn1-raw-ish:", np.abs(my_raw - (tb.attn1.to_out[0](tb.attn1.to_q(torch.from_numpy(ln1.T.astype(np.float32)).unsqueeze(0))).detach())).max())
