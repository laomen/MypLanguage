#!/usr/bin/env bash
# runtime_myp 构建+验证 —— MYP 运行时 myp化 里程碑（自举编译器）。
# 编译 runtime_myp/*.myp（MYP 实现 runtime 符号）→ 链接时置于 libmyp_rt.a 之前
# + --allow-multiple-definition → MYP 定义 shadow C runtime 版本。
# 前置：./build/myp_self 已重建（含 raw-memory 内建 + preamble declare 剔除）。
set -euo pipefail
cd "$(dirname "$0")/.."
# 自举链：oracle 种子只编译 selfhost 源码；runtime_myp 模块由不动点 selfhost
# 编译器（myp_self/myp_self2）编译（全特性）；$1 可覆盖。
SELF="${1:-./build/myp_self}"
LLC="${LLC:-llc-21}"
OUT="${OUT:-/tmp/rt_myp_out}"
CRT=/usr/lib/x86_64-linux-gnu
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

# 归档（OPT-IN，MYP_MAKE_ARCHIVE=1 才产出 build/libmyp_rt_myp.a）：
# de-gcc 关键——selfhost 链接程序时经 mypRtLib() 在 <编译器二进制旁>/build/ 找到
# 它 → 跳过 MYP 化 bridge 的 gcc 编译 + 置于 libmyp_rt.a 前 + --allow-multiple-
# definition（MYP 定义优先）。
# ⚠️ 默认关闭：MYP 运行时（coro.myp 栈池 / io / struct_arc 等）在全量 323 套件
# 的某些模式（如 1500 协程驱动）下有真实 bug，归档被 selfhost 自动拾取会导致
# selfhost 全量变红（见 MIGRATION_STATUS 收尾结论）。缺省保持 C 运行时；显式
# MYP_MAKE_ARCHIVE=1 产出归档后，用 MYP_RT_MYP=<path> 显式验证 de-gcc。
if [ "${MYP_MAKE_ARCHIVE:-0}" = "1" ]; then
    rm -f build/libmyp_rt_myp.a
    ar rcs build/libmyp_rt_myp.a $RT_OBJS
    echo "== 归档 build/libmyp_rt_myp.a （$(echo $RT_OBJS | wc -w) 个模块对象）=="
else
    rm -f build/libmyp_rt_myp.a
    echo "== 跳过归档产出（MYP_MAKE_ARCHIVE=1 启用；MYP 运行时 bug 未清前勿自动分发）=="
fi

# 每个 shadow 验证程序单独链接+运行（都有各自 main()）
for t in bench/freestanding/rt_str_test.myp bench/freestanding/rt_num_test.myp \
         bench/freestanding/rt_alloc_test.myp bench/freestanding/rt_region_test.myp \
         bench/freestanding/rt_weak_test.myp bench/freestanding/rt_io_test.myp \
         bench/freestanding/rt_float_test.myp bench/freestanding/rt_float_prec_test.myp \
         bench/freestanding/rt_time_test.myp bench/freestanding/rt_cls_release_test.myp bench/freestanding/rt_ctx_probe.myp \
         bench/freestanding/rt_asm_test.myp bench/freestanding/rt_term_test.myp \
         bench/freestanding/rt_fs_test.myp bench/freestanding/rt_args_test.myp \
         bench/freestanding/rt_env_test.myp bench/freestanding/rt_math_test.myp \
         bench/freestanding/rt_indirect_test.myp bench/freestanding/rt_pkgA_test.myp \
         bench/freestanding/rt_pkgA2_test.myp bench/freestanding/rt_pkgA_fail_test.myp \
         bench/freestanding/rt_exception_test.myp bench/freestanding/rt_thread_test.myp \
         bench/freestanding/rt_diag_test.myp bench/freestanding/rt_coro_test.myp \
         bench/freestanding/rt_coro_wait_test.myp bench/freestanding/rt_coro_chan_future_test.myp \
         bench/freestanding/rt_sync_test.myp bench/freestanding/rt_pool_test.myp \
         bench/freestanding/rt_threadpool_test.myp bench/freestanding/rt_evname_test.myp \
         bench/freestanding/rt_thin_test.myp bench/freestanding/rt_io_thread_test.myp \
         bench/freestanding/rt_gpu_test.myp bench/freestanding/rt_rtti_test.myp \
         bench/freestanding/rt_bounds_fail_test.myp \
         bench/freestanding/rt_hash_test.myp bench/freestanding/rt_date_test.myp \
         bench/freestanding/rt_regex_test.myp bench/freestanding/rt_json_test.myp \
         bench/freestanding/rt_process_test.myp bench/freestanding/rt_uds_test.myp \
         bench/freestanding/rt_net_test.myp; do
    tb="$(basename "$t" .myp)"
    "$SELF" "$t" --emit-llvm -o "/tmp/rt_${tb}" >/dev/null 2>&1
    "$LLC" "/tmp/rt_${tb}.ll" -filetype=obj -relocation-model=pic -o "/tmp/rt_${tb}.o"

    /usr/bin/ld.lld-21 --allow-multiple-definition -pie --dynamic-linker "$DL" -o "/tmp/rt_${tb}_bin" \
        "$CRT"/Scrt1.o "$CRT"/crti.o "/tmp/rt_${tb}.o" $RT_OBJS build/libmyp_rt.a \
        -L"$CRT" -lc -lm -lpthread -ldl \
        "$CRT"/crtn.o --gc-sections

    echo "== 运行（MYP 运行时 shadow C 版本）: ${tb} =="
    # 二进制可能返回非 0（断言失败）——set -e 下须先关闭再取退出码（否则脚本
    # 在运行处静默退出，看不到 exit=N 与 FAIL 定位）。
    expected=0
    set +e
    case "$tb" in
        rt_args_test)         "/tmp/rt_${tb}_bin" alpha beta gamma ;;
        rt_thin_test)         printf 'hello thin\n' | "/tmp/rt_${tb}_bin" ;;   # stdin 管道喂 readLine
        rt_pkgA_fail_test)    expected=1; "/tmp/rt_${tb}_bin" ;;   # 故意失败 → 期望 exit 1
        rt_bounds_fail_test)  expected=134; "/tmp/rt_${tb}_bin" ;;  # 越界 → abort(134)
        rt_date_test)         TZ=UTC "/tmp/rt_${tb}_bin" ;;         # 定 TZ → epoch 断言确定
        *)                    "/tmp/rt_${tb}_bin" ;;
    esac
    code=$?
    set -e
    echo "exit=$code (期望 $expected)"
    [ "$code" = "$expected" ] || { echo "FAIL: ${tb} 期望 $expected"; exit 1; }
done
echo "PASS: MYP 运行时 shadow 验证通过（str + num + fs + env + args + term + math）"
