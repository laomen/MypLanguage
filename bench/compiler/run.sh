#!/usr/bin/env bash
# run.sh — 编译器自身性能基准（bench/compiler/）
#
# 测量 mypc 对 P1..P7 规模源码的完整编译时间（外部高精度时钟）+ 分阶段耗时
# （MYP_TIMING=1：load/lexer/parser/imports/sema/eval/codegen）+ 峰值 RSS，
# 并按 N/2N/4N（P1 含 8N）规模检查复杂度斜率。
#
# 用法：
#   bash bench/compiler/run.sh [benchmarks]        # 默认 P1..P7
#   MYPCC=/path/to/mypc bash bench/compiler/run.sh P1 P4
#   ITERS=5 SCALES="500 1000 2000" bash bench/compiler/run.sh P2
#   JSON=bench/compiler/result.json bash bench/compiler/run.sh
#
# 退出码：任一基准任一规模编译失败 → 1；斜率超阈值 → 1（可 JSON=… 时用
# SLOPE_TOL=3 覆盖阈值，默认 2N/4N 耗时比 > 3 判定为超线性警告）。

set -u
cd "$(dirname "$0")"

MYPCC=${MYPCC:-../../build/mypc}
ITERS=${ITERS:-3}
SLOPE_TOL=${SLOPE_TOL:-3.0}
JSON=${JSON:-}
TIME_BIN=${TIME_BIN:-/usr/bin/time}

# ---- 基线回退校验模式 ----
#   bash run.sh --check [baseline.json]   → 跑全量并与基线比对（超 CHECK_TOL 判定回退，exit 1）
CHECK_BASELINE=${CHECK_BASELINE:-}
CHECK_TOL=${CHECK_TOL:-1.3}
if [ "${1:-}" = "--check" ]; then
    CHECK_BASELINE="${CHECK_BASELINE:-${2:-baseline.json}}"
    if [ ! -f "$CHECK_BASELINE" ]; then
        echo "error: 基线文件不存在: $CHECK_BASELINE (先跑一次生成 baseline.json)" >&2
        exit 1
    fi
    shift 2 2>/dev/null || shift
fi

# mypc 用 argv[0] 定位 stdlib —— 必须用绝对路径调用
case "$MYPCC" in
    /*) : ;;
    *) MYPCC="$(cd "$(dirname "$MYPCC")" && pwd)/$(basename "$MYPCC")" ;;
esac

if [ ! -x "$MYPCC" ]; then
    echo "error: mypc 不存在: $MYPCC (先构建，或设 MYPCC)" >&2
    exit 1
fi
if [ ! -x "$TIME_BIN" ]; then
    echo "error: 需要 GNU time ($TIME_BIN) 测峰值 RSS" >&2
    exit 1
fi

BENCHES="${*:-P1 P2 P3 P4 P5 P6 P7}"
[ -n "$CHECK_BASELINE" ] && BENCHES="P1 P2 P3 P4 P5 P6 P7"
WORK=$(mktemp -d /tmp/myp_compiler_bench.XXXXXX)
RESULTS="$WORK/results.tsv"
trap 'rm -rf "$WORK"' EXIT

# ---- 工具函数 ----
median() { # stdin 每行一个数 → 输出中位数（整数）
    sort -n | awk '{a[NR]=$1} END{ if (NR==0) print 0; else if (NR%2) print a[(NR+1)/2]; else print int((a[NR/2]+a[NR/2+1])/2) }'
}

# 单次编译：返回总毫秒，MYP_TIMING 阶段写 stderr 到文件，RSS(KB) 写到 rss 文件
compile_once() { # $1=src $2=out $3=timingfile
    local src="$1" out="$2" tf="$3"
    local start_ms end_ms ms
    start_ms=$(date +%s%N)
    $TIME_BIN -v env MYP_TIMING=1 "$MYPCC" "$src" -o "$out" \
        >/dev/null 2>"$tf"
    end_ms=$(date +%s%N)
    echo $(( (end_ms - start_ms) / 1000000 ))
}

# 从 MYP_TIMING 输出提取某阶段毫秒（行格式: [timing] <phase> <N> ms → $3）
phase_ms() { # $1=timingfile $2=phase名
    awk -v p="$2" '$1=="[timing]" && $2==p {print $3}' "$1" | head -1
}

json_start() { [ -n "$JSON" ] && printf '{\n  "mypc": "%s",\n  "commit": "%s",\n  "runs": %s,\n  "results": [\n' \
    "$MYPCC" "$(git -C ../.. rev-parse --short HEAD 2>/dev/null)" "$ITERS" > "$JSON"; }
json_end()   { [ -n "$JSON" ] && printf '  ]\n}\n' >> "$JSON"; }
json_item()  { # $1=name $2=n $3=total $4=parse+sema+codegen $5=rss $6=first
    if [ -n "$JSON" ]; then
        [ "${6:-0}" = 1 ] && printf '    ' >> "$JSON" || printf '    ,' >> "$JSON"
        printf '{"bench":"%s","n":%s,"total_ms":%s,"frontend_ms":%s,"rss_kb":%s}' \
            "$1" "$2" "$3" "$4" "$5" >> "$JSON"
        printf '\n' >> "$JSON"
    fi
}

printf '%-4s %-8s %-10s %-14s %-10s %-22s\n' 'Bench' 'N' 'total(ms)' 'front(ms)' 'RSS(KB)' 'slope'
json_start
first=1
rc=0
for p in $BENCHES; do
    # 规模档位：P1 用 1000/2000/4000/8000，其余 1000/2000/4000
    case "$p" in
        P1) scales="1000 2000 4000 8000" ;;
        *)  scales="1000 2000 4000" ;;
    esac
    prev_n=0; prev_t=0
    printf -- '-- %s -----------------------------------------------------\n' "$p"
    for n in $scales; do
        src="$WORK/${p}_${n}.myp"
        out="$WORK/${p}_${n}.o"
        if ! python3 gen.py "$p" "$n" > "$src"; then
            echo "  [$p N=$n] gen.py 失败" >&2; rc=1; continue
        fi
        # warmup（首编译也预热文件系统/LLVM 初始化）
        "$MYPCC" "$src" -o "$out" >/dev/null 2>&1
        if [ $? -ne 0 ]; then
            echo "  [$p N=$n] 编译失败" >&2; rc=1
            json_item "$p" "$n" -1 -1 -1 "$first"; first=0
            continue
        fi
        times=""; rsss=""; tf_last=""
        for i in $(seq 1 "$ITERS"); do
            tf="$WORK/${p}_${n}_t$i.txt"
            ms=$(compile_once "$src" "$out" "$tf")
            times="$times
$ms"
            rsss="$rsss
$(awk '/Maximum resident set size/{print $NF}' "$tf" | head -1)"
            tf_last="$tf"
        done
        med=$(printf '%s\n' "$times" | median)
        medrss=$(printf '%s\n' "$rsss" | median)
        # 前端阶段合计：parser + sema + eval + codegen
        fp=$(phase_ms "$tf_last" parser);   [ -z "$fp" ]   && fp=0
        fs=$(phase_ms "$tf_last" sema);     [ -z "$fs" ]   && fs=0
        fe=$(phase_ms "$tf_last" eval);     [ -z "$fe" ]   && fe=0
        fc=$(phase_ms "$tf_last" codegen);  [ -z "$fc" ]   && fc=0
        front=$(( fp + fs + fe + fc ))
        # 斜率：相对上一档（2N/N）
        slope=""
        if [ "$prev_n" -gt 0 ] && [ "$prev_t" -gt 0 ]; then
            ratio=$(awk -v t="$med" -v p="$prev_t" 'BEGIN{ printf "%.2f", t/p }')
            slope="2N/N=$ratio"
            if [ "$(awk -v r="$ratio" -v tol="$SLOPE_TOL" 'BEGIN{ print (r>tol)?1:0 }')" = 1 ]; then
                slope="$slope !超线性"
                echo "  !! [$p N=$n] 耗时比 $ratio > $SLOPE_TOL，疑有超线性路径" >&2
            fi
        fi
        printf '%-4s %-8d %-10s %-14s %-10s %-22s\n' "$p" "$n" "${med}ms" "${front}ms" "$medrss" "$slope"
        json_item "$p" "$n" "$med" "$front" "$medrss" "$first"; first=0
        printf '%s %d %d %d %d\n' "$p" "$n" "$med" "$front" "$medrss" >> "$RESULTS"
        prev_n=$n; prev_t=$med
    done
    prev_n=0; prev_t=0
done
json_end
[ -n "$JSON" ] && echo "JSON 结果: $JSON"

# ---- 基线回退校验 ----
if [ -n "$CHECK_BASELINE" ]; then
    echo
    echo "== 基线回退校验 ($(basename "$CHECK_BASELINE"), 容差 x$CHECK_TOL) =="
    python3 - "$CHECK_BASELINE" "$RESULTS" "$CHECK_TOL" <<'PY'
import json, sys
base = json.load(open(sys.argv[1]))
bmap = {(r["bench"], r["n"]): r for r in base["results"]}
cur = {}
for line in open(sys.argv[2]):
    p = line.split()
    if len(p) == 5:
        cur[(p[0], int(p[1]))] = int(p[2])
tol = float(sys.argv[3])
rows = [(k, bmap[k]["total_ms"], cur[k]) for k in bmap if k in cur and bmap[k]["total_ms"] > 0]
rows.sort()
regress = 0
print("%-5s %6s %9s %9s %7s  %s" % ("bench", "n", "base", "cur", "ratio", "verdict"))
for (b, n), base_ms, cur_ms in rows:
    ratio = cur_ms / base_ms
    bad = ratio > tol
    regress += bad
    print("%-5s %6d %9d %9d %7.2f  %s" % (b, n, base_ms, cur_ms, ratio, "REGRESS" if bad else "ok"))
print("\n回退项: %d / %d (容差 x%.2f)" % (regress, len(rows), tol))
sys.exit(1 if regress else 0)
PY
    [ $? -ne 0 ] && rc=1
fi

exit $rc
