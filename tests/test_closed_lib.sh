#!/bin/bash
# 闭源分发回归（预编译库 .so/.a + FFI 封装，2026-08-20）
# ---------------------------------------------------------------------------
# 验证: bridge 机制支持预编译库（.so/.a）——分发目录只有 <secret.so + 封装
#       .myp>（无 .c 源码），用户程序 import 封装，MYP_BRIDGES 指向库目录，
#       mypc 按「用户未定义符号 ∩ 库已定义符号」自动链接。核心算法闭源。
# 覆盖: 现有 .c bridge（sdl/ttf）不被破坏；.so 动态符号（nm -D）匹配；
#       静态 .a 也能链接。
# 用法: MYPCC=./build/mypc bash tests/test_closed_lib.sh
# 退出码: 0=全过, 1=有失败

set -u
MYPCC="${MYPCC:-./build/mypc}"
case "$MYPCC" in
    /*) ;;
    *) MYPCC="$(pwd)/$MYPCC" ;;
esac
PASS=0
FAIL=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

TMP="$(mktemp -d /tmp/myp_closedlib_XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
DIST="$TMP/dist"
mkdir -p "$DIST"

# ---- 1. 核心算法 C 源码（闭源）：只编译进 .so，不进入分发目录 ----
cat > "$TMP/secret.c" <<'EOF'
#include <stdint.h>
int myp_secret_mul(int a, int b) { return a * b * 2; }
int myp_secret_verify(int key) { return (key == 12345) ? 1 : 0; }
EOF
gcc -shared -fPIC -O2 "$TMP/secret.c" -o "$DIST/secret.so" || { bad "gcc 编译 secret.so"; exit 1; }

# ---- 2. 封装 .myp（只含 ffi 声明 + 薄封装类）----
cat > "$DIST/api.myp" <<'EOF'
ffi int myp_secret_mul(int a, int b);
ffi int myp_secret_verify(int key);

class Secret {
    static:
        int mul(int a, int b) { return myp_secret_mul(a, b); }
        int verify(int key) { return myp_secret_verify(key); }
}
EOF

# ---- 3. 用户程序：import 封装 + 调核心 ----
cat > "$DIST/main.myp" <<'EOF'
import env;
import "./api.myp";

class App {
    action:
        @constructor App() {
            Console.writeLine("mul(3,4)=" + Secret.mul(3, 4));
            Console.writeLine("verify(12345)=" + Secret.verify(12345));
            Console.writeLine("verify(1)=" + Secret.verify(1));
        }
}
int main() { App a = new App(); return 0; }
EOF

# ---- 4. MYP_BRIDGES 指向分发目录（只含 .so，无 .c）编译链接运行 ----
OUT="$(MYP_BRIDGES="$DIST" "$MYPCC" "$DIST/main.myp" -o "$TMP/main" \
      --stdlib "$(cd "$(dirname "$MYPCC")/../stdlib" 2>/dev/null && pwd || echo stdlib)" 2>&1)"
if echo "$OUT" | grep -q "Link OK"; then ok "闭源库链接（MYP_BRIDGES 自动链 secret.so）"; else bad "链接失败: $OUT"; fi

RUN="$(LD_LIBRARY_PATH="$DIST" "$TMP/main" 2>&1)"
echo "$RUN" | grep -q "mul(3,4)=24" && ok "mul 来自 .so（3*4*2=24）" || bad "mul 输出: $RUN"
echo "$RUN" | grep -q "verify(12345)=1" && ok "verify 正确密钥=1" || bad "verify12345: $RUN"
echo "$RUN" | grep -q "verify(1)=0" && ok "verify 错误密钥=0" || bad "verify1: $RUN"

# ---- 5. 闭源保证：分发目录无 .c 源码 ----
if [ -z "$(ls "$DIST"/*.c 2>/dev/null)" ]; then
    ok "闭源：分发目录不含 .c 源码（仅 api.myp + secret.so）"
else
    bad "泄露：分发目录含 .c"
fi

# ---- 6. 静态库 .a 同样可链接 ----
gcc -O2 -c "$TMP/secret.c" -o "$TMP/secret.o"
ar rcs "$DIST/secret.a" "$TMP/secret.o"
OUT2="$(MYP_BRIDGES="$DIST" "$MYPCC" "$DIST/main.myp" -o "$TMP/main_a" \
       --stdlib "$(cd "$(dirname "$MYPCC")/../stdlib" 2>/dev/null && pwd || echo stdlib)" 2>&1)"
if echo "$OUT2" | grep -q "Link OK"; then
    RUN2="$("$TMP/main_a" 2>&1)"
    echo "$RUN2" | grep -q "mul(3,4)=24" && ok "静态 .a 链接可用" || bad ".a 运行: $RUN2"
else
    bad ".a 链接失败: $OUT2"
fi

# ---- 7. MYP 源码闭源：secret.myp → mypc --shared → .so + 签名分发 ----
# 核心算法本身用 MYP 写，编译成 .so；分发「签名 .myp（无 body）+ .so」，
# 用户 import 签名，MYP_BRIDGES 链接 .so。签名方法生成外部声明（codegen
# 对无 body 方法不再生成 stub），链接器从 .so 解析。
MDIST="$TMP/mdist"
mkdir -p "$MDIST"
cat > "$TMP/secret.myp" <<'EOF'
class Secret {
    static:
        int mul(int a, int b) { return a * b * 2; }
        int verify(int key) { return key == 12345 ? 1 : 0; }
        string greet(string name) { return "hi " + name; }
}
EOF
"$MYPCC" "$TMP/secret.myp" --shared -o "$MDIST/libmypsecret.so" \
    --stdlib "$(cd "$(dirname "$MYPCC")/../stdlib" 2>/dev/null && pwd || echo stdlib)" >/dev/null 2>&1 \
    && ok "MYP 源码编译 .so（mypc --shared）" || bad "MYP .so 编译失败"
cat > "$MDIST/sig.myp" <<'EOF'
class Secret {
    static:
        int mul(int a, int b);
        int verify(int key);
        string greet(string name);
}
EOF
cat > "$MDIST/mmain.myp" <<'EOF'
import env;
import "./sig.myp";

class App {
    action:
        @constructor App() {
            Console.writeLine("m-mul=" + Secret.mul(3, 4));
            Console.writeLine("m-verify=" + Secret.verify(12345));
            Console.writeLine("m-greet=" + Secret.greet("Bob"));
        }
}
int main() { App a = new App(); return 0; }
EOF
MOUT="$(MYP_BRIDGES="$MDIST" "$MYPCC" "$MDIST/mmain.myp" -o "$TMP/mmain" \
       --stdlib "$(cd "$(dirname "$MYPCC")/../stdlib" 2>/dev/null && pwd || echo stdlib)" 2>&1)"
if echo "$MOUT" | grep -q "Link OK"; then ok "MYP 签名 + MYP_BRIDGES 链接 .so"; else bad "MYP 链接失败: $MOUT"; fi
MRUN="$("$TMP/mmain" 2>&1)"
echo "$MRUN" | grep -q "m-mul=24" && ok "MYP .so 核心 mul=24" || bad "m-mul: $MRUN"
echo "$MRUN" | grep -q "m-verify=1" && ok "MYP .so 核心 verify=1" || bad "m-verify: $MRUN"
echo "$MRUN" | grep -q "m-greet=hi Bob" && ok "MYP .so 核心 greet=hi Bob" || bad "m-greet: $MRUN"
if [ ! -f "$MDIST/secret.myp" ]; then
    ok "MYP 闭源：分发目录无实现 secret.myp（仅签名 sig.myp + .so）"
else
    bad "MYP 泄露：分发目录含实现 secret.myp"
fi

echo "closed-lib: $PASS pass, $FAIL fail"
[ "$FAIL" -eq 0 ]
