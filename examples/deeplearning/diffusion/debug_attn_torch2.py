#!/usr/bin/env python3
"""debug_attn_torch2.py — 隔离 attn1：原始 attention@v 对拍 + to_out 权重对拍。
判断：MYP/numpy 的 attention 数学是否正确；to_out 权重偏移是否正确。
"""
import numpy as np
import torch
import torch.nn.functional as F

DL = "/home/xlkj/code/MYPLanguage/examples/deeplearning/data/diffusion/unet"
def rd(name): return np.fromfile(f"{DL}/{name}", dtype=np.float32)
w = rd("weights.bin")
gn = rd("stage_d0a0_gn.f32").reshape(320, 4096)
dim=320; S=4096; heads=40; dh=8
base = 2266880
def sl(off, n): return w[base+off:base+off+n].copy()
proj_w=sl(640,dim*dim).reshape(dim,dim); proj_b=sl(640+dim*dim,dim)
n1_w=sl(640+dim*dim+dim,dim); n1_b=sl(640+dim*dim+2*dim,dim)
p=640+dim*dim+3*dim
a1q=sl(p,dim*dim).reshape(dim,dim); a1k=sl(p+dim*dim,dim*dim).reshape(dim,dim)
a1v=sl(p+2*dim*dim,dim*dim).reshape(dim,dim); a1o=sl(p+3*dim*dim,dim*dim).reshape(dim,dim)
a1ob=sl(p+4*dim*dim,dim)

proj=(proj_w@gn)+proj_b[:,None]
mu=proj.mean(0,keepdims=True); var=((proj-mu)**2).mean(0,keepdims=True)
ln1=(proj-mu)/np.sqrt(var+1e-5)*n1_w[:,None]+n1_b[:,None]

q=(a1q@ln1).astype(np.float32); k=(a1k@ln1).astype(np.float32); v=(a1v@ln1).astype(np.float32)

def raw_attn_np(q,k,v,heads,dh,kvS):
    qs=q.reshape(heads,dh,-1); ks=k.reshape(heads,dh,kvS); vs=v.reshape(heads,dh,kvS)
    sc=np.einsum('hds,hdt->hst',qs,ks)/np.sqrt(dh)
    mx=sc.max(-1,keepdims=True); e=np.exp(sc-mx); pr=e/e.sum(-1,keepdims=True)
    return np.einsum('hst,hdt->hds',pr,vs).reshape(dim,-1)

def raw_attn_torch(q,k,v,heads,dh,kvS):
    def b(x): return torch.from_numpy(x.reshape(heads,dh,-1).transpose(0,2,1)[None].copy())
    qb,kb,vb=b(q),b(k),b(v)
    o=F.scaled_dot_product_attention(qb,kb,vb).numpy().transpose(0,1,3,2).reshape(1,dim,-1)[0]
    return o

r_raw_np = raw_attn_np(q,k,v,heads,dh,S)
r_raw_t  = raw_attn_torch(q,k,v,heads,dh,S)
print("raw attn numpy vs torch diff =", np.abs(r_raw_np-r_raw_t).max())

# to_out：对 stage_d0a0_a1o（to_out.0 输出，torch 从自己 raw attn 得到）对拍
r_a1o = rd("stage_d0a0_a1o.f32").reshape(dim,S)
my_a1o = (a1o @ r_raw_t) + a1ob[:,None]
print("dense(my_raw, a1o) vs torch a1o ref diff =", np.abs(my_a1o-r_a1o).max())
# 也用 torch raw 直接过 to_out 权重（如果偏移对，应≈ref）
t_a1o = (a1o @ r_raw_t) + a1ob[:,None]
print("(same)  vs torch a1o ref  (确认) diff =", np.abs(t_a1o-r_a1o).max())

# 完整 attn1 输出 ref（含 to_out）
r_a1 = rd("stage_d0a0_a1.f32").reshape(dim,S)
print("my full attn1 vs torch attn1 ref diff =", np.abs(t_a1o-r_a1).max())
print("a1o[0,0:3]=", r_a1o[0,:3], " my=", my_a1o[0,:3])
