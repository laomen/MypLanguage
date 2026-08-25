#!/usr/bin/env bash
# 档B 探针：MYP → LLVM IR → obj → 静态无 libc ELF（GNU ld，无 gcc/无 CRT）。
# 用法: bash build.sh [mypc路径]   （默认 ../build/mypc）
set -euo pipefail
cd "$(dirname "$0")"
MYPCC="${1:-../../build/mypc}"
LLC="${LLC:-llc-21}"

rm -f hello_fs.ll hello_fs.o start.o hello_fs

# 1) MYP → LLVM IR（emit，不链接；--freestanding 跳过 main 收尾的运行时清理）
rm -f hello_fs.myp.ll
"$MYPCC" hello_fs.myp --emit-llvm --freestanding
IR="hello_fs.myp.ll"

# 2) LLVM IR → 目标文件（static 重定位模型，匹配无 PIE 的裸静态链接）
"$LLC" "$IR" -filetype=obj -relocation-model=static -o hello_fs.o

# 3) 汇编入口 _start
as _start.s -o start.o

# 4) 静态链接：无 CRT（-nostdlib）、无 libc、无 gcc；入口 _start
ld -nostdlib -static -e _start -o hello_fs start.o hello_fs.o

echo "== 链接完成 =="
file hello_fs
echo "--- 未定义符号（应为空或仅限自引用）---"
nm -u hello_fs || true
echo "--- 动态段（应无；静态）---"
readelf -l hello_fs 2>/dev/null | grep -c INTERP || true
