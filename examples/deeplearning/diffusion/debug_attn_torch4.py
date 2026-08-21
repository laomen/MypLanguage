#!/usr/bin/env python3
"""debug_attn_torch4.py — 用 proj 作为 BasicTransformerBlock 输入，钩子对拍 norm1/attn1/attn2 与 stage 参考。"""
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
gn = rd("stage_d0a0_gn.f32").reshape(dim, S)
proj = (proj_w@gn)+proj_b[:,None]     # [D,S] 特征主序

caps = {}
def mk(name):
    def fn(mod, inp, out):
        o = out[0] if isinstance(out, tuple) else out
        caps[name] = o.detach().numpy().astype(np.float32)
    return fn

print("loading model...")
m = UNet2DConditionModel.from_pretrained(f"{SD}/unet", torch_dtype=torch.float32).to("cpu")
m.eval()
at = m.down_blocks[0].attentions[0]
tb = at.transformer_blocks[0]
tb.norm1.register_forward_hook(mk("ln1"))
tb.attn1.register_forward_hook(mk("attn1"))
tb.attn1.to_out[0].register_forward_hook(mk("a1o"))
tb.attn1.to_q.register_forward_hook(mk("q1"))
tb.attn1.to_k.register_forward_hook(mk("k1"))
tb.attn1.to_v.register_forward_hook(mk("v1"))
tb.norm2.register_forward_hook(mk("ln2"))
tb.attn2.register_forward_hook(mk("attn2"))
tb.attn2.to_out[0].register_forward_hook(mk("a2o"))
tb.attn2.to_q.register_forward_hook(mk("q2"))
tb.attn2.to_k.register_forward_hook(mk("k2"))
tb.attn2.to_v.register_forward_hook(mk("v2"))
tb.norm3.register_forward_hook(mk("ln3"))
tb.ff.net[0].register_forward_hook(mk("ffp"))
tb.ff.net[2].register_forward_hook(mk("ffo"))
at.proj_out.register_forward_hook(mk("po"))

text = rd("text_emb.bin").reshape(768,77).T[None].astype(np.float32)   # [1,77,768] token 主序
print("processor type:", type(tb.attn1.processor).__name__)
# monkeypatch to_out.0 捕获真实 raw attention（to_out 输入）
raw_cap = {}
orig_fwd = tb.attn1.to_out[0].forward
def rec_fwd(x, *a, **k):
    raw_cap["a1raw"] = x.detach().numpy()
    return orig_fwd(x, *a, **k)
tb.attn1.to_out[0].forward = rec_fwd
with torch.no_grad():
    h = torch.from_numpy(proj.T.astype(np.float32)).unsqueeze(0)       # [1,4096,320]
    out = tb(h, encoder_hidden_states=torch.from_numpy(text))
print("basic block out:", tuple((out if not isinstance(out,tuple) else out[0]).shape))

def cmp(name, ref, reshape=(dim,S)):
    a = caps[name]
    a = a.transpose(0,2,1)[0] if a.ndim==3 else a[0]
    r = rd(ref).reshape(reshape)
    print(f"{name:<6} vs {ref:<16} diff = {np.abs(a-r).max():.6g}")

for nm, rf in [("attn1","stage_d0a0_a1.f32"),("a1o","stage_d0a0_a1o.f32"),
               ("attn2","stage_d0a0_a2.f32")]:
    cmp(nm, rf)

# 落盘 MYP 对拍用（文件名带 d0a0_ 前缀，与 dbgCheck 名称一致）
def dump_file(name, arr):
    arr.astype(np.float32).reshape(-1).tofile(f"{DL}/myp_{name}.f32")
for capkey, fname in [("ln1","d0a0_ln1"),("a1o","d0a0_a1o"),("ln2","d0a0_ln2")]:
    a = caps[capkey]
    a = a.transpose(0,2,1)[0] if a.ndim==3 else a[0]
    dump_file(fname, a)
# raw attention 参考（to_out 之前，用 numpy 已验证公式）
def raw_attn_np(q,k,v,heads,dh,S,kvS):
    qs=q.reshape(heads,dh,S); ks=k.reshape(heads,dh,kvS); vs=v.reshape(heads,dh,kvS)
    sc=np.einsum('hds,hdt->hst',qs,ks)/np.sqrt(dh)
    mx=sc.max(-1,keepdims=True); e=np.exp(sc-mx); pr=e/e.sum(-1,keepdims=True)
    return np.einsum('hst,hdt->hds',pr,vs).reshape(heads*dh, S)
def to_fm(x):   # [1, seq, D] token 主序 → [D, seq] 特征主序
    return x.transpose(0,2,1)[0]
raw1 = raw_attn_np(to_fm(caps["q1"]), to_fm(caps["k1"]), to_fm(caps["v1"]), 8, 40, 4096, 4096)
raw2 = raw_attn_np(to_fm(caps["q2"]), to_fm(caps["k2"]), to_fm(caps["v2"]), 8, 40, 4096, 77)

# 落盘 MYP 对拍用（文件名带 d0a0_ 前缀，与 dbgCheck 名称一致）
def dump_file(name, arr):
    arr.astype(np.float32).reshape(-1).tofile(f"{DL}/myp_{name}.f32")
for capkey, fname in [("ln1","d0a0_ln1"),("a1o","d0a0_a1o"),("ln2","d0a0_ln2")]:
    a = caps[capkey]
    a = a.transpose(0,2,1)[0] if a.ndim==3 else a[0]
    dump_file(fname, a)
dump_file("d0a0_attn1raw", raw1)
dump_file("d0a0_attn2raw", raw2)
print("dumped myp_d0a0_*.f32")

# 决定性验证：to_out(真实SDPA(q1,k1,v1)) 是否 == torch attn1 参考（a1o）
import torch.nn.functional as F
def to_bhsd(x):   # x:[B,S,D] -> [B,heads,S,dh]，heads=8, dh=D/8
    return x.reshape(1, -1, 8, x.shape[-1]//8).transpose(1, 2)
q1t = torch.from_numpy(caps["q1"]); k1t = torch.from_numpy(caps["k1"]); v1t = torch.from_numpy(caps["v1"])
sdpa1 = F.scaled_dot_product_attention(to_bhsd(q1t), to_bhsd(k1t), to_bhsd(v1t))
sdpa1 = sdpa1.transpose(1,2).reshape(1, 320, -1)[0].numpy()  # [D,S] 特征主序
print("torch SDPA(qkv) vs my numpy raw1 diff =", np.abs(sdpa1-raw1).max())
# 模型真实 raw（to_out 输入，token 主序 [B,S,D]）
real_raw = raw_cap["a1raw"][0].T  # [D,S] 特征主序
print("model real raw vs my numpy raw1 diff =", np.abs(real_raw-raw1).max())
print("model real raw vs torch SDPA diff =", np.abs(real_raw-sdpa1).max())
# to_out(SDPA) vs a1o 参考（a1o 权重偏移：p=640+dim*dim+3*dim 之后）
p = 640+dim*dim+3*dim
a1o_w = sl(p+3*dim*dim, dim*dim).reshape(dim,dim); a1o_b = sl(p+4*dim*dim, dim)
ref_a1o = rd("stage_d0a0_a1o.f32").reshape(dim, S)
print("dense(a1o, SDPA)  vs a1o ref diff =", np.abs((a1o_w@sdpa1)+a1o_b[:,None]-ref_a1o).max())
print("dense(a1o, myraw) vs a1o ref diff =", np.abs((a1o_w@raw1)+a1o_b[:,None]-ref_a1o).max())

