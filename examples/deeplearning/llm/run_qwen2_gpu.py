#!/usr/bin/env python3
"""run_qwen2_gpu.py — 一键在 GPU 上跑 Qwen2-0.5B-Instruct 推理（CUDA + MYP @gpu 内核）。

全链路（分词也在 MYP 内）：
  1) 编译 qwen2_gpu.myp → /tmp/q2gpu（源比二进制新时才编译；权重加载时转置为
     [xRows,outDim]，lm_head 另存转置 wte，GEMV 合并访存）。
  2) 整块 arena（~2.6GB）一次 H2D，随后每 step 用 GpuStream 异步流排队 24 层 +
     lm_head 全部内核，步末一次 sync 后 host 采样。
  3) 与 transformers 贪心参考对拍（mismatch=0 → QWEN2 GPU OK）+ 解码输出。

用法：
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/run_qwen2_gpu.py "prompt"
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/run_qwen2_gpu.py --talk [n]  # GPU 交互式多轮对话
  例：
    run_qwen2_gpu.py "What is the capital of France?"
    run_qwen2_gpu.py --talk
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LLM = os.path.join(ROOT, "deeplearning", "llm")
EXAMPLES = ROOT   # MYP 数据路径相对 examples/
MYPC = os.path.join(ROOT, "..", "build", "mypc")
SRC_CHAT = os.path.join(LLM, "qwen2_gpu.myp")
SRC_TALK = os.path.join(LLM, "qwen2_talk_gpu.myp")
BIN_CHAT = "/tmp/q2gpu"
BIN_TALK = "/tmp/q2tg"
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
    # GPU 交互式多轮对话模式
    print("=== Qwen2-0.5B-Instruct talk (GPU, all-in-MYP) — type 'exit' to quit ===")
    need = (not os.path.exists(BIN_TALK)) or (os.path.getmtime(SRC_TALK) > os.path.getmtime(BIN_TALK))
    if need:
        r = subprocess.run([MYPC, SRC_TALK, "-o", BIN_TALK, "--stdlib", STDLIB], cwd=os.path.join(ROOT, ".."))
        if r.returncode != 0:
            sys.exit(f"compile talk failed ({r.returncode})")
    env = dict(os.environ)
    env["MYP_GPU"] = "1"
    r = subprocess.run([BIN_TALK], cwd=EXAMPLES, env=env)
    sys.exit(r.returncode)

print("=== Qwen2-0.5B-Instruct GPU inference (CUDA) ===")
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

# ---- 3) 跑推理（权重 1.98GB 首次加载 ~19s + H2D ~0.4s）----
print("--- run (GPU resident + async stream decode) ---")
env = dict(os.environ)
env["MYP_GPU"] = "1"
r = subprocess.run([BIN_CHAT], cwd=EXAMPLES, env=env, capture_output=True, text=True)
print(r.stdout, end="")
if r.stderr:
    print(r.stderr, file=sys.stderr, end="")
sys.exit(r.returncode)
