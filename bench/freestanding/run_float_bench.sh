#!/usr/bin/env bash
# run_float_bench.sh — 浮点层性能基准复现（rt_float_bench.myp）。
# C runtime（libc strtod/snprintf）vs MYP shadow（runtime_myp/float.myp）对比；
# psum/fsum 两模式应一致 → 位精确等价。
# 用法：bash bench/freestanding/run_float_bench.sh [myp_self2 路径]
set -euo pipefail
cd "$(dirname "$0")/../.."
SELF="${1:-./build/myp_self2}"
LLC="${LLC:-llc-21}"
CRT=/usr/lib/x86_64-linux-gnu
GCCD="$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/libgcc.a | sort | tail -1 | xargs dirname)"
DL=/lib64/ld-linux-x86-64.so.2

RT_OBJS=""
for m in runtime_myp/*.myp; do
    base="$(basename "${m%.myp}")"
    "$SELF" "$m" --emit-llvm --shared -o "/tmp/rt_myp_${base}" >/dev/null 2>&1
    "$LLC" "/tmp/rt_myp_${base}.ll" -filetype=obj -relocation-model=pic -o "/tmp/rt_myp_${base}.o"
    RT_OBJS="$RT_OBJS /tmp/rt_myp_${base}.o"
done

"$SELF" bench/freestanding/rt_float_bench.myp --emit-llvm -o /tmp/rfb >/dev/null 2>&1
"$LLC" /tmp/rfb.ll -filetype=obj -relocation-model=pic -o /tmp/rfb.o

ld_runtime() {   # $1=obj $2=out $3=extra-flags $4=extra-objs
    /usr/bin/ld.lld-21 ${3:-} -pie --dynamic-linker "$DL" -o "$2" \
        "$CRT"/Scrt1.o "$CRT"/crti.o "$1" ${4:-} build/libmyp_rt.a \
        -L"$GCCD" -L"$CRT" -lgcc -lgcc_s -lc -lm -lpthread -ldl -lgcc -lgcc_s \
        "$CRT"/crtn.o --gc-sections
}

ld_runtime /tmp/rfb.o /tmp/rfb_c_bin "" ""
ld_runtime /tmp/rfb.o /tmp/rfb_s_bin "--allow-multiple-definition" "$RT_OBJS"

echo "=== C runtime (libc strtod/snprintf) ==="
/tmp/rfb_c_bin
echo "=== MYP shadow (runtime_myp/float.myp) ==="
/tmp/rfb_s_bin
echo "（psum/fsum 两模式一致 → 位精确等价）"
