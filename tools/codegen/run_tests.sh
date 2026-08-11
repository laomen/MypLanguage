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
export MYP_CC="$MYPCC"   # 让 main.myp --verify 的子进程定位到同一编译器

WORK=$(mktemp -d /tmp/myp_codegen_test.XXXXXX)
trap 'rm -rf "$WORK" tests/serde_gen.myp tests/ffi_gen.myp tests/autodiff_gen.myp tests/idl_gen.myp tests/orm_gen.myp tests/embed_gen.myp tests/dsl_gen.myp' EXIT

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

# 6) autodiff 符号求导：生成 → 编译 → 数值梯度验证
if ! "$MYPCC" run main.myp autodiff tests/schema_autodiff.json -o "$WORK" >/dev/null 2>&1; then
    echo "FAIL: 生成 autodiff 代码"; exit 1
fi
cp "$WORK/autodiff_gen.myp" tests/autodiff_gen.myp
out4=$("$MYPCC" run tests/test_autodiff.myp 2>&1) || { echo "FAIL: 编译/运行 test_autodiff"; echo "$out4"; exit 1; }
echo "$out4" | grep -q "autodiff ok f1=13 g0=6 g1=1" \
    || { echo "FAIL: autodiff 输出不符"; echo "$out4"; exit 1; }

# 7) IDL：生成 → 编译 → JSON-RPC 协议层验证
if ! "$MYPCC" run main.myp idl tests/schema_idl.json -o "$WORK" >/dev/null 2>&1; then
    echo "FAIL: 生成 idl 代码"; exit 1
fi
cp "$WORK/idl_gen.myp" tests/idl_gen.myp
out5=$("$MYPCC" run tests/test_idl.myp 2>&1) || { echo "FAIL: 编译/运行 test_idl"; echo "$out5"; exit 1; }
echo "$out5" | grep -q "idl ok add=7 echo=hi! mul3=7.5" \
    || { echo "FAIL: idl 输出不符"; echo "$out5"; exit 1; }

# 8) IDL socket 传输：真实 TCP 回环（@thread 服务器线程）
out6=$("$MYPCC" run tests/test_idl_socket.myp 2>&1) || { echo "FAIL: 编译/运行 test_idl_socket"; echo "$out6"; exit 1; }
echo "$out6" | grep -q "idl_socket ok add=7 echo=hi! mul3=7.5" \
    || { echo "FAIL: idl socket 输出不符"; echo "$out6"; exit 1; }

# 9) ORM：tables schema → 实体 struct + CRUD SQL 生成验证
if ! "$MYPCC" run main.myp orm tests/schema_orm.json -o "$WORK" >/dev/null 2>&1; then
    echo "FAIL: 生成 orm 代码"; exit 1
fi
cp "$WORK/orm_gen.myp" tests/orm_gen.myp
out7=$("$MYPCC" run tests/test_orm.myp 2>&1) || { echo "FAIL: 编译/运行 test_orm"; echo "$out7"; exit 1; }
echo "$out7" | grep -q "orm ok" \
    || { echo "FAIL: orm 输出不符"; echo "$out7"; exit 1; }

# 10) 资源嵌入：文件 → 字符串常量（字节级 round-trip）
if ! "$MYPCC" run main.myp embed tests/schema_embed.json -o "$WORK" >/dev/null 2>&1; then
    echo "FAIL: 生成 embed 代码"; exit 1
fi
cp "$WORK/embed_gen.myp" tests/embed_gen.myp
out8=$("$MYPCC" run tests/test_embed.myp 2>&1) || { echo "FAIL: 编译/运行 test_embed"; echo "$out8"; exit 1; }
echo "$out8" | grep -q "embed ok" \
    || { echo "FAIL: embed 输出不符"; echo "$out8"; exit 1; }

# 11) --verify：生成后自动编译校验（--emit-llvm 走完 codegen 跳过链接）
out9=$("$MYPCC" run main.myp orm tests/schema_orm.json -o "$WORK" --verify 2>&1) \
    || { echo "FAIL: --verify 命令失败"; echo "$out9"; exit 1; }
echo "$out9" | grep -q "verify OK" \
    || { echo "FAIL: --verify 未报 verify OK"; echo "$out9"; exit 1; }

# 12) DSL：运算符表 schema → 词法 + 优先级爬升解析 + 求值
if ! "$MYPCC" run main.myp dsl tests/schema_dsl.json -o "$WORK" >/dev/null 2>&1; then
    echo "FAIL: 生成 dsl 代码"; exit 1
fi
cp "$WORK/dsl_gen.myp" tests/dsl_gen.myp
outA=$("$MYPCC" run tests/test_dsl.myp 2>&1) || { echo "FAIL: 编译/运行 test_dsl"; echo "$outA"; exit 1; }
echo "$outA" | grep -q "dsl ok" \
    || { echo "FAIL: dsl 输出不符"; echo "$outA"; exit 1; }

echo "codegen 自测通过（serde + ffi + resources + autodiff + idl + idl_socket + orm + embed + --verify + dsl）"
exit 0
