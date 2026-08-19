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
STDLIB="${STDLIB:-$DIR/../../stdlib}"
SRC="$DIR/../src"
TARGET="${1:-counter}"

# mypview 通用子集（headless 可用；如需 SDL 窗口后端再加 backend/sdl_renderer.myp）
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
    "$SRC/layout/grid_layout.myp"
    "$SRC/layout/flow_layout.myp"
    "$SRC/layout/stack_layout.myp"
    "$SRC/uix/prop_bag.myp"
    "$SRC/uix/uix_loader.myp"
    "$TARGET.myp"
)

echo "mypview 框架示例编译（mypc 直接编译，零 MOS 依赖）：$TARGET"
"$MYPCC" "${SRCS[@]}" -o "$TARGET" --stdlib "$STDLIB"
echo "OK → ./$TARGET"
echo "运行："
./"$TARGET"
