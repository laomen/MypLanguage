#!/usr/bin/env python3
"""make_clip_bpe_tables.py — D2b：CLIP tokenizer（GPT-2 ByteLevel BPE）词表 → MYP 表 + 参考。

CLIP 用 GPT-2 ByteLevel BPE 分词器（vocab 49408，merges 48895，特殊 token
<|startoftext|>=49406 / <|endoftext|>=49407）。与 llm/make_bpe_tables.py 同表格式，
MYP 端复用 llm/bpe.myp 的 Bpe 类（load 带路径前缀）。

输出（data/diffusion/clip/）：
  bpe_merges_hash.bin / bpe_merges_rank.bin / bpe_merges_data.bin / bpe_merges_offs.bin
  bpe_vocab_hash.bin  / bpe_vocab_id.bin    / bpe_vocab_data.bin  / bpe_vocab_offs.bin
  d2b_prompt.txt                 测试 prompt 文本（UTF-8）
  d2b_ref_ids.bin / d2b_ref_mask.bin   CLIPTokenizer 参考 ids[77]+mask[77]（MYP 对拍）
用法：onnxvenv/bin/python deeplearning/diffusion/make_clip_bpe_tables.py
"""
import os
import json
import struct
import numpy as np
from transformers import CLIPTokenizer

DL = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL_DIR = os.path.join(DL, "data", "diffusion", "clip-vit-large-patch14")
OUT = os.path.join(DL, "data", "diffusion", "clip")

T = 1 << 17
MAX = 77

# ---------------- 字节 → 码点（与 make_bpe_tables.py 一致）----------------
def bytes_to_unicode():
    bs = list(range(ord("!"), ord("~") + 1)) + list(range(ord("¡"), ord("¬") + 1)) + list(range(ord("®"), ord("ÿ") + 1))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b)
            cs.append(256 + n)
            n += 1
    return dict(zip(bs, cs))

B2C = bytes_to_unicode()

tok = CLIPTokenizer.from_pretrained(MODEL_DIR)
tt = json.load(open(os.path.join(MODEL_DIR, "tokenizer.json")))
vocab = tt["model"]["vocab"]
merges = tt["model"]["merges"]
print(f"CLIP vocab={len(vocab)} merges={len(merges)}")

def char_codes(s):
    return [ord(c) for c in s]

id_codes = [None] * len(vocab)
for s, i in vocab.items():
    id_codes[i] = char_codes(s)

def fnv1a(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h

def build_hashtable(keys, vals, T):
    htab = [-1] * T
    vtab = [-1] * T
    for k, v in zip(keys, vals):
        h = fnv1a(k) & 0x7FFFFFFF
        i = h % T
        while htab[i] != -1:
            i = (i + 1) % T
        htab[i] = h
        vtab[i] = v
    return htab, vtab

def key_of_codes(codes, sep=None):
    out = bytearray()
    for c in codes:
        out += struct.pack("<H", c)
        if sep is not None:
            out += struct.pack("<H", sep)
    return bytes(out)

# ---------------- merges 表 ----------------
merge_pairs = [m.split() for m in merges]
pair_keys = []
for a, b in merge_pairs:
    ca = [ord(c) for c in a]
    cb = [ord(c) for c in b]
    pair_keys.append(key_of_codes(ca, None) + key_of_codes([0xFFFF], None) + key_of_codes(cb, None))
mhash, mrank = build_hashtable(pair_keys, list(range(len(pair_keys))), T)
offs = [0]
data = bytearray()
for k in pair_keys:
    data += k
    offs.append(len(data))
with open(os.path.join(OUT, "bpe_merges_hash.bin"), "wb") as f: f.write(np.array(mhash, np.int32).tobytes())
with open(os.path.join(OUT, "bpe_merges_rank.bin"), "wb") as f: f.write(np.array(mrank, np.int32).tobytes())
with open(os.path.join(OUT, "bpe_merges_data.bin"), "wb") as f: f.write(bytes(data))
with open(os.path.join(OUT, "bpe_merges_offs.bin"), "wb") as f: f.write(np.array(offs, np.int32).tobytes())
print("CLIP merges table:", len(pair_keys), "pairs,", len(data), "B")

# ---------------- vocab 表 ----------------
vkeys = [key_of_codes(id_codes[i], None) for i in range(len(id_codes))]
vhash, vid = build_hashtable(vkeys, list(range(len(vkeys))), T)
voffs = [0]
vdata = bytearray()
for k in vkeys:
    vdata += k
    voffs.append(len(vdata))
with open(os.path.join(OUT, "bpe_vocab_hash.bin"), "wb") as f: f.write(np.array(vhash, np.int32).tobytes())
with open(os.path.join(OUT, "bpe_vocab_id.bin"), "wb") as f: f.write(np.array(vid, np.int32).tobytes())
with open(os.path.join(OUT, "bpe_vocab_data.bin"), "wb") as f: f.write(bytes(vdata))
with open(os.path.join(OUT, "bpe_vocab_offs.bin"), "wb") as f: f.write(np.array(voffs, np.int32).tobytes())
print("CLIP vocab table:", len(vkeys), "tokens,", len(vdata), "B")

# ---------------- 参考：测试 prompt 的 ids + mask ----------------
# 可传 prompt 参数（默认猫图 prompt）；改 prompt 后重跑本脚本 + clip_tokenize.myp 对拍。
import sys
PROMPT = sys.argv[1] if len(sys.argv) > 1 else "a photo of a cat sitting on a table"
with open(os.path.join(OUT, "d2b_prompt.txt"), "w", encoding="utf-8") as f:
    f.write(PROMPT)
enc = tok(PROMPT, padding="max_length", max_length=MAX, truncation=True, return_tensors="pt")
ids = enc["input_ids"][0].numpy().astype(np.int32)
mask = enc["attention_mask"][0].numpy().astype(np.int32)
ids.tofile(os.path.join(OUT, "d2b_ref_ids.bin"))
mask.tofile(os.path.join(OUT, "d2b_ref_mask.bin"))
print(f"prompt '{PROMPT}' -> ids[:20]={ids[:20].tolist()}  n_valid={mask.sum()}")
print("special: sot=<|startoftext|>=", tok.convert_tokens_to_ids("<|startoftext|>"),
      "eot=<|endoftext|>=", tok.convert_tokens_to_ids("<|endoftext|>"))
print("D2b CLIP tables done.")
