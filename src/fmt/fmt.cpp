#include "mylang/Fmt.h"
#include "mylang/Lexer.h"

#include <cctype>
#include <iostream>
#include <sstream>

namespace mylang {

Formatter::Formatter(SourceManager& source_mgr, DiagnosticEngine& diag)
    : source_mgr_(source_mgr), diag_(diag), source_(source_mgr.source()) {
}

std::string Formatter::indentStr(int level) const {
    if (level <= 0) return "";
    return std::string(static_cast<size_t>(level * indent_size_), ' ');
}

// Extract any comments between source offsets. Whitespace is skipped.
std::string Formatter::extractComments(const std::string& src,
                                        uint32_t& offset,
                                        uint32_t end_offset) {
    std::string result;
    while (offset < end_offset) {
        char c = src[offset];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++offset;
            continue;
        }
        if (c == '/' && offset + 1 < end_offset) {
            char next = src[offset + 1];
            if (next == '/') {
                if (!result.empty()) result += '\n';
                result += "//";
                offset += 2;
                while (offset < end_offset && src[offset] != '\n') {
                    result += src[offset];
                    ++offset;
                }
                continue;
            } else if (next == '*') {
                if (!result.empty()) result += '\n';
                result += "/*";
                offset += 2;
                while (offset + 1 < end_offset && !(src[offset] == '*' && src[offset + 1] == '/')) {
                    result += src[offset];
                    ++offset;
                }
                if (offset + 1 < end_offset) {
                    result += "*/";
                    offset += 2;
                }
                continue;
            }
        }
        break;
    }
    return result;
}

static bool isBinaryOp(TokenKind k) {
    switch (k) {
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
        case TokenKind::EqualEqual:
        case TokenKind::NotEqual:
        case TokenKind::Less:
        case TokenKind::Greater:
        case TokenKind::LessEqual:
        case TokenKind::GreaterEqual:
        case TokenKind::AndAnd:
        case TokenKind::OrOr:
        case TokenKind::Equal:
        case TokenKind::PlusEqual:
        case TokenKind::MinusEqual:
        case TokenKind::StarEqual:
        case TokenKind::SlashEqual:
        case TokenKind::PercentEqual:
        case TokenKind::DoubleDot:
        case TokenKind::Arrow:
        case TokenKind::FatArrow:
            return true;
        default:
            return false;
    }
}

static bool isTypeKeyword(TokenKind k) {
    switch (k) {
        case TokenKind::Type_byte:
        case TokenKind::Type_short:
        case TokenKind::Type_int:
        case TokenKind::Type_long:
        case TokenKind::Type_ubyte:
        case TokenKind::Type_ushort:
        case TokenKind::Type_uint:
        case TokenKind::Type_ulong:
        case TokenKind::Type_uint8:
        case TokenKind::Type_uint16:
        case TokenKind::Type_uint32:
        case TokenKind::Type_uint64:
        case TokenKind::Type_char:
        case TokenKind::Type_float:
        case TokenKind::Type_double:
        case TokenKind::Type_bool:
        case TokenKind::Type_string:
        case TokenKind::Keyword_void:
        case TokenKind::Keyword_var:
            return true;
        default:
            return false;
    }
}

static bool isKeywordStmt(TokenKind k) {
    switch (k) {
        case TokenKind::Keyword_if:
        case TokenKind::Keyword_while:
        case TokenKind::Keyword_for:
        case TokenKind::Keyword_return:
        case TokenKind::Keyword_catch:
        case TokenKind::Keyword_throw:
        case TokenKind::Keyword_new:
        case TokenKind::Keyword_match:
        case TokenKind::Keyword_import:
        case TokenKind::Keyword_class:
        case TokenKind::Keyword_interface:
        case TokenKind::Keyword_mapping:
        case TokenKind::Keyword_enum:
        case TokenKind::Keyword_ffi:
            return true;
        default:
            return false;
    }
}

// 把解码后的字符串值重新转义（\n \t \r \\ \" \' \e \0），
// 避免格式化把 "\n" 变成真实换行而改变源码语义。
static std::string escapeString(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        switch (c) {
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\'': out += "\\'";  break;
            case '\x1B': out += "\\e"; break;
            case '\0': out += "\\0";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

static std::string tokenStr(const Token& tok) {
    switch (tok.kind) {
        case TokenKind::Identifier:
        case TokenKind::IntegerLiteral:
        case TokenKind::FloatLiteral:
        case TokenKind::BoolLiteral:
        case TokenKind::NullLiteral:
            return tok.value;
        case TokenKind::LongLiteral:
            // lexer 消费后缀但不写入 value，这里补回 "L"（避免被 keywordString 打成 "?"）
            return tok.value + "L";
        case TokenKind::UIntLiteral:
            return tok.value + "u";
        case TokenKind::StringLiteral:
            return "\"" + escapeString(tok.value) + "\"";
        case TokenKind::CharLiteral:
            return "'" + escapeString(tok.value) + "'";
        default:
            return Token::keywordString(tok.kind);
    }
}

std::string Formatter::format() {
    Lexer lexer(source_mgr_, diag_);
    auto tokens = lexer.tokenize();
    if (tokens.empty()) return source_;

    std::ostringstream out;
    int indent = 0;
    bool lineStart = true;
    bool prev_was_newline_comment = false;

    // Handle comments before the first token
    if (!tokens.empty()) {
        uint32_t cs = 0;
        uint32_t ce = tokens[0].range.begin_offset;
        if (ce > cs) {
            std::string preComments = extractComments(source_, cs, ce);
            if (!preComments.empty()) {
                out << preComments;
                if (preComments.find("//") != std::string::npos) {
                    out << "\n";
                    prev_was_newline_comment = true;
                } else {
                    out << "\n";
                    prev_was_newline_comment = false;
                }
            }
        }
    }

    for (size_t i = 0; i < tokens.size(); ++i) {
        auto& tok = tokens[i];
        if (tok.kind == TokenKind::EndOfFile) break;

        // --- Extract comments between tokens ---
        std::string comments;
        if (i > 0) {
            uint32_t cs = tokens[i - 1].range.end_offset;
            uint32_t ce = tok.range.begin_offset;
            if (ce > cs) {
                comments = extractComments(source_, cs, ce);
            }
        }

        // --- Determine spacing ---
        bool needNewline = false;
        bool needSpace = false;

        if (i == 0) {
            // no spacing
        } else {
            auto& prev = tokens[i - 1];

            // Newline before right brace
            if (tok.kind == TokenKind::RightBrace) {
                needNewline = true;
            }
            // Newline after semicolon (statement ends)
            else if (prev.kind == TokenKind::Semicolon) {
                needNewline = true;
            }
            // Newline after opening brace
            else if (prev.kind == TokenKind::LeftBrace) {
                needNewline = true;
            }
            // Newline after closing brace
            else if (prev.kind == TokenKind::RightBrace) {
                needNewline = true;
            }
            // Newline before top-level declarations
            else if (tok.kind == TokenKind::Keyword_import ||
                     tok.kind == TokenKind::Keyword_class ||
                     tok.kind == TokenKind::Keyword_interface ||
                     tok.kind == TokenKind::Keyword_mapping ||
                     tok.kind == TokenKind::Keyword_enum ||
                     tok.kind == TokenKind::Keyword_ffi) {
                needNewline = true;
            }
            // Newline before @ annotations (but not after , or ()
            else if (tok.kind == TokenKind::At &&
                     prev.kind != TokenKind::Comma &&
                     prev.kind != TokenKind::LeftParen) {
                needNewline = true;
            }
            // Comma: space if not followed by )
            else if (prev.kind == TokenKind::Comma) {
                needSpace = (tok.kind != TokenKind::RightParen);
            }
            // No space after (
            else if (prev.kind == TokenKind::LeftParen) {
                needSpace = false;
            }
            // No space before ), ,, ;
            else if (tok.kind == TokenKind::RightParen ||
                     tok.kind == TokenKind::Comma ||
                     tok.kind == TokenKind::Semicolon) {
                needSpace = false;
            }
            // No space around .
            else if (tok.kind == TokenKind::Dot || prev.kind == TokenKind::Dot) {
                needSpace = false;
            }
            // No space after @ (annotation)
            else if (prev.kind == TokenKind::At) {
                needSpace = false;
            }
            // No space before : (section header like action:)
            else if (tok.kind == TokenKind::Colon) {
                needSpace = false;
            }
            // Binary operators: space on both sides
            else if (isBinaryOp(tok.kind) || isBinaryOp(prev.kind)) {
                needSpace = true;
            }
            // After statement keywords: space (unless ; or :)
            else if (isKeywordStmt(prev.kind)) {
                needSpace = (tok.kind != TokenKind::Semicolon &&
                             tok.kind != TokenKind::Colon &&
                             tok.kind != TokenKind::LeftBrace);
            }
            // After type keyword: space before identifier
            else if (isTypeKeyword(prev.kind)) {
                needSpace = (tok.kind == TokenKind::Identifier ||
                             tok.kind == TokenKind::At ||
                             isTypeKeyword(tok.kind));
            }
            // After colon: space
            else if (prev.kind == TokenKind::Colon) {
                needSpace = true;
            }
            // Before opening brace: space (unless preceded by ()
            else if (tok.kind == TokenKind::LeftBrace) {
                needSpace = (prev.kind != TokenKind::LeftParen);
            }
            // Default: space
            else {
                needSpace = true;
            }

            // Special: no space before ( when it's a function call
            // (Identifier followed by ()
            if (tok.kind == TokenKind::LeftParen &&
                prev.kind == TokenKind::Identifier) {
                needSpace = false;
            }
            // But space before ( after keywords like if/while/for
            if (tok.kind == TokenKind::LeftParen &&
                isKeywordStmt(prev.kind)) {
                needSpace = true;
            }
        }

        // --- Handle comments ---
        if (!comments.empty()) {
            bool hasLineComment = (comments.find("//") != std::string::npos);
            bool hasBlockComment = (comments.find("/*") != std::string::npos);

            // If we need a newline and have a block comment, put comment on its own line
            if (needNewline && hasBlockComment) {
                if (!lineStart) out << "\n" << indentStr(indent);
                out << comments << "\n" << indentStr(indent);
                lineStart = true;
                needNewline = false; // already emitted
                needSpace = false;
                prev_was_newline_comment = true;
            }
            // If we need a newline and have a line comment, put it on its own line
            else if (needNewline && hasLineComment) {
                if (!lineStart) out << "\n" << indentStr(indent);
                out << comments;
                out << "\n" << indentStr(indent);
                lineStart = true;
                needNewline = false;
                needSpace = false;
                prev_was_newline_comment = true;
            }
            // Line comment after some code on the same line
            else if (hasLineComment) {
                out << " " << comments;
                out << "\n" << indentStr(indent);
                lineStart = true;
                needNewline = false;
                needSpace = false;
                prev_was_newline_comment = true;
            }
            // Block comment - emit with preceding space if inline
            else if (hasBlockComment) {
                if (!lineStart) out << " ";
                out << comments << " ";
                lineStart = false;
                needNewline = false;
                needSpace = false;
                prev_was_newline_comment = false;
            }
        } else {
            prev_was_newline_comment = false;
        }

        // --- Adjust indent for right brace (BEFORE spacing) ---
        if (tok.kind == TokenKind::RightBrace) {
            indent = std::max(0, indent - 1);
        }

        // --- Emit spacing before token ---
        if (needNewline) {
            if (!lineStart) {
                bool blank = (tok.kind == TokenKind::Keyword_class ||
                              tok.kind == TokenKind::Keyword_interface ||
                              tok.kind == TokenKind::Keyword_mapping ||
                              tok.kind == TokenKind::Keyword_enum ||
                              tok.kind == TokenKind::Keyword_ffi ||
                              tok.kind == TokenKind::Keyword_import);
                static bool firstTopLevel = true;
                if (blank && !firstTopLevel && !lineStart) out << "\n";
                if (blank) firstTopLevel = false;
                out << "\n" << indentStr(indent);
            }
            lineStart = false;
        } else if (needSpace) {
            if (!lineStart) out << " ";
            lineStart = false;
        }

        // --- Emit token ---
        out << tokenStr(tok);
        lineStart = false;

        // --- Adjust indent after left brace ---
        if (tok.kind == TokenKind::LeftBrace) {
            ++indent;
        }
    }

    out << "\n";
    return out.str();
}

} // namespace mylang
