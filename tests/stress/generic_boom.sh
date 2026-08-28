#!/usr/bin/env bash
# generic_boom.sh — 泛型爆炸压测（MYP 单态化编译器压测）
#
# 同一泛型函数/泛型类被 N 种不同实参类型调用 → 编译器生成 N 个单态实例。
# 测量：
#   1. 编译时间增长曲线（N = 10/20/50/100/200）
#   2. 峰值内存（/usr/bin/time -v 的 Maximum resident set size）
#   3. 重复实例检测（同一类型重复调用应去重为 1 个实例；emit-llvm 查重名 define）
#
# 用法: bash tests/stress/generic_boom.sh    （退出码 0=通过, 1=发现重复实例）
set -u
cd "$(dirname "$0")/../.."
MYPCC="${MYPCC:-./build/mypc}"
OUT=/tmp/gen_boom
mkdir -p "$OUT"
if ! command -v /usr/bin/time >/dev/null 2>&1; then
    echo "[generic_boom] 需要 /usr/bin/time（GNU time，-v 输出内存）"
    exit 1
fi

# 生成 N 个不同类型的泛型测试文件
gen() {
    local n=$1 f=$2
    {
        echo "// generic boom: $n distinct types"
        echo "import env;"
        echo "T echo<T>(T x) { return x; }"
        echo "T apply<T>(T x, (T) -> T f) { return f(x); }"
        echo "class Box<T> {"
        echo "    property:"
        echo "        T v_;"
        echo "    action:"
        echo "        void set(T v) { v_ = v; }"
        echo "        T get() { return v_; }"
        echo "}"
        for i in $(seq 0 $((n-1))); do
            echo "class C$i {"
            echo "    property:"
            echo "        int v_;"
            echo "    action:"
            echo "        @constructor C$i(int v) { v_ = v; }"
            echo "        int get() { return v_; }"
            echo "}"
        done
        echo "class Main {"
        echo "    action:"
        echo "        @constructor Main() {"
        echo "            int acc = 0;"
        for i in $(seq 0 $((n-1))); do
            echo "            C$i c$i = new C$i($i);"
            echo "            C$i r$i = echo<C$i>(c$i);"          # 泛型函数实例 1
            echo "            Box<C$i> b$i = new Box<C$i>();"     # 泛型类实例
            echo "            b$i.set(c$i);"
            echo "            C$i echoed_$i = echo<C$i>(r$i);"    # 同类型重复调用 → 应去重
            echo "            acc = acc + echoed_$i.get();"
        done
        echo "            Console.writeString(\"acc=\" + acc + \"\\n\");"
        echo "        }"
        echo "}"
        echo "int main() { Main m = new Main(); return 0; }"
    } > "$f"
}

echo "=== 泛型爆炸压测（单态化）==="
printf "%-6s %-14s %-14s %-12s\n" "N" "编译时间(s)" "峰值内存(MB)" "实例数"
GROWTH=""
for n in 10 20 50 100 200; do
    f="$OUT/boom_$n.myp"
    gen "$n" "$f"
    t=$(/usr/bin/time -v "$MYPCC" -O2 "$f" -o "$OUT/boom_$n" 2>"$OUT/time_$n.txt")
    rc=$?
    elapsed=$(grep "Elapsed" "$OUT/time_$n.txt" | awk '{print $8}' | sed 's/elapsed//')
    mem=$(grep "Maximum resident" "$OUT/time_$n.txt" | awk '{print $NF}')
    # 实例数 = .ll 里 echo_*_inst 定义数
    "$MYPCC" -O2 --emit-llvm "$f" -o "$OUT/boom_$n.ll" >/dev/null 2>&1
    inst=$(grep -oE "define[^(]*@echo_[A-Za-z0-9_]*_inst" "$OUT/boom_$n.ll" 2>/dev/null | wc -l)
    printf "%-6s %-14s %-14s %-12s\n" "$n" "${elapsed:-?}" "$((mem/1024))" "$inst"
    if [ $rc -ne 0 ]; then
        echo "[generic_boom] N=$n 编译失败"; exit 1
    fi
    GROWTH="$GROWTH $n:${elapsed:-0}"
done

# 重复实例检测：200 类型 → 每类型 2 次调用（应去重为 1 实例）
f="$OUT/boom_200.myp"
ll="$OUT/boom_200.ll"
"$MYPCC" -O2 --emit-llvm "$f" -o "$ll" >/dev/null 2>&1
# 提取 echo_*_inst 定义名，查重复
grep -oE "define[^(]*@echo_[A-Za-z0-9_]*_inst" "$ll" | sed 's/define[^(]*@//' | sort > "$OUT/inst_names.txt"
total=$(wc -l < "$OUT/inst_names.txt")
uniq=$(sort -u "$OUT/inst_names.txt" | wc -l)
echo "--- 重复实例检测（N=200，每类型调用 2 次）---"
echo "  echo 实例总数: $total    去重后: $uniq    重复: $((total - uniq))"
if [ "$total" -eq "$uniq" ] && [ "$total" -ge 200 ]; then
    echo "PASS generic_boom（无重复实例，且 200 类型全部实例化）"
    exit 0
else
    echo "FAIL generic_boom（发现重复实例 或 实例数不足）"
    exit 1
fi
