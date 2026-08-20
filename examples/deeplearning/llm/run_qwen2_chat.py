#!/usr/bin/env python3
"""run_qwen2_chat.py — 一键在 CPU 上跑 Qwen2-0.5B-Instruct 推理（文本 → 对话）。

全链路（分词也在 MYP 内）：
  1) （可选）生成参考：extract_qwen2.py（权重 1.98GB + chat 模板 prompt + transformers 贪心参考）。
  2) 编译 qwen2_chat.myp → /tmp/q2chat（源比二进制新时才编译）。
  3) 在 examples/ 下运行：MYP 读 prompt.txt → 组装 chat 模板 → MYP 分词 → KV-cache
     贪心生成 → 解码输出文本（有参考则对拍 TOKENIZE/GENERATE）。

用法：
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/run_qwen2_chat.py "prompt"
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/run_qwen2_chat.py --talk [n]  # 交互式多轮对话
  例：
    run_qwen2_chat.py "What is the capital of France?"
    run_qwen2_chat.py --talk
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LLM = os.path.join(ROOT, "deeplearning", "llm")
EXAMPLES = ROOT   # 本脚本在 examples/deeplearning/llm/ 下；MYP 数据路径相对 examples/
MYPC = os.path.join(ROOT, "..", "build", "mypc")
SRC_CHAT = os.path.join(LLM, "qwen2_chat.myp")
SRC_TALK = os.path.join(LLM, "qwen2_talk.myp")
BIN_CHAT = "/tmp/q2chat"
BIN_TALK = "/tmp/q2talk"
STDLIB = os.path.join(ROOT, "..", "stdlib")
DATA = os.path.join(ROOT, "deeplearning", "data", "llm")

# ---- 参数 ----
args = sys.argv[1:]
prompt = "What is the capital of France?"
talk = False
for a in args:
    if a == "--talk":
        talk = True
    else:
        prompt = a

if talk:
    # 交互式多轮对话模式
    print("=== Qwen2-0.5B-Instruct talk (CPU, all-in-MYP) — type 'exit' to quit ===")
    need = (not os.path.exists(BIN_TALK)) or (os.path.getmtime(SRC_TALK) > os.path.getmtime(BIN_TALK))
    if need:
        r = subprocess.run([MYPC, SRC_TALK, "-o", BIN_TALK, "--stdlib", STDLIB], cwd=os.path.join(ROOT, ".."))
        if r.returncode != 0:
            sys.exit(f"compile talk failed ({r.returncode})")
    r = subprocess.run([BIN_TALK], cwd=EXAMPLES)
    sys.exit(r.returncode)

print(f"=== Qwen2-0.5B-Instruct CPU inference ===")
print(f"prompt: {prompt!r}")

# ---- 1) 写 prompt.txt ----
with open(os.path.join(DATA, "qwen2_prompt.txt"), "wb") as f:
    f.write(prompt.encode("utf-8"))

# ---- 2) 编译（必要时）----
print("--- compile ---")
need = (not os.path.exists(BIN_CHAT)) or (os.path.getmtime(SRC_CHAT) > os.path.getmtime(BIN_CHAT))
if need:
    r = subprocess.run([MYPC, SRC_CHAT, "-o", BIN_CHAT, "--stdlib", STDLIB], cwd=os.path.join(ROOT, ".."))
    if r.returncode != 0:
        sys.exit(f"compile failed ({r.returncode})")
else:
    print("binary up to date, skip compile")

# ---- 3) 跑推理（权重 1.98GB 首次加载 ~19s）----
print("--- run (CPU KV-cache decode) ---")
r = subprocess.run([BIN_CHAT], cwd=EXAMPLES, capture_output=True, text=True)
print(r.stdout, end="")
if r.stderr:
    print(r.stderr, file=sys.stderr, end="")
sys.exit(r.returncode)
