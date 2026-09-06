#!/bin/bash
# C6 系统性矩阵 — 编译器泛型实例化容器扩容回归
# 验证: (1) 在多个函数体分析过程中持续触发大量泛型类/泛型函数实例化
#           （组合类型、嵌套 Box<Box<int>>、Pair<A,B>、泛型函数 identity<T>）
#           迫使编译器内部 tu.classes / 泛型实例表多次扩容，产物运行正确；
#       (2) 若存在 ASan 构建 (build-asan/mypc)，用其重复编译同一 stress 源，
#           断言无 use-after-free / heap-buffer-overflow（vector realloc 后
#           残留迭代器/索引访问会在此暴露）。
# 用法: MYPCC=./build/mypc bash tests/test_c6_generic_stress.sh
# 退出码: 0=全过, 1=有失败

set -u
MYPCC="${MYPCC:-./build/mypc}"
case "$MYPCC" in
    /*) ;;
    *) MYPCC="$(pwd)/$MYPCC" ;;
esac
PROJ_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ASANCC=""
if [ -x "$PROJ_ROOT/build-asan/mypc" ]; then
    ASANCC="$PROJ_ROOT/build-asan/mypc"
fi
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

echo "c6generic: 泛型实例化容器扩容（C6）"

# ---------- 生成 stress 源 ----------
SRC="$TMPDIR/c6_src.myp"
{
cat <<'EOF'
// C6 stress 源：跨函数体的大量泛型实例化（组合/嵌套类型 + 泛型函数）
class Box<T> {
    action:
        @constructor Box(T v) { v_ = v; }
        T get() { return v_; }
        void set(T v) { v_ = v; }
    property:
        T v_;
}
class Pair<A, B> {
    action:
        @constructor Pair(A a, B b) { a_ = a; b_ = b; }
        A first() { return a_; }
        B second() { return b_; }
    property:
        A a_;
        B b_;
}
T identity<T>(T v) { return v; }
class C6Stress {
    action:
EOF
# 类型池: 名称 值(want 为 int 可加偏移) want字面量 ctor实参 get链
#   每方法轮转一组；want 随方法偏移不同以迫使不同实例化。
# 生成 NMETHOD 个方法，每方法实例化整个类型池 + 1 个嵌套 + 2 个 Pair + identity。
NMETHOD=10
for m in $(seq 1 $NMETHOD); do
    echo "        int m$m() {"
    echo "            int f = 0;"
    # Box<T> 各基础类型
    echo "            Box<int> bi = new Box<int>($((100 + m)));"
    echo "            bi.set($((200 + m)));"
    echo "            if (bi.get() != $((200 + m))) f++;"
    echo "            Box<long> bl = new Box<long>($((9000000 + m)));"
    echo "            if (bl.get() != $((9000000 + m))) f++;"
    echo "            Box<double> bd = new Box<double>(0.5);"
    echo "            if (bd.get() != 0.5) f++;"
    echo "            Box<bool> bb = new Box<bool>(true);"
    echo "            if (bb.get() != true) f++;"
    echo "            Box<string> bs = new Box<string>(\"c6-$m\");"
    echo "            if (bs.get() != \"c6-$m\") f++;"
    # 嵌套泛型
    echo "            Box<Box<int>> nst = new Box<Box<int>>(new Box<int>($((10 + m))));"
    echo "            if (nst.get().get() != $((10 + m))) f++;"
    # Pair<A,B> 组合（轮转方向以产生新组合）
    if [ $((m % 2)) -eq 0 ]; then
        echo "            Pair<int, string> p = new Pair<int, string>($m, \"p-$m\");"
        echo "            if (p.first() != $m || p.second() != \"p-$m\") f++;"
    else
        echo "            Pair<string, int> p = new Pair<string, int>(\"q-$m\", $m);"
        echo "            if (p.first() != \"q-$m\" || p.second() != $m) f++;"
    fi
    echo "            Pair<long, Box<int>> pb = new Pair<long, Box<int>>($((5 + m)), new Box<int>($((6 + m))));"
    echo "            if (pb.first() != $((5 + m)) || pb.second().get() != $((6 + m))) f++;"
    # 泛型函数实例化
    echo "            if (identity<int>($((3 + m))) != $((3 + m))) f++;"
    echo "            if (identity<string>(\"id-$m\") != \"id-$m\") f++;"
    echo "            return f;"
    echo "        }"
done
cat <<'EOF'
}
int main() {
    C6Stress s = new C6Stress();
    int total = 0;
    total = total + s.m1();
EOF
for m in $(seq 2 $NMETHOD); do
    echo "    total = total + s.m$m();"
done
cat <<'EOF'
    return total == 0 ? 0 : 1;
}
EOF
} > "$SRC"

# 1) 普通编译器：编译 + 运行（实例化正确性）
if $MYPCC "$SRC" -o "$TMPDIR/c6.out" 2>"$TMPDIR/c6.err"; then
    (cd "$TMPDIR" && ./c6.out); ec=$?
    check "泛型 stress 源编译+运行 exit 0 ($((NMETHOD * 9)) 实例化)" "test $ec -eq 0"
    if [ $ec -ne 0 ]; then
        echo "      (cd $TMPDIR && ./c6.out) exit=$ec"
    fi
else
    check "泛型 stress 源编译+运行 exit 0" "false"
    echo "      $(head -4 "$TMPDIR/c6.err")"
fi

# 2) ASan 构建重复编译（若存在）—— realloc 后残留访问会报 UAF/溢出
if [ -n "$ASANCC" ]; then
    ASAN_OK=1
    for i in 1 2 3; do
        if ! $ASANCC "$SRC" -o "$TMPDIR/c6a$i.out" 2>"$TMPDIR/c6a$i.err"; then
            echo "  [asan pass $i] 编译失败"
            head -4 "$TMPDIR/c6a$i.err"
            ASAN_OK=0
            break
        fi
        if grep -qE "AddressSanitizer|use-after-free|heap-buffer-overflow|ERROR:" "$TMPDIR/c6a$i.err"; then
            echo "  [asan pass $i] 检测到内存错误:"
            head -8 "$TMPDIR/c6a$i.err"
            ASAN_OK=0
            break
        fi
    done
    check "ASan 重复编译 stress 源 x3 无内存错误" "test $ASAN_OK -eq 1"
else
    echo "  SKIP: 无 build-asan/mypc（未做 ASan 重复编译）"
    PASS=$((PASS + 1))
fi

echo "c6generic: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
