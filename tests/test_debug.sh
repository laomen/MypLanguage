#!/usr/bin/env bash
# test_debug.sh — Verify -g DWARF debug info: breakpoints, line numbers,
#                 parameters, and local variables via gdb (batch mode).
#
# Usage: bash tests/test_debug.sh
#        MYPCC=/path/to/mypc bash tests/test_debug.sh   (default ./build/mypc)
set -u

MYPCC="${MYPCC:-./build/mypc}"
PASS=0
FAIL=0

say()  { printf '%s\n' "$*"; }
ok()   { say "  PASS: $*"; PASS=$((PASS+1)); }
bad()  { say "  FAIL: $*"; FAIL=$((FAIL+1)); }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

SRC="$TMP/dbg.myp"
cat > "$SRC" <<'EOF'
class Calc {
    action:
        int multiply(int a, int b) {
            int sum = 0;
            for (int i = 0; i < a; i++) {
                sum += b;
            }
            return sum;
        }
}

int main() {
    Calc c;
    c = new Calc();
    int x = c.multiply(6, 7);
    double y = 3.5;
    bool z = true;
    return x;
}
EOF

BIN="$TMP/dbg"
if ! "$MYPCC" -g "$SRC" -o "$BIN" >/dev/null 2>&1; then
    bad "-g compile"
    exit 1
fi
ok "-g compile"

if ! command -v gdb >/dev/null 2>&1; then
    say "  (gdb not installed — skipping runtime debug checks)"
    echo "=== summary: debug PASS=$PASS FAIL=$FAIL (compile only) ==="
    exit $((FAIL>0))
fi

# 1) Breakpoint on line 4 (function body) + parameter values
OUT=$(gdb -q -batch \
    -ex "break $SRC:4" \
    -ex run \
    -ex 'print a' -ex 'print b' \
    -ex 'print sum' \
    "$BIN" 2>/dev/null)
case "$OUT" in
  *"Breakpoint 1, Calc_multiply"*) ok "breakpoint hits Calc_multiply" ;;
  *) bad "breakpoint hits Calc_multiply" ;;
esac
case "$OUT" in
  *'$1 = 6'*) ok "param a == 6" ;;
  *) bad "param a == 6" ;;
esac
case "$OUT" in
  *'$2 = 7'*) ok "param b == 7" ;;
  *) bad "param b == 7" ;;
esac

# 2) Breakpoint on line 17 (main locals) after multiply returns
OUT=$(gdb -q -batch \
    -ex "break $SRC:17" \
    -ex run \
    -ex 'print x' -ex 'print y' \
    "$BIN" 2>/dev/null)
case "$OUT" in
  *'$1 = 42'*) ok "local x == 42" ;;
  *) bad "local x == 42" ;;
esac
case "$OUT" in
  *'$2 = 3.5'*) ok "local y == 3.5" ;;
  *) bad "local y == 3.5" ;;
esac

echo "=== summary: debug PASS=$PASS FAIL=$FAIL ==="
exit $((FAIL>0))
