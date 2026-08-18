#!/bin/bash
# 多文件编译回归（manual.md §12 编译器章节核对）
# 验证: 多文件合并为单模块（函数跨文件调用）、第二个文件的 import 被加载
#       （BUG-025：原实现只合并 classes/interfaces/mappings/functions，漏了
#       imports/structs/enums/ffis 等，第二文件的 `import env` 直接 Console
#       未定义）、struct/enum 跨文件可见、多文件 + `--test` + 用户 main 不崩
#       （BUG-026：用户 main 空块无 terminator → LLVM verify 失败，且运行器
#       main 被改名 main.1 静默不跑）。
# 用法: MYPCC=./build/mypc bash tests/test_multifile.sh
# 退出码: 0=全过, 1=有失败

set -u
MYPCC="${MYPCC:-./build/mypc}"
# Resolve to absolute so it keeps working after `cd` into the temp dir.
case "$MYPCC" in
    /*) ;;
    *) MYPCC="$(pwd)/$MYPCC" ;;
esac
PASS=0
FAIL=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
ORIGDIR=$(pwd)

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

echo "multifile: 多文件编译（§12）"

# 1) 多文件基础：函数跨文件调用（无 import）
cat > "$TMPDIR/helper.myp" <<'EOF'
int helperAdd(int x, int y) { return x + y; }
EOF
cat > "$TMPDIR/main.myp" <<'EOF'
int main() { return helperAdd(3, 4) == 7 ? 0 : 1; }
EOF
if $MYPCC "$TMPDIR/helper.myp" "$TMPDIR/main.myp" -o "$TMPDIR/basic.out" 2>"$TMPDIR/basic.err"; then
    (cd "$TMPDIR" && ./basic.out); basic_ec=$?
    check "多文件函数跨文件调用 exit 0" "test $basic_ec -eq 0"
else
    check "多文件函数跨文件调用 exit 0" "false"
    echo "      $(head -2 "$TMPDIR/basic.err")"
fi

# 2) BUG-025：第二个文件的 `import env` 必须被加载（Console 可用）
#    （main() 内不允许直接函数调用，故经类 @constructor 输出）
cat > "$TMPDIR/f2_import.myp" <<'EOF'
import env;
class F2Main {
    action:
        @constructor F2Main() {
            Console.writeString("mf-import-ok\n");
        }
}
int main() { F2Main m = new F2Main(); return 0; }
EOF
if $MYPCC "$TMPDIR/helper.myp" "$TMPDIR/f2_import.myp" -o "$TMPDIR/imp.out" 2>"$TMPDIR/imp.err"; then
    imp_out=$(cd "$TMPDIR" && ./imp.out 2>&1)
    check "BUG-025 第二文件 import env 合并" "test \"\$imp_out\" = \"mf-import-ok\""
else
    check "BUG-025 第二文件 import env 合并" "false"
    echo "      $(head -2 "$TMPDIR/imp.err")"
fi

# 3) BUG-025：struct/enum 在第二个文件可见
cat > "$TMPDIR/f3_decl.myp" <<'EOF'
struct Vec3 { int x; int y; }
enum Color3 { RED; GREEN; }
EOF
cat > "$TMPDIR/f3_use.myp" <<'EOF'
import test;
@test void t_multifile_struct_enum() {
    Vec3 v;
    v.x = 3;
    v.y = 4;
    Color3 c = Color3.GREEN;
    string nm = "";
    match (c) {
        Color3.RED => { nm = "RED"; }
        Color3.GREEN => { nm = "GREEN"; }
    }
    Test.assertEq(v.x + v.y, 7, "multi-file struct");
    Test.assertStrEq(nm, "GREEN", "multi-file enum");
}
int main() { return 0; }
EOF
if $MYPCC --test "$TMPDIR/f3_decl.myp" "$TMPDIR/f3_use.myp" -o "$TMPDIR/se.out" 2>"$TMPDIR/se.err"; then
    se_out=$(cd "$TMPDIR" && ./se.out 2>&1)
    if echo "$se_out" | grep -q "tests: 1, assertions: 2 passed, 0 failed"; then
        check "BUG-025 struct/enum 第二文件可见" "true"
    else
        check "BUG-025 struct/enum 第二文件可见" "false"
        echo "      $se_out" | tail -2
    fi
else
    check "BUG-025 struct/enum 第二文件可见" "false"
    echo "      $(head -2 "$TMPDIR/se.err")"
fi

# 4) BUG-026：多文件 + `--test` + 用户 main → 不崩 + 运行器执行
cat > "$TMPDIR/f4_test.myp" <<'EOF'
import test;
@test void t_cross_file() {
    Test.assertEq(helperAdd(40, 2), 42, "cross-file call");
}
int main() { return 0; }
EOF
if $MYPCC --test "$TMPDIR/helper.myp" "$TMPDIR/f4_test.myp" -o "$TMPDIR/tm.out" 2>"$TMPDIR/tm.err"; then
    tm_out=$(cd "$TMPDIR" && ./tm.out 2>&1)
    if echo "$tm_out" | grep -q "tests: 1, assertions: 1 passed, 0 failed" \
       && echo "$tm_out" | grep -q "RUN: t_cross_file"; then
        check "BUG-026 --test + 用户 main 运行器执行" "true"
    else
        check "BUG-026 --test + 用户 main 运行器执行" "false"
        echo "      $tm_out" | tail -3
    fi
else
    check "BUG-026 --test + 用户 main 运行器执行" "false"
    echo "      $(head -2 "$TMPDIR/tm.err")"
fi

echo ""
echo "multifile: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
