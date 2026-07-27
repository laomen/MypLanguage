#include "mylang/Token.h"

namespace mylang {

const char* Token::kindName(TokenKind kind) {
    switch (kind) {
        case TokenKind::IntegerLiteral:   return "integer literal";
        case TokenKind::FloatLiteral:     return "float literal";
        case TokenKind::StringLiteral:    return "string literal";
        case TokenKind::CharLiteral:      return "char literal";
        case TokenKind::BoolLiteral:      return "bool literal";
        case TokenKind::NullLiteral:      return "null";
        case TokenKind::Identifier:       return "identifier";
        case TokenKind::EndOfFile:        return "end of file";
        case TokenKind::Unknown:          return "unknown";
        default:                          return keywordString(kind);
    }
}

const char* Token::keywordString(TokenKind kind) {
    switch (kind) {
        case TokenKind::Keyword_class:      return "class";
        case TokenKind::Keyword_action:     return "action";
        case TokenKind::Keyword_event:      return "event";
        case TokenKind::Keyword_property:   return "property";
        case TokenKind::Keyword_interface:  return "interface";
        case TokenKind::Keyword_import:     return "import";
        case TokenKind::Keyword_mapping:    return "mapping";
        case TokenKind::Keyword_if:         return "if";
        case TokenKind::Keyword_else:       return "else";
        case TokenKind::Keyword_while:      return "while";
        case TokenKind::Keyword_for:        return "for";
        case TokenKind::Keyword_return:     return "return";
        case TokenKind::Keyword_break:      return "break";
        case TokenKind::Keyword_continue:   return "continue";
        case TokenKind::Keyword_true:       return "true";
        case TokenKind::Keyword_false:      return "false";
        case TokenKind::Keyword_null:       return "null";
        case TokenKind::Keyword_this:       return "this";
        case TokenKind::Keyword_new:        return "new";
        case TokenKind::Keyword_void:       return "void";
        case TokenKind::Keyword_var:        return "var";
        case TokenKind::Keyword_enum:       return "enum";
        case TokenKind::Keyword_match:      return "match";
        case TokenKind::Keyword_ffi:         return "ffi";
        case TokenKind::Keyword_try:         return "try";
        case TokenKind::Keyword_catch:       return "catch";
        case TokenKind::Keyword_finally:     return "finally";
        case TokenKind::Keyword_throw:       return "throw";
        case TokenKind::Keyword_static:     return "static";

        case TokenKind::Type_byte:          return "byte";
        case TokenKind::Type_short:         return "short";
        case TokenKind::Type_int:           return "int";
        case TokenKind::Type_long:          return "long";
        case TokenKind::Type_ubyte:         return "ubyte";
        case TokenKind::Type_ushort:        return "ushort";
        case TokenKind::Type_uint:          return "uint";
        case TokenKind::Type_ulong:         return "ulong";
        case TokenKind::Type_char:          return "char";
        case TokenKind::Type_float:         return "float";
        case TokenKind::Type_double:        return "double";
        case TokenKind::Type_bool:          return "bool";
        case TokenKind::Type_string:        return "string";

        case TokenKind::Plus:               return "+";
        case TokenKind::Minus:              return "-";
        case TokenKind::Star:               return "*";
        case TokenKind::Slash:              return "/";
        case TokenKind::Percent:            return "%";
        case TokenKind::EqualEqual:         return "==";
        case TokenKind::NotEqual:           return "!=";
        case TokenKind::Less:               return "<";
        case TokenKind::Greater:            return ">";
        case TokenKind::LessEqual:          return "<=";
        case TokenKind::GreaterEqual:       return ">=";
        case TokenKind::AndAnd:             return "&&";
        case TokenKind::OrOr:               return "||";
        case TokenKind::Bang:               return "!";
        case TokenKind::Equal:              return "=";
        case TokenKind::LeftParen:          return "(";
        case TokenKind::RightParen:         return ")";
        case TokenKind::LeftBrace:          return "{";
        case TokenKind::RightBrace:         return "}";
        case TokenKind::LeftBracket:        return "[";
        case TokenKind::RightBracket:       return "]";
        case TokenKind::Semicolon:          return ";";
        case TokenKind::Colon:              return ":";
        case TokenKind::Question:           return "?";
        case TokenKind::Comma:              return ",";
        case TokenKind::Dot:                return ".";
        case TokenKind::DoubleDot:          return "..";
        case TokenKind::Arrow:              return "->";
        case TokenKind::FatArrow:           return "=>";
        case TokenKind::At:                 return "@";
        case TokenKind::DoubleColon:        return "::";
        case TokenKind::Keyword_struct:     return "struct";
        case TokenKind::Keyword_function:   return "function";
        case TokenKind::PlusPlus:           return "++";
        case TokenKind::MinusMinus:         return "--";
        case TokenKind::PlusEqual:          return "+=";
        case TokenKind::MinusEqual:         return "-=";
        case TokenKind::StarEqual:          return "*=";
        case TokenKind::SlashEqual:         return "/=";
        case TokenKind::PercentEqual:       return "%=";
        default:                            return "?";
    }
}

} // namespace mylang
