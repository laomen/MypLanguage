#!/bin/bash
# Run the MYP test suite with -O2 (IR optimization pipeline).
# M1 of docs/optimization_debugging.md — proves the -O pipeline does not break
# any semantics (exception/coro/arena) under optimization.
#
# Usage:
#   bash tests/run_tests_O2.sh
set -euo pipefail
cd "$(dirname "$0")/.."

export MYPCC="${MYPCC:-./build/mypc -O2}"
echo "[O2] running test suite with: $MYPCC"
bash tests/run_tests.sh "$@"
exit $?
