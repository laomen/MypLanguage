#!/usr/bin/env bash
# run_compare_py.sh — 对比 MYP vs Python（CPython，纯 Python 实现、不用 numpy）
#
# 用法：bash bench/run_compare_py.sh [iterations]   # 默认 3 轮取最小 ms
#
# 每个基准 MYP 侧输出 "verify <值> / ms <毫秒>"，Python 侧同格式。
# 比值 = Py_ms / MYP_ms（>1 表示 Python 更慢）。
#
# 说明：
#   - Python 是解释型，纯 Python 循环 vs MYP（LLVM 编译）差距通常 10-100×，属预期。
#   - 用纯 Python（列表/循环，无 numpy）对比语言本身执行效率，不引入 C 加速库。
#   - verify 与 MYP 完全对拍（整数精确、浮点 1e-3 容差），同算法同规模。
#   - 规模需 Python 能在可接受时间内跑完（否则该基准慢到失真）。
set -u
cd "$(dirname "$0")"

MYPCC=${MYPCC:-../build/mypc}
PY=${PY:-python3}
ITERS=${1:-3}
mkdir -p out

names=(sieve montepi nbody tripleloop quicksort matrix_int_mul fib_matrix dot_f64 lcs)

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
        if [ ! -f "py/$name.py" ]; then
            echo "  [PY] 缺失: py/$name.py"; ok=0
        fi
    done
    [ $ok -eq 1 ]
}

run_myp() {
    local bin=$1 out v ms
    out=$("$bin" 2>/dev/null) || { echo "0 0"; return; }
    v=$(printf '%s\n' "$out" | awk '/^verify/{print $2; exit}')
    ms=$(printf '%s\n' "$out" | awk '/^ms/{print $2; exit}')
    [ -z "$v" ] && v=0
    [ -z "$ms" ] && ms=0
    echo "$v $ms"
}

run_py() {
    local f=$1 out v ms
    out=$("$PY" "py/$f.py" 2>/dev/null) || { echo "0 0"; return; }
    v=$(printf '%s\n' "$out" | awk '/^verify/{print $2; exit}')
    ms=$(printf '%s\n' "$out" | awk '/^ms/{print $2; exit}')
    [ -z "$v" ] && v=0
    [ -z "$ms" ] && ms=0
    echo "$v $ms"
}

best_of() {
    local kind=$1 name=$2 i v ms best_v="" best_ms=1e30
    for ((i=0; i<ITERS; i++)); do
        if [ "$kind" = "myp" ]; then
            read -r v ms < <(run_myp "out/${name}_myp")
        else
            read -r v ms < <(run_py "$name")
        fi
        if [ -z "$best_v" ]; then best_v=$v; fi
        if awk -v m="$ms" -v b="$best_ms" 'BEGIN{exit !(m<b)}'; then best_ms=$ms; fi
    done
    echo "$best_v $best_ms"
}

echo "=== 编译（MYP: -O2 / Python: $PY）==="
if ! build_all; then
    echo "有编译失败，中止。"
    exit 1
fi

echo ""
printf "%-15s %-10s %-12s %-10s %s\n" "bench" "MYP(ms)" "Python(ms)" "Py/MYP" "verify"
printf "%s\n" "------------------------------------------------------------------"

for name in "${names[@]}"; do
    read -r mv mms < <(best_of "myp" "$name")
    read -r pv pms < <(best_of "py" "$name")
    ratio=$(awk -v p="$pms" -v m="$mms" 'BEGIN{ if (m+0>0) printf "%.1f", p/m; else print "inf" }')
    if verify_same "$mv" "$pv" | grep -q 1; then
        vnote="$mv"
    else
        vnote="$mv != $pv"
    fi
    printf "%-15s %-10s %-12s %-10s %s\n" "$name" "$mms" "$pms" "$ratio" "$vnote"
done

echo ""
echo "注: 比值 Py/MYP > 1 表示 Python 更慢（预期 10-100×，解释型 vs LLVM 编译）。"
echo "    verify 两语言一致才有对比意义；纯 Python 无 numpy。"
