#!/usr/bin/env python3
"""debug_mid_torch.py — 用真实模型直接对拍 mid attn1（输入=已验证 proj），判定 numpy/MYP attention 数学。
"""
import numpy as np
import torch
from diffusers import UNet2DConditionModel

DL = "/home/xlkj/code/MYPLanguage/examples/deeplearning/data/diffusion/unet"
SD = "/home/xlkj/code/MYPLanguage/examples/deeplearning/data/diffusion/sd15"
def rd(name): return np.fromfile(f"{DL}/{name}", dtype=np.float32)
w = rd("weights.bin"); bases = np.fromfile(f"{DL}/bases.bin", dtype=np.int32)
base = int(bases[19])
dim=1280; S=64; kvS=77; heads=8; dh=dim//heads
def sl(off,n): return w[base+off:base+off+n].copy()
pw=sl(2*dim,dim*dim).reshape(dim,dim); pb=sl(2*dim+dim*dim,dim)
gn = rd("stage_mid_gn.f32").reshape(dim,S)
proj = (pw@gn)+pb[:,None]

caps={}
def mk(name):
    def fn(mod,inp,out):
        o=out[0] if isinstance(out,tuple) else out
        caps[name]=o.detach().numpy().astype(np.float32)
    return fn
print("loading model...")
m = UNet2DConditionModel.from_pretrained(f"{SD}/unet", torch_dtype=torch.float32).to("cpu")
m.eval()
tb = m.mid_block.attentions[0].transformer_blocks[0]
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
with torch.no_grad():
    h = torch.from_numpy(proj.T.astype(np.float32)).unsqueeze(0)  # [1,64,1280]
    text = torch.from_numpy(rd("text_emb.bin").reshape(768,kvS).T[None].astype(np.float32))
    tb(h, encoder_hidden_states=text)
r_a1o = rd("stage_mid_a1o.f32").reshape(dim,S)
c_a1o = caps["a1o"].transpose(0,2,1)[0]
print("torch a1o vs mid_a1o ref diff =", np.abs(c_a1o-r_a1o).max())
# 对拍我的 numpy ln1（layerNorm(proj)） vs 模型真实 ln1
def ln(x, g, b, eps=1e-5):
    mu=x.mean(0,keepdims=True); var=((x-mu)**2).mean(0,keepdims=True)
    return (x-mu)/np.sqrt(var+eps)*g[:,None]+b[:,None]
n1w = sl(1642240, dim); n1b = sl(1643520, dim)
my_ln1 = ln(proj, n1w, n1b)
real_ln1 = caps["ln1"].transpose(0,2,1)[0]
print("my numpy ln1 vs model real ln1 diff =", np.abs(my_ln1-real_ln1).max())
# numpy attn1
import numpy as np
def raw_attn(qn,kn,vn, kvS=64):
    qs=qn.reshape(heads,dh,-1); ks=kn.reshape(heads,dh,kvS); vs=vn.reshape(heads,dh,kvS)
    sc=np.einsum('hds,hdt->hst',qs,ks)/np.sqrt(dh)
    mx=sc.max(-1,keepdims=True); e=np.exp(sc-mx); pr=e/e.sum(-1,keepdims=True)
    return np.einsum('hst,hdt->hds',pr,vs).reshape(dim,-1)
q = caps["q1"].transpose(0,2,1)[0]; k = caps["k1"].transpose(0,2,1)[0]; v = caps["v1"].transpose(0,2,1)[0]
my_raw = raw_attn(q,k,v)
print("numpy raw attn vs torch-true-raw? (间接) a1o via to_out:")
a1o_w = sl(1644800, dim*dim).reshape(dim,dim); a1o_b = sl(1644800+dim*dim, dim)
print("  dense(my_raw,a1o) vs mid_a1o ref diff =", np.abs((a1o_w@my_raw)+a1o_b[:,None]-r_a1o).max())
# 对比 numpy raw 与模型真实 raw（monkeypatch to_out 捕获）
raw_cap={}
orig=tb.attn1.to_out[0].forward
def rec(x,*a,**k):
    raw_cap["raw"]=x.detach().numpy(); return orig(x,*a,**k)
tb.attn1.to_out[0].forward=rec
with torch.no_grad():
    text = torch.from_numpy(rd("text_emb.bin").reshape(768,kvS).T[None].astype(np.float32))
    tb(torch.from_numpy(proj.T.astype(np.float32)).unsqueeze(0), encoder_hidden_states=text)
real_raw = raw_cap["raw"][0].T  # [D,S]
print("model real raw vs numpy raw diff =", np.abs(real_raw-my_raw).max())

# 落盘 myp_mida0_* 参考（MYP 对拍用）
def dumpf(name, arr): arr.astype(np.float32).reshape(-1).tofile(f"{DL}/myp_{name}.f32")
def fm(x): return x.transpose(0,2,1)[0] if x.ndim==3 else x[0]
dumpf("mida0_ln1", fm(caps["ln1"]))
dumpf("mida0_attn1raw", my_raw)
dumpf("mida0_a1o", fm(caps["a1o"]))
dumpf("mida0_ln2", fm(caps["ln2"]))
dumpf("mida0_attn2raw", raw_attn(fm(caps["q2"]), fm(caps["k2"]), fm(caps["v2"]), kvS=kvS))
dumpf("mida0_a2o", fm(caps["a2o"]))
print("dumped myp_mida0_*.f32")
