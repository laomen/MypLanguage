#!/usr/bin/env bash
# 档B 探针：一条命令 —— MYP → 静态无 libc ELF。
# mypc --freestanding 现在自足：codegen 发射 _start 入口 + 内部链接走
# `ld -nostdlib -static -e _start`（无 gcc / 无 CRT / 无 libc）。
# 用法: bash build.sh [mypc路径]   （默认 ../../build/mypc）
set -euo pipefail
cd "$(dirname "$0")"
MYPCC="${1:-../../build/mypc}"

rm -f hello_fs hello_fs.myp.o hello_fs.myp.ll

"$MYPCC" hello_fs.myp --freestanding -o hello_fs

echo "== 链接完成 =="
file hello_fs
echo "--- 未定义符号（应为空）---"
nm -u hello_fs || true
echo "--- 动态段（应无；静态）---"
readelf -l hello_fs 2>/dev/null | grep -c INTERP || true
echo "--- 运行 ---"
./hello_fs
echo "exit=$?"
