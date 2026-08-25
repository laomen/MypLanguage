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
    base="$(basename "${m%.myp}")"
    # --shared（库模式）：函数外部链接（define 而非 define internal）——否则符号是
    # 局部（t），无法 shadow libmyp_rt.a 里的同名全局符号（此前机制失效，测试实际
    # 走的 C runtime）。
    # 每个模块用独立 /tmp/rt_myp_<base>.o（共用同一路径会被后编译的模块覆盖）。
    "$SELF" "$m" --emit-llvm --shared -o "/tmp/rt_myp_${base}" >/dev/null 2>&1
    "$LLC" "/tmp/rt_myp_${base}.ll" -filetype=obj -relocation-model=pic -o "/tmp/rt_myp_${base}.o"
    RT_OBJS="$RT_OBJS /tmp/rt_myp_${base}.o"
done

# 每个 shadow 验证程序单独链接+运行（都有各自 main()）
for t in bench/freestanding/rt_str_test.myp bench/freestanding/rt_num_test.myp \
         bench/freestanding/rt_alloc_test.myp bench/freestanding/rt_region_test.myp \
         bench/freestanding/rt_weak_test.myp bench/freestanding/rt_io_test.myp \
         bench/freestanding/rt_float_test.myp bench/freestanding/rt_float_prec_test.myp \
         bench/freestanding/rt_time_test.myp bench/freestanding/rt_cls_release_test.myp bench/freestanding/rt_ctx_probe.myp \
         bench/freestanding/rt_asm_test.myp bench/freestanding/rt_term_test.myp \
         bench/freestanding/rt_fs_test.myp bench/freestanding/rt_args_test.myp \
         bench/freestanding/rt_env_test.myp bench/freestanding/rt_math_test.myp \
         bench/freestanding/rt_indirect_test.myp bench/freestanding/rt_pkgA_test.myp; do
    tb="$(basename "$t" .myp)"
    "$SELF" "$t" --emit-llvm -o "/tmp/rt_${tb}" >/dev/null 2>&1
    "$LLC" "/tmp/rt_${tb}.ll" -filetype=obj -relocation-model=pic -o "/tmp/rt_${tb}.o"

    /usr/bin/ld.lld-21 --allow-multiple-definition -pie --dynamic-linker "$DL" -o "/tmp/rt_${tb}_bin" \
        "$CRT"/Scrt1.o "$CRT"/crti.o "/tmp/rt_${tb}.o" $RT_OBJS build/libmyp_rt.a \
        -L"$GCCD" -L"$CRT" -lgcc -lgcc_s -lc -lm -lpthread -ldl -lgcc -lgcc_s \
        "$CRT"/crtn.o --gc-sections

    echo "== 运行（MYP 运行时 shadow C 版本）: ${tb} =="
    # 二进制可能返回非 0（断言失败）——set -e 下须先关闭再取退出码（否则脚本
    # 在运行处静默退出，看不到 exit=N 与 FAIL 定位）。
    set +e
    case "$tb" in
        rt_args_test) "/tmp/rt_${tb}_bin" alpha beta gamma ;;
        *)            "/tmp/rt_${tb}_bin" ;;
    esac
    code=$?
    set -e
    echo "exit=$code"
    [ "$code" = 0 ] || { echo "FAIL: ${tb} 期望 0"; exit 1; }
done
echo "PASS: MYP 运行时 shadow 验证通过（str + num + fs + env + args + term + math）"
