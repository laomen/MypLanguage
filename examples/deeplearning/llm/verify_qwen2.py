#!/usr/bin/env python3
"""verify_qwen2.py — numpy 复刻 Qwen2 前向（RMSNorm/RoPE/GQA/SwiGLU）vs transformers 参考 logits。

用 extract_qwen2.py 提取的 qwen2_weights.bin，实现标准 Qwen2 前向（[D,S] 布局），
对拍 transformers 的最后一个 token logits（qwen2_ref_logits.npy）。验证架构理解后再写 MYP。
"""
import os
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
D = os.path.join(ROOT, "deeplearning", "data", "llm")
ONNXW = os.path.join(D, "qwen2_weights.bin")

H, L, QH, KVH, FFN, V = 896, 24, 14, 2, 4864, 151936
HD = H // QH   # 64
THETA = 1e6
EPS = 1e-6
POS = 32768

with open(ONNXW, "rb") as f:
    raw = f.read()
def rd(off, shape):
    n = int(np.prod(shape))
    return np.frombuffer(raw, dtype=np.float32, count=n, offset=off * 4).reshape(shape).copy()

wte = rd(0, (V, H))
base0 = V * H
H2 = H * H
KV = KVH * HD * H     # 128*64... = 114688
KB = KVH * HD         # 128
F = FFN * H
# 布局: ln1,q,qb,k,kb,v,vb,o,ln2,gate,up,down
per = H + H2 + H + KV + KB + KV + KB + H2 + H + 3 * F   # 14912384
off = 0
ly = {
    "ln1": 0, "q": H, "qb": H + H2,
    "k": H + H2 + H, "kb": H + H2 + H + KV,
    "v": H + H2 + H + KV + KB, "vb": H + H2 + H + KV + KB + KV,
    "o": H + H2 + H + KV + KB + KV + KB,
    "ln2": H + H2 + H + KV + KB + KV + KB + H2,
    "gate": H + H2 + H + KV + KB + KV + KB + H2 + H,
    "up": H + H2 + H + KV + KB + KV + KB + H2 + H + F,
    "down": H + H2 + H + KV + KB + KV + KB + H2 + H + 2 * F,
}
final_norm_off = base0 + L * per
lnf = rd(final_norm_off, (H,))

# 预计算 cos/sin 表 [half, maxpos]
half = HD // 2
freq = THETA ** (-2.0 * np.arange(half) / HD)
cos_tab = np.zeros((half, POS))
sin_tab = np.zeros((half, POS))
for p in range(POS):
    ang = p * freq
    cos_tab[:, p] = np.cos(ang)
    sin_tab[:, p] = np.sin(ang)

def rmsnorm(x, w, eps=EPS):
    ms = (x ** 2).mean(axis=0, keepdims=True)   # [1,S]
    return x / np.sqrt(ms + eps) * w[:, None]

def silu(x):
    return x * (1.0 / (1.0 + np.exp(-x)))

def rope_head(x, pos):
    # x [dh, S]; 逐位置旋转（half 内配对）
    h = x.shape[0] // 2
    out = x.copy()
    for s in range(len(pos)):
        c = cos_tab[:, pos[s]]
        sn = sin_tab[:, pos[s]]
        x0 = x[:h, s]; x1 = x[h:, s]
        out[:h, s] = x0 * c - x1 * sn
        out[h:, s] = x0 * sn + x1 * c
    return out

def forward(ids):
    S = len(ids)
    x = wte[ids].T.copy()   # [D,S]
    for li in range(L):
        b = base0 + li * per
        wln1 = rd(b + ly["ln1"], (H,))
        wq = rd(b + ly["q"], (H, H))
        bq = rd(b + ly["qb"], (H,))
        wk = rd(b + ly["k"], (KVH * HD, H))
        bk = rd(b + ly["kb"], (KVH * HD,))
        wv = rd(b + ly["v"], (KVH * HD, H))
        bv = rd(b + ly["vb"], (KVH * HD,))
        wo = rd(b + ly["o"], (H, H))
        wln2 = rd(b + ly["ln2"], (H,))
        wg = rd(b + ly["gate"], (FFN, H))
        wu = rd(b + ly["up"], (FFN, H))
        wd = rd(b + ly["down"], (H, FFN))
        xn = rmsnorm(x, wln1)
        q = wq @ xn + bq[:, None]             # [H,S]（投影带 bias）
        k = wk @ xn + bk[:, None]             # [KVH*HD,S]
        v = wv @ xn + bv[:, None]
        q = q.reshape(QH, HD, S)
        k = k.reshape(KVH, HD, S)
        v = v.reshape(KVH, HD, S)
        pos = np.arange(S)
        for h in range(QH):
            q[h] = rope_head(q[h], pos)
        for h in range(KVH):
            k[h] = rope_head(k[h], pos)
        # GQA attention（因果）
        ctx = np.zeros((QH, HD, S))
        for h in range(QH):
            kvh = h // (QH // KVH)
            sc = np.einsum("ds,dt->st", q[h], k[kvh]) / np.sqrt(HD)   # [S,S] query s, key t
            mask = np.triu(np.ones((S, S)), 1) * -1e9
            sc = sc + mask
            p = np.exp(sc - sc.max(-1, keepdims=True))
            p = p / p.sum(-1, keepdims=True)
            ctx[h] = np.einsum("ij,dj->id", p, v[kvh]).T   # [S,HD] -> [HD,S]
        ctx = ctx.reshape(H, S)
        x = x + wo @ ctx
        xn = rmsnorm(x, wln2)
        h1 = silu(wg @ xn) * (wu @ xn)
        x = x + wd @ h1
    xf = rmsnorm(x, lnf)
    logits = wte @ xf     # [V,S]
    return logits[:, -1]

# ---- 加载参考 prompt ids ----
import numpy as np
p = np.fromfile(os.path.join(D, "qwen2_prompt_ids.bin"), dtype=np.int32)
ids = p[1:].tolist()
print("prompt ids n =", len(ids), ids)

lg = forward(ids)
ref = np.load(os.path.join(D, "qwen2_ref_logits.npy"))
d = np.abs(lg - ref)
print("max abs diff vs transformers:", d.max())
print("numpy argmax:", int(lg.argmax()), " ref argmax:", int(ref.argmax()))
top = np.argsort(lg)[::-1][:8]
print("numpy top8:", [int(t) for t in top])
print("ref   top8:", [int(t) for t in np.argsort(ref)[::-1][:8]])
# 参考来自 bf16 模型，fp32 numpy 前向有量化噪声；argmax 一致 + 噪声量级即 OK
print("RESULT:", "OK" if int(lg.argmax()) == int(ref.argmax()) and d.max() < 1.0 else "MISMATCH")
