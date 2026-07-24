#ifndef MYLANG_TOKEN_H
#define MYLANG_TOKEN_H

#include "SourceLocation.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace mylang {

enum class TokenKind : uint16_t {
    // ----- Literals -----
    IntegerLiteral,
    FloatLiteral,
    StringLiteral,
    CharLiteral,
    BoolLiteral,
    NullLiteral,

    // ----- Identifiers & Keywords -----
    Identifier,
    Keyword_class,
    Keyword_action,
    Keyword_event,
    Keyword_property,
    Keyword_interface,
    Keyword_import,
    Keyword_struct,
    Keyword_function,
    Keyword_static,
    Keyword_mapping,
    Keyword_if,
    Keyword_else,
    Keyword_while,
    Keyword_for,
    Keyword_return,
    Keyword_break,
    Keyword_continue,
    Keyword_true,
    Keyword_false,
    Keyword_null,
    Keyword_this,
    Keyword_new,
    Keyword_void,
    Keyword_var,

    // ----- Types (also keywords) -----
    Type_byte,
    Type_short,
    Type_int,
    Type_long,
    Type_ubyte,
    Type_ushort,
    Type_uint,
    Type_ulong,
    Type_char,
    Type_float,
    Type_double,
    Type_bool,
    Type_string,

    // ----- Operators -----
    Plus,           // +
    Minus,          // -
    Star,           // *
    Slash,          // /
    Percent,        // %
    PlusPlus,       // ++ (reserved)
    MinusMinus,     // -- (reserved)

    // Comparison
    EqualEqual,     // ==
    NotEqual,       // !=
    Less,           // <
    Greater,        // >
    LessEqual,      // <=
    GreaterEqual,   // >=

    // Logical
    AndAnd,         // &&
    OrOr,           // ||
    Bang,           // !

    // Assignment
    Equal,          // =
    PlusEqual,      // +=
    MinusEqual,     // -=
    StarEqual,      // *=
    SlashEqual,     // /=
    PercentEqual,   // %=

    // Delimiters
    LeftParen,      // (
    RightParen,     // )
    LeftBrace,      // {
    RightBrace,     // }
    LeftBracket,    // [
    RightBracket,   // ]
    Semicolon,      // ;
    Colon,          // :
    DoubleColon,    // ::
    Question,       // ?
    Comma,          // ,
    Dot,            // .
    DoubleDot,      // ..
    Arrow,          // ->

    // Annotation
    At,             // @

    // Special
    EndOfFile,
    Unknown,
};

struct Token {
    TokenKind kind = TokenKind::Unknown;
    SourceRange range;
    std::string value;  // literal value or identifier name

    Token() = default;

    Token(TokenKind k, SourceRange r, std::string v = {})
        : kind(k), range(r), value(std::move(v)) {}

    /// Check if token is a specific keyword.
    bool is(TokenKind k) const { return kind == k; }

    /// Check if token is any of the given kinds.
    bool isAny(std::initializer_list<TokenKind> kinds) const {
        for (auto k : kinds) {
            if (kind == k) return true;
        }
        return false;
    }

    /// Get a human-readable name for this token kind.
    static const char* kindName(TokenKind kind);

    /// Get the keyword string for a keyword token.
    static const char* keywordString(TokenKind kind);
};

} // namespace mylang

#endif // MYLANG_TOKEN_H
