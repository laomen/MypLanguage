#!/bin/bash
# MYP Language 回归测试框架
# 用法: ./tests/run_tests.sh [--rebuild] [--update]
#
# 目录结构:
#   tests/*/test.myp          — 验证测试（编译+运行，比对 expected 输出）
#   tests/expected/*.expected — 预期的标准输出
#   tests/negative/*.myp      — 负测试（编译应失败）
#   tests/fuzz_test.py        — 模糊测试
#
# 退出码: 0=全部通过, 1=有失败

set -o pipefail

# Compiler binary — override with MYPCC=/path/to/mypc (e.g. the ASan build)
MYPCC="${MYPCC:-./build/mypc}"
TIMEOUT_SEC=10
PASS=0
FAIL=0
FAILED_TESTS=""

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# 检测是否要重建编译器
if [ "${1:-}" = "--rebuild" ]; then
    echo -e "${YELLOW}[BUILD] Rebuilding compiler...${NC}"
    cd "$(dirname "$0")/.."
    cmake --build build 2>&1 | tail -3
    if [ $? -ne 0 ]; then
        echo -e "${RED}[BUILD] Compiler build failed!${NC}"
        exit 1
    fi
fi

UPDATE_MODE=false
if [ "${1:-}" = "--update" ] || [ "${2:-}" = "--update" ]; then
    UPDATE_MODE=true
    echo -e "${YELLOW}[UPDATE] Updating expected outputs...${NC}"
fi

cd "$(dirname "$0")/.."
PROJ_ROOT=$(pwd)

# 跨平台移植层（Linux 原样 / Windows Git Bash 提供 timeout、ulimit 等价物）
# 必须在 PROJ_ROOT 确定后、任何使用 myp_timeout/myp_resolve_bin 之前 source。
source "$PROJ_ROOT/tests/lib/portable.sh"

# T5（2026-09-07）：成功测试后清理产物，避免源码树残留 test.output/.out/.ll/.o。
# 失败路径不调用 → 保留 test.output 供 diff 诊断；--update 已把输出存入 expected。
cleanup_artifacts() {
    local base=$1   # 二进制基名（去扩展名），如 tests/time/test 或 tests/@test/xxx
    rm -f "$base.output" "$base.out" "$base.out.exe" "$base.out.ll" \
          "$base.out.opt.ll" "$base.out.o" "$base.myp.o" "$base.myp.ll" \
          "$base.myp.opt.ll" "$base.o" "$base.ll" 2>/dev/null
}

echo ""
echo "=========================================="
echo "  MYP Language Test Suite"
echo "  $(date)"
echo "=========================================="
echo ""

# =============================================
# 第1部分: 回归测试 — 编译+运行+比对输出
# =============================================
echo "--- [1/10] 回归测试 (Regression Tests) ---"
echo ""

for test_dir in tests/*/; do
    name=$(basename "$test_dir")
    # 跳过 expected/ 和 negative/ 目录
    [ "$name" = "expected" ] && continue
    [ "$name" = "negative" ] && continue

    test_file="${test_dir}test.myp"
    expected_file="tests/expected/${name}.expected"
    output_file="${test_dir}test.output"
    binary_file="${test_dir}test.out"
    # Windows 下若产出 .exe 变体则用后者（mypc 现按 -o 精确命名，仅作前向兼容）
    binary_file=$(myp_resolve_bin "$binary_file")

    [ ! -f "$test_file" ] && continue

    printf "  %-25s " "$name"

    # 编译
    compile_output=$($MYPCC "$test_file" 2>&1)
    if [ $? -ne 0 ]; then
        echo -e "${RED}COMPILE FAIL${NC}"
        echo "$compile_output" | head -5
        FAIL=$((FAIL + 1))
        FAILED_TESTS="$FAILED_TESTS $name(compile)"
        continue
    fi

    # 运行 —— 直接重定向到文件（字节级精确捕获）。此前用 $(...) 捕获会剥离
    # 程序输出的尾部换行、echo 再补一个，导致 expected 非字节精确（§二-1）。
    myp_timeout $TIMEOUT_SEC "$binary_file" > "$output_file" 2>&1
    if [ $? -ne 0 ]; then
        echo -e "${RED}RUNTIME FAIL${NC}"
        head -5 "$output_file"
        FAIL=$((FAIL + 1))
        FAILED_TESTS="$FAILED_TESTS $name(runtime)"
        continue
    fi

    # 比对 expected
    if [ -f "$expected_file" ]; then
        # ASan prints known warnings for ucontext coroutines that are NOT part
        # of the program output — filter them all (makecontext/swapcontext
        # limitation + the __asan_handle_no_return / longjmp follow-ups, which
        # appear when a coroutine raises an exception across ucontext stacks).
        # (Use a temp file: diff on process-substitution FIFOs is unreliable.)
        tmp_filtered=$(mktemp)
        grep -v -E \
            "ASan doesn't fully support makecontext|ASan is ignoring requested __asan_handle_no_return|False positive error reports may follow|For details see https://github.com/google/sanitizers/issues/189" \
            "$output_file" > "$tmp_filtered" 2>/dev/null || true
        if diff -q "$tmp_filtered" "$expected_file" > /dev/null 2>&1; then
            rm -f "$tmp_filtered"
            echo -e "${GREEN}PASS${NC}"
            PASS=$((PASS + 1))
            cleanup_artifacts "${test_dir}test"
        else
            rm -f "$tmp_filtered"
            if $UPDATE_MODE; then
                cp "$output_file" "$expected_file"
                echo -e "${YELLOW}UPDATED${NC}"
                PASS=$((PASS + 1))
                cleanup_artifacts "${test_dir}test"
            else
                echo -e "${RED}MISMATCH${NC}"
                diff "$output_file" "$expected_file" | head -10
                FAIL=$((FAIL + 1))
                FAILED_TESTS="$FAILED_TESTS $name(output)"
            fi
        fi
    else
        # 缺失 expected：仅 --update 允许创建 baseline；默认模式必须失败
        #（漏提交 expected 时 CI 不能把错误输出静默当成新基线）。
        if $UPDATE_MODE; then
            cp "$output_file" "$expected_file"
            echo -e "${YELLOW}UPDATED${NC} (created expected)"
            PASS=$((PASS + 1))
            cleanup_artifacts "${test_dir}test"
        else
            echo -e "${RED}MISSING BASELINE${NC} (no expected file; run --update to bless)"
            FAIL=$((FAIL + 1))
            FAILED_TESTS="$FAILED_TESTS $name(missing-expected)"
        fi
    fi
done

# =============================================
# 第2部分: 负测试 — 编译应该失败
# =============================================
echo ""
echo "--- [2/10] 负测试 (Negative Tests) ---"
echo ""

NEG_PASS=0
NEG_FAIL=0

for test_file in tests/negative/*.myp; do
    [ ! -f "$test_file" ] && continue
    name=$(basename "$test_file" .myp)

    # 机器断言期望诊断（可选）：`// EXPECT ERROR: <substring>`。固定字符串匹配
    # （-F），诊断措辞调整不要求字节级一致；无注释则保持旧行为（仅判非零）。
    expect_substr=$(grep -m1 -E '//[[:space:]]*EXPECT ERROR:' "$test_file" | sed -E 's|.*EXPECT ERROR:[[:space:]]*||' | tr -d '\r')

    printf "  %-25s " "$name"

    # 内存限 + 超时（编译压测惯例）：非法输入曾 OOM 崩溃系统（枚举/ match 错误恢复
    # 死循环），负测试须在受限环境跑——挂死/超限即 CRASH 而非干净拒绝。
    compile_output=$( ( ulimit -v 1048576; myp_timeout $TIMEOUT_SEC "$MYPCC" "$test_file" ) 2>&1)
    rc=$?
    if [ $rc -ne 0 ]; then
        # 崩溃（段错误/abort/ASan）不是"干净拒绝"，必须判失败（T2：意外
        # SIGSEGV/SIGABRT/ASan 报告归类为 CRASH 而非负测试通过）。
        if echo "$compile_output" | grep -qE "AddressSanitizer|Segmentation fault|SIGSEGV|SIGABRT|core dumped"; then
            echo -e "${RED}CRASH${NC} (compiler crashed, not a clean reject)"
            NEG_FAIL=$((NEG_FAIL + 1))
            FAILED_TESTS="$FAILED_TESTS $name(negative-crash)"
        elif [ -n "$expect_substr" ] && ! echo "$compile_output" | grep -qF "$expect_substr"; then
            echo -e "${RED}FAIL${NC} (rejected, but missing '${expect_substr}')"
            NEG_FAIL=$((NEG_FAIL + 1))
            FAILED_TESTS="$FAILED_TESTS $name(negative-reason)"
        else
            echo -e "${GREEN}PASS${NC} (correctly rejected)"
            NEG_PASS=$((NEG_PASS + 1))
        fi
    else
        echo -e "${RED}FAIL${NC} (should have been rejected)"
        NEG_FAIL=$((NEG_FAIL + 1))
        FAILED_TESTS="$FAILED_TESTS $name(negative)"
    fi
done

if [ $NEG_PASS -eq 0 ] && [ $NEG_FAIL -eq 0 ]; then
    echo "  (no negative tests found)"
fi

# =============================================
# 第3部分: 测试框架测试 (@test + --test)
# =============================================
echo ""
echo "--- [3/10] 测试框架 (Test Framework) ---"
echo ""

TFPASS=0
TFFAIL=0

test_file="tests/test_example.myp"
if [ -f "$test_file" ]; then
    name=$(basename "$test_file" .myp)
    printf "  %-25s " "$name"

    compile_output=$($MYPCC --test "$test_file" 2>&1)
    if [ $? -ne 0 ]; then
        echo -e "${RED}COMPILE FAIL${NC}"
        echo "$compile_output" | head -5
        TFFAIL=$((TFFAIL + 1))
        FAILED_TESTS="$FAILED_TESTS $name(test-compile)"
    else
        binary_file="${test_file%.myp}.out"
        binary_file=$(myp_resolve_bin "$binary_file")
        if [ ! -f "$binary_file" ]; then
            binary_file="./a.out"
        fi
        run_output=$(myp_timeout $TIMEOUT_SEC "$binary_file" 2>&1)
        if [ $? -ne 0 ]; then
            echo -e "${RED}RUNTIME FAIL${NC}"
            echo "$run_output" | head -5
            TFFAIL=$((TFFAIL + 1))
            FAILED_TESTS="$FAILED_TESTS $name(test-runtime)"
        elif echo "$run_output" | grep -q "FAIL"; then
            echo -e "${RED}TEST FAIL${NC}"
            echo "$run_output"
            TFFAIL=$((TFFAIL + 1))
            FAILED_TESTS="$FAILED_TESTS $name(test-fail)"
        else
            echo -e "${GREEN}PASS${NC}"
            TFPASS=$((TFPASS + 1))
            cleanup_artifacts "${test_file%.myp}"
        fi
    fi
else
    echo "  (no test framework files found)"
fi

# @test 目录测试：tests/@test/*.myp —— 语言内建测试套件的正式用例，
# 自动发现 + 逐个 --test 编译运行。每个用例必须 exit 0 且输出无 "FAIL:"。
# 以后的新测试用例均用此套件编写并放入该目录。
ATEST_DIR="$PROJ_ROOT/tests/@test"
if [ -d "$ATEST_DIR" ]; then
    for tf in "$ATEST_DIR"/*.myp; do
        [ -f "$tf" ] || continue
        tname=$(basename "$tf" .myp)
        printf "  @test %-20s " "$tname"
        compile_output=$($MYPCC --test "$tf" 2>&1)
        if [ $? -ne 0 ]; then
            echo -e "${RED}COMPILE FAIL${NC}"
            echo "$compile_output" | head -5
            TFFAIL=$((TFFAIL + 1))
            FAILED_TESTS="$FAILED_TESTS @test/$tname(compile)"
            continue
        fi
        tbin="${tf%.myp}.out"
        tbin=$(myp_resolve_bin "$tbin")
        run_output=$(myp_timeout $TIMEOUT_SEC "$tbin" 2>&1)
        if [ $? -ne 0 ]; then
            echo -e "${RED}RUNTIME FAIL${NC}"
            echo "$run_output" | head -5
            TFFAIL=$((TFFAIL + 1))
            FAILED_TESTS="$FAILED_TESTS @test/$tname(runtime)"
        elif echo "$run_output" | grep -q "FAIL:"; then
            echo -e "${RED}TEST FAIL${NC}"
            echo "$run_output" | head -8
            TFFAIL=$((TFFAIL + 1))
            FAILED_TESTS="$FAILED_TESTS @test/$tname(test-fail)"
        else
            echo -e "${GREEN}PASS${NC}"
            TFPASS=$((TFPASS + 1))
            cleanup_artifacts "${tf%.myp}"
        fi
    done
fi

# 语言内建测试套件（@test + --test）专项：正常套件 exit 0、失败套件 exit 1、
# 汇总行、Test.fail 消息、扩展断言 API（long/float/空引用）。
if [ -f "$PROJ_ROOT/tests/test_myp_test.sh" ]; then
    tf_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/test_myp_test.sh" 2>&1)
    if echo "$tf_out" | grep -qE "myp-test PASS=[0-9]+ FAIL=0"; then
        echo -e "${GREEN}PASS${NC} (@test 运行器：退出码反映失败 + 汇总 + 扩展断言)"
        TFPASS=$((TFPASS + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "$tf_out" | tail -15
        TFFAIL=$((TFFAIL + 1))
        FAILED_TESTS="$FAILED_TESTS myp_test(@test-framework)"
    fi
fi

# 多文件编译专项（manual.md §12 编译器章节）：多文件合并为单模块、第二文件的
# import/struct/enum 可见（BUG-025）、多文件 + --test + 用户 main（BUG-026）。
if [ -f "$PROJ_ROOT/tests/test_multifile.sh" ]; then
    mf_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/test_multifile.sh" 2>&1)
    if echo "$mf_out" | grep -qE "multifile: [0-9]+ passed, 0 failed"; then
        echo -e "${GREEN}PASS${NC} (多文件编译：import/struct/enum 合并 + --test)"
        TFPASS=$((TFPASS + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "$mf_out" | grep -E "FAIL:|multifile:" | tail -10
        TFFAIL=$((TFFAIL + 1))
        FAILED_TESTS="$FAILED_TESTS multifile(compile)"
    fi
fi

# 逃逸分析函数级栈预算：0 禁用、中间预算累计扣减、默认预算允许小对象。
if [ -f "$PROJ_ROOT/tests/test_stack_promotion_budget.sh" ]; then
    spb_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/test_stack_promotion_budget.sh" 2>&1)
    if echo "$spb_out" | grep -qE "stack-promotion-budget: [0-9]+ passed, 0 failed"; then
        echo -e "${GREEN}PASS${NC} (逃逸分析：函数级累计栈预算)"
        TFPASS=$((TFPASS + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "$spb_out" | tail -10
        TFFAIL=$((TFFAIL + 1))
        FAILED_TESTS="$FAILED_TESTS stack_promotion_budget(escape-analysis)"
    fi
fi

# 叶类无需空析构桩；含 ARC/weak 字段的类仍须保留级联析构。
if [ -f "$PROJ_ROOT/tests/test_leaf_destroy_dispatch.sh" ]; then
    leaf_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/test_leaf_destroy_dispatch.sh" 2>&1)
    if echo "$leaf_out" | grep -q "PASS (leaf class direct destroy dispatch)"; then
        echo -e "${GREEN}PASS${NC} (叶类直接析构分发)"
        TFPASS=$((TFPASS + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "$leaf_out" | tail -10
        TFFAIL=$((TFFAIL + 1))
        FAILED_TESTS="$FAILED_TESTS leaf_destroy_dispatch(codegen)"
    fi
fi

# 纯 MYP runtime 符号闭包：线程/通道程序不得因归档缺符号静默回退 C runtime。
if [ -f "$PROJ_ROOT/tests/test_runtime_myp_only.sh" ]; then
    rt_only_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/test_runtime_myp_only.sh" 2>&1)
    if echo "$rt_only_out" | grep -q "PASS (threaded program uses MYP runtime only)"; then
        echo -e "${GREEN}PASS${NC} (线程程序仅链接 MYP runtime)"
        TFPASS=$((TFPASS + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "$rt_only_out" | tail -10
        TFFAIL=$((TFFAIL + 1))
        FAILED_TESTS="$FAILED_TESTS runtime_myp_only(link)"
    fi
fi

# package-path 冒号分隔（BUG-015）：mypc --package-path "a:b" 按 ':' 切分逐路径
# 查找——包在 dirB 时编译成功（修复前把整串当单一目录 → cannot find import）。
if [ -f "$PROJ_ROOT/tests/test_package_path.sh" ]; then
    pp_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/test_package_path.sh" 2>&1)
    if echo "$pp_out" | grep -qE "package-path: [0-9]+ pass, 0 fail"; then
        echo -e "${GREEN}PASS${NC} (package-path 冒号分隔：BUG-015)"
        TFPASS=$((TFPASS + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "$pp_out" | grep -E "FAIL:|package-path:" | tail -10
        TFFAIL=$((TFFAIL + 1))
        FAILED_TESTS="$FAILED_TESTS package-path(BUG-015)"
    fi
fi

# 闭源分发回归（bridge 预编译库 .so/.a）：MYP_BRIDGES 目录放预编译库 +
# ffi 封装 .myp，mypc 按符号自动链接，.c 源码不进入分发（2026-08-20）。
if [ -f "$PROJ_ROOT/tests/test_closed_lib.sh" ]; then
    cl_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/test_closed_lib.sh" 2>&1)
    if echo "$cl_out" | grep -qE "closed-lib: [0-9]+ pass, 0 fail"; then
        echo -e "${GREEN}PASS${NC} (闭源分发：bridge 预编译 .so/.a + FFI)"
        TFPASS=$((TFPASS + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "$cl_out" | grep -E "FAIL:|closed-lib:" | tail -10
        TFFAIL=$((TFFAIL + 1))
        FAILED_TESTS="$FAILED_TESTS closed-lib"
    fi
fi

# 代码生成工具（tools/codegen，manual §13）：schema 驱动生成器 serde/ffi/
# autodiff/idl/orm/embed/dsl/infer_ops —— 生成 → 编译 → round-trip 验证（BUG-027）。
if [ -f "$PROJ_ROOT/tools/codegen/run_tests.sh" ]; then
    cg_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tools/codegen/run_tests.sh" 2>&1)
    if echo "$cg_out" | grep -qE "codegen 自测通过"; then
        echo -e "${GREEN}PASS${NC} (代码生成工具：serde/ffi/autodiff/idl/orm/embed/dsl/infer_ops)"
        TFPASS=$((TFPASS + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "$cg_out" | grep -E "FAIL:|自测" | tail -10
        TFFAIL=$((TFFAIL + 1))
        FAILED_TESTS="$FAILED_TESTS codegen(tools)"
    fi
fi

# =============================================
# 协程栈警告测试：@coro(stack=N) N<16 编译期警告（design.md §8.6.2 栈行）
# =============================================
echo ""
echo "--- [3.5] 协程栈警告 (@coro(stack=N) N<16 编译期警告) ---"
echo ""
CORO_PASS=0
CORO_FAIL=0
if [ -f "$PROJ_ROOT/tests/test_coro_stack_warn.sh" ]; then
    coro_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/test_coro_stack_warn.sh" 2>&1)
    if echo "$coro_out" | grep -qE "coro stack warn: [0-9]+ passed, 0 failed"; then
        echo -e "${GREEN}PASS${NC} (@coro(stack) 8/15 警告, 16/0/省略 无警告)"
        CORO_PASS=1
    else
        echo -e "${RED}FAIL${NC}"
        echo "$coro_out" | tail -10
        CORO_FAIL=1
        FAILED_TESTS="$FAILED_TESTS coro_stack_warn(compile-warning)"
    fi
else
    echo "  (no test_coro_stack_warn.sh found)"
fi

# =============================================
# 第4部分: 无崩溃回归 (compiler must never crash)
# =============================================
echo ""
echo "--- [4/10] 无崩溃回归 (No-Crash Regression) ---"
echo ""

NCRASH_PASS=0
NCRASH_FAIL=0
if [ -f "$PROJ_ROOT/tests/regression_no_crash.sh" ]; then
    no_crash_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/regression_no_crash.sh" 2>&1)
    if echo "$no_crash_out" | grep -q "全部通过"; then
        echo -e "${GREEN}PASS${NC} (compiler rejects invalid input without crashing)"
        NCRASH_PASS=1
    else
        echo -e "${RED}FAIL${NC} — compiler crashed on invalid input!"
        echo "$no_crash_out" | tail -10
        NCRASH_FAIL=1
        FAILED_TESTS="$FAILED_TESTS no_crash(regression)"
    fi
else
    echo "  (no regression_no_crash.sh found)"
fi

# =============================================
# 第5部分: MYP 包管理器自举测试
# =============================================
echo ""
echo "--- [5/10] MYP 包管理器自举测试 (Self-hosted pkg manager) ---"
echo ""
PM_PASS=0
PM_FAIL=0
if [ -f "$PROJ_ROOT/tests/test_myp_pm.sh" ]; then
    pm_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/test_myp_pm.sh" 2>&1)
    if echo "$pm_out" | grep -qE "myp-pm PASS=[0-9]+ FAIL=0"; then
        echo -e "${GREEN}PASS${NC} (tools/pm/main.myp 自举包管理器)"
        PM_PASS=1
    else
        echo -e "${RED}FAIL${NC}"
        echo "$pm_out" | tail -15
        PM_FAIL=1
        FAILED_TESTS="$FAILED_TESTS myp_pm(self-hosted)"
    fi
else
    echo "  (no test_myp_pm.sh found)"
fi

# 远程 registry 端到端（默认 file:// 离线；MYP_GITEE_REGISTRY 可切真实 Gitee）
if [ -f "$PROJ_ROOT/tests/test_myp_gitee.sh" ]; then
    gitee_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/test_myp_gitee.sh" 2>&1)
    if echo "$gitee_out" | grep -qE "myp-gitee PASS=[0-9]+ FAIL=0"; then
        echo -e "${GREEN}PASS${NC} (远程 registry：add/list/build自动安装/update/remove)"
        PM_PASS=$((PM_PASS + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "$gitee_out" | tail -15
        PM_FAIL=$((PM_FAIL + 1))
        FAILED_TESTS="$FAILED_TESTS myp_gitee(registry)"
    fi
else
    echo "  (no test_myp_gitee.sh found)"
fi

# =============================================
# 第6部分: MYP 自举格式化器测试
# =============================================
echo ""
echo "--- [6/10] MYP 自举格式化器测试 (Self-hosted formatter) ---"
echo ""
FMT_PASS=0
FMT_FAIL=0
if [ -f "$PROJ_ROOT/tests/test_myp_fmt.sh" ]; then
    fmt_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/test_myp_fmt.sh" 2>&1)
    if echo "$fmt_out" | grep -qE "myp-fmt PASS=[0-9]+ FAIL=0"; then
        echo -e "${GREEN}PASS${NC} (tools/fmt/main.myp 自举格式化器)"
        FMT_PASS=1
    else
        echo -e "${RED}FAIL${NC}"
        echo "$fmt_out" | tail -15
        FMT_FAIL=1
        FAILED_TESTS="$FAILED_TESTS myp_fmt(self-hosted)"
    fi
else
    echo "  (no test_myp_fmt.sh found)"
fi

# =============================================
# 第7部分: MYP 自举可视化器测试
# =============================================
echo ""
echo "--- [7/10] MYP 自举可视化器测试 (Self-hosted visualizer) ---"
echo ""
VIZ_PASS=0
VIZ_FAIL=0
if [ -f "$PROJ_ROOT/tests/test_myp_viz.sh" ]; then
    viz_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/test_myp_viz.sh" 2>&1)
    if echo "$viz_out" | grep -qE "myp-viz PASS=[0-9]+ FAIL=0"; then
        echo -e "${GREEN}PASS${NC} (tools/viz/main.myp 自举可视化器)"
        VIZ_PASS=1
    else
        echo -e "${RED}FAIL${NC}"
        echo "$viz_out" | tail -15
        VIZ_FAIL=1
        FAILED_TESTS="$FAILED_TESTS myp_viz(self-hosted)"
    fi
else
    echo "  (no test_myp_viz.sh found)"
fi

# =============================================
# 第8部分: mypc run（仿 go run + 单类文件自动 main）
# =============================================
echo ""
echo "--- [8/10] mypc run 测试 (go-style run + auto-main) ---"
echo ""
RUN_PASS=0
RUN_FAIL=0
if [ -f "$PROJ_ROOT/tests/test_myp_run.sh" ]; then
    run_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/test_myp_run.sh" 2>&1)
    if echo "$run_out" | grep -qE "myp-run PASS=[0-9]+ FAIL=0"; then
        echo -e "${GREEN}PASS${NC} (mypc run：单类@startup 自动 main / args 透传 / 无残留)"
        RUN_PASS=1
    else
        echo -e "${RED}FAIL${NC}"
        echo "$run_out" | tail -15
        RUN_FAIL=1
        FAILED_TESTS="$FAILED_TESTS myp_run(go-style)"
    fi
else
    echo "  (no test_myp_run.sh found)"
fi

# =============================================
# 第9部分: LSP hover / 缓存失效
# =============================================
echo ""
echo "--- [9/10] LSP 测试 (lookup caches + invalidation) ---"
echo ""
LSP_PASS=0
LSP_FAIL=0
if [ -f "$PROJ_ROOT/tests/test_lsp.js" ] && command -v node >/dev/null 2>&1; then
    MYP_LSP_BIN="${MYP_LSP:-$(dirname "$MYPCC")/myp_lsp}"
    lsp_out=$(MYP_LSP="$MYP_LSP_BIN" node "$PROJ_ROOT/tests/test_lsp.js" 2>&1)
    if echo "$lsp_out" | grep -qE "myp-lsp PASS=[0-9]+ FAIL=0"; then
        echo -e "${GREEN}PASS${NC} (hover/completion/documentSymbol 缓存失效)"
        LSP_PASS=1
    else
        echo -e "${RED}FAIL${NC}"
        echo "$lsp_out" | tail -15
        LSP_FAIL=1
        FAILED_TESTS="$FAILED_TESTS myp_lsp(hover-cache)"
    fi
else
    echo "  (node 或 test_lsp.js 不可用)"
fi

# =============================================
# 第10部分: GPU CPU 回退测试（可选：RUN_GPU_TESTS=1）
# =============================================
echo ""
echo "--- [10/11] GPU CPU 回退测试 (opt-in: RUN_GPU_TESTS=1) ---"
echo ""
GPU_PASS=0
GPU_FAIL=0
if [ "${RUN_GPU_TESTS:-0}" = "1" ]; then
    gpu_out=$(MYPCC="$MYPCC" bash "$PROJ_ROOT/tests/test_myp_gpu.sh" 2>&1)
    if echo "$gpu_out" | grep -qE "myp-gpu PASS=[0-9]+ FAIL=0"; then
        echo -e "${GREEN}PASS${NC} (@gpu for/tile/reduce/scan/scatter CPU 回退)"
        GPU_PASS=1
    else
        echo -e "${RED}FAIL${NC}"
        echo "$gpu_out" | tail -20
        GPU_FAIL=1
        FAILED_TESTS="$FAILED_TESTS gpu_cpu_fallback"
    fi
else
    echo "  (跳过：设置 RUN_GPU_TESTS=1 启用 GPU CPU 回退测试)"
fi

# =============================================
# 第11部分: 总结
# =============================================
echo ""
echo "=========================================="
echo "  测试结果汇总"
echo "=========================================="
TOTAL_PASS=$((PASS + NEG_PASS + TFPASS + NCRASH_PASS + PM_PASS + FMT_PASS + VIZ_PASS + RUN_PASS + LSP_PASS + GPU_PASS + CORO_PASS))
TOTAL_FAIL=$((FAIL + NEG_FAIL + TFFAIL + NCRASH_FAIL + PM_FAIL + FMT_FAIL + VIZ_FAIL + RUN_FAIL + LSP_FAIL + GPU_FAIL + CORO_FAIL))
echo "  回归测试: ${PASS} 通过, ${FAIL} 失败"
echo "  负测试:   ${NEG_PASS} 通过, ${NEG_FAIL} 失败"
echo "  测试框架: ${TFPASS} 通过, ${TFFAIL} 失败"
echo "  协程栈警告: ${CORO_PASS} 通过, ${CORO_FAIL} 失败"
echo "  无崩溃:   ${NCRASH_PASS} 通过, ${NCRASH_FAIL} 失败"
echo "  自举包管理: ${PM_PASS} 通过, ${PM_FAIL} 失败"
echo "  自举格式化: ${FMT_PASS} 通过, ${FMT_FAIL} 失败"
echo "  自举可视化: ${VIZ_PASS} 通过, ${VIZ_FAIL} 失败"
echo "  mypc run: ${RUN_PASS} 通过, ${RUN_FAIL} 失败"
echo "  LSP:       ${LSP_PASS} 通过, ${LSP_FAIL} 失败"
if [ "${RUN_GPU_TESTS:-0}" = "1" ]; then
    echo "  GPU 回退: ${GPU_PASS} 通过, ${GPU_FAIL} 失败"
fi
echo "  总计:     ${TOTAL_PASS} 通过, ${TOTAL_FAIL} 失败"

if [ $TOTAL_FAIL -gt 0 ]; then
    echo ""
    echo -e "${RED}  失败的测试:${NC}"
    for t in $FAILED_TESTS; do echo "    - $t"; done
    echo ""
    exit 1
else
    echo ""
    echo -e "${GREEN}  所有测试通过!${NC}"
    echo ""
    exit 0
fi
