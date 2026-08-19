#!/usr/bin/env bash
# run.sh — mypview headless 逻辑回归（UIX 构建/样式/绑定/命令/命中）
# 用法：bash run.sh
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"
MYPCC="${MYPCC:-../../build/mypc}"
STDLIB="${STDLIB:-../../stdlib}"
SRC="$DIR/../src"

SRCS=(
    "$SRC/core/renderer.myp"
    "$SRC/core/view.myp"
    "$SRC/core/root.myp"
    "$SRC/controls/label.myp"
    "$SRC/controls/button.myp"
    "$SRC/controls/text_field.myp"
    "$SRC/controls/panel.myp"
    "$SRC/layout/linear_layout.myp"
    "$SRC/uix/prop_bag.myp"
    "$SRC/uix/uix_loader.myp"
    "uix_logic.myp"
)
"$MYPCC" "${SRCS[@]}" -o uix_logic --stdlib "$STDLIB" || exit 1
OUT=$(./uix_logic 2>&1)
echo "$OUT"
echo "== 结果 =="
if printf '%s' "$OUT" | grep -q "uix nodes=5 binds=1 cmds=1 type(ok)=Button" \
   && printf '%s' "$OUT" | grep -q "uix cmdFor(ok)=login" \
   && printf '%s' "$OUT" | grep -q "uix style title=1c1c1e status=e74c3c ok=7aff" \
   && printf '%s' "$OUT" | grep -q "uix bind status=ready (init)" \
   && printf '%s' "$OUT" | grep -q "uix runCmd status=ok-login" \
   && printf '%s' "$OUT" | grep -q "uix sync status=ok-login"; then
    echo "MYPVIEW-UIX PASS"
    exit 0
else
    echo "MYPVIEW-UIX FAIL"
    exit 1
fi
