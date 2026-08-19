#!/usr/bin/env bash
# build.sh — 用 mypc 直接编译 uikit 源码集合（零 MOS 依赖，仅依赖 stdlib）
# ---------------------------------------------------------------------------
# uikit 是「源码集合」框架：把 MOS/uikit 下的核心+控件+布局+UIX 文件加入编译
# 列表即可在任何 MYP 项目使用（无需 MOS 构建系统）。
# 用法：bash build.sh   → 产物 ./counter，运行 ./counter
set -euo pipefail
cd "$(dirname "$0")"

MYPCC="${MYPCC:-../../build/mypc}"
STDLIB="${STDLIB:-../../stdlib}"
UIKIT="${UIKIT:-../../MOS/uikit}"

# uikit 通用子集（headless 可用；如需 SDL 窗口后端再加 sdl_renderer.myp）
SRCS=(
    "$UIKIT/core/renderer.myp"
    "$UIKIT/core/view.myp"
    "$UIKIT/core/root.myp"
    "$UIKIT/controls/label.myp"
    "$UIKIT/controls/button.myp"
    "$UIKIT/controls/text_field.myp"
    "$UIKIT/controls/panel.myp"
    "$UIKIT/layout/linear_layout.myp"
    "$UIKIT/uix/prop_bag.myp"
    "$UIKIT/uix/uix_loader.myp"
    "counter.myp"
)

echo "uikit 独立示例编译（mypc 直接编译，零 MOS 依赖）..."
"$MYPCC" "${SRCS[@]}" -o counter --stdlib "$STDLIB"
echo "OK → ./counter"
echo "运行："
./counter
