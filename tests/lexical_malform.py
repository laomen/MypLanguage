#!/usr/bin/env python3
"""
MYP 定向词法畸形输入测试 (Targeted lexical malformation fuzz)

Grammar fuzz 无法生成这类输入，需人工构造：
  - 未闭合字符串 / 字符串内尾反斜杠
  - 字符字面量畸形
  - 裸 `5.` / `1e` / `1e+` 等数字边界
  - 极端符号序列、UTF-8 截断
  - 空文件 / 只有注释 / 注释未闭合
每个用例喂给 mypc，仅要求不崩溃（exit code 非 139/134/ASAN 报错）。
"""
import subprocess
import sys
import os
import tempfile

MYPCC = sys.argv[1] if len(sys.argv) > 1 else "./build-asan/mypc"
ENV = dict(os.environ)
ENV["ASAN_OPTIONS"] = "detect_leaks=0"

# (名称, 内容) — 每个都是独立的畸形输入
CASES = [
    # --- 字符串边界 ---
    ("unclosed_string", 'int x = "abc;'),
    ("unclosed_string_eof", 'string s = "'),
    ("string_trailing_backslash", 'string s = "abc\\'),
    ("string_trailing_backslash2", 'string s = "a\\\nb";'),
    ("string_escaped_quote_eof", 'string s = "a\\"'),
    ("string_only_backslash", 'string s = "\\'),
    ("string_newline", 'string s = "a\nb";'),
    ("string_crlf_unclosed", 'string s = "abc\r\n'),
    ("string_tab", 'string s = "a\tb";'),
    ("string_unicode", 'string s = "\u4f60\u597d";'),
    ("string_unicode_trunc", 'string s = "\xe4\xb8'),  # 截断 UTF-8
    # --- 字符字面量 ---
    ("char_empty", "char c = '';"),
    ("char_unclosed", "char c = 'a"),
    ("char_trailing_backslash", "char c = 'a\\"),
    ("char_double", "char c = 'ab';"),
    ("char_escaped_quote", "char c = '\\'';"),
    ("char_backslash_eof", "char c = '\\"),
    # --- 数字边界 ---
    ("num_bare_dot", "int x = 5.;"),
    ("num_bare_dot_eof", "int x = 5."),
    ("num_dot_eof", "double x = ."),
    ("num_exp_eof", "double x = 1e"),
    ("num_exp_plus_eof", "double x = 1e+"),
    ("num_exp_minus_eof", "double x = 1e-"),
    ("num_exp_dot", "double x = 1e1.5;"),
    ("num_hex_eof", "int x = 0x"),
    ("num_hex_bad", "int x = 0xZZ;"),
    ("num_octal", "int x = 08;"),
    ("num_double_dot", "double x = 1.2.3;"),
    ("num_leading_dot", "double x = .5;"),
    ("num_neg_exp", "double x = -1e-10;"),
    ("num_float_suffix", "float x = 1.5f;"),
    # --- 符号序列 ---
    ("sym_arrow", "int x = a -> b;"),
    ("sym_double_arrow", "int x = a => b;"),
    ("sym_triple_eq", "bool x = a === b;"),
    ("sym_amp", "int x = a & b;"),
    ("sym_pipe", "int x = a | b;"),
    ("sym_caret", "int x = a ^ b;"),
    ("sym_tilde", "int x = ~a;"),
    ("sym_shift", "int x = a << b >> c;"),
    ("sym_ellipsis", "void f(int... a) {}"),
    ("sym_dollar", "int x = $a;"),
    ("sym_at", "int x = @a;"),
    ("sym_backtick", "int x = `a`;"),
    ("sym_question_dot", "int x = a ?. b;"),
    ("sym_semicolon_flood", ";;;;;"),
    ("sym_brace_mismatch", "int main() { return 0; "),
    ("sym_paren_mismatch", "int main() ( return 0; }"),
    ("sym_bracket_mismatch", "int main() { int x = a[0; }"),
    ("sym_slash_dot", "int x = a / .5;"),
    ("sym_dots", "....."),
    ("sym_dots_in_num", "int x = 1...2;"),
    ("sym_colon_colon", "int x = a::b;"),
    ("sym_dollar_dollar", "$$$$$"),
    # --- 注释边界 ---
    ("comment_only", "// just a comment\n"),
    ("comment_unclosed_block", "/* unclosed block comment\nint x = 1;\n"),
    ("comment_nested", "/* outer /* inner */ still */"),
    ("comment_backslash", "// comment with \\\nint x = 1;"),
    ("comment_after_code", "int x = 1; // trailing"),
    ("comment_crlf", "// comment\r\nint x = 1;"),
    ("comment_block_crlf", "/* a\r\nb */ int x = 1;"),
    # --- 其他 ---
    ("empty_file", ""),
    ("only_whitespace", "   \n\t  \n"),
    ("only_newlines", "\n\n\n\n\n"),
    ("nul_byte", "int x = \x00;"),
    ("control_chars", "\x01\x02\x03int x = 1;"),
    ("very_long_identifier", "int " + "a"*100000 + " = 1;"),
    ("very_long_string", 'string s = "' + "x"*100000 + '";'),
    ("many_semicolons", ";"*10000),
    ("crash_parens", "("*100 + "int x = 1;" + ")"*100),
    ("unicode_ident", "int \u4f60 = 1;"),
    ("bom", "\ufeffint x = 1;"),
    ("deep_chain", "a" + ".b"*2000 + ";"),
    ("zero_width_space", "int\u200bx = 1;"),
    ("long_number", "int x = " + "9"*10000 + ";"),
    ("float_long", "double x = 1." + "0"*10000 + ";"),
    ("string_interp_dollar", 'string s = "$x";'),
    ("string_interp_brace", 'string s = "${x}";'),
    ("template_like", "int x = `abc`;"),
]

def run_case(name, content):
    fd, path = tempfile.mkstemp(suffix=".myp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(content.encode("utf-8", "surrogateescape"))
        p = subprocess.run([MYPCC, path], capture_output=True, timeout=20,
                           env=ENV)
        rc = p.returncode
        out = (p.stdout + p.stderr).decode("utf-8", "replace")
        # 崩溃判据: 信号退出, 或 ASAN 报错
        crashed = rc < 0 or rc >= 128 or "AddressSanitizer" in out or \
            "runtime error:" in out or "Segmentation" in out
        if crashed:
            print(f"[CRASH] {name} rc={rc}")
            print(out[:2000])
            return False
        return True
    except subprocess.TimeoutExpired:
        print(f"[HANG]  {name}")
        return False
    finally:
        os.unlink(path)

def main():
    ok = 0
    bad = 0
    for name, content in CASES:
        if run_case(name, content):
            ok += 1
        else:
            bad += 1
    print(f"\n=== 定向词法畸形测试: {ok} 通过, {bad} 失败/崩溃 ===")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
