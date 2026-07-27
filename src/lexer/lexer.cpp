#include "mylang/Lexer.h"

#include <cctype>
#include <string>

namespace mylang {

Lexer::Lexer(SourceManager& source_mgr, DiagnosticEngine& diag)
    : source_mgr_(source_mgr), diag_(diag) {
    source_ = source_mgr_.source();
}

std::vector<Token> Lexer::tokenize() {
    tokens_.clear();
    while (!isAtEnd()) {
        skipWhitespace();
        if (isAtEnd()) break;

        scanToken();
    }

    // Add EOF token
    tokens_.emplace_back(TokenKind::EndOfFile, SourceRange{}, "");
    return tokens_;
}

void Lexer::skipWhitespace() {
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            advance();
        } else {
            break;
        }
    }
}

void Lexer::scanToken() {
    char c = advance();

    switch (c) {
        // Single-character tokens
        case '(': tokens_.emplace_back(TokenKind::LeftParen, currentRange()); break;
        case ')': tokens_.emplace_back(TokenKind::RightParen, currentRange()); break;
        case '{': tokens_.emplace_back(TokenKind::LeftBrace, currentRange()); break;
        case '}': tokens_.emplace_back(TokenKind::RightBrace, currentRange()); break;
        case '[': tokens_.emplace_back(TokenKind::LeftBracket, currentRange()); break;
        case ']': tokens_.emplace_back(TokenKind::RightBracket, currentRange()); break;
        case ';': tokens_.emplace_back(TokenKind::Semicolon, currentRange()); break;
        case ':':
            if (match(':')) {
                tokens_.emplace_back(TokenKind::DoubleColon, currentRange());
            } else {
                tokens_.emplace_back(TokenKind::Colon, currentRange());
            }
            break;
        case ',': tokens_.emplace_back(TokenKind::Comma, currentRange()); break;
        case '.':
            if (match('.')) {
                tokens_.emplace_back(TokenKind::DoubleDot, currentRange());
            } else {
                tokens_.emplace_back(TokenKind::Dot, currentRange());
            }
            break;
        case '@': tokens_.emplace_back(TokenKind::At, currentRange()); break;
        case '?': tokens_.emplace_back(TokenKind::Question, currentRange()); break;

        // Operators that could be multi-character
        case '+':
            if (match('+')) {
                tokens_.emplace_back(TokenKind::PlusPlus, currentRange());
            } else if (match('=')) {
                tokens_.emplace_back(TokenKind::PlusEqual, currentRange());
            } else {
                tokens_.emplace_back(TokenKind::Plus, currentRange());
            }
            break;
        case '-':
            if (match('>')) {
                tokens_.emplace_back(TokenKind::Arrow, currentRange());
            } else if (match('-')) {
                tokens_.emplace_back(TokenKind::MinusMinus, currentRange());
            } else if (match('=')) {
                tokens_.emplace_back(TokenKind::MinusEqual, currentRange());
            } else {
                tokens_.emplace_back(TokenKind::Minus, currentRange());
            }
            break;
        case '*':
            if (match('=')) {
                tokens_.emplace_back(TokenKind::StarEqual, currentRange());
            } else {
                tokens_.emplace_back(TokenKind::Star, currentRange());
            }
            break;
        case '/':
            if (peek() == '/' || peek() == '*') {
                scanComment();
            } else if (match('=')) {
                tokens_.emplace_back(TokenKind::SlashEqual, currentRange());
            } else {
                tokens_.emplace_back(TokenKind::Slash, currentRange());
            }
            break;
        case '%':
            if (match('=')) {
                tokens_.emplace_back(TokenKind::PercentEqual, currentRange());
            } else {
                tokens_.emplace_back(TokenKind::Percent, currentRange());
            }
            break;
        case '!':
            if (match('=')) {
                tokens_.emplace_back(TokenKind::NotEqual, currentRange());
            } else {
                tokens_.emplace_back(TokenKind::Bang, currentRange());
            }
            break;
        case '=':
            if (match('=')) {
                tokens_.emplace_back(TokenKind::EqualEqual, currentRange());
            } else if (peek() == '>') {
                // => (fat arrow) — note: we consume both chars here
                // advance past '>' since peek() already sees it without consuming
                advance();
                tokens_.emplace_back(TokenKind::FatArrow, currentRange());
            } else {
                tokens_.emplace_back(TokenKind::Equal, currentRange());
            }
            break;
        case '<':
            if (match('=')) {
                tokens_.emplace_back(TokenKind::LessEqual, currentRange());
            } else {
                tokens_.emplace_back(TokenKind::Less, currentRange());
            }
            break;
        case '>':
            if (match('=')) {
                tokens_.emplace_back(TokenKind::GreaterEqual, currentRange());
            } else {
                tokens_.emplace_back(TokenKind::Greater, currentRange());
            }
            break;
        case '&':
            if (match('&')) {
                tokens_.emplace_back(TokenKind::AndAnd, currentRange());
            } else {
                diag_.error(currentRange(), "expected '&&'");
            }
            break;
        case '|':
            if (match('|')) {
                tokens_.emplace_back(TokenKind::OrOr, currentRange());
            } else {
                diag_.error(currentRange(), "expected '||'");
            }
            break;

        // String literals
        case '"': {
            auto token = scanString();
            tokens_.push_back(token);
            break;
        }

        // Character literal: 'A', '\n', etc.
        case '\'': {
            if (isAtEnd()) {
                diag_.error(currentRange(), "empty character literal");
                break;
            }
            char ch;
            if (peek() == '\\') {
                advance(); // consume backslash
                switch (peek()) {
                    case 'n':  ch = '\n'; break;
                    case 't':  ch = '\t'; break;
                    case '\\': ch = '\\'; break;
                    case '\'': ch = '\''; break;
                    case '0':  ch = '\0'; break;
                    default:
                        diag_.error(currentRange(), "unknown escape sequence in char literal");
                        ch = peek();
                        break;
                }
                advance();
            } else {
                ch = advance();
            }
            if (peek() != '\'') {
                diag_.error(currentRange(), "unclosed character literal");
            } else {
                advance(); // consume closing '
            }
            std::string val(1, ch);
            auto start_offset = offset_ - (val.size() + 2); // +2 for quotes
            tokens_.emplace_back(TokenKind::CharLiteral,
                SourceRange{(unsigned)start_offset, offset_, {line_, column_}, {line_, column_}},
                val);
            break;
        }

        default:
            if (std::isalpha(c) || c == '_') {
                // Push back the character we already read for scanning
                // Our scanIdentifierOrKeyword needs the current position
                auto token = scanIdentifierOrKeyword();
                tokens_.push_back(token);
            } else if (std::isdigit(c)) {
                auto token = scanNumber();
                tokens_.push_back(token);
            } else {
                diag_.error(currentRange(),
                    std::string("unexpected character '") + c + "'");
            }
            break;
    }
}

Token Lexer::scanString() {
    auto start_offset = offset_ - 1; // include opening "
    auto start_line = line_;
    auto start_col = column_;

    std::string value;

    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\n') {
            ++line_;
            column_ = 0;
        }
        if (peek() == '\\') {
            advance();
            switch (peek()) {
                case 'n':  value += '\n'; break;
                case 't':  value += '\t'; break;
                case '\\': value += '\\'; break;
                case '"':  value += '"';  break;
                case '0':  value += '\0'; break;
                default:
                    diag_.error(currentRange(), "unknown escape sequence");
                    break;
            }
            advance();
        } else {
            value += advance();
        }
    }

    if (isAtEnd()) {
        diag_.error(currentRange(), "unterminated string literal");
    } else {
        advance(); // consume closing "
    }

    return Token(TokenKind::StringLiteral,
        SourceRange{start_offset, offset_, {start_line, start_col}, {line_, column_}},
        value);
}

Token Lexer::scanNumber() {
    auto start_offset = offset_ - 1;
    auto start_line = line_;
    auto start_col = column_;

    std::string value;
    value += source_[start_offset];

    // Hex integer: 0xFF
    if (value[0] == '0' && (peek() == 'x' || peek() == 'X')) {
        value += advance();
        while (std::isxdigit(peek())) {
            value += advance();
        }
        return Token(TokenKind::IntegerLiteral,
            SourceRange{start_offset, offset_, {start_line, start_col}, {line_, column_}},
            value);
    }

    // Decimal number
    bool is_float = false;
    while (std::isdigit(peek()) || peek() == '.') {
        if (peek() == '.') {
            if (is_float) break; // second dot -> stop
            // Check for ".." range operator — don't consume if followed by another dot
            if (source_[offset_] == '.' && source_[offset_ + 1] == '.') break;
            is_float = true;
        }
        value += advance();
    }

    // Float exponent
    if (peek() == 'e' || peek() == 'E') {
        value += advance();
        if (peek() == '+' || peek() == '-') value += advance();
        while (std::isdigit(peek())) value += advance();
        is_float = true;
    }

    auto kind = is_float ? TokenKind::FloatLiteral : TokenKind::IntegerLiteral;
    return Token(kind, SourceRange{start_offset, offset_, {start_line, start_col}, {line_, column_}}, value);
}

Token Lexer::scanIdentifierOrKeyword() {
    auto start_offset = offset_ - 1;
    auto start_line = line_;
    auto start_col = column_;

    std::string value;
    value += source_[start_offset];

    while (std::isalnum(peek()) || peek() == '_') {
        value += advance();
    }

    // Check keyword
    TokenKind kind = TokenKind::Identifier;

    if (value == "class")         kind = TokenKind::Keyword_class;
    else if (value == "action")   kind = TokenKind::Keyword_action;
    else if (value == "event")    kind = TokenKind::Keyword_event;
    else if (value == "property") kind = TokenKind::Keyword_property;
    else if (value == "interface") kind = TokenKind::Keyword_interface;
    else if (value == "import")   kind = TokenKind::Keyword_import;
    else if (value == "mapping")  kind = TokenKind::Keyword_mapping;
    else if (value == "struct")   kind = TokenKind::Keyword_struct;
    else if (value == "function") kind = TokenKind::Keyword_function;
    else if (value == "static")   kind = TokenKind::Keyword_static;
    else if (value == "if")       kind = TokenKind::Keyword_if;
    else if (value == "else")     kind = TokenKind::Keyword_else;
    else if (value == "while")    kind = TokenKind::Keyword_while;
    else if (value == "for")      kind = TokenKind::Keyword_for;
    else if (value == "return")   kind = TokenKind::Keyword_return;
    else if (value == "break")    kind = TokenKind::Keyword_break;
    else if (value == "continue") kind = TokenKind::Keyword_continue;
    else if (value == "true")     kind = TokenKind::Keyword_true;
    else if (value == "false")    kind = TokenKind::Keyword_false;
    else if (value == "null")     kind = TokenKind::Keyword_null;
    else if (value == "this")     kind = TokenKind::Keyword_this;
    else if (value == "new")      kind = TokenKind::Keyword_new;
    else if (value == "void")     kind = TokenKind::Keyword_void;
    else if (value == "var")      kind = TokenKind::Keyword_var;
    else if (value == "enum")     kind = TokenKind::Keyword_enum;
    else if (value == "match")    kind = TokenKind::Keyword_match;
    else if (value == "ffi")      kind = TokenKind::Keyword_ffi;
    else if (value == "try")      kind = TokenKind::Keyword_try;
    else if (value == "catch")    kind = TokenKind::Keyword_catch;
    else if (value == "finally")  kind = TokenKind::Keyword_finally;
    else if (value == "throw")    kind = TokenKind::Keyword_throw;

    // Type keywords
    else if (value == "byte")     kind = TokenKind::Type_byte;
    else if (value == "short")    kind = TokenKind::Type_short;
    else if (value == "int")      kind = TokenKind::Type_int;
    else if (value == "long")     kind = TokenKind::Type_long;
    else if (value == "ubyte")    kind = TokenKind::Type_ubyte;
    else if (value == "ushort")   kind = TokenKind::Type_ushort;
    else if (value == "uint")     kind = TokenKind::Type_uint;
    else if (value == "ulong")    kind = TokenKind::Type_ulong;
    else if (value == "char")     kind = TokenKind::Type_char;
    else if (value == "float")    kind = TokenKind::Type_float;
    else if (value == "double")   kind = TokenKind::Type_double;
    else if (value == "bool")     kind = TokenKind::Type_bool;
    else if (value == "string")   kind = TokenKind::Type_string;

    return Token(kind,
        SourceRange{start_offset, offset_, {start_line, start_col}, {line_, column_}},
        value);
}

Token Lexer::scanComment() {
    // We already consumed '/'
    char next = advance();

    if (next == '/') {
        // Line comment: skip until newline
        while (!isAtEnd() && peek() != '\n') advance();
    } else if (next == '*') {
        // Block comment
        while (!isAtEnd()) {
            if (peek() == '*' && !isAtEnd()) {
                advance();
                if (peek() == '/') {
                    advance();
                    break;
                }
            } else {
                if (peek() == '\n') {
                    ++line_;
                    column_ = 0;
                }
                advance();
            }
        }
        if (isAtEnd()) {
            diag_.error(currentRange(), "unterminated block comment");
        }
    }

    return Token(TokenKind::Unknown, currentRange());
}

// ---- Utility ----

char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return source_[offset_];
}

char Lexer::advance() {
    char c = source_[offset_++];
    ++column_;
    if (c == '\n') {
        ++line_;
        column_ = 0;
    }
    return c;
}

bool Lexer::match(char expected) {
    if (isAtEnd()) return false;
    if (source_[offset_] != expected) return false;
    advance();
    return true;
}

bool Lexer::isAtEnd() const {
    return offset_ >= source_.size();
}

SourceRange Lexer::currentRange() const {
    return SourceRange{offset_, offset_, {line_, column_}, {line_, column_}};
}

SourcePosition Lexer::currentPosition() const {
    return {line_, column_};
}

} // namespace mylang
