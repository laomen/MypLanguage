#!/usr/bin/env python3
"""debug_attn.py — 逐步复算 down0 attention0 的 basic block，定位分歧步。
输入：stage_d0a0_gn.f32（已验证 1.6e-4）+ 修正后权重。逐步对照 torch 参考。
"""
import numpy as np

DL = "/home/xlkj/code/MYPLanguage/examples/deeplearning/data/diffusion/unet"
def rd(name):
    return np.fromfile(f"{DL}/{name}", dtype=np.float32)

w = rd("weights.bin")
gn = rd("stage_d0a0_gn.f32").reshape(320, 4096)   # [C,S] 特征主序
dim = 320; S = 4096; D = 320; kvS = 77; heads = 40; dh = 8; ff = 1280

base = 2266880  # base[2]
def slice_off(off, n):
    return w[base+off:base+off+n].copy()

# ---- 权重偏移（与 extraction attention_block 布局一致）----
proj_w = slice_off(640, dim*dim).reshape(dim, dim)          # [out,in]
proj_b = slice_off(640+dim*dim, dim)
n1_w = slice_off(640+dim*dim+dim, dim); n1_b = slice_off(640+dim*dim+2*dim, dim)
p = 640+dim*dim+3*dim   # a1q 起点
a1q = slice_off(p, dim*dim).reshape(dim,dim); a1k = slice_off(p+dim*dim, dim*dim).reshape(dim,dim)
a1v = slice_off(p+2*dim*dim, dim*dim).reshape(dim,dim); a1o = slice_off(p+3*dim*dim, dim*dim).reshape(dim,dim)
a1ob = slice_off(p+4*dim*dim, dim)
n2_w = slice_off(p+4*dim*dim+dim, dim); n2_b = slice_off(p+4*dim*dim+2*dim, dim)
p2 = p+4*dim*dim+3*dim
a2q = slice_off(p2, dim*dim).reshape(dim,dim)
a2k = slice_off(p2+dim*dim, dim*768).reshape(dim,768)
a2v = slice_off(p2+dim*dim+dim*768, dim*768).reshape(dim,768)
a2o = slice_off(p2+dim*dim+2*dim*768, dim*dim).reshape(dim,dim)
a2ob = slice_off(p2+dim*dim+2*dim*768+dim*dim, dim)

def dense(x, W, b):
    return (W @ x) + b[:, None]          # [out,S] = W[out,in] @ x[in,S]

def layernorm(x, g, b, eps=1e-5):
    mu = x.mean(axis=0, keepdims=True)
    var = ((x-mu)**2).mean(axis=0, keepdims=True)
    return (x-mu)/np.sqrt(var+eps)*g[:,None] + b[:,None]

def attn(q, k, v, heads, dh, kvS):
    # q,k,v: [D,S]；返回 [D,S]（合并头）
    H = heads
    Dm = dh*H
    qs = q.reshape(H, dh, -1)   # [H,dh,S]
    ks = k.reshape(H, dh, kvS)
    vs = v.reshape(H, dh, kvS)
    sc = np.einsum('hds,hdt->hst', qs, ks) / np.sqrt(dh)
    p = np.exp(sc - sc.max(axis=-1, keepdims=True))
    p = p / p.sum(axis=-1, keepdims=True)
    o = np.einsum('hst,hdt->hds', p, vs)
    return o.reshape(Dm, -1)

def mm(x, W):   # 无偏 matmul：W[out,in] @ x[in,batch]
    return W @ x

# ---- 逐步复算 ----
print("gn OK (输入, 已验证 1.6e-4)")
proj = dense(gn, proj_w, proj_b)
r_proj = rd("stage_d0a0_proj.f32").reshape(D, S)
print("proj_in   diff =", np.abs(proj-r_proj).max())

ln1 = layernorm(proj, n1_w, n1_b)
a1 = attn(ln1, ln1, ln1, heads, dh, S)
r_a1 = rd("stage_d0a0_a1.f32").reshape(D, S)
print("attn1     diff =", np.abs(a1-r_a1).max())

a1o = dense(a1, a1o, a1ob)
r_a1o = rd("stage_d0a0_a1o.f32").reshape(D, S)
print("attn1 to_out diff =", np.abs(a1o-r_a1o).max())

y1 = proj + a1o
ln2 = layernorm(y1, n2_w, n2_b)
text = rd("text_emb.bin").reshape(768, 77)
q = mm(ln2, a2q)
k = mm(text, a2k)
v = mm(text, a2v)
a2 = attn(q, k, v, heads, dh, kvS)
r_a2 = rd("stage_d0a0_a2.f32").reshape(D, S)
print("attn2     diff =", np.abs(a2-r_a2).max())
