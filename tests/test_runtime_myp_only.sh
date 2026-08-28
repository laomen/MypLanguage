#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MYPCC="${MYPCC:-$ROOT/build/mypc}"
OUT="$(mktemp /tmp/myp_runtime_only.XXXXXX)"
trap 'rm -f "$OUT" "$OUT.o" "$OUT.ll" "$OUT.opt.ll"' EXIT

link_out="$($MYPCC -O2 "$ROOT/tests/channel_multi_consumer/test.myp" -o "$OUT" 2>&1)"
if ! printf '%s\n' "$link_out" | grep -Fq '(MYP runtime only)'; then
    printf 'FAIL: threaded program fell back to C runtime\n%s\n' "$link_out"
    exit 1
fi

run_out="$($OUT 2>&1)"
if ! printf '%s\n' "$run_out" | grep -Fq 'PASS multi-consumer channel'; then
    printf 'FAIL: pure MYP runtime program failed\n%s\n' "$run_out"
    exit 1
fi

echo 'PASS (threaded program uses MYP runtime only)'