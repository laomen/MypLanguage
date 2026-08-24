#!/usr/bin/env python3
"""gen_image.py — 交互式文生图（MYP SD1.5 GPU 管线）。

单次：  onnxvenv/bin/python deeplearning/diffusion/gen_image.py "a red fox in snow"
交互：  onnxvenv/bin/python deeplearning/diffusion/gen_image.py
        （逐行输入 prompt，空行退出）
选项：  --steps N   去噪步数（默认 30；50 画质更好，20 更快）
        --skip-ref  不重生成 transformers 参考 embedding（更快，仅省 ~5s）

流程：prompt → make_clip_bpe_tables（分词参考）→ tsteps →（transformers ref_emb）
     → MYP clip_tokenize → clip_encoder → GPU DDIM → GPU VAE → 转 PNG → 输出路径。
"""
import os
import subprocess
import sys
import argparse

HERE = os.path.dirname(os.path.abspath(__file__))               # .../diffusion
EXAMPLES = os.path.dirname(os.path.dirname(HERE))               # examples
DL = os.path.join(EXAMPLES, "deeplearning", "data", "diffusion")
CLIP = os.path.join(DL, "clip")
D5 = os.path.join(DL, "d5")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))  # MYPLanguage
MYPC = os.path.join(REPO, "build", "mypc")
STDLIB = os.path.join(REPO, "stdlib")
BIN = os.path.join(HERE, "bin")
PY = sys.executable

TOOLS = [
    ("clip_tokenize.myp", "clip_tokenize"),
    ("clip_encoder.myp", "clip_encoder"),
    ("ddim_sampler.myp", "ddim_sampler"),
    ("vae_decode.myp", "vae_decode"),
]


def build():
    os.makedirs(BIN, exist_ok=True)
    for src, name in TOOLS:
        m = os.path.join(HERE, src)
        out = os.path.join(BIN, name)
        if os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(m):
            continue
        r = subprocess.run([MYPC, "-O3", m, "-o", out, "--stdlib", STDLIB],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout + r.stderr, file=sys.stderr)
            sys.exit(f"[build] {name} failed")
        print(f"[build] {name}")


def make_bpe(prompt):
    subprocess.run([PY, os.path.join(HERE, "make_clip_bpe_tables.py"), prompt],
                   check=True, capture_output=True)
    with open(os.path.join(CLIP, "d2b_prompt.txt"), "w") as f:
        f.write(prompt)


def gen_tsteps(n):
    code = (
        "import numpy as np, os\n"
        f"from diffusers import DDIMScheduler\n"
        f"sched = DDIMScheduler.from_pretrained({os.path.join(DL,'sd15','scheduler')!r})\n"
        f"sched.set_timesteps({n})\n"
        "t = sched.timesteps.numpy().astype(np.int32)\n"
        f"with open({os.path.join(D5,'d5_tsteps.bin')!r},'wb') as f:\n"
        "    f.write(np.int32(len(t)).tobytes()); f.write(t.tobytes())\n"
    )
    subprocess.run([PY, "-c", code], check=True)


def regen_ref(prompt):
    code = (
        "import os, numpy as np, torch\n"
        "from transformers import CLIPTextModel, CLIPTokenizer\n"
        f"MD = {os.path.join(DL,'clip-vit-large-patch14')!r}\n"
        "tok = CLIPTokenizer.from_pretrained(MD)\n"
        "m = CLIPTextModel.from_pretrained(MD, torch_dtype=torch.float32)\n"
        "m.eval()\n"
        f"ids = tok({prompt!r}, padding='max_length', max_length=77, truncation=True, return_tensors='pt')\n"
        "with torch.no_grad():\n"
        "    emb = m(input_ids=ids['input_ids'], attention_mask=ids['attention_mask']).last_hidden_state\n"
        f"emb[0].numpy().astype(np.float32).tofile({os.path.join(CLIP,'ref_emb.bin')!r})\n"
    )
    subprocess.run([PY, "-c", code], check=True, capture_output=True)


def run(bin_name, env_extra=None):
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    subprocess.run([os.path.join(BIN, bin_name)], cwd=EXAMPLES, env=env, check=True)


def gen_one(prompt, steps, do_ref):
    print(f"\n=== prompt: {prompt} ===")
    make_bpe(prompt)
    gen_tsteps(steps)
    if do_ref:
        regen_ref(prompt)
    run("clip_tokenize")
    run("clip_encoder", {"MYP_CLIP_OUT": "1"})
    run("ddim_sampler", {"MYP_GPU": "1", "MYP_D5_MODE": "ddim"})
    run("vae_decode", {"MYP_GPU": "1", "MYP_VAE_PPM": "1"})
    slug = "".join(c if c.isalnum() else "_" for c in prompt)[:40].strip("_") or "img"
    png = os.path.join(D5, f"gen_{slug}.png")
    subprocess.run([PY, "-c",
                    f"from PIL import Image; Image.open({os.path.join(D5,'myp_image.ppm')!r}).convert('RGB').save({png!r})"],
                   check=True)
    return png


def main():
    ap = argparse.ArgumentParser(description="MYP SD1.5 交互式文生图（GPU）")
    ap.add_argument("prompt", nargs="*", help="prompt（不填则进入交互循环）")
    ap.add_argument("--steps", type=int, default=30, help="去噪步数（默认 30）")
    ap.add_argument("--skip-ref", action="store_true", help="跳过 transformers 参考 embedding")
    args = ap.parse_args()
    build()
    if args.prompt:
        png = gen_one(" ".join(args.prompt), args.steps, not args.skip_ref)
        print(f"✅ 图片生成: {png}")
        return
    print("交互模式：输入 prompt 生成图片，空行退出。")
    while True:
        try:
            p = input("prompt> ").strip()
        except EOFError:
            break
        if not p:
            break
        try:
            png = gen_one(p, args.steps, not args.skip_ref)
            print(f"✅ 图片生成: {png}")
        except Exception as e:
            print(f"✗ 失败: {e}")


if __name__ == "__main__":
    main()
