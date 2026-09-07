#!/bin/bash
# MYP 测试框架自测 —— 人为造失败验证框架能报告失败（防假绿）
#
# 覆盖（roadmap 二-T1/T2「需要补充的框架自测」+ 顶层"framework 自测（人为造
# 失败验框架）"）：
#   S1  回归：只有 test.myp、无 expected → 普通模式 MISSING BASELINE + 非零退出，
#       且不得自动创建 expected 文件。
#   S2  回归：--update 模式创建 expected 并返回零（只在临时假树内写，安全）。
#   S3  回归：输出错配 MISMATCH / 运行崩溃 RUNTIME FAIL / 正常 PASS 一并检出。
#   S4  负测试：干净拒绝且含 EXPECT ERROR 子串 → 通过。
#   S5  负测试：拒绝但诊断不含 EXPECT ERROR 子串 → 失败。
#   S6  负测试：编译器 ASan/abort（rc=134+文本）→ CRASH 而非干净拒绝。
#   S7  负测试：编译器真实段错误且 stderr 为空（rc=139）→ CRASH（此前漏判为
#       干净拒绝假 PASS，run_tests.sh 已加 rc>=128 判定）。
#   S8  负测试：应拒绝却被接受（exit 0）→ 失败。
#
# 驱动方式：以临时假树为 TESTS_DIR + SELFCHECK=1 调用真实 run_tests.sh 的
# 回归/负测试两段（不复制循环逻辑、不依赖真实编译器；mypc 用假 stub 生成
# test.out / 模拟诊断）。正常套件只跑两段即出汇总退出，故不会递归。
#
# 用法: bash tests/test_framework_selftest.sh
# 退出码: 0=全过, 1=有失败

set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
RUN="$REPO/tests/run_tests.sh"
PASS=0
FAIL=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

check() {
    local name="$1"; local cond="$2"
    if eval "$cond"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}

# ---- 假编译器 stub：回归文件 → 生成兄弟 test.out；负测试按文件名模拟行为 ----
mkdir -p "$TMP/bin"
cat > "$TMP/bin/mypc" <<'STUB'
#!/bin/bash
# 假 mypc：不做真实编译。按被编译文件/所在目录名分派行为。
src="$1"
dir=$(dirname "$src")
base=$(basename "$src")
case "$base" in
  neg_good.myp)    echo "syntax error near token: neg_good" >&2; exit 1 ;;
  neg_wrong.myp)   echo "unrelated diagnostic text" >&2; exit 1 ;;
  neg_crash.myp)   echo "AddressSanitizer: heap-use-after-free on address 0x..." >&2; exit 134 ;;
  neg_accept.myp)  exit 0 ;;
  neg_sigsegv.myp) kill -SEGV $$ ;;     # 真实段错误（stderr 为空，rc=139）
esac
d=$(basename "$dir")
case "$d" in
  reg_ok)          marker="HELLO_OK" ;;
  reg_missing)     marker="HELLO_MISSING" ;;
  reg_mismatch)    marker="ACTUAL_MISMATCH_OUTPUT" ;;
  reg_runtimefail)
    cat > "$dir/test.out" <<'OUT'
#!/bin/bash
echo "boom before crash"
exit 3
OUT
    chmod +x "$dir/test.out"
    exit 0 ;;
  *) marker="UNKNOWN_FIXTURE" ;;
esac
printf '#!/bin/bash\necho "%s"\n' "$marker" > "$dir/test.out"
chmod +x "$dir/test.out"
exit 0
STUB
chmod +x "$TMP/bin/mypc"
STUB="$TMP/bin/mypc"

# 建临时假测试树：mkrt 名字 预期 内容...
#   mkrt <rtdir> <reg_name> <reg_marker> [<expected_text>|MISSING]
#   mkrt_neg <rtdir> <neg_name> <expect_substr>   （可选第 4 参覆盖 stub 行为文件）
mkrt() { mkdir -p "$1/expected" "$1/negative"; }
mkreg() { # $1=rt $2=dirname $3=marker $4=expected|MISSING
    mkdir -p "$1/$2"
    printf 'import env;\nint main() { return 0; }\n' > "$1/$2/test.myp"
    if [ "$4" != "MISSING" ]; then
        printf '%s\n' "$4" > "$1/expected/$2.expected"
    fi
}
mkneg() { # $1=rt $2=basename(no .myp) $3=EXPECT ERROR 子串（空=无注释）
    if [ -n "$3" ]; then
        printf '// EXPECT ERROR: %s\nint main( {\n' "$3" > "$1/negative/$2.myp"
    else
        printf 'int main( {\n' > "$1/negative/$2.myp"
    fi
}

# 驱动真实 run_tests.sh 的回归+负测试两段（SELFCHECK 模式）
run_harness() { # $1=RT  $2=--update?
    if [ "${2:-}" = "UPDATE" ]; then
        TESTS_DIR="$1" SELFCHECK=1 MYPCC="$STUB" bash "$RUN" --update 2>&1
    else
        TESTS_DIR="$1" SELFCHECK=1 MYPCC="$STUB" bash "$RUN" 2>&1
    fi
}

echo "framework-selfcheck: 测试框架自测（人为造失败）"

# ---- S1: 无 expected → MISSING BASELINE + 非零 + 不创建文件 ----
RT1="$TMP/rt1"; mkrt "$RT1"; mkreg "$RT1" reg_missing HELLO_MISSING MISSING
s1=$(run_harness "$RT1"); s1_ec=$?
check "S1 缺 expected 返回非零" "test $s1_ec -ne 0"
check "S1 报 MISSING BASELINE" "echo \"$s1\" | grep -q 'MISSING BASELINE'"
check "S1 不自动创建 expected" "test ! -f \"$RT1/expected/reg_missing.expected\""

# ---- S2: --update 创建 expected 并返回零 ----
s2=$(run_harness "$RT1" UPDATE); s2_ec=$?
check "S2 --update 返回零" "test $s2_ec -eq 0"
check "S2 创建 expected" "test -f \"$RT1/expected/reg_missing.expected\""
check "S2 expected 内容为运行输出" "grep -q 'HELLO_MISSING' \"$RT1/expected/reg_missing.expected\""

# ---- S3: 输出错配 / 运行崩溃 / 正常 PASS 一并检出 ----
RT3="$TMP/rt3"; mkrt "$RT3"
mkreg "$RT3" reg_ok HELLO_OK HELLO_OK
mkreg "$RT3" reg_mismatch ACTUAL_MISMATCH_OUTPUT DIFFERENT_EXPECTED
mkreg "$RT3" reg_runtimefail boom MISSING
s3=$(run_harness "$RT3"); s3_ec=$?
check "S3 含失败返回非零" "test $s3_ec -ne 0"
check "S3 检出 MISMATCH" "echo \"$s3\" | grep -q 'MISMATCH'"
check "S3 检出 RUNTIME FAIL" "echo \"$s3\" | grep -q 'RUNTIME FAIL'"
check "S3 正常用例仍 PASS" "echo \"$s3\" | grep -q 'PASS'"

# ---- S4: 负测试干净拒绝 + 含 EXPECT 子串 → 通过 ----
RT4="$TMP/rt4"; mkrt "$RT4"; mkneg "$RT4" neg_good "syntax error near token"
s4=$(run_harness "$RT4"); s4_ec=$?
check "S4 干净拒绝返回零" "test $s4_ec -eq 0"

# ---- S5: 拒绝但诊断缺 EXPECT 子串 → 失败 ----
RT5="$TMP/rt5"; mkrt "$RT5"; mkneg "$RT5" neg_wrong "must mention interface method"
s5=$(run_harness "$RT5"); s5_ec=$?
check "S5 诊断缺失返回非零" "test $s5_ec -ne 0"
check "S5 报缺子串" "echo \"$s5\" | grep -q \"missing 'must mention interface method'\""

# ---- S6: 编译器 ASan/abort（rc=134 + 文本）→ CRASH ----
RT6="$TMP/rt6"; mkrt "$RT6"; mkneg "$RT6" neg_crash "some expected reason"
s6=$(run_harness "$RT6"); s6_ec=$?
check "S6 ASan 崩溃返回非零" "test $s6_ec -ne 0"
check "S6 归类 CRASH" "echo \"$s6\" | grep -q 'CRASH'"

# ---- S7: 编译器真实段错误、stderr 为空（rc=139）→ CRASH（非干净拒绝）----
RT7="$TMP/rt7"; mkrt "$RT7"; mkneg "$RT7" neg_sigsegv ""
s7=$(run_harness "$RT7"); s7_ec=$?
check "S7 段错误返回非零" "test $s7_ec -ne 0"
check "S7 段错误归类 CRASH 而非 PASS" "echo \"$s7\" | grep -q 'CRASH'"

# ---- S8: 应拒绝却被接受（exit 0）→ 失败 ----
RT8="$TMP/rt8"; mkrt "$RT8"; mkneg "$RT8" neg_accept "whatever"
s8=$(run_harness "$RT8"); s8_ec=$?
check "S8 误接受返回非零" "test $s8_ec -ne 0"
check "S8 报 should have been rejected" "echo \"$s8\" | grep -q 'should have been rejected'"

echo "framework-selfcheck: PASS=$PASS FAIL=$FAIL"
[ $FAIL -eq 0 ]
