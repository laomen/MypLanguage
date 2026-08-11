#!/usr/bin/env bash
# run_compare.sh — 编译并运行 MYP vs C++ 计算基准，输出对比表。
#
# 用法：
#   bash bench/run_compare.sh [iterations]     # 默认每项跑 3 轮取最小 ms
#   MYPCC=/path/to/mypc bash bench/run_compare.sh
#   CXX=g++ bash bench/run_compare.sh
#
# 每个基准二进制打印两行（机器可解析）：
#   verify <值>   校验值（MYP 与 C++ 应一致，容差 1e-3）
#   ms <毫秒>     实测耗时（整数毫秒）
# 脚本取多轮最小值，输出 MYP(ms) / C++(ms) / 比值(C++÷MYP) / verify。
#
# 说明：
#   - 比值 > 1 表示 C++ 更快；比值越接近 1，MYP 的代码生成越接近 C++。
#   - 若要启用 C++ 自动向量化（-march=native），取消下方注释。
set -u
cd "$(dirname "$0")"

MYPCC=${MYPCC:-../build/mypc}
# 自动探测 C++ 编译器：clang++ 优先，退化为 g++
if [ -n "${CXX:-}" ]; then
    :
elif command -v clang++ >/dev/null 2>&1; then
    CXX=clang++
else
    CXX=g++
fi
# C++ 用 -O3：MYP 的 -O2 精确映射 LLVM O2（= clang++ -O2，含循环展开/向量化）；
# g++ 的 -O2 更保守（gol 项 -O2 时 LLVM-O2 ≈ 2.7x 于 g++-O2，-O3 才持平）。
# 用 -O3 让 C++ 端与 LLVM O2 的激进程度对齐，才是诚实的对比。
CXXFLAGS="-O3 -std=c++17"
ITERS=${1:-3}

# mypc 用 argv[0] 定位 stdlib —— 必须用绝对路径调用
case "$MYPCC" in
    /*) : ;;
    *) MYPCC="$(cd "$(dirname "$MYPCC")" && pwd)/$(basename "$MYPCC")" ;;
esac

names=(sieve sieve_odd montepi matmul nbody nbodybg mandelbrot hashmap tripleloop raytracer gol fft astar sha256 quicksort nqueens bst dijkstra base64 alphabeta spmv kmeans bigint huffman convolution knapsack kmp radixsort sobel floyd heapsort crc32 dotprod slicevec slicemat parcomp parreduce)
mkdir -p out

# verify 数值一致性（整数精确、浮点容差 1e-3 相对误差）
verify_same() {
    awk -v a="$1" -v b="$2" 'BEGIN{
        if (a == b) { print 1; exit }
        if (a+0 == 0 && b+0 == 0) { print 1; exit }
        d = a - b; if (d < 0) d = -d;
        m = a < 0 ? -a : a; if (m < 1e-9) m = 1;
        print (d/m < 1e-3) ? 1 : 0
    }'
}

build_all() {
    local ok=1
    for name in "${names[@]}"; do
        if ! $MYPCC -O2 "myp/$name.myp" -o "out/${name}_myp" >/dev/null 2>&1; then
            echo "  [MYP] 编译失败: $name"; ok=0
        fi
        if ! $CXX $CXXFLAGS "cpp/$name.cpp" -o "out/${name}_cpp" >/dev/null 2>&1; then
            echo "  [C++] 编译失败: $name"; ok=0
        fi
    done
    [ $ok -eq 1 ]
}

# 运行一次二进制，返回 "verify ms"
run_once() {
    local bin=$1 out v ms
    out=$("$bin" 2>/dev/null) || { echo "0 0"; return; }
    v=$(printf '%s\n' "$out" | awk '/^verify/{print $2; exit}')
    ms=$(printf '%s\n' "$out" | awk '/^ms/{print $2; exit}')
    [ -z "$v" ] && v=0
    [ -z "$ms" ] && ms=0
    echo "$v $ms"
}

# 跑 ITERS 轮，取最小 ms（首轮 verify 为基准，轮间不一致则告警）
best_of() {
    local bin=$1 i v ms best_v="" best_ms=1e30
    for ((i=0; i<ITERS; i++)); do
        read -r v ms < <(run_once "$bin")
        if [ -z "$best_v" ]; then best_v=$v; fi
        if ! verify_same "$v" "$best_v" | grep -q 1; then
            echo "  [WARN] $bin verify 波动: 第${i}轮 $v != $best_v" >&2
        fi
        if awk -v m="$ms" -v b="$best_ms" 'BEGIN{exit !(m<b)}'; then best_ms=$ms; fi
    done
    echo "$best_v $best_ms"
}

echo "=== 编译（MYP: -O2 / C++: $CXXFLAGS）==="
if ! build_all; then
    echo "有编译失败，中止。"
    exit 1
fi

echo ""
printf "%-11s %-10s %-10s %-9s %s\n" "bench" "MYP(ms)" "C++(ms)" "C++/MYP" "verify"
printf "%s\n" "--------------------------------------------------------------"

for name in "${names[@]}"; do
    read -r mv mms < <(best_of "out/${name}_myp")
    read -r cv cms < <(best_of "out/${name}_cpp")
    ratio=$(awk -v c="$cms" -v m="$mms" 'BEGIN{ if (m+0>0) printf "%.2f", c/m; else print "inf" }')
    if verify_same "$mv" "$cv" | grep -q 1; then
        vnote="$mv"
    else
        vnote="$mv != $cv"
    fi
    printf "%-11s %-10s %-10s %-9s %s\n" "$name" "$mms" "$cms" "$ratio" "$vnote"
done

echo ""
echo "注: 比值 C++/MYP > 1 表示 C++ 更快；verify 两语言一致才有对比意义。"
echo "    hashmap 一项包含 MYP 泛型/类实例 ARC 成本，与 std::unordered_map 非纯 CPU 对比。"
