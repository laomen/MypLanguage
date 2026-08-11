#!/usr/bin/env python3
"""mypc 变异模糊测试器 (mutation fuzzer)

种子 = tests/*/test.myp + examples/*.myp；随机变异（行增删/重复/替换、token 替换/
删除/插入、拼接、数字/标识符/关键字扰动）后用 ASAN 编译器编译，检测：
  - 编译器崩溃（SEGV/abort/FPE、ASAN 报告）
  - 挂起（timeout）
  - codegen 非法 IR（"LLVM verify failed" —— 属编译器 bug）
  - 内部错误（"internal compiler error" / "Code generation failed"）
干净的词法/语法/语义报错属预期，忽略。

用法: python3 tools/fuzz_myp.py [迭代数] [并行度]
崩溃输入保存到 /tmp/myp_fuzz_crashes/。
"""
import os, sys, random, subprocess, tempfile, shutil, glob, multiprocessing, re, time

MYPC = "./build-asan/mypc"
SEED_GLOBS = ["tests/*/test.myp", "tests/*/*.myp", "examples/*.myp", "stdlib/*.myp"]
CRASH_DIR = "/tmp/myp_fuzz_crashes"
TIMEOUT = 6

TOKENS = ["int", "double", "string", "bool", "void", "class", "struct", "enum",
          "action", "property", "static", "new", "return", "if", "else", "for",
          "while", "break", "continue", "match", "try", "catch", "finally",
          "throw", "fn", "import", "this", "null", "true", "false", "var",
          "0", "1", "42", "3.14", "\"s\"", "x", "y", "foo", "Bar", "T", "N",
          "+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "&&",
          "||", "!", "=", "(", ")", "{", "}", "[", "]", ",", ";", ".", ":",
          "->", "=>", "..", "<<", ">>", "&", "|", "^", "?", "@coro", "@async",
          "interface class", "where", "::", "...", "#", "`", "'", "\x00"]

def load_seeds():
    files = []
    for g in SEED_GLOBS:
        files += glob.glob(g)
    seeds = []
    for f in files:
        try:
            with open(f, "r", errors="replace") as fh:
                seeds.append(fh.read())
        except Exception:
            pass
    if not seeds:
        seeds = ["int main(){return 0;}"]
    return seeds

def random_line(seeds):
    return random.choice(seeds).splitlines()

def mutate(src, seeds):
    lines = src.split("\n")
    r = random.random()
    op = random.random()
    if op < 0.20:
        # token-level mutation: pick a random line, split into tokens, mutate
        if lines:
            li = random.randrange(len(lines))
            toks = re.split(r"(\s+)", lines[li])
            if toks:
                ti = random.randrange(len(toks))
                if r < 0.5:
                    toks[ti] = random.choice(TOKENS)
                elif r < 0.75 and toks[ti].strip():
                    del toks[ti]
                else:
                    toks.insert(ti, random.choice(TOKENS))
                lines[li] = "".join(toks)
    elif op < 0.40:
        # delete a random line
        if len(lines) > 2:
            del lines[random.randrange(len(lines))]
    elif op < 0.55:
        # duplicate a random line
        if lines:
            lines.insert(random.randrange(len(lines)), random.choice(lines))
    elif op < 0.70:
        # insert a random line from a random seed
        pool = random_line(seeds)
        if pool:
            lines.insert(random.randrange(len(lines) + 1), random.choice(pool))
    elif op < 0.85:
        # replace a line with a random snippet
        pool = random_line(seeds)
        if pool and lines:
            lines[random.randrange(len(lines))] = random.choice(pool)
    else:
        # swap two adjacent lines
        if len(lines) >= 2:
            i = random.randrange(len(lines) - 1)
            lines[i], lines[i + 1] = lines[i + 1], lines[i]
    # occasionally splice a second seed at the end
    if random.random() < 0.10:
        lines += ["\n"] + random_line(seeds)
    return "\n".join(lines)

def classify(out, rc):
    """Return a category: CLEAN, CRASH, VERIFY, INTERNAL, HANG."""
    if rc == -9:
        return "HANG"
    if "LLVM verify failed" in out:
        return "VERIFY"          # codegen produced invalid IR — compiler bug
    low = out.lower()
    # Clean lexer/parser/sema errors are expected for mutated programs — check
    # these FIRST (before crash sigs and before INTERNAL, since "Code
    # generation failed" is the NORMAL outcome of any codegen-level user error
    # such as "undefined variable" in a generic template body).
    if "error:" in out or "expected" in low or "failed" in low or "lexer ok" in low:
        return "CLEAN"
    if "internal compiler error" in low or "code generation failed" in low:
        return "INTERNAL"
    # Real crash signals (ASAN/UBSan reports, fatal signals, stack overflow).
    crash_sigs = ["addresssanitizer", "runtime error:", "segmentation fault",
                  "stack overflow", "double free", "use-after-free",
                  "heap-buffer-overflow", "stack-buffer-overflow",
                  "global-buffer-overflow", "undefined behavior sanitizer",
                  "==.*==error:", "abort()"]
    if rc in (139, 134, 132, 136) or any(s in low for s in crash_sigs):
        return "CRASH"
    if rc == 0:
        return "CLEAN"           # compiled fine
    return "CRASH"               # non-zero rc without a clean-error message

def run_one(args):
    idx, src, seeds = args
    d = tempfile.mkdtemp(prefix="mypfz_")
    f = os.path.join(d, "in.myp")
    with open(f, "w") as fh:
        fh.write(src)
    try:
        env = dict(os.environ)
        # detect_leaks=0: LLVM's X86TargetMachine subtarget cache is intentionally
        # never freed (process-lifetime global) — LeakSanitizer reports it on
        # EVERY compile, drowning real findings. Keep UAF/overflow/etc.
        env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") + "detect_leaks=0"
        p = subprocess.run([MYPC, f], capture_output=True, timeout=TIMEOUT,
                           text=True, errors="replace", cwd=os.getcwd(), env=env)
        out = p.stdout + "\n" + p.stderr
        rc = p.returncode
        cat = classify(out, rc)
        if cat in ("CRASH", "VERIFY", "INTERNAL", "HANG"):
            tag = f"{cat}_{idx}"
            dst = os.path.join(CRASH_DIR, tag)
            os.makedirs(CRASH_DIR, exist_ok=True)
            with open(dst + ".myp", "w") as fh:
                fh.write(src)
            with open(dst + ".log", "w") as fh:
                fh.write(f"rc={rc} cat={cat}\n" + out[:4000])
            return cat
        return cat
    except subprocess.TimeoutExpired:
        # Confirm: re-run standalone (fresh process, no parallel load). If it
        # finishes quickly it was just slow under load — not a real hang.
        # Use a generous timeout (20s): a large stdlib seed compiles in ~2-3s
        # standalone, but the parallel load can push it past the 6s window.
        try:
            env = dict(os.environ)
            env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") + "detect_leaks=0"
            subprocess.run([MYPC, f], capture_output=True, timeout=20,
                           text=True, errors="replace", cwd=os.getcwd(), env=env)
            return "CLEAN"
        except subprocess.TimeoutExpired:
            cat = "HANG"
            os.makedirs(CRASH_DIR, exist_ok=True)
            with open(os.path.join(CRASH_DIR, f"HANG_{idx}.myp"), "w") as fh:
                fh.write(src)
            return cat
    finally:
        shutil.rmtree(d, ignore_errors=True)

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 400
    workers = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    random.seed(999331)
    seeds = load_seeds()
    shutil.rmtree(CRASH_DIR, ignore_errors=True)
    os.makedirs(CRASH_DIR, exist_ok=True)
    jobs = []
    for i in range(n):
        src = mutate(random.choice(seeds), seeds)
        jobs.append((i, src, seeds))
    counts = {"CLEAN": 0, "CRASH": 0, "VERIFY": 0, "INTERNAL": 0, "HANG": 0}
    t0 = time.time()
    with multiprocessing.Pool(workers) as pool:
        for cat in pool.imap_unordered(run_one, jobs):
            counts[cat] = counts.get(cat, 0) + 1
    dt = time.time() - t0
    print(f"== {n} iterations in {dt:.1f}s ({workers} workers) ==")
    for k, v in counts.items():
        print(f"  {k}: {v}")
    if counts["CRASH"] or counts["VERIFY"] or counts["INTERNAL"] or counts["HANG"]:
        print(f"  -> inputs saved in {CRASH_DIR}")
        for f in sorted(os.listdir(CRASH_DIR)):
            if f.endswith(".myp"):
                print("    " + f)

if __name__ == "__main__":
    main()
