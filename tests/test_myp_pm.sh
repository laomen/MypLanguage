#!/bin/bash
# test_myp_pm.sh — MYP 包管理器（tools/pm/main.myp）端到端测试
#
# 验证：myp.myp 可编译；init/build/run/install/legacy；依赖导入；失败退出码。
# 用法：bash tests/test_myp_pm.sh
#       MYPCC=/path/to/mypc bash tests/test_myp_pm.sh   (默认 ./build/mypc)
#
# 关联：docs/pkg_manager.md（T1）、docs/self_hosting.md

set -u
MYPCC="${MYPCC:-./build/mypc}"
PASS=0
FAIL=0

say() { printf '%s\n' "$*"; }
ok()  { say "  PASS: $*"; PASS=$((PASS+1)); }
bad() { say "  FAIL: $*"; FAIL=$((FAIL+1)); }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
MYP_ABS="$(cd "$(dirname "$MYPCC")" && pwd)/$(basename "$MYPCC")"
export MYP_CC="$MYP_ABS"

# ---- 1) 编译 myp.myp ----
if ! "$MYPCC" tools/pm/main.myp -o "$TMP/myp" >/dev/null 2>&1; then
    bad "tools/pm/main.myp 编译失败"
    exit 1
fi
ok "tools/pm/main.myp 编译"

# ---- 2) init ----
mkdir -p "$TMP/proj"
( cd "$TMP/proj" && "$TMP/myp" init pmtest >/dev/null 2>&1 )
if [ -f "$TMP/proj/pmtest/package.myp" ] && [ -f "$TMP/proj/pmtest/src/pmtest.myp" ]; then
    ok "init 生成 package.myp + src/pmtest.myp"
else
    bad "init 生成文件缺失"
fi

# ---- 3) build ----
( cd "$TMP/proj/pmtest" && "$TMP/myp" build >/dev/null 2>&1 )
if [ -f "$TMP/proj/pmtest/pmtest.out" ]; then
    ok "build 产出 pmtest.out"
else
    bad "build 未产出可执行"
fi

# ---- 4) run（验证 init 模板可编译且运行）----
out=$( cd "$TMP/proj/pmtest" && "$TMP/myp" run 2>&1 )
if printf '%s' "$out" | grep -q "Hello from pmtest v0.1.0!"; then
    ok "run 输出 Hello from pmtest"
else
    bad "run 输出异常: $out"
fi

# ---- 5) 依赖安装 + 导入 ----
mkdir -p "$TMP/mydep/src" "$TMP/app/src"
printf 'name: mydep\nversion: 0.1.0\n' > "$TMP/mydep/package.myp"
printf 'class MyDep {\n    static:\n        int add(int a, int b) { return a + b; }\n}\n' > "$TMP/mydep/src/mydep.myp"
printf 'name: app\nversion: 0.1.0\ndepends: mydep\n' > "$TMP/app/package.myp"
printf 'import env;\nimport mydep;\n\nclass App {\n    action:\n        @constructor App() {\n            Console.writeString("dep add = ");\n            Console.write(MyDep.add(2, 3));\n            Console.writeString("\\n");\n        }\n}\n\nint main() {\n    App a = new App();\n    return 0;\n}\n' > "$TMP/app/src/app.myp"
( cd "$TMP/app" && "$TMP/myp" install "$TMP/mydep" >/dev/null 2>&1 )
if [ -f "$TMP/app/myp_packages/mydep/src/mydep.myp" ]; then
    ok "install 复制依赖源码"
else
    bad "install 未复制依赖"
fi
app_out=$( cd "$TMP/app" && "$TMP/myp" run 2>&1 )
if printf '%s' "$app_out" | grep -q "dep add = 5"; then
    ok "依赖导入并运行（dep add = 5）"
else
    bad "依赖导入失败: $app_out"
fi

# ---- 6) legacy 单文件编译 ----
"$TMP/myp" examples/hello.myp -o "$TMP/hello.out" >/dev/null 2>&1
if [ -f "$TMP/hello.out" ]; then
    ok "legacy 单文件编译"
else
    bad "legacy 编译失败"
fi

# ---- 7) 失败退出码传播 ----
printf 'int main() { int x = ; return 0; }\n' > "$TMP/proj/pmtest/src/pmtest.myp"
( cd "$TMP/proj/pmtest" && "$TMP/myp" build >/dev/null 2>&1 )
if [ $? -ne 0 ]; then
    ok "编译失败退出码非 0"
else
    bad "编译失败应返回非 0 退出码"
fi

# ---- 8) help ----
"$TMP/myp" --help >/dev/null 2>&1
if [ $? -eq 0 ]; then
    ok "--help 退出码 0"
else
    bad "--help 失败"
fi

say ""
say "=== summary: myp-pm PASS=$PASS FAIL=$FAIL ==="
[ $FAIL -eq 0 ]
