#!/usr/bin/env python3
"""make_bpe_tables.py — 为 MYP 端 BPE 编码器构建二进制表 + 算法验证（vs transformers）。

BPE 编码（GPT-2，ByteLevel）三步：
  1) 字节编码：每字节 → 码点（bytes_to_unicode 表；空格→U+0120 'Ġ' 等）。
  2) 预分词：正则 's|'t|'re|'ve|'m|'ll|'d| ?\\p{L}+| ?\\p{N}+| ?[^\\s\\p{L}\\p{N}]+|\\s+(?!\\S)|\\s+
     （本脚本用 regex/re 实现；MYP 端用等价的字节扫描器）。
  3) BPE 合并：反复合并 rank 最低的相邻 pair（merges.txt，pair 元素可为多字符），
     查 vocab 得 id。

输出二进制表（data/llm/）：
  bpe_merges_hash.bin / bpe_merges_rank.bin（开放寻址，T=2^17）
  bpe_merges_data.bin / bpe_merges_offs.bin（key 字节池，int16 LE 每码点，pair 间 0xFFFF）
  bpe_vocab_hash.bin / bpe_vocab_id.bin（开放寻址，T=2^17）
  bpe_vocab_data.bin / bpe_vocab_offs.bin（token 码点池，int16 LE）
  参考编码：bpe_ref_encode.bin（若干 prompt 的 token ids，供 MYP 对拍）

用法（onnxvenv 内）：
  onnxvenv/bin/python make_bpe_tables.py
"""
import os
import json
import struct
import numpy as np
from transformers import GPT2Tokenizer

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
D = os.path.join(ROOT, "deeplearning", "data", "llm")
MODEL_DIR = os.path.join(D, "distilgpt2")

T = 1 << 17   # 开放寻址表大小

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

# ---------------- 读 tokenizer ----------------
tok = GPT2Tokenizer.from_pretrained(MODEL_DIR)
tt = json.load(open(os.path.join(MODEL_DIR, "tokenizer.json")))
vocab = tt["model"]["vocab"]      # token字符串 → id
merges = tt["model"]["merges"]    # ["a b", ...] 50000

# 反转 vocab 得到 id → 码点序列（每个 token 字符串 → 码点列表）
def char_codes(s):
    return [ord(c) for c in s]

id_codes = [None] * len(vocab)
for s, i in vocab.items():
    id_codes[i] = char_codes(s)

# ---------------- 哈希表工具 ----------------
def fnv1a(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h

def build_hashtable(keys, vals, T):
    """keys: list[bytes], vals: list[int]（与 key 一一对应）"""
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
    """码点序列 → key bytes（int16 LE 每码点；sep 为插入的分隔码点，None 不加）"""
    out = bytearray()
    for c in codes:
        out += struct.pack("<H", c)
        if sep is not None:
            out += struct.pack("<H", sep)
    return bytes(out)

# ---------------- 构建 merges 表 ----------------
# merges[rank] = ("a","b")，pair key = codes(a) + [0xFFFF] + codes(b)
merge_pairs = [m.split() for m in merges]
pair_keys = []
for a, b in merge_pairs:
    ca = [ord(c) for c in a]
    cb = [ord(c) for c in b]
    # key = pack(ca) + pack(0xFFFF 分隔) + pack(cb)
    pair_keys.append(key_of_codes(ca, None) + key_of_codes([0xFFFF], None) + key_of_codes(cb, None))
mhash, mrank = build_hashtable(pair_keys, list(range(len(pair_keys))), T)
# key 池（按 rank 顺序存，供 MYP 端 key 比较）
offs = [0]
data = bytearray()
for k in pair_keys:
    data += k
    offs.append(len(data))
with open(os.path.join(D, "bpe_merges_hash.bin"), "wb") as f: f.write(np.array(mhash, np.int32).tobytes())
with open(os.path.join(D, "bpe_merges_rank.bin"), "wb") as f: f.write(np.array(mrank, np.int32).tobytes())
with open(os.path.join(D, "bpe_merges_data.bin"), "wb") as f: f.write(bytes(data))
with open(os.path.join(D, "bpe_merges_offs.bin"), "wb") as f: f.write(np.array(offs, np.int32).tobytes())
print("merges table built:", len(pair_keys), "pairs, data", len(data), "B")

# ---------------- 构建 vocab 表 ----------------
vkeys = [key_of_codes(id_codes[i], None) for i in range(len(id_codes))]
vhash, vid = build_hashtable(vkeys, list(range(len(vkeys))), T)
voffs = [0]
vdata = bytearray()
for k in vkeys:
    vdata += k
    voffs.append(len(vdata))
with open(os.path.join(D, "bpe_vocab_hash.bin"), "wb") as f: f.write(np.array(vhash, np.int32).tobytes())
with open(os.path.join(D, "bpe_vocab_id.bin"), "wb") as f: f.write(np.array(vid, np.int32).tobytes())
with open(os.path.join(D, "bpe_vocab_data.bin"), "wb") as f: f.write(bytes(vdata))
with open(os.path.join(D, "bpe_vocab_offs.bin"), "wb") as f: f.write(np.array(voffs, np.int32).tobytes())
print("vocab table built:", len(vkeys), "tokens, data", len(vdata), "B")

# ---------------- 算法验证：用刚建的表实现 BPE 编码 vs transformers ----------------
# 用 dict 便于验证（哈希表逻辑单独验）
rank_map = {}
for rank, (a, b) in enumerate(merge_pairs):
    rank_map[(a, b)] = rank

def bpe_encode(text):
    # 预分词（regex 版，ASCII 足够验证）
    import regex as re
    pat = re.compile(r"""'s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+""")
    words = pat.findall(text)
    out = []
    for w in words:
        codes = [B2C[b] for b in w.encode("utf-8")]
        # BPE merge（GPT-2 算法）
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

# 用哈希表查 rank/vocab 的路径（模拟 MYP 端）：
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

# 加载刚写的表，验证哈希查找与 dict 一致
mdata = bytes(data); moffs = np.array(offs, np.int32)
vdata_b = bytes(vdata); voffs = np.array(voffs, np.int32)
def bpe_encode_hash(text):
    import regex as re
    pat = re.compile(r"""'s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+""")
    out = []
    for w in pat.findall(text):
        codes = [B2C[b] for b in w.encode("utf-8")]
        pieces = [[c] for c in codes]
        while len(pieces) > 1:
            best_r = float("inf"); best_j = -1
            for j in range(len(pieces) - 1):
                ka = key_of_codes(pieces[j], None)
                kb = key_of_codes(pieces[j + 1], None)
                key = ka + key_of_codes([0xFFFF], None) + kb
                r = hash_lookup(mhash, mrank, key, moffs, mdata)
                if r != -1 and r < best_r:
                    best_r = r; best_j = j
            if best_j < 0:
                break
            pieces[best_j] = pieces[best_j] + pieces[best_j + 1]
            del pieces[best_j + 1]
        for p in pieces:
            key = key_of_codes(p, None)
            tid = hash_lookup(vhash, vid, key, voffs, vdata_b)
            out.append(tid)
    return out

tests = [
    "Once upon a time",
    "The capital of France",
    "Hello, how are you today?",
    "In a world where machines think",
    "The quick brown fox jumps over the lazy dog.",
    "I love to eat pizza with extra cheese!",
    "A 'very' important lesson: don't panic.",
]
print("\n=== algorithm verification (dict vs hash-table vs transformers) ===")
allok = True
for txt in tests:
    r1 = bpe_encode(txt)          # dict 版
    r2 = bpe_encode_hash(txt)     # 哈希表版（模拟 MYP）
    r3 = tok.encode(txt)
    ok1 = r1 == r3
    ok2 = r2 == r3
    if not (ok1 and ok2):
        allok = False
    print(f"{txt!r:55s} dict={'OK' if ok1 else 'FAIL'} hash={'OK' if ok2 else 'FAIL'}")
    if not ok1:
        print("   dict:", r1)
        print("   tf  :", r3)
    if not ok2:
        print("   hash:", r2)
        print("   tf  :", r3)

# 写参考编码（供 MYP 对拍）：格式 [n_prompts, per prompt: count + ids]
with open(os.path.join(D, "bpe_ref_encode.bin"), "wb") as f:
    f.write(np.array([len(tests)], np.int32).tobytes())
    for txt in tests:
        ids = tok.encode(txt)
        f.write(np.array([len(ids)] + ids, np.int32).tobytes())
# 写测试 prompt 文本（供 MYP 端编码）：per prompt: int32 字节长 + utf-8 字节
with open(os.path.join(D, "bpe_ref_texts.bin"), "wb") as f:
    f.write(np.array([len(tests)], np.int32).tobytes())
    for txt in tests:
        tb = txt.encode("utf-8")
        f.write(np.array([len(tb)], np.int32).tobytes())
        f.write(tb)
print("\nreference encodes written:", len(tests), "prompts")
print("ALL", "OK" if allok else "FAIL")
