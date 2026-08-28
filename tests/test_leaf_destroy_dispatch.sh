#!/bin/bash
# Leaf classes dispatch directly to myp_free_object; owning classes retain
# generated destroy stubs that cascade their reference fields.

set -u
MYPCC="${MYPCC:-./build/mypc}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cp tests/@test/esc_escape.myp "$TMP/leaf.myp"
"$MYPCC" "$TMP/leaf.myp" --test --emit-llvm -o "$TMP/leaf" \
    >/dev/null 2>&1 || exit 1
IR="$TMP/leaf.ll"
if [ ! -f "$IR" ]; then IR="$TMP/leaf.myp.ll"; fi

table=$(grep '@__myp_release_table = global' "$IR" | head -1)
if [[ "$table" != *'ptr @myp_free_object'* ]]; then
    echo "FAIL: release table has no direct leaf-class free entry"
    exit 1
fi
if [[ "$table" != *'ptr @__myp_destroy_Holder'* ]]; then
    echo "FAIL: reference-owning class lost its destroy stub"
    exit 1
fi
if grep -q 'define internal void @__myp_destroy_Node' "$IR"; then
    echo "FAIL: leaf class still emits an unreachable destroy stub"
    exit 1
fi

echo "PASS (leaf class direct destroy dispatch)"