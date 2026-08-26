#!/usr/bin/env bash
# gen_torture.sh — MYP 编译器 Torture Test 生成器（仿 GCC torture suite 方法）
#
# 生成大量小、聚焦、自验证的 .myp 测试，覆盖表达式/控制流/字符串/数组/对象/
# 深度嵌套/大 match/多类，机械性组合变体。分两类：
#   execute/  — 自验证（返回 0 = 通过，含 "PASS <name>"）
#   compile/  — 仅编译压力（深嵌套/大 match/多类，编译器不得崩溃）
#
# 用法: bash tests/torture/gen_torture.sh [输出目录] [每类数量]
#   默认输出 tests/torture/generated/，每类默认 8 个
set -u
cd "$(dirname "$0")/../.."
OUT="${1:-tests/torture/generated}"
COUNT="${2:-8}"
rm -rf "$OUT/execute" "$OUT/compile" "$OUT/deep"
mkdir -p "$OUT/execute" "$OUT/compile" "$OUT/deep"
echo "== 生成 torture 测试 → $OUT (每类 $COUNT 个)"

n=$COUNT
i=0
while [ $i -lt $n ]; do
    seed=$((i * 131 + 17))
    # ---------- execute: 算术恒等式（自验证，包类构造） ----------
    {
        echo "import env;"
        echo "// expr_arith_$i : 算术恒等式自验证（seed=$seed）"
        echo "class T$i {"
        echo "    action:"
        echo "        @constructor T$i() {"
        echo "            int n = $(( (i % 10) * 50 + 40 ));"
        echo "            long sumOdd = 0;"
        echo "            for (int k = 0; k < n; k = k + 1) sumOdd = sumOdd + (long(k) * 2 + 1);"
        echo "            long sq = long(n) * long(n);"
        echo "            if (sumOdd != sq) { Console.writeString(\"FAIL sumOdd \" + sumOdd + \" != \" + sq + \"\\n\"); return; }"
        echo "            long fact = 1;"
        echo "            for (int k = 1; k <= n; k = k + 1) fact = fact * long(k);"
        echo "            long factRef = 1;"
        echo "            for (int k = 1; k < n; k = k + 1) factRef = factRef * long(k + 1);"
        echo "            if (fact != factRef) { Console.writeString(\"FAIL fact\\n\"); return; }"
        echo "            long x = long($seed) * 2654435761;"
        echo "            if ((x ^ x) != 0) { Console.writeString(\"FAIL xor\\n\"); return; }"
        echo "            if ((x >> 7) != (x / 128)) { Console.writeString(\"FAIL shift\\n\"); return; }"
        echo "            Console.writeString(\"PASS expr_arith_$i\\n\");"
        echo "        }"
        echo "}"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/execute/expr_arith_$i.myp"

    # ---------- execute: 深算术表达式 ----------
    d=$((i % 6 + 3))
    {
        echo "import env;"
        echo "// expr_deep_$i : 深度 $d 算术表达式（seed=$seed）"
        echo "class T$i {"
        echo "    static:"
        echo "        long deepEval(long v) {"
        printf '            long e = v;'
        j=0
        while [ $j -lt $d ]; do
            printf ' e = ((e * %d + %d) %% %d);' $(( (seed + j * 3) % 97 + 2 )) $(( (seed + j * 5) % 31 )) $(( (seed + j * 7) % 90 + 10 ))
            j=$((j+1))
        done
        printf ' return e;'
        echo ""
        echo "        }"
        echo "    action:"
        echo "        @constructor T$i() {"
        echo "            long acc = 0;"
        echo "            for (int k = 0; k < 500; k = k + 1) acc = acc + T$i.deepEval(long(k));"
        echo "            if (acc == -1) { Console.writeString(\"FAIL deep\\n\"); return; }"
        echo "            Console.writeString(\"PASS expr_deep_$i acc=\" + acc + \"\\n\");"
        echo "        }"
        echo "}"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/execute/expr_deep_$i.myp"

    # ---------- execute: 控制流（嵌套循环 + break/continue） ----------
    {
        echo "import env;"
        echo "// ctrl_$i : 嵌套循环 break/continue 计数（seed=$seed）"
        echo "class T$i {"
        echo "    action:"
        echo "        @constructor T$i() {"
        echo "            int a = $((i % 4 + 2));"
        echo "            int b = $((i % 5 + 3));"
        echo "            int c = $((i % 6 + 4));"
        echo "            int count = 0;"
        echo "            for (int x = 0; x < a; x = x + 1) {"
        echo "                for (int y = 0; y < b; y = y + 1) {"
        echo "                    for (int z = 0; z < c; z = z + 1) {"
        echo "                        if (z == c - 1) continue;"
        echo "                        count = count + 1;"
        echo "                    }"
        echo "                }"
        echo "            }"
        echo "            int expect = a * b * (c - 1);"
        echo "            if (count != expect) { Console.writeString(\"FAIL ctrl \" + count + \" != \" + expect + \"\\n\"); return; }"
        echo "            int brk = 0;"
        echo "            for (int x = 0; x < 10; x = x + 1) {"
        echo "                for (int y = 0; y < 10; y = y + 1) {"
        echo "                    brk = brk + 1;"
        echo "                    if (y >= $((i % 4 + 1))) break;"
        echo "                }"
        echo "            }"
        echo "            Console.writeString(\"PASS ctrl_$i (count=\" + count + \" brk=\" + brk + \")\\n\");"
        echo "        }"
        echo "}"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/execute/ctrl_$i.myp"

    # ---------- execute: 字符串 ----------
    {
        echo "import env;"
        echo "// str_$i : 字符串操作自验证（seed=$seed）"
        echo "class T$i {"
        echo "    action:"
        echo "        @constructor T$i() {"
        echo "            string s = \"torture-$i-\" + $i + \"-suffix\";"
        echo "            if (s + \"\" != s) { Console.writeString(\"FAIL concat\\n\"); return; }"
        echo "            long acc = 0;"
        echo "            for (int k = 0; k < 100; k = k + 1) acc = acc + myp_strlen(\"x\" + k);"
        echo "            Console.writeString(\"PASS str_$i (len=\" + myp_strlen(s) + \" acc=\" + acc + \")\\n\");"
        echo "        }"
        echo "}"
        echo "ffi int myp_strlen(string s);"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/execute/str_$i.myp"

    # ---------- execute: 数组（翻转两次 = 恒等） ----------
    {
        echo "import env;"
        echo "// arr_$i : 数组翻转两次 = 恒等（seed=$seed）"
        echo "class T$i {"
        echo "    action:"
        echo "        @constructor T$i() {"
        echo "            int n = $((i % 8 + 8));"
        echo "            long[] a = new long[n];"
        echo "            for (int k = 0; k < n; k = k + 1) a[k] = long(k * $((seed % 97 + 3)));"
        echo "            for (int k = 0; k < n / 2; k = k + 1) {"
        echo "                long t = a[k]; a[k] = a[n - 1 - k]; a[n - 1 - k] = t;"
        echo "            }"
        echo "            for (int k = 0; k < n / 2; k = k + 1) {"
        echo "                long t = a[k]; a[k] = a[n - 1 - k]; a[n - 1 - k] = t;"
        echo "            }"
        echo "            int bad = 0;"
        echo "            for (int k = 0; k < n; k = k + 1) if (a[k] != long(k * $((seed % 97 + 3)))) bad = 1;"
        echo "            if (bad != 0) { Console.writeString(\"FAIL arr\\n\"); return; }"
        echo "            Console.writeString(\"PASS arr_$i (n=\" + n + \")\\n\");"
        echo "        }"
        echo "}"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/execute/arr_$i.myp"

    # ---------- compile: 深嵌套表达式（仅编译，深度分级） ----------
    d=$(( (i % 5 + 1) * 20 ))
    {
        echo "import env;"
        echo "// deep_nest_$i : 深度 $d 表达式嵌套（仅编译压力）"
        echo "class T$i {"
        echo "    static:"
        echo "        long f(long v) {"
        printf '            long e = v;'
        j=0
        while [ $j -lt $d ]; do
            printf ' e = (e + %d) * 2 - (e %% %d);' $((j % 7)) $((j % 11 + 3))
            j=$((j+1))
        done
        printf ' return e;'
        echo ""
        echo "        }"
        echo "    action:"
        echo "        @constructor T$i() { long r = T$i.f(1); }"
        echo "}"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/compile/deep_nest_$i.myp"

    # ---------- compile: 大枚举 match（仅编译） ----------
    nv=$((i * 20 + 40))   # 40..180 个枚举变体
    {
        echo "import env;"
        echo "// big_match_$i : 大枚举 match 分发（$nv 变体）"
        echo "enum E$i {"
        j=0
        while [ $j -lt $nv ]; do
            echo "    V$j;"
            j=$((j+1))
        done
        echo "}"
        echo "class T$i {"
        echo "    action:"
        echo "        int pick(E$i e) {"
        echo "            int r = 0;"
        echo "            match (e) {"
        j=0
        while [ $j -lt $nv ]; do
            echo "                E$i.V$j => { r = $((j * 3 + 1)); }"
            j=$((j+1))
        done
        echo "            }"
        echo "            return r;"
        echo "        }"
        echo "        @constructor T$i() {"
        echo "            int acc = 0;"
        # MYP 枚举用变体字面量构造（不支持 new E(index)）——compile-only 压测，
        # 首尾变体各构造一次调 pick 即可（大 match 编译压力保留）。
        echo "            E$i e0 = E$i.V0;"
        echo "            E$i e1 = E$i.V$((nv-1));"
        echo "            acc = acc + this.pick(e0) + this.pick(e1);"
        echo "        }"
        echo "}"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/compile/big_match_$i.myp"

    # ---------- compile: 大整型字面量 match（v3.15.86 扩展，仅编译） ----------
    nv=$((i * 20 + 40))   # 40..180 个整型字面量臂
    {
        echo "import env;"
        echo "// big_scmatch_$i : 大整型字面量 match 分发（$nv 臂 + _ 通配）"
        echo "class T$i {"
        echo "    action:"
        echo "        int pick(int v) {"
        echo "            int r = 0;"
        echo "            match (v) {"
        j=0
        while [ $j -lt $nv ]; do
            echo "                $j => { r = $((j * 3 + 1)); }"
            j=$((j+1))
        done
        echo "                _ => { r = -1; }"
        echo "            }"
        echo "            return r;"
        echo "        }"
        echo "        @constructor T$i() {"
        echo "            int acc = 0;"
        echo "            acc = acc + this.pick(0) + this.pick($((nv-1))) + this.pick(-7);"
        echo "        }"
        echo "}"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/compile/big_scmatch_$i.myp"

    # ---------- compile: 大字符串字面量 match（v3.15.86 扩展，仅编译） ----------
    nv=$((i * 20 + 40))   # 40..180 个字符串字面量臂
    {
        echo "import env;"
        echo "// big_smatch_$i : 大字符串字面量 match 分发（$nv 臂 + _ 通配）"
        echo "class T$i {"
        echo "    action:"
        echo "        string pick(string s) {"
        echo "            string r = \"\";"
        echo "            match (s) {"
        j=0
        while [ $j -lt $nv ]; do
            echo "                \"key$j\" => { r = \"val$j\"; }"
            j=$((j+1))
        done
        echo "                _ => { r = \"?\"; }"
        echo "            }"
        echo "            return r;"
        echo "        }"
        echo "        @constructor T$i() {"
        echo "            string a = this.pick(\"key0\");"
        echo "            string b = this.pick(\"key$((nv-1))\");"
        echo "        }"
        echo "}"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/compile/big_smatch_$i.myp"

    # ---------- execute: 标量 match 自验证（v3.15.86 扩展） ----------
    tgt=$((i % 7 + 2))
    {
        echo "import env;"
        echo "// match_ext_$i : 标量 match 自验证（int/long/string/char/double + _，target=$tgt）"
        echo "class T$i {"
        echo "    action:"
        echo "        string pickInt(int v) {"
        echo "            string r = \"\";"
        echo "            match (v) {"
        echo "                0 => { r = \"zero\"; }"
        echo "                -1 => { r = \"neg\"; }"
        echo "                $tgt => { r = \"target\"; }"
        echo "                _ => { r = \"other\"; }"
        echo "            }"
        echo "            return r;"
        echo "        }"
        echo "        string pickStr(string s) {"
        echo "            string r = \"\";"
        echo "            match (s) {"
        echo "                \"alpha\" => { r = \"a\"; }"
        echo "                \"beta$i\" => { r = \"b\"; }"
        echo "                _ => { r = \"z\"; }"
        echo "            }"
        echo "            return r;"
        echo "        }"
        echo "        string pickD(double d) {"
        echo "            string r = \"\";"
        echo "            match (d) {"
        echo "                0.5 => { r = \"half\"; }"
        echo "                2.0 => { r = \"two\"; }"
        echo "                _ => { r = \"oth\"; }"
        echo "            }"
        echo "            return r;"
        echo "        }"
        echo "        @constructor T$i() {"
        echo "            int ok = 0;"
        echo "            ok = ok + (this.pickInt(0) == \"zero\" ? 1 : 0);"
        echo "            ok = ok + (this.pickInt(-1) == \"neg\" ? 1 : 0);"
        echo "            ok = ok + (this.pickInt($tgt) == \"target\" ? 1 : 0);"
        echo "            ok = ok + (this.pickInt(99) == \"other\" ? 1 : 0);"
        echo "            ok = ok + (this.pickStr(\"alpha\") == \"a\" ? 1 : 0);"
        echo "            ok = ok + (this.pickStr(\"beta$i\") == \"b\" ? 1 : 0);"
        echo "            ok = ok + (this.pickStr(\"nope\") == \"z\" ? 1 : 0);"
        echo "            ok = ok + (this.pickD(0.5) == \"half\" ? 1 : 0);"
        echo "            ok = ok + (this.pickD(9.9) == \"oth\" ? 1 : 0);"
        echo "            if (ok != 9) { Console.writeString(\"FAIL match_ext_$i ok=\" + ok + \"\\n\"); return; }"
        echo "            Console.writeString(\"PASS match_ext_$i\\n\");"
        echo "        }"
        echo "}"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/execute/match_ext_$i.myp"

    # ---------- execute: 嵌套 match（match 体内再 match） ----------
    {
        echo "import env;"
        echo "// match_nested_$i : 嵌套 match 自验证"
        echo "class T$i {"
        echo "    action:"
        echo "        string pick(int a, int b) {"
        echo "            string r = \"\";"
        echo "            match (a) {"
        echo "                0 => {"
        echo "                    match (b) {"
        echo "                        0 => { r = \"00\"; }"
        echo "                        1 => { r = \"01\"; }"
        echo "                        _ => { r = \"0x\"; }"
        echo "                    }"
        echo "                }"
        echo "                1 => {"
        echo "                    match (b) {"
        echo "                        0 => { r = \"10\"; }"
        echo "                        _ => { r = \"1x\"; }"
        echo "                    }"
        echo "                }"
        echo "                _ => { r = \"o\"; }"
        echo "            }"
        echo "            return r;"
        echo "        }"
        echo "        @constructor T$i() {"
        echo "            int ok = 0;"
        echo "            ok = ok + (this.pick(0, 0) == \"00\" ? 1 : 0);"
        echo "            ok = ok + (this.pick(0, 1) == \"01\" ? 1 : 0);"
        echo "            ok = ok + (this.pick(0, 9) == \"0x\" ? 1 : 0);"
        echo "            ok = ok + (this.pick(1, 0) == \"10\" ? 1 : 0);"
        echo "            ok = ok + (this.pick(1, 9) == \"1x\" ? 1 : 0);"
        echo "            ok = ok + (this.pick(7, 7) == \"o\" ? 1 : 0);"
        echo "            if (ok != 6) { Console.writeString(\"FAIL match_nested_$i ok=\" + ok + \"\\n\"); return; }"
        echo "            Console.writeString(\"PASS match_nested_$i\\n\");"
        echo "        }"
        echo "}"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/execute/match_nested_$i.myp"

    # ---------- execute: 百个 i++ + 巨长 + 链（一万个加号起，深度表达式压测） ----------
    plus=$((10000 + i * 1000))   # 10000..17000 个加号（左深 + 链）
    inc=$((100 + i * 20))        # 100..240 个 i++ 语句
    {
        echo "import env;"
        echo "// plus_bomb_$i : 百个 i++ + $plus 个加号的巨表达式（左深链 codegen 压测）"
        echo "class T$i {"
        echo "    action:"
        echo "        @constructor T$i() {"
        printf '            long r = 0'
        j=0
        while [ $j -lt $plus ]; do
            printf ' + 1'
            j=$((j+1))
        done
        echo ";"
        echo "            int i = 0;"
        j=0
        while [ $j -lt $inc ]; do
            echo "            i++;"
            j=$((j+1))
        done
        echo "            if (r != $plus) { Console.writeString(\"FAIL plus r=\" + r + \"\\n\"); return; }"
        echo "            if (i != $inc) { Console.writeString(\"FAIL inc i=\" + i + \"\\n\"); return; }"
        echo "            Console.writeString(\"PASS plus_bomb_$i r=\" + r + \" i=\" + i + \"\\n\");"
        echo "        }"
        echo "}"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/execute/plus_bomb_$i.myp"

    # ---------- execute: 深嵌套 struct 链式访问 ----------
    sd=$((6 + i * 2))   # 6..20 层嵌套
    {
        echo "import env;"
        echo "// struct_nest_$i : $sd 层嵌套 struct 链式读写自验证"
        echo "struct L$sd { int v; }"
        j=$((sd - 1))
        while [ $j -ge 0 ]; do
            echo "struct L$j { L$((j+1)) inner; }"
            j=$((j-1))
        done
        echo "class T$i {"
        echo "    action:"
        echo "        @constructor T$i() {"
        echo "            L0 root;"
        printf '            root'
        j=0
        while [ $j -lt $sd ]; do printf '.inner'; j=$((j+1)); done
        echo ".v = $((i * 7 + 42));"
        printf '            int got = root'
        j=0
        while [ $j -lt $sd ]; do printf '.inner'; j=$((j+1)); done
        echo ".v;"
        echo "            if (got != $((i * 7 + 42))) { Console.writeString(\"FAIL struct_nest_$i got=\" + got + \"\\n\"); return; }"
        echo "            Console.writeString(\"PASS struct_nest_$i v=\" + got + \"\\n\");"
        echo "        }"
        echo "}"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/execute/struct_nest_$i.myp"

    # ---------- compile: 超深嵌套 struct 定义（仅编译压测） ----------
    cdd=$((40 + i * 20))   # 40..180 层嵌套定义
    {
        echo "import env;"
        echo "// struct_deep_$i : $cdd 层嵌套 struct 定义（仅编译压测）"
        echo "struct D$cdd { int v; }"
        j=$((cdd - 1))
        while [ $j -ge 0 ]; do
            echo "struct D$j { D$((j+1)) inner; }"
            j=$((j-1))
        done
        echo "class T$i {"
        echo "    action:"
        echo "        @constructor T$i() {"
        echo "            D0 root;"
        printf '            root'
        j=0
        while [ $j -lt $cdd ]; do printf '.inner'; j=$((j+1)); done
        echo ".v = 1;"
        echo "        }"
        echo "}"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/compile/struct_deep_$i.myp"

    # ---------- deep/ 解析器递归深度压测（BUG-055，编译不得崩溃） ----------
    # 深嵌套括号/三元/块/if 链/合并/一元——超 300 层守卫应**干净报错**（rc=1，
    # "expression/statement/block nested too deeply"），绝不许栈溢出 SIGSEGV。
    dp=$((4000 + i * 4000))   # 4000..32000 层
    {
        echo "import env;"
        echo "// deep_paren_$i : $dp 层嵌套括号（parsePrimary 守卫 300，干净报错）"
        echo "class T$i { action:"
        echo "  @constructor T$i() {"
        printf '    int x = '
        j=0; while [ $j -lt $dp ]; do printf '('; j=$((j+1)); done
        printf '1'
        j=0; while [ $j -lt $dp ]; do printf ')'; j=$((j+1)); done
        echo ";"
        echo "  } }"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/deep/paren_$i.myp"
    {
        echo "import env;"
        echo "// deep_ternary_$i : $dp 层嵌套三元（parseExpr 守卫，干净报错）"
        echo "class T$i { action:"
        echo "  @constructor T$i() {"
        printf '    int x = '
        j=0; while [ $j -lt $dp ]; do printf '1 ? 2 : '; j=$((j+1)); done
        printf '9'
        echo ";"
        echo "  } }"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/deep/ternary_$i.myp"
    {
        echo "import env;"
        echo "// deep_blocks_$i : $dp 层嵌套块（parseBlock 守卫，干净报错）"
        echo "class T$i { action:"
        echo "  @constructor T$i() {"
        j=0; while [ $j -lt $dp ]; do printf '{'; j=$((j+1)); done
        printf 'int x = 1;'
        j=0; while [ $j -lt $dp ]; do printf '}'; j=$((j+1)); done
        echo ""
        echo "  } }"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/deep/blocks_$i.myp"
    {
        echo "import env;"
        echo "// deep_ifchain_$i : $dp 层 if 链（parseStatement 守卫，干净报错）"
        echo "class T$i { action:"
        echo "  @constructor T$i() {"
        printf '    int x = 0; '
        j=0; while [ $j -lt $dp ]; do printf 'if (1) '; j=$((j+1)); done
        printf 'x = 1;'
        echo ""
        echo "  } }"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/deep/ifchain_$i.myp"
    {
        echo "import env;"
        echo "// deep_coalesce_$i : $dp 层 ?? 合并（parseCoalesce 守卫，干净报错）"
        echo "class T$i { action:"
        echo "  @constructor T$i() {"
        printf '    string x = '
        j=0; while [ $j -lt $dp ]; do printf '"a" ?? '; j=$((j+1)); done
        printf '"z"'
        echo ";"
        echo "  } }"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/deep/coalesce_$i.myp"
    {
        echo "import env;"
        echo "// deep_unary_$i : $dp 层一元 ! 链（parseUnary 守卫，干净报错）"
        echo "class T$i { action:"
        echo "  @constructor T$i() {"
        printf '    int x = '
        j=0; while [ $j -lt $dp ]; do printf '!'; j=$((j+1)); done
        printf '0'
        echo ";"
        echo "  } }"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/deep/unary_$i.myp"
    {
        echo "import env;"
        echo "// deep_generic_$i : $dp 层嵌套泛型 Box<Box<...<int>>>（BUG-056 守卫，干净报错）"
        echo "class Box<T> { property: T v; }"
        echo "class T$i { action:"
        echo "  @constructor T$i() {"
        printf '    Box<'
        j=0; while [ $j -lt $dp ]; do printf 'Box<'; j=$((j+1)); done
        printf 'int'
        j=0; while [ $j -lt $dp ]; do printf '>'; j=$((j+1)); done
        printf '> x;'
        echo ""
        echo "  } }"
        echo "int main() { T$i t = new T$i(); return 0; }"
    } > "$OUT/deep/generic_$i.myp"

    i=$((i+1))
done

echo "== 生成完成：$(ls "$OUT/execute" | wc -l) execute + $(ls "$OUT/compile" | wc -l) compile"
