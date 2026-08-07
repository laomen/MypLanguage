#!/bin/bash
# test_myp_gitee.sh — MYP 包管理器远程 registry 端到端测试
#
# 验证（docs/pkg_manager.md §4.6/§11）：
#   myp add <pkg>          从 registry git 仓库解析 latest 并安装 + 写 myp.lock + 更新 depends
#   myp add <pkg>@<ver>    显式版本
#   myp list / update / remove
#   myp build 缺失依赖自动安装 + 首装后首次 build 可编译（回归 §11.5）
#   registry 缓存落盘（二次 add 不重复 clone）
#
# registry 来源（环境变量开关）：
#   MYP_GITEE_REGISTRY   设置则用真实远程（Gitee 或任意 git URL），如：
#                          MYP_GITEE_REGISTRY=https://gitee.com/tomatosoft_0/myplibtest.git
#                          （远端须含 mymath 与 stringutil 两包；示例见 myplibtest）
#   未设置                本地构建 file:// git 仓库离线模拟（无网络，CI 安全）
#
# 用法：bash tests/test_myp_gitee.sh
#       MYPCC=/path/to/mypc bash tests/test_myp_gitee.sh   (默认 ./build/mypc)
#       MYP_GITEE_REGISTRY=<url> bash tests/test_myp_gitee.sh   (真 Gitee)
#
# 关联：docs/pkg_manager.md、docs/self_hosting.md

set -u
MYPCC="${MYPCC:-./build/mypc}"
PASS=0
FAIL=0

say() { printf '%s\n' "$*"; }
ok()  { say "  PASS: $*"; PASS=$((PASS+1)); }
bad() { say "  FAIL: $*"; FAIL=$((FAIL+1)); }

if ! command -v git >/dev/null 2>&1; then
    say "  SKIP: git 不可用"
    say "=== summary: myp-gitee PASS=0 FAIL=0 (skipped) ==="
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# MYPCC 可能带优化标志（run_tests_O2.sh 传 "./build/mypc -O2"）→ 取首个词为编译器路径
MYPCC_BIN="${MYPCC%% *}"
MYP_ABS="$(cd "$(dirname "$MYPCC_BIN")" && pwd)/$(basename "$MYPCC_BIN")"
# 钉住编译器：myp build 在临时目录里找不到 ./build/mypc，必须经 MYP_CC 指定
# （否则回退到 PATH 上的 mypc —— 测试环境未必有，导致 "mypc: not found"）
export MYP_CC="$MYP_ABS"

# ---- 0) 编译 myp.myp ----
if ! $MYPCC tools/pm/main.myp -o "$TMP/myp" >/dev/null 2>&1; then
    bad "tools/pm/main.myp 编译失败"
    exit 1
fi
ok "tools/pm/main.myp 编译"
MYP="$TMP/myp"

# ---- registry 来源 ----
EXPECT_MY_MATH="1.1.0"      # mymath 最新版（远端须含该版本或更高）
EXPECT_STRUTIL="0.1.0"      # stringutil 显式版本
GITEE="${MYP_GITEE_REGISTRY:-}"
if [ -n "$GITEE" ]; then
    export MYP_REGISTRY="$GITEE"
    export MYP_CACHE="$TMP/cache_gitee"
    say ""
    say "--- registry: 真实远程 $GITEE ---"
else
    say ""
    say "--- registry: 本地 file:// 离线模拟 ---"
    mkdir -p "$TMP/regrepo/packages/mymath/1.0.0/src" \
             "$TMP/regrepo/packages/mymath/1.1.0/src" \
             "$TMP/regrepo/packages/stringutil/0.1.0/src"
    printf 'name: mymath\nversion: 1.0.0\n' > "$TMP/regrepo/packages/mymath/1.0.0/package.myp"
    printf 'class MyMath {\n    static:\n        int add(int a, int b) { return a + b; }\n}\n' > "$TMP/regrepo/packages/mymath/1.0.0/src/mymath.myp"
    printf 'name: mymath\nversion: 1.1.0\n' > "$TMP/regrepo/packages/mymath/1.1.0/package.myp"
    printf 'class MyMath {\n    static:\n        int add(int a, int b) { return a + b; }\n        int mul(int a, int b) { return a * b; }\n}\n' > "$TMP/regrepo/packages/mymath/1.1.0/src/mymath.myp"
    printf 'name: stringutil\nversion: 0.1.0\n' > "$TMP/regrepo/packages/stringutil/0.1.0/package.myp"
    printf 'class StrUtil {\n    static:\n        string shout(string s) { return s + "!"; }\n}\n' > "$TMP/regrepo/packages/stringutil/0.1.0/src/stringutil.myp"
    ( cd "$TMP/regrepo" && git init -q && git add -A \
      && git -c user.email=t@t -c user.name=t commit -qm init )
    export MYP_REGISTRY="file://$TMP/regrepo"
    export MYP_CACHE="$TMP/cache"
fi

# ---- 1) add（解析 latest + 写 lock + 更新 depends）----
mkdir -p "$TMP/app"
printf 'name: app\nversion: 1.0.0\n' > "$TMP/app/package.myp"
( cd "$TMP/app" && "$MYP" add mymath >/dev/null 2>&1 )
if grep -q "mymath: $EXPECT_MY_MATH" "$TMP/app/myp.lock" 2>/dev/null && grep -q "depends: mymath" "$TMP/app/package.myp"; then
    ok "add 解析 latest($EXPECT_MY_MATH) + 写 lock + 更新 depends"
else
    bad "add 失败（lock/depends）"
fi

# ---- 2) add 显式版本 ----
( cd "$TMP/app" && "$MYP" add stringutil@0.1.0 >/dev/null 2>&1 )
if grep -q "stringutil: 0.1.0" "$TMP/app/myp.lock" 2>/dev/null; then
    ok "add 显式版本 stringutil@0.1.0"
else
    bad "add 显式版本失败"
fi

# ---- 3) list ----
list_out=$( cd "$TMP/app" && "$MYP" list 2>&1 )
if printf '%s' "$list_out" | grep -q "mymath $EXPECT_MY_MATH" && printf '%s' "$list_out" | grep -q "stringutil 0.1.0"; then
    ok "list 显示两依赖"
else
    bad "list 输出异常: $list_out"
fi

# ---- 4) 已装源码 ----
if [ -f "$TMP/app/myp_packages/mymath/src/mymath.myp" ] && [ -f "$TMP/app/myp_packages/stringutil/src/stringutil.myp" ]; then
    ok "源码复制到 myp_packages/"
else
    bad "myp_packages/ 缺源码"
fi

# ---- 5) 全新项目：build 自动安装缺失依赖 + 首装后首次 build 可编译（回归 §11.5）----
mkdir -p "$TMP/app2/src"
printf 'name: app2\nversion: 1.0.0\ndepends: mymath\n' > "$TMP/app2/package.myp"
printf 'import env;\nimport mymath;\n\nclass App {\n    action:\n        @constructor App() {\n            Console.writeString("mul=");\n            Console.write(MyMath.mul(6, 7));\n            Console.writeString("\\n");\n        }\n}\n\nint main() {\n    App a = new App();\n    return 0;\n}\n' > "$TMP/app2/src/app2.myp"
auto_out=$( cd "$TMP/app2" && "$MYP" build 2>&1 )
if printf '%s' "$auto_out" | grep -q "Installed mymath v$EXPECT_MY_MATH"; then
    ok "build 自动安装缺失依赖"
else
    bad "build 自动安装失败: $auto_out"
fi
if printf '%s' "$auto_out" | grep -q "Build successful"; then
    ok "首装后首次 build 编译成功（package-path 生效）"
else
    bad "首次 build 编译失败: $auto_out"
fi
run_out=$( cd "$TMP/app2" && "$MYP" run 2>&1 )
if printf '%s' "$run_out" | grep -q "mul=42"; then
    ok "自动安装依赖可运行 (mul=42)"
else
    bad "运行失败: $run_out"
fi

# ---- 6) update（按 lock 重装并升级到 latest）----
( cd "$TMP/app" && "$MYP" update >/dev/null 2>&1 )
if grep -q "mymath: $EXPECT_MY_MATH" "$TMP/app/myp.lock" 2>/dev/null; then
    ok "update 保持/升级到 latest"
else
    bad "update 失败"
fi

# ---- 7) remove（清理 lock + depends）----
( cd "$TMP/app" && "$MYP" remove stringutil >/dev/null 2>&1 )
if ! grep -q "stringutil" "$TMP/app/myp.lock" 2>/dev/null && ! grep -q "depends: mymath, stringutil" "$TMP/app/package.myp" 2>/dev/null; then
    ok "remove 清理 lock + depends"
else
    bad "remove 未清理干净"
fi

# ---- 8) registry 缓存落盘（二次 add 不重复 clone 的依据）----
if [ -d "$MYP_CACHE/registry/packages" ]; then
    ok "registry 缓存落盘 ($MYP_CACHE/registry)"
else
    bad "registry 缓存缺失"
fi

say ""
say "=== summary: myp-gitee PASS=$PASS FAIL=$FAIL ==="
[ $FAIL -eq 0 ]
