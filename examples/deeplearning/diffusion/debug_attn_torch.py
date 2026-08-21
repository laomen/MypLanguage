#!/usr/bin/env python3
"""debug_attn_torch.py — 用 torch 直接复算 down0 attn1，判定 numpy/MYP attention 与参考谁错。
"""
import numpy as np
import torch
import torch.nn.functional as F

DL = "/home/xlkj/code/MYPLanguage/examples/deeplearning/data/diffusion/unet"
def rd(name):
    return np.fromfile(f"{DL}/{name}", dtype=np.float32)

w = rd("weights.bin")
ln1 = None
# 复算到 ln1（与 debug_attn.py 相同步骤）
gn = rd("stage_d0a0_gn.f32").reshape(320, 4096)
dim = 320; S = 4096; heads = 40; dh = 8
base = 2266880
def slice_off(off, n): return w[base+off:base+off+n].copy()
proj_w = slice_off(640, dim*dim).reshape(dim, dim)
proj_b = slice_off(640+dim*dim, dim)
n1_w = slice_off(640+dim*dim+dim, dim); n1_b = slice_off(640+dim*dim+2*dim, dim)
p = 640+dim*dim+3*dim
a1q = slice_off(p, dim*dim).reshape(dim,dim); a1k = slice_off(p+dim*dim, dim*dim).reshape(dim,dim)
a1v = slice_off(p+2*dim*dim, dim*dim).reshape(dim,dim)

proj = (proj_w @ gn) + proj_b[:, None]
mu = proj.mean(axis=0, keepdims=True); var = ((proj-mu)**2).mean(axis=0, keepdims=True)
ln1 = (proj-mu)/np.sqrt(var+1e-5)*n1_w[:, None] + n1_b[:, None]

# torch 复算 attn1
q = torch.from_numpy((a1q @ ln1).astype(np.float32))       # [D,S]
k = torch.from_numpy((a1k @ ln1).astype(np.float32))
v = torch.from_numpy((a1v @ ln1).astype(np.float32))
# 重排 [D,S] -> [1, heads*dh, S] -> [1, heads, S, dh] -> [1, S, heads, dh]
def to_bhsd(x, heads, dh):
    x = x.numpy().reshape(heads, dh, -1)        # [heads,dh,S]
    x = x.transpose(0, 2, 1)                     # [heads,S,dh]
    return torch.from_numpy(x[None].astype(np.float32))
qb = to_bhsd(q, heads, dh); kb = to_bhsd(k, heads, dh); vb = to_bhsd(v, heads, dh)
ref = rd("stage_d0a0_a1.f32").reshape(dim, S)    # [D,S]
ref_t = torch.from_numpy(ref[None, None].astype(np.float32).repeat(heads, 1))  # dummy

# SDPA
o_sdpa = F.scaled_dot_product_attention(qb, kb, vb).numpy()   # [1,heads,S,dh]
o_sdpa = o_sdpa.transpose(0, 1, 3, 2).reshape(1, dim, S)[0]    # [D,S]
print("torch SDPA vs ref  diff =", np.abs(o_sdpa-ref).max())

# 手动 softmax（fp32）
sc = (qb @ kb.transpose(-1,-2)) / np.sqrt(dh)
scn = sc.numpy()
mx = scn.max(axis=-1, keepdims=True)
e = np.exp(scn - mx)
probs = e / e.sum(axis=-1, keepdims=True)
o_man = np.einsum('hst,hdt->hds', probs, vb.numpy()).reshape(1, dim, S)[0]
print("torch manual     vs ref  diff =", np.abs(o_man-ref).max())

# 我的 numpy 公式（float64 内部）
qs = q.numpy().reshape(heads, dh, S); ks = k.numpy().reshape(heads, dh, S); vs = v.numpy().reshape(heads, dh, S)
scm = np.einsum('hds,hdt->hst', qs, ks)/np.sqrt(dh)
mx = scm.max(axis=-1, keepdims=True)
e = np.exp(scm-mx); p = e/e.sum(axis=-1, keepdims=True)
o_my = np.einsum('hst,hdt->hds', p, vs).reshape(dim, S)
print("numpy manual     vs ref  diff =", np.abs(o_my-ref).max())
print("ref[0,0:4] =", ref[0,:4])
print("o_my[0,0:4] =", o_my[0,:4])
