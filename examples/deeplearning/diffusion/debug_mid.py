#!/usr/bin/env python3
"""debug_mid.py — 逐步复算 mid attention 块，定位分歧步。
输入：stage_mid_gn.f32（已验证 7.2e-5）+ base[19] 权重。逐步对照 stage_mid_* 参考。
"""
import numpy as np

DL = "/home/xlkj/code/MYPLanguage/examples/deeplearning/data/diffusion/unet"
def rd(name): return np.fromfile(f"{DL}/{name}", dtype=np.float32)
w = rd("weights.bin")
bases = np.fromfile(f"{DL}/bases.bin", dtype=np.int32)
base = int(bases[19])
print("base[19] =", base)

dim = 1280; S = 64; kvS = 77; heads = 8; dh = dim//heads; ff = dim*4
def sl(off, n): return w[base+off:base+off+n].copy()
# 布局（与 extraction attention_block 一致，含 norm2）
nw  = sl(0, dim); nb = sl(dim, dim)
pw  = sl(2*dim, dim*dim).reshape(dim,dim); pb = sl(2*dim+dim*dim, dim)
n1w = sl(2*dim+dim*dim+dim, dim); n1b = sl(2*dim+dim*dim+2*dim, dim)
p = 2*dim+dim*dim+3*dim
a1q = sl(p, dim*dim).reshape(dim,dim); a1k = sl(p+dim*dim, dim*dim).reshape(dim,dim)
a1v = sl(p+2*dim*dim, dim*dim).reshape(dim,dim); a1o = sl(p+3*dim*dim, dim*dim).reshape(dim,dim)
a1ob = sl(p+4*dim*dim, dim)
n2w = sl(p+4*dim*dim+dim, dim); n2b = sl(p+4*dim*dim+2*dim, dim)
p2 = p+4*dim*dim+3*dim
a2q = sl(p2, dim*dim).reshape(dim,dim)
a2k = sl(p2+dim*dim, dim*768).reshape(dim,768)
a2v = sl(p2+dim*dim+dim*768, dim*768).reshape(dim,768)
a2o = sl(p2+dim*dim+2*dim*768, dim*dim).reshape(dim,dim)
a2ob = sl(p2+dim*dim+2*dim*768+dim*dim, dim)
n3w = sl(p2+dim*dim+2*dim*768+dim*dim+dim, dim); n3b = sl(p2+dim*dim+2*dim*768+dim*dim+2*dim, dim)
p3 = p2+dim*dim+2*dim*768+dim*dim+3*dim
ffpw = sl(p3, 2*ff*dim).reshape(2*ff, dim); ffpb = sl(p3+2*ff*dim, 2*ff)
fflw = sl(p3+2*ff*dim+2*ff, dim*ff).reshape(dim, ff); fflb = sl(p3+2*ff*dim+2*ff+dim*ff, dim)
pow  = sl(p3+2*ff*dim+2*ff+dim*ff+dim, dim*dim).reshape(dim,dim)
pob  = sl(p3+2*ff*dim+2*ff+dim*ff+dim+dim*dim, dim)

def dense(x, W, b): return (W @ x) + b[:, None]
def ln(x, g, b, eps=1e-5):
    mu = x.mean(0, keepdims=True); var = ((x-mu)**2).mean(0, keepdims=True)
    return (x-mu)/np.sqrt(var+eps)*g[:,None] + b[:,None]
def attn(q, k, v, kvS):
    qs=q.reshape(heads,dh,-1); ks=k.reshape(heads,dh,kvS); vs=v.reshape(heads,dh,kvS)
    sc=np.einsum('hds,hdt->hst',qs,ks)/np.sqrt(dh)
    mx=sc.max(-1,keepdims=True); e=np.exp(sc-mx); pr=e/e.sum(-1,keepdims=True)
    return np.einsum('hst,hdt->hds',pr,vs).reshape(dim,-1)
def gelu(x):
    # MYP InferOps.gelu 同款 Abramowitz-Stegun erf 近似
    z = x / np.sqrt(2.0)
    a = np.abs(z)
    t = 1.0/(1.0+0.3275911*a)
    y = 1.0 - (((((1.061405429*t - 1.453152027)*t) + 1.421413741)*t - 0.284496736)*t + 0.254829592)*t*np.exp(-a*a)
    erf = np.where(z < 0, -y, y)
    return 0.5*x*(1.0+erf)

gn = rd("stage_mid_gn.f32").reshape(dim, S)
print("== gn OK（输入，已验证） ==")
proj = dense(gn, pw, pb)
print("proj vs mid_proj  diff =", np.abs(proj - rd("stage_mid_proj.f32").reshape(dim,S)).max())
ln1 = ln(proj, n1w, n1b)
q = dense(ln1, a1q, np.zeros(dim)); k = dense(ln1, a1k, np.zeros(dim)); v = dense(ln1, a1v, np.zeros(dim))
a1 = attn(q, k, v, S)
a1o = dense(a1, a1o, a1ob)
print("attn1+to_out vs mid_a1o diff =", np.abs(a1o - rd("stage_mid_a1o.f32").reshape(dim,S)).max())
y1 = proj + a1o
ln2 = ln(y1, n2w, n2b)
print("ln2 vs mid_ln2     diff =", np.abs(ln2 - rd("stage_mid_ln2.f32").reshape(dim,S)).max())
text = rd("text_emb.bin").reshape(768, kvS)
q = dense(ln2, a2q, np.zeros(dim)); k = dense(text, a2k, np.zeros(dim)); v = dense(text, a2v, np.zeros(dim))
a2 = attn(q, k, v, kvS)
a2o = dense(a2, a2o, a2ob)
print("attn2+to_out vs mid_a2o diff =", np.abs(a2o - rd("stage_mid_a2o.f32").reshape(dim,S)).max())
y1 = y1 + a2o
ln3 = ln(y1, n3w, n3b)
ffp = dense(ln3, ffpw, ffpb)
a = ffp[:ff]; b = ffp[ff:]
g = gelu(b)
ffm = a*g
ffo = dense(ffm, fflw, fflb)
y1 = y1 + ffo
po = dense(y1, pow, pob)
print("proj_out vs mid_po diff =", np.abs(po - rd("stage_mid_po.f32").reshape(dim,S)).max())

# 落盘 MYP 对拍参考
def dumpf(name, arr): arr.astype(np.float32).reshape(-1).tofile(f"{DL}/myp_{name}.f32")
dumpf("mida0_ln3", ln3)
dumpf("mida0_ffp", ffp)
dumpf("mida0_ffm", ffm)
dumpf("mida0_ffo", ffo)
print("dumped myp_mida0_ln3/ffp/ffm/ffo")
