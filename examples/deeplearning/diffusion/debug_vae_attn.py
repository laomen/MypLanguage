#!/usr/bin/env python3
"""debug_vae_attn.py — 逐步复算 VAE mid attention，定位分歧步。
输入：stage_mid_r0.f32（已验证 1.8e-5）+ base[3] 权重（group_norm + q/k/v + proj_attn 全带 bias）。
"""
import numpy as np

DL = "/home/xlkj/code/MYPLanguage/examples/deeplearning/data/diffusion/vae"
def rd(name): return np.fromfile(f"{DL}/{name}", dtype=np.float32)
w = rd("vae_weights.bin")
bases = np.fromfile(f"{DL}/bases.bin", dtype=np.int32)
base = int(bases[3])
print("base[3] =", base)
dim = 512; S = 4096; heads = 1; dh = dim // heads

def sl(off, n): return w[base+off:base+off+n].copy()
gnw = sl(0, dim); gnb = sl(dim, dim)
qw = sl(2*dim, dim*dim).reshape(dim, dim); qb = sl(2*dim+dim*dim, dim)
kw = sl(2*dim+dim*dim+dim, dim*dim).reshape(dim, dim); kb = sl(2*dim+dim*dim+dim+dim*dim, dim)
vw = sl(2*dim+dim*dim+2*dim+dim*dim, dim*dim).reshape(dim, dim); vb = sl(2*dim+dim*dim+2*dim+dim*dim+dim*dim, dim)
p = 2*dim+dim*dim+2*dim+2*dim*dim+dim
pw = sl(p, dim*dim).reshape(dim, dim); pb = sl(p+dim*dim, dim)

x = rd("stage_mid_r0.f32").reshape(dim, S)

def groupnorm(x, g, b, groups, eps=1e-6):
    C = x.shape[0]; cpg = C // groups
    out = np.zeros_like(x)
    for gr in range(groups):
        seg = x[gr*cpg:(gr+1)*cpg]
        mean = seg.mean()
        var = ((seg-mean)**2).mean()
        rstd = 1.0/np.sqrt(var+eps)
        out[gr*cpg:(gr+1)*cpg] = (seg-mean)*rstd*g[gr*cpg:(gr+1)*cpg,None] + b[gr*cpg:(gr+1)*cpg,None]
    return out

def dense(x, W, b): return (W @ x) + b[:, None]
def attn(q, k, v, heads, dh, kvS):
    qs=q.reshape(heads,dh,-1); ks=k.reshape(heads,dh,kvS); vs=v.reshape(heads,dh,kvS)
    sc=np.einsum('hds,hdt->hst',qs,ks)/np.sqrt(dh)
    mx=sc.max(-1,keepdims=True); e=np.exp(sc-mx); pr=e/e.sum(-1,keepdims=True)
    return np.einsum('hst,hdt->hds',pr,vs).reshape(heads*dh,-1)

g = groupnorm(x, gnw, gnb, 32)
print("== gn done ==")
q = dense(g, qw, qb); k = dense(g, kw, kb); v = dense(g, vw, vb)
a = attn(q, k, v, heads, dh, S)
o = dense(a, pw, pb)
y = x + o
r_a0 = rd("stage_mid_a0.f32").reshape(dim, S)
print("attn+proj+residual vs mid_a0 diff =", np.abs(y-r_a0).max())
print("ref mid_a0[0,:4] =", r_a0[0,:4])
print("my  y[0,:4]      =", y[0,:4])
print("gn[0,:4] =", g[0,:4])
for nm, arr in [("vae_gn", g), ("vae_q", q), ("vae_attn", a), ("vae_proj", o)]:
    arr.astype(np.float32).reshape(-1).tofile(f"{DL}/myp_{nm}.f32")
print("dumped myp_vae_*.f32")
