#!/usr/bin/env bash
# build.sh — 用 mypc 直接编译 mypview 框架源码集合（零 MOS 依赖，仅依赖 stdlib）
# ---------------------------------------------------------------------------
# mypview 是「源码集合」框架：把 src/ 下的核心+控件+布局+UIX 文件加入编译列表
# 即可在任何 MYP 项目使用（无需 MOS 构建系统）。
# 用法：bash build.sh [示例名]   → 默认 counter，产物 ./<示例名>，运行之
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

MYPCC="${MYPCC:-$DIR/../../build/mypc}"
# 源码/标准库用相对路径（cd $DIR 后稳定）：myp_self 对「绝对路径源码 + 相对
# target」的混合路径会丢 main（undefined main，BUG-041 关联），全相对才稳。
STDLIB="${STDLIB:-../../stdlib}"
SRC="../src"
TARGET="${1:-counter}"

# mypview 源码集合。⚠ 两个编译器对文件顺序偏好不同（BUG-041）：
#   - mypc 多文件编译对顺序敏感，须「被引用类型先编译」（依赖顺序固定列表）；
#     字母序（通配符）会让 ConstraintLayout 等对象 ARC 错乱 → 运行崩溃。
#   - myp_self 通配符（字母序）正常；但固定列表（run.sh 依赖顺序）反而触发
#     main 丢失（undefined main）。
# 故按编译器分支：myp_self → 目录通配；mypc → 依赖顺序固定列表。新增控件按依赖追加。
EXTRA=()
if [ "$TARGET" = "player" ]; then
    EXTRA+=("$SRC/backend/sdl_renderer.myp")
fi

SRCS=()
if [[ "$MYPCC" == *myp_self* ]]; then
    SRCS=( "$SRC/core/"*.myp "$SRC/controls/"*.myp "$SRC/layout/"*.myp
           "$SRC/uix/"*.myp "$SRC/animation/"*.myp
           "${EXTRA[@]}" "$TARGET.myp" )
else
SRCS=(
    "$SRC/core/renderer.myp"
    "$SRC/core/view.myp"
    "$SRC/core/root.myp"
    "$SRC/core/focus_manager.myp"
    "$SRC/core/gesture.myp"
    "$SRC/core/theme.myp"
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
    "$SRC/controls/sortable_list.myp"
    "$SRC/controls/long_press_button.myp"
    "$SRC/controls/dialog.myp"
    "$SRC/controls/ttf_label.myp"
    "$SRC/controls/list.myp"
    "$SRC/controls/notification_banner.myp"
    "$SRC/controls/app_icon.myp"
    "$SRC/layout/linear_layout.myp"
    "$SRC/layout/constraint_layout.myp"
    "$SRC/layout/flow_layout.myp"
    "$SRC/layout/stack_layout.myp"
    "$SRC/layout/grid_layout.myp"
    "$SRC/uix/prop_bag.myp"
    "$SRC/uix/expr.myp"
    "$SRC/uix/uix_loader.myp"
    "$SRC/animation/tween.myp"
    "$SRC/animation/coro_anim.myp"
    "${EXTRA[@]}"
    "$TARGET.myp"
)
fi

echo "mypview 框架示例编译（mypc 直接编译，零 MOS 依赖）：$TARGET"
"$MYPCC" "${SRCS[@]}" -o "$TARGET" --stdlib "$STDLIB"
echo "OK → ./$TARGET"
echo "运行："
./"$TARGET"
