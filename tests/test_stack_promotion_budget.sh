#!/bin/bash
# Per-function escape-analysis stack budget: disabled, cumulative, and default.

set -u
MYPCC="${MYPCC:-./build/mypc}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

compile_and_count() {
    local budget="$1" out="$2"
    if [ "$budget" = default ]; then
        (unset MYP_STACK_PROMOTION_BUDGET
         $MYPCC tests/@test/arraylist_small_buffer.myp --test --emit-llvm -o "$out") \
            >/dev/null 2>&1 || return 1
    else
        MYP_STACK_PROMOTION_BUDGET="$budget" $MYPCC \
            tests/@test/arraylist_small_buffer.myp --test --emit-llvm -o "$out" \
            >/dev/null 2>&1 || return 1
    fi
    grep -c '2147483646' "$out.ll" || true
}

PASS=0
FAIL=0
check() {
    local label="$1" budget="$2" expected="$3" got
    got=$(compile_and_count "$budget" "$TMP/$label") || got="compile-failed"
    if [ "$got" = "$expected" ]; then
        PASS=$((PASS + 1))
    else
        echo "FAIL: $label budget=$budget expected=$expected got=$got"
        FAIL=$((FAIL + 1))
    fi
}

check disabled 0 0
check cumulative 100 1
check default default 3

echo "stack-promotion-budget: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]