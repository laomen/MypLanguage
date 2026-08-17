#!/usr/bin/env bash
# test_myp_pass.sh — Verify the custom MYP LLVM pass infrastructure:
#   1) `mypc --passes myp-pass` is callable and cleans dead stores;
#   2) the -O pipeline still appends the MYP pass without breaking semantics;
#   3) unknown pass names are rejected.
#
# Usage: bash tests/test_myp_pass.sh
#        MYPCC=/path/to/mypc bash tests/test_myp_pass.sh   (default ./build/mypc)
set -u

MYPCC="${MYPCC:-./build/mypc}"
PASS=0
FAIL=0

say() { printf '%s\n' "$*"; }
ok()  { say "  PASS: $*"; PASS=$((PASS+1)); }
bad() { say "  FAIL: $*"; FAIL=$((FAIL+1)); }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

SRC="$TMP/dup.myp"
cat > "$SRC" <<'EOF'
int compute(int n) {
    int sum = 0;
    int dup = 0;
    dup = sum;
    for (int i = 0; i < n; i++) {
        sum += i;
    }
    return dup + sum;
}

int main() {
    return compute(5);
}
EOF

# 1) unknown pass rejected
OUT=$("$MYPCC" --passes does-not-exist "$SRC" -o "$TMP/x" 2>&1)
case "$OUT" in
  *"unknown pass pipeline"*) ok "unknown pass rejected" ;;
  *) bad "unknown pass rejected: $OUT" ;;
esac

# 2) myp-pass is callable and the binary still computes the right result
if "$MYPCC" --passes myp-pass "$SRC" -o "$TMP/out" >/dev/null 2>&1; then
    ok "--passes myp-pass callable"
else
    bad "--passes myp-pass callable"
fi
RES=$("$TMP/out" 2>/dev/null); RC=$?
if [ "$RC" -eq 10 ]; then
    ok "myp-pass result == 10"
else
    bad "myp-pass result == 10 (got rc=$RC out='$RES')"
fi

# 3) dead-store cleanup: count stores in @compute before vs after.
#    BEFORE (no pass): 3 adjacent double-stores (sum/dup/i) -> 10 stores.
#    AFTER  (myp-pass): 3 stores removed -> 7 stores in the body.
BEFORE=$("$MYPCC" --emit-llvm "$SRC" -o "$TMP/before" >/dev/null 2>&1 && \
    sed -n '/define.*@compute/,/^}/p' "$SRC.ll" | grep -c "store")
if [ "$BEFORE" -ge 9 ]; then
    ok "baseline has >=9 stores in @compute (got $BEFORE)"
else
    bad "baseline has >=9 stores in @compute (got $BEFORE)"
fi

MYPC_DUMP_OPT_IR=1 "$MYPCC" --passes myp-pass "$SRC" -o "$TMP/out2" >/dev/null 2>"$TMP/after.txt"
AFTER=$(sed -n '/define.*@compute/,/^}/p' "$TMP/after.txt" | grep -c "store")
if [ "$AFTER" -lt "$BEFORE" ]; then
    ok "myp-pass removed dead stores ($BEFORE -> $AFTER)"
else
    bad "myp-pass removed dead stores ($BEFORE -> $AFTER)"
fi

# 4) -O2 still correct with the appended MYP pass
if "$MYPCC" -O2 --passes myp-pass "$SRC" -o "$TMP/out3" >/dev/null 2>&1; then
    "$TMP/out3" >/dev/null 2>&1
    if [ $? -eq 10 ]; then
        ok "-O2 + myp-pass result == 10"
    else
        bad "-O2 + myp-pass result == 10"
    fi
else
    bad "-O2 + myp-pass compile"
fi

echo "=== summary: myp-pass PASS=$PASS FAIL=$FAIL ==="
exit $((FAIL>0))
