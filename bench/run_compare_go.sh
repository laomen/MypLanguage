#!/usr/bin/env bash
# run_compare_go.sh — 对比 MYP vs Go 效率（主套件 + @coro 协程专项）
#
# 用法：bash bench/run_compare_go.sh [iterations]   # 默认 3 轮取最小 ms
#
# 每个基准 MYP 侧输出 "verify <值> / ms <毫秒>"，Go 侧同格式。
# 比值 = Go_ms / MYP_ms（<1 表示 Go 更快）。
#
# 覆盖：
#   21 个主套件基准（sieve..bigint）— 与 bench/cpp/*.cpp 同算法同规模，
#   Go 版逐文件移植，verify 与 MYP 完全对拍（整数精确、浮点 1e-3 容差）。
#   mixed           — ARC 密集混合：短命对象 churn + slice/容器元素 churn +
#                     string 拼接 + 可变长数组（MYP 容器 ARC 分解的 Go 对照）
#   channel_pingpong — 通道 ping-pong：MYP Channel vs Go channel（capacity=1）
#   io_socket        — @coro I/O 密集：MYP await fd（loopback TCP + waitFd）
#                      vs Go goroutine + 阻塞 socket
#   coro_switch  — 协程上下文切换吞吐（K 协程 × M 次挂起/恢复）
#   coro_spawn   — 协程 spawn 开销（K 个只返回的协程）
set -u
cd "$(dirname "$0")"

MYPCC=${MYPCC:-../build/mypc}
GO=${GO:-go}
ITERS=${1:-3}
mkdir -p out

PIN_CORE=${PIN_CORE:-none}
RUN_PREFIX=""
if [ "$PIN_CORE" = "none" ]; then
    :
elif command -v taskset >/dev/null 2>&1 && taskset -c "$PIN_CORE" true 2>/dev/null; then
    RUN_PREFIX="taskset -c $PIN_CORE"
else
    echo "[warn] taskset 不可用，未钉核（结果可能受调度/频率漂移影响）" >&2
fi

names=(
    sieve sieve_odd montepi matmul nbody mandelbrot tripleloop fft sha256 quicksort knapsack
    kmp crc32 radixsort sobel floyd heapsort convolution base64 spmv kmeans
    huffman bigint
    fannkuch spectral_norm binary_trees
    mixed
    channel_pingpong io_socket
    coro_switch coro_spawn
)
if [ -n "${BENCHES:-}" ]; then
    read -r -a names <<< "$BENCHES"
fi

build_all() {
    local ok=1
    for name in "${names[@]}"; do
        if ! $MYPCC -O2 "myp/$name.myp" -o "out/${name}_myp" >/dev/null 2>&1; then
            echo "  [MYP] 编译失败: $name"; ok=0
        fi
        if ! (cd go && $GO build -o "../out/${name}_go" "$name.go") >/dev/null 2>&1; then
            echo "  [Go] 编译失败: $name"; ok=0
        fi
    done
    [ $ok -eq 1 ]
}

run_once() {
    local bin=$1 out v ms
    if [ -n "$RUN_PREFIX" ]; then
        out=$($RUN_PREFIX "$bin" 2>/dev/null) || { echo "[bench] 运行失败: $bin" >&2; echo "0 0"; return 1; }
    else
        out=$("$bin" 2>/dev/null) || { echo "[bench] 运行失败: $bin" >&2; echo "0 0"; return 1; }
    fi
    v=$(printf '%s\n' "$out" | awk '/^verify/{print $2; exit}')
    ms=$(printf '%s\n' "$out" | awk '/^ms/{print $2; exit}')
    if [ -z "$v" ] || [ -z "$ms" ]; then
        echo "[bench] 输出缺 verify/ms: $bin" >&2
        echo "0 0"; return 1
    fi
    echo "$v $ms"
}

verify_same() {
    awk -v a="$1" -v b="$2" 'BEGIN{
        if (a == b) { print 1; exit }
        d=a-b; if(d<0)d=-d;
        m=a<0?-a:a; if(m<1e-9)m=1;
        print (d/m < 1e-3) ? 1 : 0
    }'
}

best_pair() {
    local mb=$1 gb=$2 i v ms line
    local mv="" gv="" mms=99999999 gms=99999999
    PAIR_FAIL=0
    if ! line=$(run_once "$mb"); then PAIR_FAIL=1; line="0 0"; fi
    if ! line=$(run_once "$gb"); then PAIR_FAIL=1; fi
    for ((i=0; i<ITERS; i++)); do
        if ((i % 2 == 0)); then
            if ! line=$(run_once "$mb"); then PAIR_FAIL=1; line="0 0"; fi
            read -r v ms <<< "$line"
            [ -z "$mv" ] && mv=$v
            [ "$ms" -lt "$mms" ] && mms=$ms
            if ! line=$(run_once "$gb"); then PAIR_FAIL=1; line="0 0"; fi
            read -r v ms <<< "$line"
            [ -z "$gv" ] && gv=$v
            [ "$ms" -lt "$gms" ] && gms=$ms
        else
            if ! line=$(run_once "$gb"); then PAIR_FAIL=1; line="0 0"; fi
            read -r v ms <<< "$line"
            [ -z "$gv" ] && gv=$v
            [ "$ms" -lt "$gms" ] && gms=$ms
            if ! line=$(run_once "$mb"); then PAIR_FAIL=1; line="0 0"; fi
            read -r v ms <<< "$line"
            [ -z "$mv" ] && mv=$v
            [ "$ms" -lt "$mms" ] && mms=$ms
        fi
    done
    BV_MV=$mv; BV_MMS=$mms; BV_CV=$gv; BV_CMS=$gms
}

build_all || { echo "[run_compare_go] 编译失败，中止"; exit 1; }

echo "--------------------------------------------------------------"
printf "%-12s %-9s %-9s %-7s %s\n" "bench" "MYP(ms)" "Go(ms)" "Go/MYP" "verify"
echo "--------------------------------------------------------------"

summary=$(mktemp /tmp/myp_go_bench.XXXXXX)
trap 'rm -f "$summary"' EXIT

FAILED=0
for name in "${names[@]}"; do
    best_pair "out/${name}_myp" "out/${name}_go"
    mv=$BV_MV; mms=$BV_MMS; gv=$BV_CV; gms=$BV_CMS
    ratio=$(awk -v a="$mms" -v b="$gms" 'BEGIN{ if (a<=0) print "inf"; else printf "%.2f", b/a }')
    same=$(verify_same "$mv" "$gv")
    if [ "$same" = 1 ]; then
        vnote="$mv (OK)"
        printf '%s %s %s %s\n' "$name" "$mms" "$gms" "$ratio" >> "$summary"
    else
        vnote="$mv != $gv"
        echo "  [FAIL] $name verify 不一致: MYP=$mv Go=$gv" >&2
        FAILED=1
    fi
    if [ "$PAIR_FAIL" = 1 ]; then
        echo "  [FAIL] $name 运行失败（二进制未输出有效 verify/ms）" >&2
        FAILED=1
    fi
    printf "%-12s %-9s %-9s %-7s %s\n" "$name" "$mms" "$gms" "$ratio" "$vnote"
done

echo "--------------------------------------------------------------"
echo "注: Go/MYP = Go_ms/MYP_ms，<1 表示 Go 更快；verify 两语言一致(或数值容差)才有对比意义。"
awk '
    $4 > 0 {
        n++; logsum += log($4);
        if ($4 >= 1) myp_wins++; else { go_wins++; deficit += (1/$4 - 1) * 100 }
        if ($2 >= 5 && $3 >= 5) { stable++; stable_logsum += log($4) }
    }
    END {
        if (n == 0) exit;
        printf "有效项目: %d；MYP 胜 %d，Go 胜 %d；Go/MYP 几何均值 %.2f\n", n, myp_wins, go_wins, exp(logsum/n);
        if (stable > 0) printf "双方均 >=5ms 的稳健项目: %d；Go/MYP 几何均值 %.2f\n", stable, exp(stable_logsum/stable);
        if (go_wins > 0) printf "Go 获胜项目中，MYP 平均耗时缺口 %.1f%%\n", deficit/go_wins;
    }
' "$summary"
echo "Go 获胜项（按 MYP 缺口从大到小）:"
sort -k4,4n "$summary" | awk '$4 < 1 { printf "%-20s MYP=%sms Go=%sms 缺口=%.1f%%\n", $1, $2, $3, (1/$4-1)*100 }'
if [ "$FAILED" = 1 ]; then
    echo "[FAIL] 存在失败项（运行失败或 verify 不一致），退出码非零。" >&2
    exit 1
fi
exit 0
