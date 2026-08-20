#!/usr/bin/env python3
"""run_distilgpt2_chat.py — 一键在 CPU 上跑 distilgpt2 推理（任意 prompt 文本 → 生成文本）。

全链路（BPE 编码也在 MYP 内）：
  1) prep_distilgpt2_gen.py：写 prompt.txt（UTF-8 文本）+ prompt_ids.bin（GPT2Tokenizer
     编码参考）+ gen_cfg.bin + （可选）transformers 贪心参考 ref_gen_ids.bin。
  2) 编译 distilgpt2_chat.myp → /tmp/dgpt_chat（源比二进制新或不存在时才编译）。
  3) 在 examples/ 下运行：MYP 读 prompt.txt → BPE 编码（对拍 prompt_ids.bin）→
     KV-cache 推理 → 解码输出文本（有参考则对拍生成）。

用法：
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/run_distilgpt2_chat.py "prompt" [n_tokens] [--no-ref]
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/run_distilgpt2_chat.py --talk [n_tokens]  # 交互式多轮对话
  例：
    run_distilgpt2_chat.py "The future of AI" 64
    run_distilgpt2_chat.py "Hello, how are you today?" 48 --no-ref
    run_distilgpt2_chat.py --talk
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LLM = os.path.join(ROOT, "deeplearning", "llm")
EXAMPLES = ROOT   # 本脚本在 examples/deeplearning/llm/ 下；MYP 数据路径相对 examples/
PY = os.path.join(ROOT, "deeplearning", "infer", "tools", "onnxvenv", "bin", "python")
MYPC = os.path.join(ROOT, "..", "build", "mypc")
SRC = os.path.join(LLM, "distilgpt2_chat.myp")
SRC_TALK = os.path.join(LLM, "distilgpt2_talk.myp")
BIN = "/tmp/dgpt_chat"
BIN_TALK = "/tmp/dgpt_talk"
STDLIB = os.path.join(ROOT, "..", "stdlib")

# ---- 参数 ----
args = sys.argv[1:]
prompt = "Once upon a time"
n_tokens = 32
talk = False
extra = []
for a in args:
    if a == "--no-ref":
        extra.append(a)
    elif a == "--talk":
        talk = True
    elif a.isdigit():
        n_tokens = int(a)
    else:
        prompt = a

if talk:
    # 交互式多轮对话模式
    print(f"=== distilgpt2 talk (CPU, all-in-MYP) — type 'exit' to quit ===")
    if n_tokens != 32:
        with open(os.path.join(ROOT, "deeplearning", "data", "llm", "distilgpt2_gen_cfg.bin"), "wb") as f:
            import numpy as np
            f.write(np.array([n_tokens], np.int32).tobytes())
    need = (not os.path.exists(BIN_TALK)) or (os.path.getmtime(SRC_TALK) > os.path.getmtime(BIN_TALK))
    if need:
        r = subprocess.run([MYPC, SRC_TALK, "-o", BIN_TALK, "--stdlib", STDLIB], cwd=os.path.join(ROOT, ".."))
        if r.returncode != 0:
            sys.exit(f"compile talk failed ({r.returncode})")
    r = subprocess.run([BIN_TALK], cwd=EXAMPLES)
    sys.exit(r.returncode)

print(f"=== distilgpt2 CPU inference ===")
print(f"prompt: {prompt!r}  tokens to generate: {n_tokens}")

# ---- 1) 编码 + 准备 ----
print("--- 1/3 prep (encode prompt, build vocab if needed, optional ref) ---")
cmd = [PY, os.path.join(LLM, "prep_distilgpt2_gen.py"), prompt, str(n_tokens)] + extra
r = subprocess.run(cmd, cwd=EXAMPLES)
if r.returncode != 0:
    sys.exit(f"prep failed ({r.returncode})")

# ---- 2) 编译（必要时）----
print("--- 2/3 compile ---")
need = (not os.path.exists(BIN)) or (os.path.getmtime(SRC) > os.path.getmtime(BIN))
if need:
    r = subprocess.run([MYPC, SRC, "-o", BIN, "--stdlib", STDLIB], cwd=os.path.join(ROOT, ".."))
    if r.returncode != 0:
        sys.exit(f"compile failed ({r.returncode})")
else:
    print("binary up to date, skip compile")

# ---- 3) 跑推理 ----
print("--- 3/3 run (CPU KV-cache decode) ---")
r = subprocess.run([BIN], cwd=EXAMPLES, capture_output=True, text=True)
print(r.stdout, end="")
if r.stderr:
    print(r.stderr, file=sys.stderr, end="")
sys.exit(r.returncode)
