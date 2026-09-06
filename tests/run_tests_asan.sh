#!/bin/bash
# Run the MYP test suite under AddressSanitizer + UndefinedBehaviorSanitizer.
#
# Requires the ASan build of the compiler:
#   cmake -B build-asan -DCMAKE_PREFIX_PATH=/usr/lib/llvm-21/lib/cmake/llvm -DMYP_SANITIZE=ON
#   cmake --build build-asan -j$(nproc)
#
# Usage:
#   bash tests/run_tests_asan.sh [--update]
#
# Notes:
#   - detect_leaks=0: LLVM (linked un-instrumented) reports its own globals as
#     leaks; we disable leak detection but keep all buffer-overflow / use-after-
#     free / UBSan checks (the point of the sanitizer build).
#   - MYP_SANITIZE=1 makes mypc also instrument the generated .out programs.
set -euo pipefail
cd "$(dirname "$0")/.."

export MYPCC="${MYPCC:-./build-asan/mypc}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
export MYP_SANITIZE=1

if [ ! -x "$MYPCC" ]; then
    echo "[ASAN] compiler not found: $MYPCC"
    echo "  Build it first:"
    echo "    cmake -B build-asan -DCMAKE_PREFIX_PATH=/usr/lib/llvm-21/lib/cmake/llvm -DMYP_SANITIZE=ON"
    echo "    cmake --build build-asan -j\$(nproc)"
    exit 1
fi

echo "[ASAN] running test suite with $MYPCC (MYP_SANITIZE=1, ASAN_OPTIONS=$ASAN_OPTIONS)"
bash tests/run_tests.sh "$@"
# 退出码直接透传 run_tests.sh（其内置 ASAN makecontext 警告过滤等最新逻辑）。
# 注意：此脚本尾部曾内嵌 run_tests.sh 的旧副本，已废弃不可达，勿再依赖。
exit $?
