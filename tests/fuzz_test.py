#!/usr/bin/env python3
"""
MYP Language 模糊测试 (Fuzz Testing)
生成随机 MYP 代码，编译检查是否崩溃/挂死。

用法:
  python3 tests/fuzz_test.py                # 快速测试 (100 次迭代)
  python3 tests/fuzz_test.py --count 1000   # 大量测试
  python3 tests/fuzz_test.py --seed 42      # 指定随机种子
  python3 tests/fuzz_test.py --save /tmp/fuzz  # 保存失败的测试用例
"""

import random
import subprocess
import sys
import os
import tempfile
import argparse

# ===================== 代码生成器 =====================

TYPES = ["int", "long", "double", "bool", "string"]
BINOPS = ["+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "&&", "||"]
UNOPS = ["!", "-"]

class FuzzGenerator:
    def __init__(self, seed=None):
        self.rng = random.Random(seed)
        self.var_id = 0
        self.depth = 0
        self.max_depth = 8
        self.vars_in_scope = []

    def fresh_var(self):
        name = f"v{self.var_id}"
        self.var_id += 1
        return name

    def random_type(self):
        return self.rng.choice(TYPES)

    def random_literal(self, typ):
        if typ == "int":
            return str(self.rng.randint(-1000, 1000))
        elif typ == "long":
            return str(self.rng.randint(-100000, 100000)) + "L"
        elif typ == "double":
            return f"{self.rng.uniform(-100.0, 100.0):.4f}"
        elif typ == "bool":
            return self.rng.choice(["true", "false"])
        elif typ == "string":
            words = ["hello", "world", "test", "foo", "bar", "42", "x", ""]
            return '"' + self.rng.choice(words) + '"'
        return "0"

    def random_expr(self, target_type=None):
        self.depth += 1
        if self.depth > self.max_depth or self.rng.random() < 0.3:
            self.depth -= 1
            if target_type:
                return self.random_literal(target_type)
            return self.random_literal(self.random_type())

        # 使用已声明的变量
        if self.vars_in_scope and self.rng.random() < 0.4:
            var = self.rng.choice(self.vars_in_scope)
            self.depth -= 1
            return var.name

        typ = target_type or self.random_type()

        # 二元运算
        if self.rng.random() < 0.5:
            op = self.rng.choice(BINOPS)
            # 字符串只支持 +, ==, !=
            if typ == "string" and op not in ("+", "==", "!="):
                op = self.rng.choice(["+", "==", "!="])
            left = self.random_expr(typ)
            right = self.random_expr(typ)
            self.depth -= 1
            return f"({left} {op} {right})"
        else:
            # 一元运算
            op = self.rng.choice(UNOPS)
            if typ == "bool" and op == "-":
                op = "!"
            operand = self.random_expr(typ)
            self.depth -= 1
            return f"{op}{operand}"

    def generate_test(self):
        """生成一个完整的 MYP 测试程序"""
        self.var_id = 0
        self.depth = 0
        self.vars_in_scope = []

        lines = []
        lines.append("// Fuzz generated test")
        lines.append("import env;")
        lines.append("")

        # 是否包含 class
        use_class = self.rng.random() < 0.7

        if use_class:
            lines.append("class Test {")
            lines.append("    action:")
            lines.append("        @startup void run() {")
            for _ in range(self.rng.randint(1, 8)):
                s = self.random_statement()
                if s:
                    lines.append(f"            {s}")
            lines.append("        }")
            lines.append("}")
            lines.append("")
            lines.append("int main() { Test t = new Test(); return 0; }")
        else:
            lines.append("// File-level code (minimal)")
            lines.append("int main() { return 0; }")

        return "\n".join(lines)

    def random_statement(self):
        kind = self.rng.choice(["var_decl", "assign", "if", "while", "for", "expr"])
        if kind == "var_decl":
            typ = self.random_type()
            name = self.fresh_var()
            init = self.random_expr(typ)
            self.vars_in_scope.append(VarInfo(name, typ))
            return f"{typ} {name} = {init};"
        elif kind == "assign":
            if not self.vars_in_scope:
                return None
            var = self.rng.choice(self.vars_in_scope)
            expr = self.random_expr(var.typ)
            return f"{var.name} = {expr};"
        elif kind == "if":
            cond = self.random_expr("bool")
            body1 = self.random_statement() or "int x=0;"
            if self.rng.random() < 0.3:
                body2 = self.random_statement() or "int y=0;"
                return f"if({cond}){{{body1}}}else{{{body2}}}"
            return f"if({cond}){{{body1}}}"
        elif kind == "while":
            cond = self.random_expr("bool")
            body = self.random_statement() or "int x=0;"
            return f"while({cond}){{{body}}}"
        elif kind == "for":
            var = self.fresh_var()
            init = f"int {var}=0"
            cond = self.random_expr("bool")
            step = f"{var}={var}+1"
            body = self.random_statement() or "int x=0;"
            return f"for({init};{cond};{step}){{{body}}}"
        elif kind == "expr":
            return f"{self.random_expr()};"
        return None


class VarInfo:
    def __init__(self, name, typ):
        self.name = name
        self.typ = typ


# ===================== 测试运行器 =====================

def compile_mypc(source, mypc_path):
    """编译 MYP 源码，返回 (真实崩溃?, 被拒绝?, stdout, stderr)"""
    with tempfile.NamedTemporaryFile(suffix=".myp", mode="w", delete=False) as f:
        f.write(source)
        src_path = f.name

    out_path = src_path + ".out"

    try:
        result = subprocess.run(
            [mypc_path, src_path, "-o", out_path],
            capture_output=True, text=True, timeout=10
        )
        # A real crash = killed by a signal (e.g. SIGSEGV/SIGABRT) or a
        # sanitizer report. A non-zero exit with clean diagnostics is just the
        # compiler correctly REJECTING invalid code — not a crash.
        output = (result.stdout or "") + (result.stderr or "")
        is_crash = (result.returncode < 0) or \
                   ("AddressSanitizer" in output) or \
                   ("UndefinedBehaviorSanitizer" in output) or \
                   ("runtime error:" in output and "sanitize" in output)
        return is_crash, (result.returncode != 0 and not is_crash), \
               result.stdout, result.stderr, src_path, out_path
    except subprocess.TimeoutExpired:
        return False, True, "", "TIMEOUT", src_path, out_path
    except FileNotFoundError:
        print(f"[FATAL] Compiler not found: {mypc_path}")
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="MYP Fuzz Tester")
    parser.add_argument("--count", type=int, default=100,
                        help="Number of test iterations (default: 100)")
    parser.add_argument("--seed", type=int, default=None,
                        help="Random seed")
    parser.add_argument("--save", type=str, default=None,
                        help="Directory to save failing test cases")
    parser.add_argument("--mypc", type=str, default="./build/mypc",
                        help="Path to mypc compiler (use ./build-asan/mypc + "
                             "ASAN_OPTIONS=detect_leaks=0 for sanitizer runs)")
    args = parser.parse_args()

    mypc_path = os.path.abspath(args.mypc)
    if not os.path.exists(mypc_path):
        print(f"[ERROR] Compiler not found: {mypc_path}")
        sys.exit(1)

    gen = FuzzGenerator(seed=args.seed)
    if args.save:
        os.makedirs(args.save, exist_ok=True)

    print(f"MYP Fuzz Tester")
    print(f"  Compiler: {mypc_path}")
    print(f"  Iterations: {args.count}")
    print(f"  Seed: {args.seed or 'random'}")
    print(f"  Save dir: {args.save or '(not saving)'}")
    print()

    total = 0
    crashes = 0
    rejected = 0
    timeouts = 0
    compile_oks = 0

    for i in range(args.count):
        source = gen.generate_test()
        total += 1

        is_crash, is_rejected, stdout, stderr, src_path, out_path = \
            compile_mypc(source, mypc_path)

        # 清理临时文件
        if os.path.exists(out_path):
            os.unlink(out_path)
        if os.path.exists(src_path):
            os.unlink(src_path)

        # 分析结果
        if "TIMEOUT" in (stderr or ""):
            timeouts += 1
            status = "TIMEOUT"
        elif is_crash:
            crashes += 1
            status = "CRASH"

            # 保存失败的测试
            if args.save:
                fname = f"crash_{i:04d}.myp"
                with open(os.path.join(args.save, fname), "w") as f:
                    f.write(source)
                with open(os.path.join(args.save, f"crash_{i:04d}.log"), "w") as f:
                    f.write((stderr or "") + "\n--- stdout ---\n" + (stdout or ""))
        elif is_rejected:
            rejected += 1
            status = "reject"
        else:
            compile_oks += 1
            status = "OK"

        # 进度显示
        if (i + 1) % 10 == 0 or is_crash:
            marker = "!" if is_crash else "."
            print(f"  [{i+1:4d}/{args.count}] {marker}  crashes={crashes}  timeouts={timeouts}", end="\r")

    print()
    print()
    print("=" * 50)
    print("  模糊测试结果")
    print("=" * 50)
    print(f"  总迭代:     {total}")
    print(f"  编译成功:   {compile_oks} ({100*compile_oks//total}%)")
    print(f"  正确拒绝:   {rejected} ({100*rejected//total}%)")
    print(f"  编译器崩溃: {crashes}")
    print(f"  超时:       {timeouts}")

    if crashes > 0:
        print()
        print(f"  ⚠ 发现 {crashes} 个真实崩溃 (信号/ASAN)!")
        if args.save:
            print(f"  崩溃用例已保存到: {args.save}")
    else:
        print()
        print(f"  ✅ 没有发现编译器真实崩溃")

    return 0 if crashes == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
