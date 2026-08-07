#!/bin/bash
# Probe runner: compile+run each .myp at -O0 and -O2, report crashes/divergence.
# Usage: bash probe.sh file1.myp file2.myp ...
cd "$(dirname "$0")/.." || exit 1
MYPCC=./build/mypc
PASS=0; FAIL=0
for f in "$@"; do
    name=$(basename "$f")
    o0=$($MYPCC run "$f" 2>&1); e0=$?
    o2=$($MYPCC -O2 run "$f" 2>&1); e2=$?
    # strip compiler status lines for output compare
    o0c=$(echo "$o0" | grep -vE "^Lexer OK|^Parser OK|^Sema OK|^CodeGen OK|^Link OK")
    o2c=$(echo "$o2" | grep -vE "^Lexer OK|^Parser OK|^Sema OK|^CodeGen OK|^Link OK")
    ok=1; msg=""
    if [ $e0 -ne 0 ] || [ $e2 -ne 0 ]; then
        ok=0; msg="EXIT(o0=$e0 o2=$e2)"
    fi
    if [ -n "$(echo "$o0" | grep -iE 'SEGV|crash|stack overflow|abort|LLVM verify|internal compiler|fatal')" ]; then
        ok=0; msg="$msg O0_CRASH"
    fi
    if [ -n "$(echo "$o2" | grep -iE 'SEGV|crash|stack overflow|abort|LLVM verify|internal compiler|fatal')" ]; then
        ok=0; msg="$msg O2_CRASH"
    fi
    if [ "$o0c" != "$o2c" ]; then
        ok=0; msg="$msg DIVERGE"
    fi
    if [ $ok -eq 1 ]; then
        echo "PASS  $name"
        PASS=$((PASS+1))
    else
        echo "FAIL  $name  $msg"
        echo "--- O0 ---"; echo "$o0c" | head -12
        echo "--- O2 ---"; echo "$o2c" | head -12
        FAIL=$((FAIL+1))
    fi
done
echo "== $PASS pass, $FAIL fail =="
