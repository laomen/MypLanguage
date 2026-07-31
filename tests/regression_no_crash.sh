#!/bin/bash
# Regression: the compiler must cleanly REJECT invalid input, never crash.
#
# Each case below used to crash the compiler (assert/SIGABRT/ASAN) before the
# fixes. They must exit non-zero (correctly rejected) WITHOUT a signal or a
# sanitizer/assert report.
#
# Run with the normal build by default; set MYPCC=/path/to/mypc to test another
# build (e.g. the ASan build via tests/run_tests_asan.sh, which sets MYPCC).
set -u
cd "$(dirname "$0")/.."
MYPCC="${MYPCC:-./build/mypc}"

fail=0
cases=(
  # unary-minus chains via prefix -- desugar → cloneExpr(Assignment) used to be null
  'class Test { action: @startup void run() { ----(-398 == 5); } } int main(){Test t=new Test();return 0;}'
  'class Test { action: @startup void run() { !----(-!398 == ((-454 + -514) <= -433)); } } int main(){Test t=new Test();return 0;}'
  'class Test { action: @startup void run() { ----(398); } } int main(){Test t=new Test();return 0;}'
  'class Test { action: @startup void run() { ----x; } } int main(){Test t=new Test();return 0;}'
  'class Test { action: @startup void run() { --5; } } int main(){Test t=new Test();return 0;}'
  'class Test { action: @startup void run() { ++--3; } } int main(){Test t=new Test();return 0;}'
)

echo "--- 无崩溃回归测试 (no-crash regression) ---"
for src in "${cases[@]}"; do
    tmp=$(mktemp --suffix=.myp)
    echo "$src" > "$tmp"
    out=$("$MYPCC" "$tmp" -o /tmp/myp_nocrash.out 2>&1)
    code=$?
    rm -f "$tmp" /tmp/myp_nocrash.out

    if [ $code -lt 0 ] || echo "$out" | grep -qE \
        "AddressSanitizer|UndefinedBehaviorSanitizer|Assertion .* failed|unique_ptr.*Assertion|Segmentation fault"; then
        echo -e "  FAIL (crash, exit=$code): $(echo "$src" | head -c 70)..."
        echo "$out" | tail -3
        fail=1
    else
        echo -e "  ok (exit=$code): $(echo "$src" | head -c 70)..."
    fi
done

if [ $fail -eq 0 ]; then
    echo "  无崩溃回归: 全部通过"
else
    echo "  无崩溃回归: 有失败!"
fi
exit $fail
