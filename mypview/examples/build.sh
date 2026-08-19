#!/usr/bin/env bash
# build.sh — 用 mypc 直接编译 mypview 框架源码集合（零 MOS 依赖，仅依赖 stdlib）
# ---------------------------------------------------------------------------
# mypview 是「源码集合」框架：把 src/ 下的核心+控件+布局+UIX 文件加入编译列表
# 即可在任何 MYP 项目使用（无需 MOS 构建系统）。
# 用法：bash build.sh   → 产物 ./counter，运行 ./counter
set -euo pipefail
cd "$(dirname "$0")"

MYPCC="${MYPCC:-../../build/mypc}"
STDLIB="${STDLIB:-../../stdlib}"
SRC="$(dirname "$0")/../src"

# mypview 通用子集（headless 可用；如需 SDL 窗口后端再加 backend/sdl_renderer.myp）
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
    "counter.myp"
)

echo "mypview 框架示例编译（mypc 直接编译，零 MOS 依赖）..."
"$MYPCC" "${SRCS[@]}" -o counter --stdlib "$STDLIB"
echo "OK → ./counter"
echo "运行："
./counter
