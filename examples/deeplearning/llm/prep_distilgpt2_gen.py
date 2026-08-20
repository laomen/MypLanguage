#!/usr/bin/env python3
"""prep_distilgpt2_gen.py — 为 MYP 端 distilgpt2 CPU 推理准备输入 + 生成权威参考。

1) 用 GPT2Tokenizer 把 prompt 编码成 token id → data/llm/distilgpt2_prompt_ids.bin
   （int32 LE：count + ids）。
2) 构建 id→字节文本 查表（byte-level decode 后的 UTF-8 字节）→ 供 MYP 端 decode：
   - data/llm/distilgpt2_vocab_offs.bin（int32 LE，V+1 个偏移）
   - data/llm/distilgpt2_vocab_data.bin（全部 token 的原始字节拼接）
   （已存在则跳过，避免每次重建 50K 项。）
3) 写生成配置 data/llm/distilgpt2_gen_cfg.bin（int32：生成 token 数）。
4) 可选（默认开）用 transformers 同 prompt 贪心生成 → 期望 id 序列
   data/llm/distilgpt2_ref_gen_ids.bin + 打印参考文本（供 MYP 对拍验证）。

用法（onnxvenv 内）：
  onnxvenv/bin/python prep_distilgpt2_gen.py ["prompt"] [n_tokens] [--no-ref]
  例：
    prep_distilgpt2_gen.py                          # 默认 "Once upon a time" / 32
    prep_distilgpt2_gen.py "The capital of France" 64
    prep_distilgpt2_gen.py "Hello world" 24 --no-ref
"""
import os
import sys
import numpy as np
import torch
from transformers import GPT2LMHeadModel, GPT2Tokenizer

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
D = os.path.join(ROOT, "deeplearning", "data", "llm")
MODEL_DIR = os.path.join(D, "distilgpt2")

PROMPT = "Once upon a time"
GENERATE = 32   # 生成 token 数（+prompt 后总长应 < 滑窗 W=128，保证逐位一致）
NO_REF = False

argv = sys.argv[1:]
ai = 0
while ai < len(argv):
    a = argv[ai]
    if a == "--no-ref":
        NO_REF = True
    elif a.isdigit():
        GENERATE = int(a)
    elif not a.startswith("-") and PROMPT == "Once upon a time":
        PROMPT = a
    ai = ai + 1

tok = GPT2Tokenizer.from_pretrained(MODEL_DIR)
m = GPT2LMHeadModel.from_pretrained(MODEL_DIR)
m.eval()

ids = tok.encode(PROMPT)
print("prompt:", repr(PROMPT))
print("prompt ids:", ids)
print("generate tokens:", GENERATE, "(no-ref)" if NO_REF else "")

# ---- 1) 写 prompt ids + prompt 文本（MYP 端 BPE 编码输入）----
with open(os.path.join(D, "distilgpt2_prompt_ids.bin"), "wb") as f:
    f.write(np.array([len(ids)] + ids, dtype=np.int32).tobytes())
with open(os.path.join(D, "distilgpt2_prompt.txt"), "wb") as f:
    f.write(PROMPT.encode("utf-8"))

# ---- 3) 写生成配置 ----
with open(os.path.join(D, "distilgpt2_gen_cfg.bin"), "wb") as f:
    f.write(np.array([GENERATE], dtype=np.int32).tobytes())

# ---- 2) 构建 id→字节 查表（已存在则跳过）----
V = m.config.vocab_size
offs_path = os.path.join(D, "distilgpt2_vocab_offs.bin")
data_path = os.path.join(D, "distilgpt2_vocab_data.bin")
if os.path.exists(offs_path) and os.path.exists(data_path):
    print("vocab table exists, skip rebuild")
else:
    offs = [0]
    data = bytearray()
    for i in range(V):
        b = tok.decode([i]).encode("utf-8")
        data += b
        offs.append(len(data))
    with open(offs_path, "wb") as f:
        f.write(np.array(offs, dtype=np.int32).tobytes())
    with open(data_path, "wb") as f:
        f.write(bytes(data))
    print(f"vocab table built: V={V} offs={len(offs)} data={len(data)}B")

# ---- 3) transformers 贪心生成（权威参考；--no-ref 时跳过）----
ref_path = os.path.join(D, "distilgpt2_ref_gen_ids.bin")
if NO_REF:
    if os.path.exists(ref_path):
        os.remove(ref_path)
    print("skipped reference generation (--no-ref); removed stale ref for clean run")
else:
    g = list(ids)
    past = None
    with torch.no_grad():
        for _ in range(GENERATE):
            inp = torch.tensor([g[-1:] if past is not None else g])
            out = m(inp, past_key_values=past, use_cache=True)
            past = out.past_key_values
            nxt = int(out.logits[0, -1].argmax())
            g.append(nxt)

    with open(os.path.join(D, "distilgpt2_ref_gen_ids.bin"), "wb") as f:
        f.write(np.array([len(g)] + g, dtype=np.int32).tobytes())

    print("full greedy ids:", g)
    print("generated (after prompt):", g[len(ids):])
    print("\n==== generated text ====")
    print(tok.decode(g))
    print("====")
