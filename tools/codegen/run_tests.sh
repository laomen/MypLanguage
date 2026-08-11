#!/usr/bin/env bash
# run_tests.sh — tools/codegen 自测：生成 → 编译 → round-trip 验证
# 用法: bash tools/codegen/run_tests.sh
# 关联: design.md §9

set -u
cd "$(dirname "$0")"

MYPCC=${MYPCC:-../../build/mypc}
case "$MYPCC" in
    /*) : ;;
    *) MYPCC="$(cd "$(dirname "$MYPCC")" && pwd)/$(basename "$MYPCC")" ;;
esac
if [ ! -x "$MYPCC" ]; then echo "error: mypc 不存在 ($MYPCC)"; exit 1; fi

WORK=$(mktemp -d /tmp/myp_codegen_test.XXXXXX)
trap 'rm -rf "$WORK" tests/serde_gen.myp tests/ffi_gen.myp' EXIT

# 1) 生成 serde 代码
if ! "$MYPCC" run main.myp serde tests/schema.json -o "$WORK" >/dev/null 2>&1; then
    echo "FAIL: 生成 serde 代码"; exit 1
fi
cp "$WORK/serde_gen.myp" tests/serde_gen.myp

# 2) 编译 + 运行 round-trip 测试
out=$("$MYPCC" run tests/test_serde.myp 2>&1) || { echo "FAIL: 编译/运行 test_serde"; echo "$out"; exit 1; }

# 3) 校验关键输出
echo "$out" | grep -q 'json = {"name":"Alice","hp":100,"pos":{"x":1.5,"y":2.5}}' \
    || { echo "FAIL: toJson 输出不符"; echo "$out"; exit 1; }
echo "$out" | grep -q 'json2 = {"name":"say \\"hi\\"","hp":7,"pos":{"x":0,"y":0}}' \
    || { echo "FAIL: 字符串转义不符"; echo "$out"; exit 1; }
echo "$out" | grep -q "round-trip OK" \
    || { echo "FAIL: round-trip 失败"; echo "$out"; exit 1; }

# 4) ffi 生成器：生成 → 编译 → 运行
if ! "$MYPCC" run main.myp ffi tests/schema_ffi.json -o "$WORK" >/dev/null 2>&1; then
    echo "FAIL: 生成 ffi 代码"; exit 1
fi
cp "$WORK/ffi_gen.myp" tests/ffi_gen.myp
out2=$("$MYPCC" run tests/test_ffi.myp 2>&1) || { echo "FAIL: 编译/运行 test_ffi"; echo "$out2"; exit 1; }
echo "$out2" | grep -q "ffi ok exists=1 isdir=1 sqrt=8" \
    || { echo "FAIL: ffi 输出不符"; echo "$out2"; exit 1; }

# 5) 资源包装类：生成 → 编译 → 运行
if ! "$MYPCC" run main.myp ffi tests/schema_res.json -o "$WORK" >/dev/null 2>&1; then
    echo "FAIL: 生成资源代码"; exit 1
fi
cp "$WORK/ffi_gen.myp" tests/ffi_gen.myp
out3=$("$MYPCC" run tests/test_res.myp 2>&1) || { echo "FAIL: 编译/运行 test_res"; echo "$out3"; exit 1; }
echo "$out3" | grep -q "res ok h=" \
    || { echo "FAIL: 资源输出不符"; echo "$out3"; exit 1; }

# 6) 组合 schema（serde + ffi 同文件）：串行生成验证
if ! "$MYPCC" run main.myp serde tests/schema.json -o "$WORK" >/dev/null 2>&1; then
    echo "FAIL: 组合 serde"; exit 1
fi

echo "codegen 自测通过（serde + ffi + resources）"
exit 0
