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
# MYPCC 可能带优化标志（run_tests_O2.sh 传 "./build/mypc -O2"）→ 取首个词为编译器路径
MYPCC_BIN="${MYPCC%% *}"
MYP_ABS="$(cd "$(dirname "$MYPCC_BIN")" && pwd)/$(basename "$MYPCC_BIN")"
export MYP_CC="$MYP_ABS"

# ---- 1) 编译 myp.myp ----
if ! $MYPCC tools/pm/main.myp -o "$TMP/myp" >/dev/null 2>&1; then
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

# ---- 9) v2: registry add/list/update/remove + build 自动安装 ----
say ""
say "--- v2 registry ---"
mkdir -p "$TMP/reg/packages/mymath/0.1.0/src" "$TMP/reg/packages/mymath/0.2.0/src"
printf 'name: mymath\nversion: 0.1.0\n' > "$TMP/reg/packages/mymath/0.1.0/package.myp"
printf 'class MyMath {\n    static:\n        int add(int a, int b) { return a + b; }\n}\n' > "$TMP/reg/packages/mymath/0.1.0/src/mymath.myp"
printf 'name: mymath\nversion: 0.2.0\n' > "$TMP/reg/packages/mymath/0.2.0/package.myp"
printf 'class MyMath {\n    static:\n        int add(int a, int b) { return a + b; }\n        int mul(int a, int b) { return a * b; }\n}\n' > "$TMP/reg/packages/mymath/0.2.0/src/mymath.myp"
export MYP_REGISTRY="$TMP/reg"
mkdir -p "$TMP/regapp"
printf 'name: regapp\nversion: 1.0.0\n' > "$TMP/regapp/package.myp"

# add（解析最新 + 写 lock + 更新 depends）
( cd "$TMP/regapp" && "$TMP/myp" add mymath >/dev/null 2>&1 )
if grep -q "mymath: 0.2.0" "$TMP/regapp/myp.lock" 2>/dev/null && grep -q "depends: mymath" "$TMP/regapp/package.myp"; then
    ok "add 解析最新版本 + 写 lock + 更新 depends"
else
    bad "add 失败"
fi

# list
list_out=$( cd "$TMP/regapp" && "$TMP/myp" list 2>&1 )
if printf '%s' "$list_out" | grep -q "mymath 0.2.0"; then
    ok "list 显示锁定依赖"
else
    bad "list 输出异常: $list_out"
fi

# build 自动安装（全新项目：depends 有但 myp_packages 空）
mkdir -p "$TMP/regapp2/src"
printf 'name: regapp2\nversion: 1.0.0\ndepends: mymath\n' > "$TMP/regapp2/package.myp"
printf 'import env;\nimport mymath;\n\nclass App {\n    action:\n        @constructor App() {\n            Console.writeString("mul=");\n            Console.write(MyMath.mul(6, 7));\n            Console.writeString("\\n");\n        }\n}\n\nint main() {\n    App a = new App();\n    return 0;\n}\n' > "$TMP/regapp2/src/regapp2.myp"
auto_out=$( cd "$TMP/regapp2" && "$TMP/myp" build 2>&1 )
if printf '%s' "$auto_out" | grep -q "Installed mymath v0.2.0"; then
    ok "build 自动安装缺失依赖"
else
    bad "build 自动安装失败: $auto_out"
fi
# 关键回归：首次 build（myp_packages 尚不存在）必须能编译成功。
# 曾在 resolvePackagePath 先于自动安装求值时漏传 --package-path → cannot find import
# （§11.5）。只查 "Installed" 会漏掉该 bug（run 二次 build 时 myp_packages 已存在）。
if printf '%s' "$auto_out" | grep -q "Build successful"; then
    ok "首装后首次 build 编译成功（package-path 生效）"
else
    bad "首次 build 编译失败: $auto_out"
fi
run_out=$( cd "$TMP/regapp2" && "$TMP/myp" run 2>&1 )
if printf '%s' "$run_out" | grep -q "mul=42"; then
    ok "自动安装依赖可运行 (mul=42)"
else
    bad "运行失败: $run_out"
fi

# remove（清理 lock + depends）
( cd "$TMP/regapp" && "$TMP/myp" remove mymath >/dev/null 2>&1 )
if ! grep -q "mymath" "$TMP/regapp/myp.lock" 2>/dev/null && ! grep -q "depends: mymath" "$TMP/regapp/package.myp" 2>/dev/null; then
    ok "remove 清理 lock + depends"
else
    bad "remove 未清理干净"
fi

# ---- 10) v2 git-clone registry（file:// 离线模拟远程仓库 / Gitee）----
if command -v git >/dev/null 2>&1; then
    say ""
    say "--- v2 git-clone registry ---"
    ( cd "$TMP" && rm -rf regrepo && mkdir -p regrepo/packages/foo/1.0.0/src \
        && printf 'name: foo\nversion: 1.0.0\n' > regrepo/packages/foo/1.0.0/package.myp \
        && printf 'class Foo {\n    static:\n        int v() { return 1; }\n}\n' > regrepo/packages/foo/1.0.0/src/foo.myp \
        && cd regrepo && git init -q && git add -A \
        && git -c user.email=t@t.com -c user.name=t commit -qm init )
    export MYP_REGISTRY="file://$TMP/regrepo"
    export MYP_CACHE="$TMP/cache"
    rm -rf "$TMP/cloneapp"; mkdir -p "$TMP/cloneapp"
    printf 'name: cloneapp\nversion: 1.0.0\n' > "$TMP/cloneapp/package.myp"
    ( cd "$TMP/cloneapp" && "$TMP/myp" add foo >/dev/null 2>&1 )
    if [ -f "$TMP/cache/registry/packages/foo/1.0.0/package.myp" ] && [ -f "$TMP/cloneapp/myp_packages/foo/src/foo.myp" ]; then
        ok "git-clone registry 拉取（file:// 离线模拟）"
    else
        bad "git-clone registry 拉取失败"
    fi
    unset MYP_REGISTRY MYP_CACHE
fi

say ""
say "=== summary: myp-pm PASS=$PASS FAIL=$FAIL ==="
[ $FAIL -eq 0 ]
