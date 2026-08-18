#!/bin/bash
# test_coro_stack_warn.sh — @coro(stack=N) 栈大小编译警告断言（编译期 stderr）
#
# 覆盖 design.md §8.6.2 栈行：
#   - @coro(stack=N) 以 KB 计；0/省略 = 默认 128KB
#   - 无硬性最小，N<16 仅编译警告（recommend >= 16KB），编译仍通过（exit 0）
#   - N>=16（含 0=默认）不触发警告
#
# 用法: bash tests/test_coro_stack_warn.sh   （退出码 0 = 通过）
set -u
MYPCC="${MYPCC:-./build/mypc}"
WARN_RE="very small coroutine stack"
PASS=0
FAIL=0

check() {  # check <label> <stackN> <expect_warning: 1|0>
    local label="$1" n="$2" expect="$3"
    local src=/tmp/coro_stack_warn_$$.myp
    local bin=/tmp/coro_stack_warn_$$.out
    cat > "$src" <<EOF
import env;
import coro;
class W {
    action:
        @coro(stack=$n) void w() { await; }
}
class Main {
    action:
        @constructor Main() {
            W w = new W();
            long h = w.w();          // 必须真正 spawn（警告在 spawn 处触发）
            Coro.resume(h, 0);
        }
}
int main() { Main m = new Main(); return 0; }
EOF
    local out
    out=$("$MYPCC" "$src" -o "$bin" 2>&1)
    local rc=$?
    rm -f "$src" "$bin"
    if [ $rc -ne 0 ]; then
        echo -e "\033[0;31mFAIL\033[0m $label: 编译应成功(仅警告), 实际退出码 $rc"
        echo "$out" | head -3
        FAIL=$((FAIL + 1)); return
    fi
    local got=0
    echo "$out" | grep -qE "$WARN_RE" && got=1
    if [ "$got" -eq "$expect" ]; then
        echo -e "\033[0;32mPASS\033[0m $label (stack=$n, 警告=$got)"
        PASS=$((PASS + 1))
    else
        echo -e "\033[0;31mFAIL\033[0m $label: stack=$n 期望警告=$expect 实际=$got"
        echo "$out" | head -3
        FAIL=$((FAIL + 1))
    fi
}

check "@coro(stack=8) 应发警告"        8  1
check "@coro(stack=15) 应发警告"       15 1
check "@coro(stack=16) 不应发警告"     16 0
check "@coro(stack=0)=默认 不应发警告" 0  0
check "省略=默认 不应发警告"           128 0   # 128 仅作占位：上面模板始终带 stack=N
# 覆盖省略形态：单独编译一个不带 stack 参数的 @coro（需 spawn 才触发检查）
src=/tmp/coro_stack_warn_$$_nokeep.myp
cat > "$src" <<'EOF'
import env;
import coro;
class W {
    action:
        @coro void w() { await; }
}
class Main {
    action:
        @constructor Main() {
            W w = new W();
            long h = w.w();
            Coro.resume(h, 0);
        }
}
int main() { Main m = new Main(); return 0; }
EOF
out=$("$MYPCC" "$src" -o /tmp/coro_stack_warn_$$.out 2>&1)
rc=$?
rm -f "$src" /tmp/coro_stack_warn_$$.out
if [ $rc -eq 0 ] && ! echo "$out" | grep -qE "$WARN_RE"; then
    echo -e "\033[0;32mPASS\033[0m 省略=默认 不应发警告"
    PASS=$((PASS + 1))
else
    echo -e "\033[0;31mFAIL\033[0m 省略=默认 (rc=$rc)"
    echo "$out" | head -3
    FAIL=$((FAIL + 1))
fi

echo ""
echo "coro stack warn: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
