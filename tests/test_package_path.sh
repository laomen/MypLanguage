#!/bin/bash
# package-path 冒号分隔回归（BUG-015）
# 验证: mypc --package-path "dirA:dirB" 按 ':' 切分逐路径查找——包在 dirB 时
#       应编译成功（修复前把整串当单一目录 → cannot find import 'foo'）。
# 用法: MYPCC=./build/mypc bash tests/test_package_path.sh
# 退出码: 0=全过, 1=有失败

set -u
MYPCC="${MYPCC:-./build/mypc}"
case "$MYPCC" in
    /*) ;;
    *) MYPCC="$(pwd)/$MYPCC" ;;
esac
PASS=0
FAIL=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

say() { printf '%s\n' "$*"; }
ok()  { say "  PASS: $*"; PASS=$((PASS+1)); }
bad() { say "  FAIL: $*"; FAIL=$((FAIL+1)); }

echo "package-path: --package-path 冒号分隔（BUG-015）"

# 包 foo 放在 dirB/foo/src/foo.myp；dirA 留空（验证冒号切分而非合并）
mkdir -p "$TMPDIR/dirA" "$TMPDIR/dirB/foo/src"
cat > "$TMPDIR/dirB/foo/src/foo.myp" <<'PKGEOF'
int fooValue() { return 7; }
PKGEOF
cat > "$TMPDIR/main.myp" <<'MAINEOF'
import foo;
int main() { return fooValue() == 7 ? 0 : 1; }
MAINEOF

# 1) 冒号分隔多路径：包在 dirB → 应成功
if $MYPCC "$TMPDIR/main.myp" --package-path "$TMPDIR/dirA:$TMPDIR/dirB" -o "$TMPDIR/a.out" >/dev/null 2>&1; then
    if "$TMPDIR/a.out"; then ok "colon multi-path resolves pkg in dirB (run ok)"
    else bad "colon multi-path compiles but wrong result"; fi
else
    bad "colon multi-path: cannot find import 'foo' (BUG-015)"
fi

# 2) 单路径（无冒号）仍正常
if $MYPCC "$TMPDIR/main.myp" --package-path "$TMPDIR/dirB" -o "$TMPDIR/b.out" >/dev/null 2>&1 \
   && "$TMPDIR/b.out"; then
    ok "single path still works"
else
    bad "single path regression"
fi

echo ""
echo "package-path: $PASS pass, $FAIL fail"
[ $FAIL -eq 0 ]
