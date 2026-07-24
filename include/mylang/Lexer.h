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
    Token scanString();
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
};

} // namespace mylang

#endif // MYLANG_LEXER_H
