#!/usr/bin/env bash
# runtime_myp 构建+验证 —— MYP 运行时 myp化 里程碑（自举编译器）。
# 编译 runtime_myp/*.myp（MYP 实现 runtime 符号）→ 链接时置于 libmyp_rt.a 之前
# + --allow-multiple-definition → MYP 定义 shadow C runtime 版本。
# 前置：./build/myp_self 已重建（含 raw-memory 内建 + preamble declare 剔除）。
set -euo pipefail
cd "$(dirname "$0")/.."
SELF="${1:-./build/myp_self}"
LLC="${LLC:-llc-21}"
OUT="${OUT:-/tmp/rt_myp_out}"
CRT=/usr/lib/x86_64-linux-gnu
GCCD="$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/libgcc.a 2>/dev/null | sort | tail -1 | xargs dirname)"
DL=/lib64/ld-linux-x86-64.so.2

rm -f "$OUT" "$OUT".o
RT_OBJS=""
for m in runtime_myp/*.myp; do
    base="${m%.myp}"
    "$SELF" "$m" --emit-llvm -o /tmp/rt_myp_m >/dev/null 2>&1
    "$LLC" /tmp/rt_myp_m.ll -filetype=obj -relocation-model=pic -o /tmp/rt_myp_m.o
    RT_OBJS="$RT_OBJS /tmp/rt_myp_m.o"
done

"$SELF" bench/freestanding/rt_str_test.myp --emit-llvm -o /tmp/rt_myp_t >/dev/null 2>&1
"$LLC" /tmp/rt_myp_t.ll -filetype=obj -relocation-model=pic -o /tmp/rt_myp_t.o

/usr/bin/ld.lld-21 --allow-multiple-definition -pie --dynamic-linker "$DL" -o "$OUT" \
    "$CRT"/Scrt1.o "$CRT"/crti.o /tmp/rt_myp_t.o $RT_OBJS build/libmyp_rt.a \
    -L"$GCCD" -L"$CRT" -lgcc -lgcc_s -lc -lm -lpthread -ldl -lgcc -lgcc_s \
    "$CRT"/crtn.o --gc-sections

echo "== 运行（MYP 运行时 shadow C 版本）=="
"$OUT"
code=$?
echo "exit=$code"
[ "$code" = 0 ] || { echo "FAIL: rt_str_test 期望 0"; exit 1; }
echo "PASS: MYP 运行时字符串层 shadow 验证通过"
