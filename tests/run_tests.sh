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

MYPCC="./build/mypc"
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
echo "--- [1/3] 回归测试 (Regression Tests) ---"
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
        if diff -q "$output_file" "$expected_file" > /dev/null 2>&1; then
            echo -e "${GREEN}PASS${NC}"
            PASS=$((PASS + 1))
        else
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
echo "--- [2/3] 负测试 (Negative Tests) ---"
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
# 第3部分: 总结
# =============================================
echo ""
echo "=========================================="
echo "  测试结果汇总"
echo "=========================================="
TOTAL_PASS=$((PASS + NEG_PASS))
TOTAL_FAIL=$((FAIL + NEG_FAIL))
echo "  回归测试: ${PASS} 通过, ${FAIL} 失败"
echo "  负测试:   ${NEG_PASS} 通过, ${NEG_FAIL} 失败"
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
