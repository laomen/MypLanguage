#!/usr/bin/env python3
"""make_qwen2_tiktoken_tables.py — 为 MYP 端 Qwen2 tiktoken 分词器构建二进制表 + 验证（vs transformers）。

Qwen2 分词 = GPT-2 式 ByteLevel BPE，但：
  - 预分词正则不同（Qwen2 版，见下），add_prefix_space=False；
  - 3 个特殊 token：<|endoftext|>=151643, <|im_start|>=151644, <|im_end|>=151645；
  - BPE vocab 151643 项（0..151642），其中 151640-151642 是三个特殊串的字节编码形式
    （tiktoken 在正则层拦截特殊串 → 直接出 151643-151645，BPE 永不产出 151640-151642）；
  - merges 151387 条 > 2^17 → 开放寻址表用 T=2^19。

预分词正则（Qwen2，tiktoken GPT2-like）：
  (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+

输出（data/llm/）：
  qwen2_merges_hash.bin / qwen2_merges_rank.bin（开放寻址，T=2^19）
  qwen2_merges_data.bin / qwen2_merges_offs.bin（key 码点池，int16 LE，pair 间 0xFFFF）
  qwen2_vocab_hash.bin / qwen2_vocab_id.bin（编码查表，ids 0..151639，T=2^19）
  qwen2_vocab_data.bin / qwen2_vocab_offs.bin（编码码点池）
  qwen2_dec_offs.bin / qwen2_dec_data.bin（解码：id → 原始 UTF-8 字节，ids 0..151645）
  qwen2_ref_encode.bin / qwen2_ref_texts.bin（测试 prompt 的 tiktoken 参考 ids/文本）

用法（onnxvenv 内）：
  onnxvenv/bin/python make_qwen2_tiktoken_tables.py
"""
import os
import json
import struct
import numpy as np
import regex as re
from transformers import AutoTokenizer

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
D = os.path.join(ROOT, "deeplearning", "data", "llm")
MODEL_DIR = os.path.join(D, "qwen2-0.5b-instruct")

T = 1 << 19   # 开放寻址表大小（merges 151387 > 2^17）

# ---------------- 字节 → 码点表 ----------------
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
C2B = {c: b for b, c in B2C.items()}

# ---------------- 读 tokenizer ----------------
tok = AutoTokenizer.from_pretrained(MODEL_DIR)
tt = json.load(open(os.path.join(MODEL_DIR, "tokenizer.json")))
vocab = tt["model"]["vocab"]      # 字节编码串 → id（0..151642）
merges = tt["model"]["merges"]    # ["a b", ...] 151387

SPECIAL_STR = {151643: "<|endoftext|>", 151644: "<|im_start|>", 151645: "<|im_end|>"}

# 编码 vocab：ids 0..151639（排除字节编码的特殊串 151640-151642）
id_codes = [None] * 151640
for s, i in vocab.items():
    if i < 151640:
        id_codes[i] = [ord(c) for c in s]

# ---------------- 哈希表工具（同 make_bpe_tables.py） ----------------
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

# ---------------- 构建 merges 表 ----------------
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
with open(os.path.join(D, "qwen2_merges_hash.bin"), "wb") as f: f.write(np.array(mhash, np.int32).tobytes())
with open(os.path.join(D, "qwen2_merges_rank.bin"), "wb") as f: f.write(np.array(mrank, np.int32).tobytes())
with open(os.path.join(D, "qwen2_merges_data.bin"), "wb") as f: f.write(bytes(data))
with open(os.path.join(D, "qwen2_merges_offs.bin"), "wb") as f: f.write(np.array(offs, np.int32).tobytes())
print("merges table:", len(pair_keys), "pairs, data", len(data), "B")

# ---------------- 构建编码 vocab 表（ids 0..151639） ----------------
vkeys = [key_of_codes(id_codes[i], None) for i in range(151640)]
vhash, vid = build_hashtable(vkeys, list(range(151640)), T)
voffs = [0]
vdata = bytearray()
for k in vkeys:
    vdata += k
    voffs.append(len(vdata))
with open(os.path.join(D, "qwen2_vocab_hash.bin"), "wb") as f: f.write(np.array(vhash, np.int32).tobytes())
with open(os.path.join(D, "qwen2_vocab_id.bin"), "wb") as f: f.write(np.array(vid, np.int32).tobytes())
with open(os.path.join(D, "qwen2_vocab_data.bin"), "wb") as f: f.write(bytes(vdata))
with open(os.path.join(D, "qwen2_vocab_offs.bin"), "wb") as f: f.write(np.array(voffs, np.int32).tobytes())
print("encode vocab table:", 151640, "tokens, data", len(vdata), "B")

# ---------------- 构建解码表（id → 原始字节；0..151645） ----------------
# 0..151639：字节编码串解码回原始 UTF-8；151640-151645：特殊串（模型只生成 151643-151645）
def codes_to_bytes(codes):
    return bytes(C2B[c] for c in codes)

dec_offs = [0]
dec_data = bytearray()
for i in range(151640):
    b = codes_to_bytes(id_codes[i])
    dec_data += b
    dec_offs.append(len(dec_data))
for i in range(151640, 151646):
    # 151640-151642 与 151643-151645 都是特殊串（tiktoken 正则层拦截才用 151643+）
    si = i - 151640 if i < 151643 else i - 151643
    b = SPECIAL_STR[151643 + si].encode("utf-8")
    dec_data += b
    dec_offs.append(len(dec_data))
with open(os.path.join(D, "qwen2_dec_offs.bin"), "wb") as f: f.write(np.array(dec_offs, np.int32).tobytes())
with open(os.path.join(D, "qwen2_dec_data.bin"), "wb") as f: f.write(bytes(dec_data))
print("decode table: 151646 ids, data", len(dec_data), "B")

# ---------------- 算法验证：regex BPE vs transformers ----------------
PAT = re.compile(r"""(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+""")
rank_map = {}
for rank, (a, b) in enumerate(merge_pairs):
    rank_map[(a, b)] = rank

def bpe_encode(text):
    out = []
    for w in PAT.findall(text):
        codes = [B2C[b] for b in w.encode("utf-8")]
        pieces = [[c] for c in codes]
        while len(pieces) > 1:
            best = None
            for j in range(len(pieces) - 1):
                a = "".join(chr(c) for c in pieces[j])
                b = "".join(chr(c) for c in pieces[j + 1])
                r = rank_map.get((a, b), float("inf"))
                if best is None or r < best[0]:
                    best = (r, j)
            if best[0] == float("inf"):
                break
            j = best[1]
            pieces[j] = pieces[j] + pieces[j + 1]
            del pieces[j + 1]
        for p in pieces:
            s = "".join(chr(c) for c in p)
            out.append(vocab[s])
    return out

def hash_lookup(hash_tab, val_tab, key, offs, data_pool):
    h = fnv1a(key) & 0x7FFFFFFF
    i = h % T
    while hash_tab[i] != -1:
        if hash_tab[i] == h:
            v = val_tab[i]
            lo = offs[v]; hi = offs[v + 1]
            if data_pool[lo:hi] == key:
                return v
        i = (i + 1) % T
    return -1

mdata = bytes(data); moffs = np.array(offs, np.int32)
vdata_b = bytes(vdata); voffs = np.array(voffs, np.int32)

def bpe_encode_hash(text):
    out = []
    for w in PAT.findall(text):
        codes = [B2C[b] for b in w.encode("utf-8")]
        pieces = [[c] for c in codes]
        while len(pieces) > 1:
            best_r = float("inf"); best_j = -1
            for j in range(len(pieces) - 1):
                key = key_of_codes(pieces[j], None) + key_of_codes([0xFFFF], None) + key_of_codes(pieces[j + 1], None)
                r = hash_lookup(mhash, mrank, key, moffs, mdata)
                if r != -1 and r < best_r:
                    best_r = r; best_j = j
            if best_j < 0:
                break
            pieces[best_j] = pieces[best_j] + pieces[best_j + 1]
            del pieces[best_j + 1]
        for p in pieces:
            tid = hash_lookup(vhash, vid, key_of_codes(p, None), voffs, vdata_b)
            out.append(tid)
    return out

tests = [
    "Hello, how are you today?",
    "Once upon a time",
    "The capital of France is Paris.",
    "In a world where machines think",
    "The quick brown fox jumps over the lazy dog.",
    "I love to eat pizza with extra cheese!",
    "A 'very' important lesson: don't panic.",
    "The year is 2026 and AI is everywhere.",
    "Can you explain quantum computing in simple terms?",
    "Write a short story about a dragon.",
]
print("\n=== tokenizer verification (regex vs hash-table vs transformers) ===")
allok = True
for txt in tests:
    r1 = bpe_encode(txt)
    r2 = bpe_encode_hash(txt)
    r3 = tok.encode(txt)
    ok1 = r1 == r3
    ok2 = r2 == r3
    if not (ok1 and ok2):
        allok = False
    print(f"{txt[:52]!r:56s} regex={'OK' if ok1 else 'FAIL'} hash={'OK' if ok2 else 'FAIL'}")
    if not ok1:
        print("   regex:", r1); print("   tf   :", r3)
    if not ok2:
        print("   hash :", r2); print("   tf   :", r3)

# 特殊 token 拦截验证：tiktoken 先按特殊串切分文本，再对每段独立预分词+BPE
SPECIALS = [("<|endoftext|>", 151643), ("<|im_start|>", 151644), ("<|im_end|>", 151645)]

def bpe_piece(w):
    codes = [B2C[b] for b in w.encode("utf-8")]
    pieces = [[c] for c in codes]
    while len(pieces) > 1:
        best = None
        for j in range(len(pieces) - 1):
            a = "".join(chr(c) for c in pieces[j])
            b = "".join(chr(c) for c in pieces[j + 1])
            r = rank_map.get((a, b), float("inf"))
            if best is None or r < best[0]:
                best = (r, j)
        if best[0] == float("inf"):
            break
        j = best[1]
        pieces[j] = pieces[j] + pieces[j + 1]
        del pieces[j + 1]
    out = []
    for p in pieces:
        out.append(vocab["".join(chr(c) for c in p)])
    return out

def encode_with_special(text):
    out = []
    seg_start = 0
    i = 0
    n = len(text)
    while i < n:
        matched = False
        for s, sid in SPECIALS:
            if text.startswith(s, i):
                # 编码 [seg_start, i) 段
                for w in PAT.findall(text[seg_start:i]):
                    out.extend(bpe_piece(w))
                out.append(sid)
                i += len(s)
                seg_start = i
                matched = True
                break
        if matched:
            continue
        i += 1
    for w in PAT.findall(text[seg_start:]):
        out.extend(bpe_piece(w))
    return out

# ---------------- 写参考编码（MYP 对拍用） ----------------
with open(os.path.join(D, "qwen2_ref_encode.bin"), "wb") as f:
    f.write(np.array([len(tests)], np.int32).tobytes())
    for txt in tests:
        ids = tok.encode(txt)
        f.write(np.array([len(ids)] + ids, np.int32).tobytes())
with open(os.path.join(D, "qwen2_ref_texts.bin"), "wb") as f:
    f.write(np.array([len(tests)], np.int32).tobytes())
    for txt in tests:
        tb = txt.encode("utf-8")
        f.write(np.array([len(tb)], np.int32).tobytes())
        f.write(tb)

# 特殊 token 切分验证（chat 模板文本）
chat_txt = "<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n<|im_start|>user\nhi<|im_end|>\n<|im_start|>assistant\n"
my_spec = encode_with_special(chat_txt)
tf_spec = tok.encode(chat_txt, add_special_tokens=False)
print("\nspecial-token chat template:", "OK" if my_spec == tf_spec else "FAIL")
if my_spec != tf_spec:
    print("  my :", my_spec)
    print("  tf :", tf_spec)

# 写 chat 模板参考（MYP 特殊 token 对拍）：[文本字节长][文本][ids 数][ids...]
with open(os.path.join(D, "qwen2_ref_chat.bin"), "wb") as f:
    tb = chat_txt.encode("utf-8")
    f.write(np.array([len(tb)], np.int32).tobytes())
    f.write(tb)
    f.write(np.array([len(tf_spec)] + tf_spec, np.int32).tobytes())

print("\nRESULT:", "ALL OK" if (allok and my_spec == tf_spec) else "MISMATCH")
