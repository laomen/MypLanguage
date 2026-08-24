#ifndef MYLANG_LEXER_H
#define MYLANG_LEXER_H

#include "DiagnosticEngine.h"
#include "SourceLocation.h"
#include "Token.h"

#include <string>
#include <vector>

namespace mylang {

/// Lexer: tokenizes source text into a stream of Tokens.
class Lexer {
public:
    Lexer(SourceManager& source_mgr, DiagnosticEngine& diag);

    /// Tokenize the entire source file.
    std::vector<Token> tokenize();

private:
    void scanToken();
    // 从当前 offset 扫描字符串体（前导引号已被消费），遇结束引号或插值 '${' 时
    // 直接向 tokens_ 发射 token（插值会回到主循环扫描表达式）。续扫（插值 '}' 后）
    // 也复用本函数——此时无前导引号，start_offset = 当前 offset。
    void scanStringBody(unsigned start_offset, SourcePosition start_pos);
    Token scanTripleString();
    Token scanNumber();
    Token scanIdentifierOrKeyword();
    Token scanComment();
    void skipWhitespace();

    char peek() const;
    char advance();
    bool match(char expected);
    bool isAtEnd() const;

    SourceRange currentRange() const;
    SourcePosition currentPosition() const;

    SourceManager& source_mgr_;
    DiagnosticEngine& diag_;
    std::vector<Token> tokens_;

    std::string_view source_;
    uint32_t offset_ = 0;
    uint32_t line_ = 1;
    uint32_t column_ = 1;

    // 字符串插值 "${expr}" 状态：空栈 = 无插值；非空时栈顶 = 当前插值表达式内
    // 未闭合的 '{' 深度（用于区分表达式内的块/结构体花括号与结束 '}'）。
    std::vector<int> interp_stack_;
};

} // namespace mylang

#endif // MYLANG_LEXER_H
