#!/bin/bash
# MYP 语言内建测试套件（@test + --test）验证
# 验证: 运行器输出汇总、正常套件 exit 0、含失败断言的套件 exit 1（退出码反映失败）、
#       Test.fail 消息、扩展断言 API（long/float/空引用）。
# 用法: MYPCC=./build/mypc bash tests/test_myp_test.sh
# 退出码: 0=全过, 1=有失败

set -u
MYPCC="${MYPCC:-./build/mypc}"
PASS=0
FAIL=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

check() {
    local name="$1"; local cond="$2"
    if eval "$cond"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}

echo "myp-test: @test 语言内建测试套件"

# 1) 正常套件: 全部断言通过 → exit 0 + 汇总行
cat > "$TMPDIR/ok.myp" <<'EOF'
import test;
@test void t1() {
    Test.assertEq(2 + 2, 4);
    Test.assertStrEq("hi", "hi");
    Test.assertFloatEq(1.5, 1.5, 0.001);
    Test.report("t1", true);
}
EOF
$MYPCC --test "$TMPDIR/ok.myp" 2>/dev/null
ok_out=$(cd "$TMPDIR" && ./ok.out 2>&1); ok_ec=$?
check "正常套件 exit 0" "test $ok_ec -eq 0"
check "正常套件 汇总行" "echo \"$ok_out\" | grep -q 'assertions: 3 passed, 0 failed'"
check "正常套件 PASS 标记" "echo \"$ok_out\" | grep -q 'PASS: t1'"

# 2) 失败套件: 失败断言 → exit 1（关键: 此前恒返回 0，脚本无法感知）
cat > "$TMPDIR/bad.myp" <<'EOF'
import test;
@test void t2() {
    Test.assertEq(1, 2);
    Test.fail("deliberate");
    Test.report("t2", false);
}
EOF
$MYPCC --test "$TMPDIR/bad.myp" 2>/dev/null
bad_out=$(cd "$TMPDIR" && ./bad.out 2>&1); bad_ec=$?
check "失败套件 exit 1" "test $bad_ec -ne 0"
check "失败套件 汇总 2 failed" "echo \"$bad_out\" | grep -q 'assertions: 0 passed, 2 failed'"
check "失败套件 FAILED 消息" "echo \"$bad_out\" | grep -q 'FAILED: deliberate'"
check "失败套件 FAIL 标记" "echo \"$bad_out\" | grep -q 'FAIL: t2'"

# 3) 扩展断言 API（long/float 不等、空引用泛型）
cat > "$TMPDIR/ext.myp" <<'EOF'
import test;
class Node { action: @constructor Node() {} }
@test void t3() {
    Test.assertLongNeq(1L, 2L);
    Test.assertFloatNeq(1.0, 2.0, 0.001);
    Node p = new Node();
    Node n = null;
    Test.assertNotNull<Node>(p);
    Test.assertNull<Node>(n);
    Test.report("t3", true);
}
EOF
$MYPCC --test "$TMPDIR/ext.myp" 2>/dev/null
ext_out=$(cd "$TMPDIR" && ./ext.out 2>&1); ext_ec=$?
check "扩展断言 exit 0" "test $ext_ec -eq 0"
check "扩展断言 汇总 4 passed" "echo \"$ext_out\" | grep -q 'assertions: 4 passed, 0 failed'"

echo "myp-test PASS=$PASS FAIL=$FAIL"
[ $FAIL -eq 0 ]
