#!/usr/bin/env bash
# build.sh — 用 mypc / myp_self 编译 mypview 框架源码集合（零 MOS 依赖）
# ---------------------------------------------------------------------------
# mypview 是「源码集合」框架：把 src/ 下的核心+控件+布局+UIX 文件加入编译列表
# 即可在任何 MYP 项目使用（无需 MOS 构建系统）。
# 用法：
#   bash build.sh [示例名]                          → 用 $MYPCC（默认 mypc）编译运行
#   MYPCC=../../build/myp_self bash build.sh [示例名] → 用自举编译器
#   bash build.sh [示例名] both                     → 同一源码列表，mypc + myp_self
#                                                    分别编译运行并对比输出
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

MYPCC="${MYPCC:-$DIR/../../build/mypc}"
# 源码/标准库用相对路径（cd $DIR 后稳定）：myp_self 对「绝对路径源码 + 相对
# target」混合路径会丢 main（undefined main，BUG-041b），全相对才稳。
STDLIB="${STDLIB:-../../stdlib}"
SRC="../src"
TARGET="${1:-counter}"
MODE="${2:-}"

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

# 统一源码列表（目录通配 + 相对路径）。BUG-041 根治后 mypc 对字母序通配符也
# 正常；相对路径避免 myp_self 混合路径丢 main（BUG-041b）。两个编译器共用同一
# 份内容，可用 `bash build.sh <示例> both` 同时跑 mypc + myp_self 对比。
SRCS=( "$SRC/core/"*.myp "$SRC/controls/"*.myp "$SRC/layout/"*.myp
       "$SRC/uix/"*.myp "$SRC/animation/"*.myp
       "${EXTRA[@]}" "$TARGET.myp" )

if [ "$MODE" = "both" ]; then
    MYPCC_SELF="${MYPCC_SELF:-$DIR/../../build/myp_self}"
    echo "=== 双编译器对比（同一源码列表，$TARGET）==="
    for pair in "mypc:$MYPCC" "myp_self:$MYPCC_SELF"; do
        name="${pair%%:*}"; cc="${pair#*:}"
        out="$TARGET.$name"
        echo "--- $name: $cc ---"
        "$cc" "${SRCS[@]}" -o "$out" --stdlib "$STDLIB"
        ./"$out" > "/tmp/${TARGET}_${name}.out" 2>&1
        echo "OK → ./$out"
    done
    echo "=== 输出对比 ==="
    if diff "/tmp/${TARGET}_mypc.out" "/tmp/${TARGET}_myp_self.out" >/dev/null 2>&1; then
        echo "mypc 与 myp_self 输出完全一致 ✓"
    else
        echo "⚠ 输出有差异："
        diff "/tmp/${TARGET}_mypc.out" "/tmp/${TARGET}_myp_self.out" | head -20
    fi
else
    echo "mypview 框架示例编译（$MYPCC）：$TARGET"
    "$MYPCC" "${SRCS[@]}" -o "$TARGET" --stdlib "$STDLIB"
    echo "OK → ./$TARGET"
    echo "运行："
    ./"$TARGET"
fi
