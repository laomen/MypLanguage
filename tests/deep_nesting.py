#!/usr/bin/env python3
"""
MYP 深层嵌套/极端输入测试 (Deep nesting & adversarial input)

Grammar fuzz 生成不了深度嵌套；需构造深层括号/花括号/链式表达式/数组下标，
验证 ParserDepthGuard(300) / SemaDepthGuard(300) 不崩溃、不挂死，而是干净报错。
"""
import subprocess
import sys
import os
import tempfile

MYPCC = sys.argv[1] if len(sys.argv) > 1 else "./build-asan/mypc"
ENV = dict(os.environ)
ENV["ASAN_OPTIONS"] = "detect_leaks=0"

def run_case(name, content, expect_ok=False):
    fd, path = tempfile.mkstemp(suffix=".myp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(content.encode())
        p = subprocess.run([MYPCC, path], capture_output=True, timeout=30,
                           env=ENV)
        rc = p.returncode
        out = (p.stdout + p.stderr).decode("utf-8", "replace")
        crashed = rc < 0 or rc >= 128 or "AddressSanitizer" in out or \
            "Segmentation" in out or "stack-overflow" in out
        if crashed:
            print(f"[CRASH] {name} rc={rc}")
            print(out[:1500])
            return False
        if expect_ok and rc != 0:
            # 期望通过却失败（非崩溃）—— 算可疑但不致命
            print(f"[WARN]  {name} failed rc={rc} (expected ok)")
            print(out[:800])
        return True
    except subprocess.TimeoutExpired:
        print(f"[HANG]  {name}")
        return False
    finally:
        os.unlink(path)

def main():
    results = []
    # 深层括号: 应干净报错（深度守卫），不崩溃
    for n in [50, 200, 299, 300, 301, 1000, 10000, 100000]:
        content = "int x = " + "("*n + "1" + ")"*n + ";"
        results.append(run_case(f"parens_{n}", content))
    # 深层花括号 (块嵌套)
    for n in [100, 299, 1000, 10000]:
        content = "int main() {\n" + "{\n"*n + "int y = 1;\n" + "}\n"*n + "return 0;\n}"
        results.append(run_case(f"braces_{n}", content))
    # 深层链式 .member
    for n in [1000, 10000]:
        content = "int x = a" + ".b"*n + ";"
        results.append(run_case(f"chain_{n}", content))
    # 深层二元运算
    for n in [500, 5000, 50000]:
        content = "int x = " + "+".join(["1"]*(n+1)) + ";"
        results.append(run_case(f"binop_{n}", content))
    # 深层数组下标
    for n in [500, 5000]:
        content = "int x = a" + "[0]"*n + ";"
        results.append(run_case(f"subscript_{n}", content))
    # 深层 unary
    for n in [500, 5000]:
        content = "int x = " + "-"*n + "1;"
        results.append(run_case(f"unary_{n}", content))
    # 深层 lambda 嵌套 (括号内 lambda)
    for n in [100, 500, 2000]:
        content = "int x = " + "("*n + "() => 1" + ")"*n + ";"
        results.append(run_case(f"lambda_{n}", content))
    # 深层泛型参数
    for n in [100, 500, 2000]:
        content = "int x = Foo" + "<Foo>"*n + " v;"
        results.append(run_case(f"generic_{n}", content))
    # 深层三元 (如果支持)
    content = "int x = " + " ? 1 : ".join(["1"]*200) + ";"
    results.append(run_case("ternary_200", content))

    ok = sum(results)
    bad = len(results) - ok
    print(f"\n=== 深层嵌套测试: {ok} 通过, {bad} 崩溃/挂死 ===")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
