#!/usr/bin/env bash
# run_compare_go.sh — 对比 MYP @coro 协程 vs Go goroutine 效率
#
# 用法：bash bench/run_compare_go.sh [iterations]   # 默认 3 轮取最小 ms
#
# 每个基准 MYP 侧输出 "verify <值> / ms <毫秒>"，Go 侧同格式。
# 比值 = Go_ms / MYP_ms（>1 表示 Go 更快）。
#
# 覆盖：
#   coro_switch  — 协程上下文切换吞吐（K 协程 × M 次挂起/恢复）
#   coro_spawn   — 协程 spawn 开销（K 个只返回的协程）
set -u
cd "$(dirname "$0")"

MYPCC=${MYPCC:-../build/mypc}
GO=${GO:-go}
ITERS=${1:-3}
mkdir -p out

names=(coro_switch coro_spawn)

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
    out=$("$bin" 2>/dev/null) || { echo "0 0"; return; }
    v=$(printf '%s\n' "$out" | awk '/^verify/{print $2; exit}')
    ms=$(printf '%s\n' "$out" | awk '/^ms/{print $2; exit}')
    [ -z "$v" ] && v=0
    [ -z "$ms" ] && ms=0
    echo "$v $ms"
}

best_of() {
    local bin=$1 i v ms best_ms=99999999 best_v=0
    for ((i=0;i<ITERS;i++)); do
        read -r v ms <<< "$(run_once "$bin")"
        if [ "$ms" -lt "$best_ms" ]; then best_ms=$ms; best_v=$v; fi
    done
    echo "$best_v $best_ms"
}

echo "--------------------------------------------------------------"
printf "%-12s %-9s %-9s %-7s %s\n" "bench" "MYP(ms)" "Go(ms)" "Go/MYP" "verify"
echo "--------------------------------------------------------------"

for name in "${names[@]}"; do
    read -r mv mms <<< "$(best_of "out/${name}_myp")"
    read -r gv gms <<< "$(best_of "out/${name}_go")"
    ratio=$(awk -v a="$mms" -v b="$gms" 'BEGIN{ if (a<=0) print "inf"; else printf "%.2f", b/a }')
    same=0
    if [ "$mv" = "$gv" ]; then same=1; else
        # 数值容差 1e-3
        same=$(awk -v a="$mv" -v b="$gv" 'BEGIN{ d=a-b; if(d<0)d=-d; print (d/(a<0?-a:a)<1e-3 || (a==0&&b==0)) ? 1 : 0 }')
    fi
    printf "%-12s %-9s %-9s %-7s %s\n" "$name" "$mms" "$gms" "$ratio" "${mv}${same:+ (OK)}"
done

echo "--------------------------------------------------------------"
echo "注: Go/MYP = Go_ms/MYP_ms，<1 表示 Go 更快；verify 两语言一致(或数值容差)才有对比意义。"
