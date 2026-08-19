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
    "$SRC/controls/switch.myp"
    "$SRC/controls/checkbox.myp"
    "$SRC/controls/slider.myp"
    "$SRC/controls/progress_bar.myp"
    "$SRC/layout/linear_layout.myp"
    "$SRC/layout/flow_layout.myp"
    "$SRC/layout/stack_layout.myp"
    "$SRC/uix/prop_bag.myp"
    "$SRC/uix/expr.myp"
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
   && printf '%s' "$OUT" | grep -q "uix sync status=ok-login" \
   && printf '%s' "$OUT" | grep -q "uix ext text=NEW nodes=3 hit=badge" \
   && printf '%s' "$OUT" | grep -q "uix ctrl nodes=5 sw=1 swc=34c759 cb=1/Agree sl=42 pb=70" \
   && printf '%s' "$OUT" | grep -q "uix flow b1=8/8 b2=42/8 b3=8/24" \
   && printf '%s' "$OUT" | grep -q "uix stack s1=10/20 w=200/100 s2=10/20" \
   && printf '%s' "$OUT" | grep -q "uix layout flowKids=2 stackKids=2 nodes=7 ftype=Flow" \
   && printf '%s' "$OUT" | grep -q "uix qml pct=vol: 50 half=100 st=low mode=OFF/888888" \
   && printf '%s' "$OUT" | grep -q "uix qml2 pct=vol: 75 st=high" \
   && printf '%s' "$OUT" | grep -q "uix qml3 mode=ON/34c759" \
   && printf '%s' "$OUT" | grep -q "uix rep n=4 c0=Alpha c1=Beta c2=Gamma type=Checkbox" \
   && printf '%s' "$OUT" | grep -q "uix big n=81 last=item79"; then
    echo "MYPVIEW-UIX PASS"
else
    echo "MYPVIEW-UIX FAIL"
    exit 1
fi

# ---- 完整链路示例（pipeline）：load json → build → render → 交互 → 动画 ----
# 在 examples/ 下编译运行（.uix/.usp 文件相对 cwd 打开）
PIPESRCS=(
    "$SRC/core/renderer.myp"
    "$SRC/core/view.myp"
    "$SRC/core/root.myp"
    "$SRC/controls/label.myp"
    "$SRC/controls/button.myp"
    "$SRC/controls/text_field.myp"
    "$SRC/controls/panel.myp"
    "$SRC/controls/switch.myp"
    "$SRC/controls/checkbox.myp"
    "$SRC/controls/slider.myp"
    "$SRC/controls/progress_bar.myp"
    "$SRC/layout/linear_layout.myp"
    "$SRC/layout/flow_layout.myp"
    "$SRC/layout/stack_layout.myp"
    "$SRC/animation/tween.myp"
    "$SRC/animation/coro_anim.myp"
    "$SRC/uix/prop_bag.myp"
    "$SRC/uix/expr.myp"
    "$SRC/uix/uix_loader.myp"
    "../examples/pipeline.myp"
)
cd "$DIR/../examples"
"$MYPCC" "${PIPESRCS[@]}" -o pipeline --stdlib "$STDLIB" || exit 1
PIPE=$(./pipeline 2>&1)
echo "$PIPE"
cd "$DIR"
if printf '%s' "$PIPE" | grep -q "pipe OK" \
   && printf '%s' "$PIPE" | grep -q "pipe load uix=" \
   && printf '%s' "$PIPE" | grep -q "pipe render rect=6 text=6 rounded=1" \
   && printf '%s' "$PIPE" | grep -q "pipe cmd play status=playing" \
   && printf '%s' "$PIPE" | grep -q "pipe expr seek=42 pos=42%" \
   && printf '%s' "$PIPE" | grep -q "pipe state mode=Pressed/7aff" \
   && printf '%s' "$PIPE" | grep -q "pipe anim tween steps=10 bar=100 mono=1" \
   && printf '%s' "$PIPE" | grep -q "pipe anim coro steps=10 bar=100 fin=1"; then
    echo "MYPVIEW-PIPE PASS"
    exit 0
else
    echo "MYPVIEW-PIPE FAIL"
    exit 1
fi

