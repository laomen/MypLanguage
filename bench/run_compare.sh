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
#   - ratio = C++ms ÷ MYPms。> 1 表示 MYP 更快（C++ 耗时更多）；< 1 表示 C++ 更快。
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
    echo "[warn] 未找到 clang++，回退 g++ -O3：g++ 对浮点除法循环的向量化比 LLVM/clang 激进，" >&2
    echo "       float 密集型项（spectral_norm/sieve_odd 等）对比会失真。建议: sudo apt install clang" >&2
fi
# C++ 优化级别（对称对齐）：
#   clang++ -O2 == LLVM O2 == MYP -O2（同源后端，直接可比）→ 用 -O2 对称对比。
#   g++ -O2 更保守（gol 项 -O2 时 LLVM-O2 ≈ 2.7x 于 g++-O2，-O3 才持平）→
#   g++ 回退时仍用 -O3 对齐 LLVM O2 的激进程度。
if [ "$CXX" = "clang++" ]; then
    CXXFLAGS="-O2 -std=c++17"
else
    CXXFLAGS="-O3 -std=c++17"
fi
ITERS=${1:-3}

# ---- 测量科学化（无需 sudo）----
# 1) CPU 亲和性：MYP 与 C++ 都钉在同一物理核（默认 0，可 PIN_CORE= 覆盖），
#    避免线程迁移/SMT 兄弟核争用/NUMA 偏差。taskset 不可用时自动跳过。
# 2) 交错测量：每轮交替 MYP/C++ 的先后顺序（A/B 轮换），抵消热降频/变频的时间偏置。
# 3) 预热：每个基准计时前先各跑一次丢弃，消除 DVFS 爬升/冷 TLB 偏差。
PIN_CORE=${PIN_CORE:-0}
RUN_PREFIX=""
if command -v taskset >/dev/null 2>&1 && taskset -c "$PIN_CORE" true 2>/dev/null; then
    RUN_PREFIX="taskset -c $PIN_CORE"
else
    echo "[warn] taskset 不可用，未钉核（结果可能受调度/频率漂移影响）" >&2
fi

# 变频/boost 状态检查（仅提示，不自动改系统）：
#   固定频率（需 sudo，自行执行一次）：
#     sudo cpupower frequency-set -g performance
#     echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost
_freq_check() {
    local gov boost
    gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
    boost=$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null)
    if [ -n "$gov" ] && [ "$gov" != "performance" ]; then
        echo "[warn] cpufreq governor=$gov（建议 performance）: sudo cpupower frequency-set -g performance" >&2
    fi
    if [ "$boost" = "1" ]; then
        echo "[warn] boost 开启，频率会在 base~max 间浮动（建议关闭以获得稳定频率）:" >&2
        echo "       echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost" >&2
    fi
}
_freq_check

# mypc 用 argv[0] 定位 stdlib —— 必须用绝对路径调用
case "$MYPCC" in
    /*) : ;;
    *) MYPCC="$(cd "$(dirname "$MYPCC")" && pwd)/$(basename "$MYPCC")" ;;
esac

names=(sieve sieve_odd montepi matmul nbody nbodybg mandelbrot hashmap tripleloop raytracer gol fft astar sha256 quicksort nqueens bst dijkstra base64 alphabeta spmv kmeans bigint huffman convolution knapsack kmp radixsort sobel floyd heapsort crc32 dotprod slicevec slicemat parcomp parreduce matrix_int_mul dot_f64 lcs fib_matrix fannkuch spectral_norm binary_trees iface_dispatch slicedot)
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
    if [ -n "$RUN_PREFIX" ]; then
        out=$($RUN_PREFIX "$bin" 2>/dev/null) || { echo "0 0"; return; }
    else
        out=$("$bin" 2>/dev/null) || { echo "0 0"; return; }
    fi
    v=$(printf '%s\n' "$out" | awk '/^verify/{print $2; exit}')
    ms=$(printf '%s\n' "$out" | awk '/^ms/{print $2; exit}')
    [ -z "$v" ] && v=0
    [ -z "$ms" ] && ms=0
    echo "$v $ms"
}

# 交错测量 MYP/C++ 一对：ITERS 轮，每轮交替先后顺序（A/B 轮换抵消热漂移）；
# 取各自最小 ms。每名字先各预热一次（丢弃）。首轮 verify 为基准，波动告警。
best_pair() {
    local mb=$1 cb=$2 i v ms
    local mv="" cv="" mms=1e30 cms=1e30
    run_once "$mb" >/dev/null
    run_once "$cb" >/dev/null
    for ((i=0; i<ITERS; i++)); do
        if (( i % 2 == 0 )); then
            read -r v ms < <(run_once "$mb")
            if [ -z "$mv" ]; then mv=$v
            elif ! verify_same "$v" "$mv" | grep -q 1; then
                echo "  [WARN] $mb verify 波动: $v != $mv" >&2; fi
            if awk -v m="$ms" -v b="$mms" 'BEGIN{exit !(m<b)}'; then mms=$ms; fi
            read -r v ms < <(run_once "$cb")
            if [ -z "$cv" ]; then cv=$v
            elif ! verify_same "$v" "$cv" | grep -q 1; then
                echo "  [WARN] $cb verify 波动: $v != $cv" >&2; fi
            if awk -v m="$ms" -v b="$cms" 'BEGIN{exit !(m<b)}'; then cms=$ms; fi
        else
            read -r v ms < <(run_once "$cb")
            if [ -z "$cv" ]; then cv=$v
            elif ! verify_same "$v" "$cv" | grep -q 1; then
                echo "  [WARN] $cb verify 波动: $v != $cv" >&2; fi
            if awk -v m="$ms" -v b="$cms" 'BEGIN{exit !(m<b)}'; then cms=$ms; fi
            read -r v ms < <(run_once "$mb")
            if [ -z "$mv" ]; then mv=$v
            elif ! verify_same "$v" "$mv" | grep -q 1; then
                echo "  [WARN] $mb verify 波动: $v != $mv" >&2; fi
            if awk -v m="$ms" -v b="$mms" 'BEGIN{exit !(m<b)}'; then mms=$ms; fi
        fi
    done
    echo "$mv $mms $cv $cms"
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
    read -r mv mms cv cms < <(best_pair "out/${name}_myp" "out/${name}_cpp")
    ratio=$(awk -v c="$cms" -v m="$mms" 'BEGIN{ if (m+0>0) printf "%.2f", c/m; else print "inf" }')
    if verify_same "$mv" "$cv" | grep -q 1; then
        vnote="$mv"
    else
        vnote="$mv != $cv"
    fi
    printf "%-11s %-10s %-10s %-9s %s\n" "$name" "$mms" "$cms" "$ratio" "$vnote"
done

echo ""
echo "注: 比值 C++/MYP > 1 表示 MYP 更快（C++ 耗时更多）；< 1 表示 C++ 更快。"
echo "    verify 两语言一致才有对比意义；hashmap 含 MYP 泛型 ARC 成本，非纯 CPU 对比。"
