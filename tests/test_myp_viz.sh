#!/bin/bash
# test_myp_viz.sh — MYP 自举 mapping 可视化器（tools/viz/main.myp）端到端测试
#
# 验证：
#   1) tools/viz/main.myp 可编译
#   2) 输出与 C++ myp_viz 字节级一致（stdlib/examples/tools/tests/BNCTDoseEngine
#      的合法 .myp 文件；tests/negative 语法错误文件除外——C++ 全解析报错，MYP
#      迷你解析容错，属预期差异）
#   3) 空文件 / 无 mapping 文件输出一致的 DOT 头
#
# 用法：bash tests/test_myp_viz.sh
#       MYPCC=/path/to/mypc     bash tests/test_myp_viz.sh  (默认 ./build/mypc)
#       MYP_VIZ=/path/to/myp_viz bash tests/test_myp_viz.sh (默认 ./build/myp_viz)
#
# 关联：docs/self_hosting.md §5（T3/M4）、tools/viz/

set -u
MYPCC="${MYPCC:-./build/mypc}"
MYP_VIZ="${MYP_VIZ:-./build/myp_viz}"
PASS=0
FAIL=0

say() { printf '%s\n' "$*"; }
ok()  { say "  PASS: $*"; PASS=$((PASS+1)); }
bad() { say "  FAIL: $*"; FAIL=$((FAIL+1)); }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---- 1) 编译 ----
if ! $MYPCC tools/viz/main.myp -o "$TMP/myp_viz2" >/dev/null 2>&1; then
    bad "tools/viz/main.myp 编译失败"
    echo "myp-viz PASS=0 FAIL=1"
    exit 1
fi
ok "tools/viz/main.myp 编译"

# ---- 2) 全语料对拍（跳过 tests/negative 语法错误文件）----
# 冻结 oracle 参考不认识的新特性文件（参考端报错）→ 跳过对拍：冻结基线无法对
# 新语法字节级对比（如 @static @thread class，v3.15.77 新增）。共享语料仍全检。
CMP_OK=0
CMP_FAIL=0
for f in $(find stdlib examples tools tests -name "*.myp" \
    -not -path "*/build*" -not -path "*/negative/*" 2>/dev/null); do
    a=$("$TMP/myp_viz2" "$f" 2>&1)
    b=$("$MYP_VIZ" "$f" 2>&1)
    if echo "$b" | grep -q "error"; then
        continue
    fi
    if [ "$a" == "$b" ]; then
        CMP_OK=$((CMP_OK+1))
    else
        CMP_FAIL=$((CMP_FAIL+1))
        bad "对拍不一致: $f"
    fi
done
if [ "$CMP_FAIL" -eq 0 ]; then
    ok "全语料对拍一致 ($CMP_OK 文件)"
else
    bad "对拍: $CMP_OK 一致, $CMP_FAIL 差异"
fi

# ---- 3) 空文件 / 无 mapping ----
printf '' > "$TMP/empty.myp"
a=$("$TMP/myp_viz2" "$TMP/empty.myp" 2>&1)
b=$("$MYP_VIZ" "$TMP/empty.myp" 2>&1)
if [ "$a" == "$b" ]; then
    ok "空文件一致"
else
    bad "空文件不一致"
fi

say ""
echo "myp-viz PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
