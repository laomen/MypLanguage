#!/usr/bin/env bash
# compare_compile.sh — 跨编译器编译时间对比（mypc vs g++ vs go build）
#
# 对同一组基准源码（bench/myp、bench/cpp、bench/go 同算法同规模），分别用
#   mypc -O2 / g++ -O3 / go build 编译，测完整编译时间（外部高精度时钟）。
# 输出：bench / MYP(ms) / C++(ms) / Go(ms) / C++÷MYP / Go÷MYP。
#
# 用法：bash bench/compiler/compare_compile.sh [iters]   # 默认 3 轮取中位数
# 说明：编译时间含编译器初始化/IR 生成/后端；MYP 走 LLVM，C++ 用 g++，Go 用官方
#       go toolchain。仅测编译（不运行），反映各编译器对等价工作量的前端+后端速度。
set -u
cd "$(dirname "$0")/../.."   # 到仓库根（bench/compiler/ 上两级）

MYPCC=${MYPCC:-build/mypc}
CXX=${CXX:-g++}
GO=${GO:-go}
ITERS=${ITERS:-3}
WORK=$(mktemp -d /tmp/myp_ccompare.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

names=(sieve montepi matmul nbody mandelbrot tripleloop fft sha256 quicksort knapsack huffman gol)

# mypc 用 argv[0] 定位 stdlib —— 绝对路径
case "$MYPCC" in
    /*) : ;;
    *) MYPCC="$(cd "$(dirname "$MYPCC")" && pwd)/$(basename "$MYPCC")" ;;
esac
[ -x "$MYPCC" ] || { echo "error: mypc 不存在: $MYPCC"; exit 1; }

median() {
    sort -n | awk '{a[NR]=$1} END{ if (NR==0) print 0; else if (NR%2) print a[(NR+1)/2]; else print int((a[NR/2]+a[NR/2+1])/2) }'
}

compile_time() { # $1=cmd...  输出单次编译毫秒
    local start_ms end_ms
    start_ms=$(date +%s%N)
    "$@" >/dev/null 2>&1
    end_ms=$(date +%s%N)
    echo $(( (end_ms - start_ms) / 1000000 ))
}

best3() { # $1.. = 命令（完整）；跑 ITERS 次取中位数
    local i vals=""
    for ((i=0; i<ITERS; i++)); do
        vals="$vals
$(compile_time "$@")"
    done
    printf '%s\n' "$vals" | median
}

echo "=== 编译时间对比（mypc -O2 / $CXX -O3 / $GO build，ITERS=$ITERS 中位数）==="
printf "%-12s %-9s %-9s %-9s %-9s %-9s %s\n" "bench" "MYP(ms)" "C++(ms)" "Go(ms)" "C++/MYP" "Go/MYP" "ok"
printf "%s\n" "-------------------------------------------------------------------------"

rc=0
for name in "${names[@]}"; do
    msrc="bench/myp/$name.myp"; csrc="bench/cpp/$name.cpp"; gsrc="bench/go/$name.go"
    outm="$WORK/${name}_myp.o"; outc="$WORK/${name}_cpp"; outg="$WORK/${name}_go"
    [ -f "$msrc" ] || { echo "  [MYP] 缺 $msrc"; continue; }
    # 编译时间（warmup 一次 + ITERS 次取中位数）
    tm=$(best3 "$MYPCC" -O2 "$msrc" -o "$outm")
    [ $? -ne 0 ] && { tm="ERR"; rc=1; }
    tc="-"; tg="-"
    if [ -f "$csrc" ]; then
        tc=$(best3 "$CXX" -O3 -std=c++17 "$csrc" -o "$outc")
    fi
    if [ -f "$gsrc" ]; then
        tg=$(best3 sh -c "cd bench/go && '$GO' build -o ../$WORK/${name}_go ${name}.go")
    fi
    ratio_c=$(awk -v c="$tc" -v m="$tm" 'BEGIN{ if (m+0>0 && c+0>0) printf "%.1f", c/m; else print "-" }')
    ratio_g=$(awk -v g="$tg" -v m="$tm" 'BEGIN{ if (m+0>0 && g+0>0) printf "%.1f", g/m; else print "-" }')
    printf "%-12s %-9s %-9s %-9s %-9s %-9s %s\n" "$name" "$tm" "$tc" "$tg" "$ratio_c" "$ratio_g" "-"
done

echo ""
echo "注: 比值 = 对方编译ms ÷ mypc编译ms（>1 表示对方编译更慢）。"
echo "    go 项可能首次下载/缓存；mypc 已含 LLVM 初始化；仅比编译不运行。"
exit $rc
