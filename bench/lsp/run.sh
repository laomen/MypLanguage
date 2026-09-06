#!/usr/bin/env bash
# bench/lsp/run.sh — LSP 性能基准（roadmap 5.3）：5k 类 hover/completion 吞吐
# + 区分「计算」vs「写出」。
#
# 前置: build/myp_lsp 已构建（含 MYP_LSP_TIMING 逐消息 handler 计时钩子，
#       src/lsp/lsp_server.cpp；`cmake --build build --target myp_lsp`）。
# 用法: bash bench/lsp/run.sh [nclass] [nhover]
# 退出码: 0 = 基准跑通且校验 PASS
set -u
cd "$(dirname "$0")"
ROOT=../..
MYPC="$ROOT/build/myp_lsp"
NCLASS="${1:-5000}"
NHOVER="${2:-120}"
[ -x "$MYPC" ] || { echo "缺 $MYPC（先 cmake --build build --target myp_lsp）"; exit 2; }
which node >/dev/null 2>&1 || { echo "缺 node（LSP 基准驱动用）"; exit 2; }
echo "== bench/lsp: 生成 ${NCLASS} 类合成文件并驱动 myp_lsp =="
MYP_LSP="$MYPC" node driver.js "$NCLASS" "$NHOVER"
exit $?
