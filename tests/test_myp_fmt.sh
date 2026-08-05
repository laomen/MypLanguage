#!/bin/bash
# test_myp_fmt.sh — MYP 自举格式化器（tools/fmt/main.myp）端到端测试
#
# 验证：
#   1) tools/fmt/main.myp 可编译
#   2) --stdout 输出与 C++ myp_fmt 字节级一致（stdlib + examples）
#   3) 幂等性：fmt(fmt(x)) == fmt(x)
#   4) --check 退出码与 C++ myp_fmt 一致
#   5) 文件模式（in-place）输出与 C++ 一致
#
# 用法：bash tests/test_myp_fmt.sh
#       MYPCC=/path/to/mypc    bash tests/test_myp_fmt.sh  (默认 ./build/mypc)
#       MYP_FMT=/path/to/myp_fmt bash tests/test_myp_fmt.sh (默认 ./build/myp_fmt)
#
# 关联：docs/self_hosting.md §4（T2）、tools/fmt/

set -u
MYPCC="${MYPCC:-./build/mypc}"
MYP_FMT="${MYP_FMT:-./build/myp_fmt}"
PASS=0
FAIL=0

say() { printf '%s\n' "$*"; }
ok()  { say "  PASS: $*"; PASS=$((PASS+1)); }
bad() { say "  FAIL: $*"; FAIL=$((FAIL+1)); }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---- 1) 编译 ----
if ! "$MYPCC" tools/fmt/main.myp -o "$TMP/myp_fmt2" >/dev/null 2>&1; then
    bad "tools/fmt/main.myp 编译失败"
    echo "myp-fmt PASS=0 FAIL=1"
    exit 1
fi
ok "tools/fmt/main.myp 编译"

# ---- 2) --stdout 字节级对拍（stdlib + examples）----
CMP_OK=0
CMP_FAIL=0
for f in stdlib/*.myp examples/*.myp; do
    a=$("$TMP/myp_fmt2" --stdout "$f" 2>/dev/null)
    b=$("$MYP_FMT" --stdout "$f" 2>/dev/null)
    if [ "$a" == "$b" ]; then
        CMP_OK=$((CMP_OK+1))
    else
        CMP_FAIL=$((CMP_FAIL+1))
        bad "--stdout 对拍不一致: $f"
    fi
done
if [ "$CMP_FAIL" -eq 0 ]; then
    ok "--stdout 字节级一致 ($CMP_OK 文件)"
else
    bad "--stdout 对拍: $CMP_OK 一致, $CMP_FAIL 差异"
fi

# ---- 3) 幂等性：fmt(fmt(x)) == fmt(x)（剥 banner 首行）----
IDEM_OK=0
IDEM_FAIL=0
for f in stdlib/*.myp examples/*.myp; do
    "$TMP/myp_fmt2" --stdout "$f" 2>/dev/null | tail -n +2 > "$TMP/once.txt"
    "$TMP/myp_fmt2" --stdout "$TMP/once.txt" 2>/dev/null | tail -n +2 > "$TMP/twice.txt"
    if [ "$(cat "$TMP/once.txt")" == "$(cat "$TMP/twice.txt")" ]; then
        IDEM_OK=$((IDEM_OK+1))
    else
        IDEM_FAIL=$((IDEM_FAIL+1))
        bad "非幂等: $f"
    fi
done
if [ "$IDEM_FAIL" -eq 0 ]; then
    ok "幂等性 ($IDEM_OK 文件)"
else
    bad "幂等性: $IDEM_OK 幂等, $IDEM_FAIL 非幂等"
fi

# ---- 4) --check 退出码对拍 ----
CHK_MISMATCH=0
CHK_TOTAL=0
for f in stdlib/*.myp examples/*.myp; do
    CHK_TOTAL=$((CHK_TOTAL+1))
    "$TMP/myp_fmt2" --check "$f" >/dev/null 2>&1
    m=$?
    "$MYP_FMT" --check "$f" >/dev/null 2>&1
    c=$?
    if [ "$m" != "$c" ]; then
        CHK_MISMATCH=$((CHK_MISMATCH+1))
        bad "--check 判定不一致: $f (MYP=$m C++=$c)"
    fi
done
if [ "$CHK_MISMATCH" -eq 0 ]; then
    ok "--check 退出码一致 ($CHK_TOTAL 文件)"
else
    bad "--check 对拍: $CHK_MISMATCH 不一致"
fi

# ---- 5) 文件模式（in-place）与 C++ 一致 ----
FM_OK=0
FM_FAIL=0
for f in stdlib/args.myp examples/simple.myp; do
    cp "$f" "$TMP/a.myp"
    cp "$f" "$TMP/b.myp"
    "$TMP/myp_fmt2" "$TMP/a.myp" >/dev/null 2>&1
    "$MYP_FMT" "$TMP/b.myp" >/dev/null 2>&1
    if [ "$(cat "$TMP/a.myp")" == "$(cat "$TMP/b.myp")" ]; then
        FM_OK=$((FM_OK+1))
    else
        FM_FAIL=$((FM_FAIL+1))
        bad "文件模式不一致: $f"
    fi
done
if [ "$FM_FAIL" -eq 0 ]; then
    ok "文件模式一致 ($FM_OK 文件)"
else
    bad "文件模式: $FM_OK 一致, $FM_FAIL 差异"
fi

say ""
echo "myp-fmt PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
