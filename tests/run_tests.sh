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
if [ "$1" = "--rebuild" ]; then
    echo -e "${YELLOW}[BUILD] Rebuilding compiler...${NC}"
    cd "$(dirname "$0")/.."
    cmake --build build 2>&1 | tail -3
    if [ $? -ne 0 ]; then
        echo -e "${RED}[BUILD] Compiler build failed!${NC}"
        exit 1
    fi
fi

UPDATE_MODE=false
if [ "$1" = "--update" ] || [ "$2" = "--update" ]; then
    UPDATE_MODE=true
    echo -e "${YELLOW}[UPDATE] Updating expected outputs...${NC}"
fi

cd "$(dirname "$0")/.."
PROJ_ROOT=$(pwd)

echo ""
echo "=========================================="
echo "  MYP Language Test Suite"
echo "  $(date)"
echo "=========================================="
echo ""

# =============================================
# 第1部分: 回归测试 — 编译+运行+比对输出
# =============================================
echo "--- [1/5] 回归测试 (Regression Tests) ---"
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

    # 运行
    run_output=$(timeout $TIMEOUT_SEC "$binary_file" 2>&1)
    if [ $? -ne 0 ]; then
        echo -e "${RED}RUNTIME FAIL${NC}"
        echo "$run_output" | head -5
        FAIL=$((FAIL + 1))
        FAILED_TESTS="$FAILED_TESTS $name(runtime)"
        continue
    fi

    # 保存输出
    echo "$run_output" > "$output_file"

    # 比对 expected
    if [ -f "$expected_file" ]; then
        # ASan prints a fixed warning for makecontext/swapcontext (ucontext
        # coroutines) that is not part of the program output — filter it.
        # (Use a temp file: diff on process-substitution FIFOs is unreliable.)
        tmp_filtered=$(mktemp)
        grep -v "ASan doesn't fully support makecontext" "$output_file" > "$tmp_filtered" 2>/dev/null || true
        if diff -q "$tmp_filtered" "$expected_file" > /dev/null 2>&1; then
            rm -f "$tmp_filtered"
            echo -e "${GREEN}PASS${NC}"
            PASS=$((PASS + 1))
        else
            rm -f "$tmp_filtered"
            if $UPDATE_MODE; then
                cp "$output_file" "$expected_file"
                echo -e "${YELLOW}UPDATED${NC}"
                PASS=$((PASS + 1))
            else
                echo -e "${RED}MISMATCH${NC}"
                diff "$output_file" "$expected_file" | head -10
                FAIL=$((FAIL + 1))
                FAILED_TESTS="$FAILED_TESTS $name(output)"
            fi
        fi
    else
        # 首次运行 — 保存为 expected
        cp "$output_file" "$expected_file"
        echo -e "${YELLOW}BASELINE${NC} (created expected)"
        PASS=$((PASS + 1))
    fi
done

# =============================================
# 第2部分: 负测试 — 编译应该失败
# =============================================
echo ""
echo "--- [2/5] 负测试 (Negative Tests) ---"
echo ""

NEG_PASS=0
NEG_FAIL=0

for test_file in tests/negative/*.myp; do
    [ ! -f "$test_file" ] && continue
    name=$(basename "$test_file" .myp)

    printf "  %-25s " "$name"

    compile_output=$($MYPCC "$test_file" 2>&1)
    if [ $? -ne 0 ]; then
        echo -e "${GREEN}PASS${NC} (correctly rejected)"
        NEG_PASS=$((NEG_PASS + 1))
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
echo "--- [3/5] 测试框架 (Test Framework) ---"
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
        if [ ! -f "$binary_file" ]; then
            binary_file="./a.out"
        fi
        run_output=$(timeout $TIMEOUT_SEC "$binary_file" 2>&1)
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
        fi
    fi
else
    echo "  (no test framework files found)"
fi

# =============================================
# 第4部分: 无崩溃回归 (compiler must never crash)
# =============================================
echo ""
echo "--- [4/5] 无崩溃回归 (No-Crash Regression) ---"
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
# 第5部分: 总结
# =============================================
echo ""
echo "=========================================="
echo "  测试结果汇总"
echo "=========================================="
TOTAL_PASS=$((PASS + NEG_PASS + TFPASS + NCRASH_PASS))
TOTAL_FAIL=$((FAIL + NEG_FAIL + TFFAIL + NCRASH_FAIL))
echo "  回归测试: ${PASS} 通过, ${FAIL} 失败"
echo "  负测试:   ${NEG_PASS} 通过, ${NEG_FAIL} 失败"
echo "  测试框架: ${TFPASS} 通过, ${TFFAIL} 失败"
echo "  无崩溃:   ${NCRASH_PASS} 通过, ${NCRASH_FAIL} 失败"
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
