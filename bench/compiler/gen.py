#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bench/compiler/gen.py — 生成编译器性能基准的 MYP 源码。

用法:  python3 gen.py <P1..P7> <N>

每项生成 N 规模的 MYP 源文件（输出到 stdout）。对应
docs/testing_benchmark_roadmap.md 第五节《需要新增的编译器性能基准》：

  P1  类数量 × 裸属性读取    (N-1 无关类 + 目标类方法内读裸属性 N 次)
  P2  接口数量 × 接口方法调用 (N-1 无关接口 + 目标接口 + N 次接口方法调用)
  P3  接口数量 × 接口变量声明 (N-1 无关接口 + 函数内声明 N 个目标接口变量)
  P4  struct 数量 × 字段读取  (N 个 struct + N 次目标 struct 字段读取)
  P5  enum 数量 × variant 构造(N-1 无关 enum + N 次目标 enum variant 构造)
  P6  类数量 × 方法调用 fallback (N 类 + 多形态方法调用，测方法解析回退)
  P7  泛型实例数量            (N 个泛型类/函数实例，测实例查找与表扩容)

仅需编译成功即可（编译通过即正确性门槛；main 返回 0）。
"""
import sys


def _out(lines):
    return "\n".join(lines) + "\n"


def p1_class_bare_property(n):
    lines = []
    # N-1 个无关类
    for i in range(n - 1):
        lines.append(f"class C{i} {{ property: int v = {i}; }}")
    # 目标类放最后，方法内读裸属性 N 次
    lines.append("class Target {")
    lines.append("    property: int x = 7;")
    lines.append("    action:")
    lines.append("        int run() {")
    lines.append("            int s = 0;")
    for _ in range(n):
        lines.append("            s = s + x;")
    lines.append("            return s;")
    lines.append("        }")
    lines.append("}")
    lines.append("int main() { Target t = new Target(); return t.run(); }")
    return _out(lines)


def p2_interface_method_call(n):
    lines = []
    for i in range(n - 1):
        lines.append(f"interface I{i} {{ int m{i}(); }}")
    lines.append("interface ITarget { int m(); }")
    lines.append("class Target { interface class ITarget; action: int m() { return 1; } }")
    lines.append("int main() {")
    lines.append("    ITarget t = new Target();")
    lines.append("    int s = 0;")
    for _ in range(n):
        lines.append("    s = s + t.m();")
    lines.append("    return s;")
    lines.append("}")
    return _out(lines)


def p3_interface_var_decl(n):
    lines = []
    for i in range(n - 1):
        lines.append(f"interface I{i} {{ int m{i}(); }}")
    lines.append("interface ITarget { int m(); }")
    lines.append("class Target { interface class ITarget; action: int m() { return 1; } }")
    # main 内声明 N 个目标接口变量（main 中不允许直接调用普通函数）
    lines.append("int main() {")
    lines.append("    int s = 0;")
    for i in range(n):
        lines.append(f"    ITarget a{i} = new Target();")
        lines.append(f"    s = s + a{i}.m();")
    lines.append("    return s;")
    lines.append("}")
    return _out(lines)


def p4_struct_field_read(n):
    lines = []
    for i in range(n - 1):
        lines.append(f"struct S{i} {{ int x; }}")
    lines.append("struct STarget { int x; }")
    lines.append("int main() {")
    lines.append("    STarget t;")
    lines.append("    int s = 0;")
    for _ in range(n):
        lines.append("    s = s + t.x;")
    lines.append("    return s;")
    lines.append("}")
    return _out(lines)


def p5_enum_variant(n):
    lines = []
    for i in range(n - 1):
        lines.append(f"enum E{i} {{ A{i}; }}")
    lines.append("enum ETarget { ATarget; }")
    lines.append("int main() {")
    lines.append("    int s = 0;")
    for i in range(n):
        lines.append(f"    ETarget e{i} = ETarget.ATarget;")
        lines.append(f"    s = s + 1;")
    lines.append("    return s;")
    lines.append("}")
    return _out(lines)


def p6_method_call_fallback(n):
    lines = []
    # N-1 个无关类，方法名与目标冲突（强制方法解析回退路径）
    for i in range(n - 1):
        lines.append(f"class C{i} {{ action: int m() {{ return {i}; }} }}")
    lines.append("class CTarget {")
    lines.append("    action:")
    lines.append("        int m() { return 100; }")
    lines.append("        // function: 节方法仅类内可调用，包一层 action")
    lines.append("        int runFn() { return fn(); }")
    lines.append("    function:")
    lines.append("        int fn() { return 200; }")
    lines.append("}")
    lines.append("class CStatic {")
    lines.append("    static: int sm() { return 300; }")
    lines.append("}")
    lines.append("int main() {")
    lines.append("    CTarget c = new CTarget();")
    lines.append("    int s = 0;")
    # 已知对象 action 调用
    for _ in range(n):
        lines.append("    s = s + c.m();")
    # function: 调用（经包装 action）
    for _ in range(n):
        lines.append("    s = s + c.runFn();")
    # static action 调用
    for _ in range(n):
        lines.append("    s = s + CStatic.sm();")
    # new C().method() 链式
    for _ in range(n):
        lines.append("    s = s + new CTarget().m();")
    lines.append("    return s;")
    lines.append("}")
    return _out(lines)


def p7_generic_instances(n):
    lines = []
    # N 个不同 struct 类型实参 → 同一 Box<T> 模板的 N 个不同实例 + N 个泛型函数
    # 调用（测已存在实例查找 / 新实例追加 / tu.classes/functions 扩容 / O(N²) 斜率）。
    # 注：struct 实参泛型缺陷已修复（sema typeName 补 TypeKind::Struct）。
    for i in range(n):
        lines.append(f"struct T{i} {{ int x; }}")
    lines.append("class Box<T> {")
    lines.append("    property: T v;")
    lines.append("    action: void set(T val) { v = val; }")
    lines.append("    action: T get() { return v; }")
    lines.append("}")
    lines.append("T id<T>(T x) { return x; }")
    lines.append("int run() {")
    lines.append("    int s = 0;")
    for i in range(n):
        lines.append(f"    Box<T{i}> b{i} = new Box<T{i}>();")
        lines.append(f"    T{i} z{i};")
        lines.append(f"    b{i}.set(z{i});")
        lines.append(f"    T{i} v{i} = b{i}.get();")
        lines.append(f"    T{i} w{i} = id<T{i}>(v{i});")
        lines.append("    s = s + 1;")
    lines.append("    return s;")
    lines.append("}")
    lines.append("int main() { return run(); }")
    return _out(lines)


BENCHES = {
    "P1": p1_class_bare_property,
    "P2": p2_interface_method_call,
    "P3": p3_interface_var_decl,
    "P4": p4_struct_field_read,
    "P5": p5_enum_variant,
    "P6": p6_method_call_fallback,
    "P7": p7_generic_instances,
}


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: gen.py <P1..P7> <N>\n")
        return 2
    bench, n = sys.argv[1], int(sys.argv[2])
    if bench not in BENCHES:
        sys.stderr.write(f"unknown benchmark '{bench}'\n")
        return 2
    sys.stdout.write(BENCHES[bench](n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
