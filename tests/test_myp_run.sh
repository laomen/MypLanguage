#!/bin/bash
# MYP `mypc run` 测试（§六-5：仿 go run + 单类文件自动 main）
# 用法: MYPCC=./build/mypc bash tests/test_myp_run.sh
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

echo "myp-run: mypc run 测试"

# 1) 单类文件（无 main）带 @startup → run 自动 main 并输出
cat > "$TMPDIR/hello.myp" <<'EOF'
import env;
class Hello {
    action:
        @startup void go() {
            Console.writeString("hello-run sum=" + (2 + 3));
            Console.writeString("\n");
        }
}
EOF
out1=$("$MYPCC" run "$TMPDIR/hello.myp" 2>&1)
ec1=$?
check "单类@startup run 输出" "echo \"$out1\" | grep -q 'hello-run sum=5'"
check "单类@startup run 退出码0" "test $ec1 -eq 0"

# 2) 无 main 且无 @startup → 报错
cat > "$TMPDIR/noentry.myp" <<'EOF'
import env;
class A { action: void f() {} }
EOF
out2=$("$MYPCC" run "$TMPDIR/noentry.myp" 2>&1)
ec2=$?
check "无@startup 报错提示" "echo \"$out2\" | grep -q '@startup'"
check "无@startup 非零退出" "test $ec2 -ne 0"

# 3) args 透传（经构造器，避开 main() 直接调用限制）
cat > "$TMPDIR/args.myp" <<'EOF'
import env;
class Args {
    action:
        void Args(int ac, string[] av) {
            Console.writeString("argc=" + ac + " a1=" + av[1] + " a2=" + av[2]);
            Console.writeString("\n");
        }
}
int main(int argc, string[] argv) {
    Args a = new Args(argc, argv);
    return 0;
}
EOF
out3=$("$MYPCC" run "$TMPDIR/args.myp" alpha beta 2>&1)
ec3=$?
check "args 透传输出" "echo \"$out3\" | grep -q 'argc=3 a1=alpha a2=beta'"
check "args 透传退出码0" "test $ec3 -eq 0"

# 4) 正常编译（非 run）无 main → 仍链接失败（行为不变）
cat > "$TMPDIR/nomain.myp" <<'EOF'
class B { action: void g() {} }
EOF
out4=$("$MYPCC" "$TMPDIR/nomain.myp" -o "$TMPDIR/nomain.out" 2>&1)
ec4=$?
check "正常编译无 main 链接失败" "test $ec4 -ne 0"

# 5) run 后无残留临时产物（myp_run_* / <file>.myp.o）
leftover=$(ls /tmp/myp_run_* "$TMPDIR/hello.myp.o" 2>/dev/null | wc -l)
check "无残留临时产物" "test \"$leftover\" -eq 0"

echo ""
echo "myp-run PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
