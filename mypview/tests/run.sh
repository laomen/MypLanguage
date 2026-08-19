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
    "$SRC/core/focus_manager.myp"
    "$SRC/core/gesture.myp"
    "$SRC/controls/label.myp"
    "$SRC/controls/button.myp"
    "$SRC/controls/text_field.myp"
    "$SRC/controls/panel.myp"
    "$SRC/controls/switch.myp"
    "$SRC/controls/checkbox.myp"
    "$SRC/controls/slider.myp"
    "$SRC/controls/progress_bar.myp"
    "$SRC/controls/dropdown.myp"
    "$SRC/controls/radio_button.myp"
    "$SRC/controls/tab_view.myp"
    "$SRC/controls/toast.myp"
    "$SRC/controls/rating.myp"
    "$SRC/controls/image.myp"
    "$SRC/controls/icon_button.myp"
    "$SRC/controls/divider.myp"
    "$SRC/controls/progress_spinner.myp"
    "$SRC/controls/text_area.myp"
    "$SRC/controls/search_bar.myp"
    "$SRC/controls/segmented_control.myp"
    "$SRC/controls/stepper.myp"
    "$SRC/controls/badge.myp"
    "$SRC/controls/avatar.myp"
    "$SRC/controls/chip.myp"
    "$SRC/controls/tooltip.myp"
    "$SRC/controls/bottom_nav.myp"
    "$SRC/controls/drawer.myp"
    "$SRC/controls/refresh_indicator.myp"
    "$SRC/controls/context_menu.myp"
    "$SRC/controls/color_picker.myp"
    "$SRC/controls/date_picker.myp"
    "$SRC/controls/data_grid.myp"
    "$SRC/controls/tree_view.myp"
    "$SRC/controls/time_picker.myp"
    "$SRC/controls/action_sheet.myp"
    "$SRC/controls/pagination.myp"
    "$SRC/controls/page_view.myp"
    "$SRC/controls/popover.myp"
    "$SRC/controls/banner.myp"
    "$SRC/controls/scroll_view.myp"
    "$SRC/layout/linear_layout.myp"
    "$SRC/layout/constraint_layout.myp"
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
   && printf '%s' "$OUT" | grep -q "uix pseudo base=7aff" \
   && printf '%s' "$OUT" | grep -q "uix pseudo hover=a84ff" \
   && printf '%s' "$OUT" | grep -q "uix pseudo clear=7aff" \
   && printf '%s' "$OUT" | grep -q "uix pseudo checked=34c759" \
   && printf '%s' "$OUT" | grep -q "uix pseudo uncheck=888888" \
   && printf '%s' "$OUT" | grep -q "uix ext text=NEW nodes=3 hit=badge" \
   && printf '%s' "$OUT" | grep -q "uix ctrl nodes=5 sw=1 swc=34c759 cb=1/Agree sl=42 pb=70" \
   && printf '%s' "$OUT" | grep -q "uix flow b1=8/8 b2=42/8 b3=8/24" \
   && printf '%s' "$OUT" | grep -q "uix stack s1=10/20 w=200/100 s2=10/20" \
   && printf '%s' "$OUT" | grep -q "uix layout flowKids=2 stackKids=2 nodes=7 ftype=Flow" \
   && printf '%s' "$OUT" | grep -q "uix qml pct=vol: 50 half=100 st=low mode=OFF/888888" \
   && printf '%s' "$OUT" | grep -q "uix qml2 pct=vol: 75 st=high" \
   && printf '%s' "$OUT" | grep -q "uix qml3 mode=ON/34c759" \
   && printf '%s' "$OUT" | grep -q "uix rep n=4 c0=Alpha c1=Beta c2=Gamma type=Checkbox" \
   && printf '%s' "$OUT" | grep -q "uix big n=81 last=item79" \
   && printf '%s' "$OUT" | grep -q "uix dd opened=1 sel=Beta open=0" \
   && printf '%s' "$OUT" | grep -q "uix radio ra=0 rb=1" \
   && printf '%s' "$OUT" | grep -q "uix tab cur=1 n=2" \
   && printf '%s' "$OUT" | grep -q "uix toast v=1->0 text=saved" \
   && printf '%s' "$OUT" | grep -q "uix rating v=3 max=5" \
   && printf '%s' "$OUT" | grep -q "uix backspace n0=0 t=你好 n=6" \
   && printf '%s' "$OUT" | grep -q "uix img handle=-1 mode=1 w=120" \
   && printf '%s' "$OUT" | grep -q "uix icon hit=1 dis=0 size=40" \
   && printf '%s' "$OUT" | grep -q "uix div w=200 h=1 c=e74c3c" \
   && printf '%s' "$OUT" | grep -q "uix spin a=30->34 stop=34 run=0" \
   && printf '%s' "$OUT" | grep -q "uix ta lines=2 n=11 l0=你好 l1=worl" \
   && printf '%s' "$OUT" | grep -q "uix pw real=你好ab mask=\*\*\*\* m=1" \
   && printf '%s' "$OUT" | grep -q "uix search t=ab你好 clr= f=1" \
   && printf '%s' "$OUT" | grep -q "uix seg n=3 sel=1" \
   && printf '%s' "$OUT" | grep -q "uix step v=4" \
   && printf '%s' "$OUT" | grep -q "uix badge t=12 w=32" \
   && printf '%s' "$OUT" | grep -q "uix avatar name=Alice size=48 img=0" \
   && printf '%s' "$OUT" | grep -q "uix chip ck=1 after=1" \
   && printf '%s' "$OUT" | grep -q "uix tip v=1->0 t=帮助" \
   && printf '%s' "$OUT" | grep -q "uix nav n=3 cur=1" \
   && printf '%s' "$OUT" | grep -q "uix drawer v=1->1 x=20" \
   && printf '%s' "$OUT" | grep -q "uix refresh st=1->2->0 off=0" \
   && printf '%s' "$OUT" | grep -q "uix menu n=3 v=1->0" \
   && printf '%s' "$OUT" | grep -q "uix color n=12 sel=0" \
   && printf '%s' "$OUT" | grep -q "uix date y=2026 m=2 d=1 dim=28" \
   && printf '%s' "$OUT" | grep -q "uix grid cols=2 rows=2 c=香蕉" \
   && printf '%s' "$OUT" | grep -q "uix tree n=3 l1=1 ex0=1" \
   && printf '%s' "$OUT" | grep -q "uix time h=9 m=31" \
   && printf '%s' "$OUT" | grep -q "uix sheet n=2 v=1->0" \
   && printf '%s' "$OUT" | grep -q "uix page t=5 cur=3" \
   && printf '%s' "$OUT" | grep -q "uix pv n=2 cur=1->0" \
   && printf '%s' "$OUT" | grep -q "uix pop v=1->0" \
   && printf '%s' "$OUT" | grep -q "uix banner f=120->90 v=1" \
   && printf '%s' "$OUT" | grep -q "uix focus c1=0 b1=1 wrapA=1" \
   && printf '%s' "$OUT" | grep -q "uix const c1=75/40 c2=155/75" \
   && printf '%s' "$OUT" | grep -q "uix gest probe p=1 m=2 r=1" \
   && printf '%s' "$OUT" | grep -q "uix gest tap=1 x=12/11" \
   && printf '%s' "$OUT" | grep -q "uix gest long=1 rel=2" \
   && printf '%s' "$OUT" | grep -q "uix gest drag=1 dx=30/10" \
   && printf '%s' "$OUT" | grep -q "uix gest drel=3" \
   && printf '%s' "$OUT" | grep -q "uix gest svy=30" \
   && printf '%s' "$OUT" | grep -q "uix gest sl=75"; then
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
    "$SRC/core/focus_manager.myp"
    "$SRC/core/gesture.myp"
    "$SRC/controls/label.myp"
    "$SRC/controls/button.myp"
    "$SRC/controls/text_field.myp"
    "$SRC/controls/panel.myp"
    "$SRC/controls/switch.myp"
    "$SRC/controls/checkbox.myp"
    "$SRC/controls/slider.myp"
    "$SRC/controls/progress_bar.myp"
    "$SRC/controls/dropdown.myp"
    "$SRC/controls/radio_button.myp"
    "$SRC/controls/tab_view.myp"
    "$SRC/controls/toast.myp"
    "$SRC/controls/rating.myp"
    "$SRC/controls/image.myp"
    "$SRC/controls/icon_button.myp"
    "$SRC/controls/divider.myp"
    "$SRC/controls/progress_spinner.myp"
    "$SRC/controls/text_area.myp"
    "$SRC/controls/search_bar.myp"
    "$SRC/controls/segmented_control.myp"
    "$SRC/controls/stepper.myp"
    "$SRC/controls/badge.myp"
    "$SRC/controls/avatar.myp"
    "$SRC/controls/chip.myp"
    "$SRC/controls/tooltip.myp"
    "$SRC/controls/bottom_nav.myp"
    "$SRC/controls/drawer.myp"
    "$SRC/controls/refresh_indicator.myp"
    "$SRC/controls/context_menu.myp"
    "$SRC/controls/color_picker.myp"
    "$SRC/controls/date_picker.myp"
    "$SRC/controls/data_grid.myp"
    "$SRC/controls/tree_view.myp"
    "$SRC/controls/time_picker.myp"
    "$SRC/controls/action_sheet.myp"
    "$SRC/controls/pagination.myp"
    "$SRC/controls/page_view.myp"
    "$SRC/controls/popover.myp"
    "$SRC/controls/banner.myp"
    "$SRC/controls/scroll_view.myp"
    "$SRC/layout/linear_layout.myp"
    "$SRC/layout/constraint_layout.myp"
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
   && printf '%s' "$PIPE" | grep -q "pipe render rect=5 text=6 rounded=2" \
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

