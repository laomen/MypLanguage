#!/bin/bash
# 多文件编译回归（manual.md §12 编译器章节核对）
# 验证: 多文件合并为单模块（函数跨文件调用）、第二个文件的 import 被加载
#       （BUG-025：原实现只合并 classes/interfaces/mappings/functions，漏了
#       imports/structs/enums/ffis 等，第二文件的 `import env` 直接 Console
#       未定义）、struct/enum 跨文件可见、多文件 + `--test` + 用户 main 不崩
#       （BUG-026：用户 main 空块无 terminator → LLVM verify 失败，且运行器
#       main 被改名 main.1 静默不跑）。
# 用法: MYPCC=./build/mypc bash tests/test_multifile.sh
# 退出码: 0=全过, 1=有失败

set -u
MYPCC="${MYPCC:-./build/mypc}"
# Resolve to absolute so it keeps working after `cd` into the temp dir.
case "$MYPCC" in
    /*) ;;
    *) MYPCC="$(pwd)/$MYPCC" ;;
esac
PASS=0
FAIL=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
ORIGDIR=$(pwd)

check() {
    local name="$1"; local cond="$2"
    if eval "$cond"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}

# Helper: 编译多个源文件（同一 CLI = 合并单 TU）→ 运行，断言 stdout 含 expect。
mfc() {
    local name="$1"; shift
    local expect="$1"; shift
    if $MYPCC "$@" -o "$TMPDIR/mfc.out" 2>"$TMPDIR/mfc.err"; then
        local out; out=$(cd "$TMPDIR" && ./mfc.out 2>&1)
        if echo "$out" | grep -qF "$expect"; then
            check "$name" "true"
        else
            check "$name" "false"
            echo "      out=[$out]"
        fi
    else
        check "$name" "false"
        echo "      $(head -3 "$TMPDIR/mfc.err")"
    fi
}

echo "multifile: 多文件编译（§12）"

# 1) 多文件基础：函数跨文件调用（无 import）
cat > "$TMPDIR/helper.myp" <<'EOF'
int helperAdd(int x, int y) { return x + y; }
EOF
cat > "$TMPDIR/main.myp" <<'EOF'
int main() { return helperAdd(3, 4) == 7 ? 0 : 1; }
EOF
if $MYPCC "$TMPDIR/helper.myp" "$TMPDIR/main.myp" -o "$TMPDIR/basic.out" 2>"$TMPDIR/basic.err"; then
    (cd "$TMPDIR" && ./basic.out); basic_ec=$?
    check "多文件函数跨文件调用 exit 0" "test $basic_ec -eq 0"
else
    check "多文件函数跨文件调用 exit 0" "false"
    echo "      $(head -2 "$TMPDIR/basic.err")"
fi

# 2) BUG-025：第二个文件的 `import env` 必须被加载（Console 可用）
#    （main() 内不允许直接函数调用，故经类 @constructor 输出）
cat > "$TMPDIR/f2_import.myp" <<'EOF'
import env;
class F2Main {
    action:
        @constructor F2Main() {
            Console.writeString("mf-import-ok\n");
        }
}
int main() { F2Main m = new F2Main(); return 0; }
EOF
if $MYPCC "$TMPDIR/helper.myp" "$TMPDIR/f2_import.myp" -o "$TMPDIR/imp.out" 2>"$TMPDIR/imp.err"; then
    imp_out=$(cd "$TMPDIR" && ./imp.out 2>&1)
    check "BUG-025 第二文件 import env 合并" "test \"\$imp_out\" = \"mf-import-ok\""
else
    check "BUG-025 第二文件 import env 合并" "false"
    echo "      $(head -2 "$TMPDIR/imp.err")"
fi

# 3) BUG-025：struct/enum 在第二个文件可见
cat > "$TMPDIR/f3_decl.myp" <<'EOF'
struct Vec3 { int x; int y; }
enum Color3 { RED; GREEN; }
EOF
cat > "$TMPDIR/f3_use.myp" <<'EOF'
import test;
@test void t_multifile_struct_enum() {
    Vec3 v;
    v.x = 3;
    v.y = 4;
    Color3 c = Color3.GREEN;
    string nm = "";
    match (c) {
        Color3.RED => { nm = "RED"; }
        Color3.GREEN => { nm = "GREEN"; }
    }
    Test.assertEq(v.x + v.y, 7, "multi-file struct");
    Test.assertStrEq(nm, "GREEN", "multi-file enum");
}
int main() { return 0; }
EOF
if $MYPCC --test "$TMPDIR/f3_decl.myp" "$TMPDIR/f3_use.myp" -o "$TMPDIR/se.out" 2>"$TMPDIR/se.err"; then
    se_out=$(cd "$TMPDIR" && ./se.out 2>&1)
    if echo "$se_out" | grep -q "tests: 1, assertions: 2 passed, 0 failed"; then
        check "BUG-025 struct/enum 第二文件可见" "true"
    else
        check "BUG-025 struct/enum 第二文件可见" "false"
        echo "      $se_out" | tail -2
    fi
else
    check "BUG-025 struct/enum 第二文件可见" "false"
    echo "      $(head -2 "$TMPDIR/se.err")"
fi

# 4) BUG-026：多文件 + `--test` + 用户 main → 不崩 + 运行器执行
cat > "$TMPDIR/f4_test.myp" <<'EOF'
import test;
@test void t_cross_file() {
    Test.assertEq(helperAdd(40, 2), 42, "cross-file call");
}
int main() { return 0; }
EOF
if $MYPCC --test "$TMPDIR/helper.myp" "$TMPDIR/f4_test.myp" -o "$TMPDIR/tm.out" 2>"$TMPDIR/tm.err"; then
    tm_out=$(cd "$TMPDIR" && ./tm.out 2>&1)
    if echo "$tm_out" | grep -q "tests: 1, assertions: 1 passed, 0 failed" \
       && echo "$tm_out" | grep -q "RUN: t_cross_file"; then
        check "BUG-026 --test + 用户 main 运行器执行" "true"
    else
        check "BUG-026 --test + 用户 main 运行器执行" "false"
        echo "      $tm_out" | tail -3
    fi
else
    check "BUG-026 --test + 用户 main 运行器执行" "false"
    echo "      $(head -2 "$TMPDIR/tm.err")"
fi

# 5) 类跨文件：@constructor 带参 + new + 方法调用 + 顶层函数
cat > "$TMPDIR/mf5_defs.myp" <<'EOF'
class Counter {
    property: int n = 0;
    action: @constructor Counter(int start) { n = start; }
    void bump(int d) { n = n + d; }
    int get() { return n; }
}
int doubleOf(int x) { return x * 2; }
EOF
cat > "$TMPDIR/mf5_use.myp" <<'EOF'
import env;
class Use {
    action: @constructor Use() {
        Counter c = new Counter(5);
        c.bump(3);
        Console.writeLine("mf5 get=" + c.get() + " double=" + doubleOf(c.get()));
    }
}
int main() { Use u = new Use(); return 0; }
EOF
mfc "类跨文件 new+ctor+方法+顶层函数" "mf5 get=8 double=16" \
    "$TMPDIR/mf5_defs.myp" "$TMPDIR/mf5_use.myp"

# 6) 泛型类跨文件实例化 + struct 值类型（字段私有规避：struct 字段可直读）
cat > "$TMPDIR/mf6_defs.myp" <<'EOF'
struct Vec2 { int x; int y; }
class Box<T> {
    property: T v;
    action:
        void put(T x) { v = x; }
        T get() { return v; }
}
EOF
cat > "$TMPDIR/mf6_use.myp" <<'EOF'
import env;
class Use {
    action: @constructor Use() {
        Box<int> bi = new Box<int>(); bi.put(42);
        Box<string> bs = new Box<string>(); bs.put("hi");
        Box<Vec2> bv = new Box<Vec2>();
        Vec2 v2; v2.x = 3; v2.y = 4;
        bv.put(v2);
        Vec2 r = bv.get();
        Console.writeLine("mf6 int=" + bi.get() + " str=" + bs.get() + " vec=(" + r.x + "," + r.y + ")");
    }
}
int main() { Use u = new Use(); return 0; }
EOF
mfc "泛型类+struct 跨文件实例化" "mf6 int=42 str=hi vec=(3,4)" \
    "$TMPDIR/mf6_defs.myp" "$TMPDIR/mf6_use.myp"

# 7) 接口跨文件：接口定义在 A、实现类在 B，从局部变量与 new 上转后分派
cat > "$TMPDIR/mf7_defs.myp" <<'EOF'
interface Shape { double area(); double perimeter(); }
EOF
cat > "$TMPDIR/mf7_use.myp" <<'EOF'
import env;
class Circle {
    interface class Shape;
    property: double r = 2.0;
    action:
        double area() { return 3.14 * r * r; }
        double perimeter() { return 2 * 3.14 * r; }
}
class Use {
    action: @constructor Use() {
        Circle c = new Circle();
        Shape s = c;                       // 局部变量上转（需 concrete 类名）
        Shape s2 = new Circle();           // new 直接上转
        Console.writeLine("mf7 area=" + s.area() + " per=" + s.perimeter()
            + " area2=" + s2.area());
    }
}
int main() { Use u = new Use(); return 0; }
EOF
mfc "接口跨文件 上转+虚表分派" "mf7 area=12.56 per=12.56 area2=12.56" \
    "$TMPDIR/mf7_defs.myp" "$TMPDIR/mf7_use.myp"

# 8) 接口默认方法跨文件（默认实现 + 默认方法链式互调）
cat > "$TMPDIR/mf8_defs.myp" <<'EOF'
interface IDef {
    double area();
    string describe() { return "area=" + area(); }
}
interface IDef2 {
    double base();
    string label() { return "L" + base(); }
    string describe() { return "D(" + label() + ")"; }
}
EOF
cat > "$TMPDIR/mf8_use.myp" <<'EOF'
import env;
class Cir {
    interface class IDef;
    property: double r = 1.0;
    action: double area() { return 3.14 * r * r; }
}
class BaseImpl {
    interface class IDef2;
    property: double r = 1.0;
    action: double base() { return 7.0; }
}
class Use {
    action: @constructor Use() {
        IDef c = new Cir();
        IDef2 d = new BaseImpl();
        Console.writeLine("mf8 default=" + c.describe() + " chain=" + d.describe());
    }
}
int main() { Use u = new Use(); return 0; }
EOF
mfc "接口默认方法跨文件" "mf8 default=area=3.14 chain=D(L7)" \
    "$TMPDIR/mf8_defs.myp" "$TMPDIR/mf8_use.myp"

# 9) 静态类/静态方法跨文件
cat > "$TMPDIR/mf9_defs.myp" <<'EOF'
class MathX {
    static:
        int twice(int x) { return x * 2; }
        double sqrt(double v) { return __myp_math_sqrt(v); }
}
EOF
cat > "$TMPDIR/mf9_use.myp" <<'EOF'
import env;
class Use {
    action: @constructor Use() {
        Console.writeLine("mf9 twice=" + MathX.twice(21) + " sqrt=" + MathX.sqrt(64.0));
    }
}
int main() { Use u = new Use(); return 0; }
EOF
mfc "静态类方法跨文件" "mf9 twice=42 sqrt=8" \
    "$TMPDIR/mf9_defs.myp" "$TMPDIR/mf9_use.myp"

# 10) 顶层函数带类形参/返回 跨文件（ARC 引用跨文件传参/返回）
cat > "$TMPDIR/mf10_defs.myp" <<'EOF'
class Wrapper {
    property: int v = 0;
    action: @constructor Wrapper(int x) { v = x; }
    int get() { return v; }
}
Wrapper makeWrap(int x) { Wrapper w = new Wrapper(x); return w; }
int readWrap(Wrapper w) { return w.get(); }
EOF
cat > "$TMPDIR/mf10_use.myp" <<'EOF'
import env;
class Use {
    action: @constructor Use() {
        Wrapper w = makeWrap(99);
        Console.writeLine("mf10 make+read=" + readWrap(w));
    }
}
int main() { Use u = new Use(); return 0; }
EOF
mfc "函数带类形参/返回 跨文件" "mf10 make+read=99" \
    "$TMPDIR/mf10_defs.myp" "$TMPDIR/mf10_use.myp"

# 11) 纯类环引用跨文件（A 方法形参/调用 B 类，B 方法引用 A 类，方法互调）
cat > "$TMPDIR/mf11_a.myp" <<'EOF'
import env;
class RefA {
    action: @constructor RefA() {}
    string greet(Player p) { return "hi P" + p.getId(); }
}
EOF
cat > "$TMPDIR/mf11_b.myp" <<'EOF'
import env;
class Player {
    property: int id = 0;
    action: @constructor Player(int i) { id = i; }
    int getId() { return id; }
    string withRefA(RefA a) { return a.greet(this); }
}
class Use {
    action: @constructor Use() {
        Player p = new Player(9);
        RefA a = new RefA();
        Console.writeLine("mf11 cycle=" + p.withRefA(a));
    }
}
int main() { Use u = new Use(); return 0; }
EOF
mfc "纯类环引用跨文件 方法互调" "mf11 cycle=hi P9" \
    "$TMPDIR/mf11_a.myp" "$TMPDIR/mf11_b.myp"

# 12) 文件顺序无关：use 文件在前、defs 在后（合并后统一 sema）
cat > "$TMPDIR/mf12_use.myp" <<'EOF'
import env;
class Use {
    action: @constructor Use() {
        Counter c = new Counter(1);
        c.bump(2);
        Console.writeLine("mf12 order=" + c.get() + " d=" + doubleOf(3));
    }
}
int main() { Use u = new Use(); return 0; }
EOF
mfc "文件顺序调换仍编译" "mf12 order=3 d=6" \
    "$TMPDIR/mf12_use.myp" "$TMPDIR/mf5_defs.myp"

# 13) @coro 方法跨文件：A 定义 @coro 方法、B spawn + 泵调度
cat > "$TMPDIR/mf13_defs.myp" <<'EOF'
class Gen {
    action: @coro void run(Channel out, int n) {
        for (int i = 1; i <= n; i = i + 1) out.send(long(i));
        out.send(0L);
    }
}
EOF
cat > "$TMPDIR/mf13_use.myp" <<'EOF'
import env;
import channel;
import coro;
class Use {
    action: @constructor Use() {
        Channel ch = new Channel();
        ch.init(8);
        Gen g = new Gen();
        g.run(ch, 3);
        long acc = 0; long v = 1;
        while (v != 0L) {
            while (Coro.count() > 0 && ch.size() == 0) Coro.scheduler();
            v = ch.recv();
            acc = acc + v;
        }
        Console.writeLine("mf13 coro=" + acc);
    }
}
int main() { Use u = new Use(); return 0; }
EOF
mfc "@coro 方法跨文件 spawn" "mf13 coro=6" \
    "$TMPDIR/mf13_defs.myp" "$TMPDIR/mf13_use.myp"

echo ""
echo "multifile: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
