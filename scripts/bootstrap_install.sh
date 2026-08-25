#!/usr/bin/env bash
# bootstrap_install.sh — 自举 MD5 门禁：把自举不动点编译器安装为用户级 mypc。
#
# 语义（"只有自举 2 级 MD5 一致才编译成功"）：
#   stage1（myp_self2，myp_self 编译自身）与 stage2（myp_self3，myp_self2 再编译）
#   字节一致（MD5 相同）⟺ 自举成立 → 安装 myp_self2 为 build/mypc；
#   MD5 不一致 → 编译器自不一致，安装失败（CMake 构建即失败）。
#
# 由 CMake 目标 mypc 调用（CMakeLists.txt myp_self2/myp_self3 之后）：
#   MYPC_S2=<myp_self2 路径> MYPC_S3=<myp_self3 路径> MYPC_OUT=<build/mypc>
#   bash scripts/bootstrap_install.sh
set -euo pipefail

S2="${MYPC_S2:?MYPC_S2 未设置}"
S3="${MYPC_S3:?MYPC_S3 未设置}"
OUT="${MYPC_OUT:?MYPC_OUT 未设置}"

H2=$(md5sum "$S2" | awk '{print $1}')
H3=$(md5sum "$S3" | awk '{print $1}')

echo "== bootstrap: myp_self2 md5=$H2"
echo "== bootstrap: myp_self3 md5=$H3"

if [ "$H2" != "$H3" ]; then
    echo "ERROR: 自举不动点 MD5 不一致（myp_self2 != myp_self3）——编译失败。"
    echo "      编译器自不一致（self-host 不成立），mypc 不安装。"
    exit 1
fi

cp -f "$S2" "$OUT"
chmod +x "$OUT"
echo "== 自举成立（2 级 MD5 一致），mypc = myp_self2 = $OUT"
