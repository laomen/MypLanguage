#!/usr/bin/env python3
"""差分模糊测试：随机合法 MYP 程序 oracle vs selfhost 对拍输出。

生成确定性随机程序（算术表达式 + 打印结果），两个编译器各自编译+运行，
比对 stdout。差异 = selfhost codegen bug（runtime 是同一份 C runtime，故
只有 codegen 不同）。同时报告"oracle 过但 selfhost 编译失败"的缺口。

用法:
  python3 tests/diff_fuzz.py                    # 默认 200 迭代
  python3 tests/diff_fuzz.py --count 2000 --seed 42
  python3 tests/diff_fuzz.py --oracle ./build/mypc --self ./build/myp_self --save /tmp/df
"""
import random
import subprocess
import sys
import os
import tempfile
import argparse

# 类型 → 打印函数（同一 C runtime，故两边打印格式一致）
PRINTER = {"int": "Console.write", "long": "Console.writeLong",
           "double": "Console.writeFloat", "string": "Console.writeString"}
TYPES = ["int", "long", "double", "string"]
# 规避除零/移位 UB：只用 + - * % & | ^（% 的右操作数保证非零字面量）
INT_OPS = ["+", "-", "*", "&", "|", "^"]
STR_OPS = ["+"]  # 只用连接（== 返回 bool，不能赋给 string）


class Gen:
    def __init__(self, seed=None):
        self.rng = random.Random(seed)
        self.vid = 0
        self.vars = []  # (name, type)

    def lit(self, typ):
        r = self.rng
        if typ == "int":
            return str(r.randint(-1000, 1000))
        if typ == "long":
            return str(r.randint(-10**9, 10**9)) + "L"
        if typ == "double":
            return f"{r.uniform(-100.0, 100.0):.4f}"
        if typ == "string":
            return '"' + r.choice(["a", "bb", "xyz", "42", "", "Hello", "MYP"]) + '"'
        return "0"

    def expr(self, typ, depth=0):
        r = self.rng
        # 递归到深处 / 有可用变量时，退回字面量或变量（保证确定性且不爆栈）
        if depth >= 3 or (self.vars and r.random() < 0.4):
            cands = [v for v in self.vars if v[1] == typ]
            if cands:
                return r.choice(cands)[0]
            return self.lit(typ)
        if typ == "string":
            op = r.choice(STR_OPS)
            return f"({self.expr(typ, depth+1)} {op} {self.expr(typ, depth+1)})"
        if typ == "double":
            op = r.choice(["+", "-", "*"])
            return f"({self.expr(typ, depth+1)} {op} {self.expr(typ, depth+1)})"
        # int / long
        op = r.choice(INT_OPS)
        left = self.expr(typ, depth + 1)
        right = self.expr(typ, depth + 1)
        if op == "%":
            # 保证右操作数非零字面量（避免除零）
            right = str(r.randint(1, 100)) if typ == "int" else str(r.randint(1, 100)) + "L"
        return f"({left} {op} {right})"

    def program(self):
        self.vid = 0
        self.vars = []
        lines = ["// diff-fuzz generated", "import env;", "",
                 "class F {", "    action:", "        @constructor F() {"]
        n = self.rng.randint(3, 8)
        for _ in range(n):
            typ = self.rng.choice(TYPES)
            name = f"v{self.vid}"
            self.vid += 1
            e = self.expr(typ)
            lines.append(f"            {typ} {name} = {e};")
            self.vars.append((name, typ))
        # 追加一个 if 分支，练习控制流（用 int/long 比较，右操作数同类型字面量）
        if self.vars:
            v = self.rng.choice(self.vars)
            if v[1] == "int":
                lines.append(f"            if ({v[0]} > 0) {{ Console.write(1); }} else {{ Console.write(0); }}")
            elif v[1] == "long":
                lines.append(f"            if ({v[0]} > 0L) {{ Console.write(1); }} else {{ Console.write(0); }}")
        for name, typ in self.vars:
            lines.append(f"            {PRINTER[typ]}({name});")
        lines += ["        }", "}", "", "int main() { F f = new F(); return 0; }"]
        return "\n".join(lines)


def compile_run(cc, src_path):
    """编译并运行，返回 (ok, stdout, stderr)。"""
    out = src_path + ".out"
    r = subprocess.run([cc, src_path, "-o", out], capture_output=True, text=True, timeout=20)
    if r.returncode != 0:
        return False, "", (r.stdout or "") + (r.stderr or "")
    try:
        rr = subprocess.run([out], capture_output=True, text=True, timeout=10)
        return True, rr.stdout, rr.stderr
    except subprocess.TimeoutExpired:
        return False, "", "RUNTIME TIMEOUT"


def main():
    ap = argparse.ArgumentParser(description="MYP 差分模糊测试")
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--oracle", type=str, default="./build/mypc")
    ap.add_argument("--self", type=str, default="./build/myp_self")
    ap.add_argument("--save", type=str, default=None)
    args = ap.parse_args()

    oracle = os.path.abspath(args.oracle)
    selfc = os.path.abspath(args.self)
    for p in (oracle, selfc):
        if not os.path.exists(p):
            print(f"[FATAL] compiler not found: {p}")
            sys.exit(1)
    if args.save:
        os.makedirs(args.save, exist_ok=True)

    gen = Gen(args.seed)
    print(f"差分模糊测试  oracle={oracle}  self={selfc}  count={args.count}")

    mismatches = []   # 输出不一致（codegen bug）
    compile_gaps = [] # oracle 过但 selfhost 编译失败
    both_fail = []    # 两边都编译失败（生成器/共享缺口，跳过）
    both_ok = 0

    for i in range(args.count):
        src = gen.program()
        with tempfile.NamedTemporaryFile(suffix=".myp", mode="w", delete=False) as f:
            f.write(src)
            sp = f.name
        o_ok, o_out, o_err = compile_run(oracle, sp)
        s_ok, s_out, s_err = compile_run(selfc, sp)

        if o_ok and s_ok:
            both_ok += 1
            if o_out != s_out:
                mismatches.append((i, sp, o_out, s_out))
                if args.save:
                    open(os.path.join(args.save, f"mismatch_{i:04d}.myp"), "w").write(src)
                    open(os.path.join(args.save, f"mismatch_{i:04d}.log"), "w").write(
                        f"oracle={o_out!r}\nselfhost={s_out!r}\nself stderr={s_err!r}\n")
        elif o_ok and not s_ok:
            compile_gaps.append((i, sp, s_err))
            if args.save:
                open(os.path.join(args.save, f"gap_{i:04d}.myp"), "w").write(src)
                open(os.path.join(args.save, f"gap_{i:04d}.log"), "w").write(s_err)
        elif not o_ok and not s_ok:
            both_fail.append(i)
        # 两边都编译失败 = 生成器产生非法程序（本应避免），或共享缺口

        for p in (sp, sp + ".out"):
            if os.path.exists(p):
                os.unlink(p)

    print()
    print("=" * 60)
    print(f"  迭代: {args.count}  两边都 OK: {both_ok}  输出不一致: {len(mismatches)}")
    print(f"  selfhost 编译缺口: {len(compile_gaps)}  两边都挂(跳过): {len(both_fail)}")
    print("=" * 60)
    if mismatches:
        print(f"\n  ⚠ {len(mismatches)} 个输出不一致（codegen bug）:")
        for i, sp, oo, so in mismatches[:10]:
            print(f"    case {i}: oracle={oo!r}  selfhost={so!r}")
    if compile_gaps:
        print(f"\n  ⚠ {len(compile_gaps)} 个 selfhost 编译缺口:")
        for i, sp, err in compile_gaps[:10]:
            first = (err or "").strip().splitlines()[:1]
            print(f"    case {i}: {first[0] if first else ''}")
    if args.save and (mismatches or compile_gaps):
        print(f"\n  用例已保存到 {args.save}")
    return 1 if (mismatches or compile_gaps) else 0


if __name__ == "__main__":
    sys.exit(main())
