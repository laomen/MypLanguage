// expression — part of the Parser implementation, split from src/parser/parser.cpp.
// Pure refactor: member definitions moved out of a large monolith;
// no behavior change. See parser.h for the class.

#include "mylang/Parser.h"

namespace mylang {

// 整数字面量解析（§4.3 P2/P4）：统一进制前缀——0x/0X 十六进制、0b/0B 二进制、
// 0o/0O 八进制、前导零（0755）C 风格八进制、其余十进制（stoll base 0）。
static int64_t parseIntegerLiteralValue(const std::string& s) {
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
        return std::stoll(s.substr(2), nullptr, 2);
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O'))
        return std::stoll(s.substr(2), nullptr, 8);
    return std::stoll(s, nullptr, 0);
}

static TypeKind typeTokenToKind(TokenKind k) {
    switch (k) {
        case TokenKind::Type_byte:  case TokenKind::Type_int8:   return TypeKind::Byte;
        case TokenKind::Type_short: case TokenKind::Type_int16:  return TypeKind::Short;
        case TokenKind::Type_int:   case TokenKind::Type_int32:  return TypeKind::Int;
        case TokenKind::Type_long:  case TokenKind::Type_int64:  return TypeKind::Long;
        case TokenKind::Type_ubyte: case TokenKind::Type_uint8:  return TypeKind::UByte;
        case TokenKind::Type_ushort:case TokenKind::Type_uint16: return TypeKind::UShort;
        case TokenKind::Type_uint:  case TokenKind::Type_uint32: return TypeKind::UInt;
        case TokenKind::Type_ulong: case TokenKind::Type_uint64: return TypeKind::ULong;
        case TokenKind::Type_char:  return TypeKind::Char;
        case TokenKind::Type_bool:  return TypeKind::Bool;
        case TokenKind::Type_bit:   return TypeKind::Bit;
        case TokenKind::Type_bitvector: return TypeKind::BitVector;
        case TokenKind::Type_float: return TypeKind::Float;
        case TokenKind::Type_double:return TypeKind::Double;
        case TokenKind::Type_float4: return TypeKind::Float4;
        case TokenKind::Type_double2:return TypeKind::Double2;
        case TokenKind::Type_int4:   return TypeKind::Int4;
        default: return TypeKind::Void;
    }
}

static std::unique_ptr<Expr> cloneExpr(const Expr& e) {
    switch (e.kind) {
        case ExprKind::IntegerLiteral: {
            auto& v = static_cast<const IntegerLiteralExpr&>(e);
            return std::make_unique<IntegerLiteralExpr>(v.value, v.range,
                v.is_long, v.is_unsigned, v.is_char);
        }
        case ExprKind::FloatLiteral: {
            auto& v = static_cast<const FloatLiteralExpr&>(e);
            return std::make_unique<FloatLiteralExpr>(v.value, v.range, v.is_f32);
        }
        case ExprKind::BoolLiteral: {
            auto& v = static_cast<const BoolLiteralExpr&>(e);
            return std::make_unique<BoolLiteralExpr>(v.value, v.range);
        }
        case ExprKind::StringLiteral: {
            auto& v = static_cast<const StringLiteralExpr&>(e);
            return std::make_unique<StringLiteralExpr>(v.value, v.range);
        }
        case ExprKind::Identifier: {
            auto& v = static_cast<const IdentifierExpr&>(e);
            return std::make_unique<IdentifierExpr>(v.name, v.range);
        }
        case ExprKind::NullLiteral:
            return std::make_unique<NullLiteralExpr>(e.range);
        case ExprKind::ThisExpr:
            return std::make_unique<ThisExpr>(e.range);
        case ExprKind::MemberAccess: {
            auto& v = static_cast<const MemberAccessExpr&>(e);
            return std::make_unique<MemberAccessExpr>(
                cloneExpr(*v.object), v.member_name, v.range);
        }
        case ExprKind::Subscript: {
            auto& v = static_cast<const SubscriptExpr&>(e);
            auto arr = cloneExpr(*v.array);
            auto idx = cloneExpr(*v.index);
            if (!arr || !idx) return nullptr;
            return std::make_unique<SubscriptExpr>(
                std::move(arr), std::move(idx), v.range);
        }
        case ExprKind::BinaryOp: {
            auto& v = static_cast<const BinaryOpExpr&>(e);
            auto l = cloneExpr(*v.lhs);
            auto r = cloneExpr(*v.rhs);
            if (!l || !r) return nullptr;
            return std::make_unique<BinaryOpExpr>(
                std::move(l), v.op, std::move(r), v.range);
        }
        case ExprKind::UnaryOp: {
            auto& v = static_cast<const UnaryOpExpr&>(e);
            auto o = cloneExpr(*v.operand);
            if (!o) return nullptr;
            return std::make_unique<UnaryOpExpr>(v.op, std::move(o), v.range);
        }
        case ExprKind::Convert: {
            auto& v = static_cast<const ConvertExpr&>(e);
            auto o = cloneExpr(*v.operand);
            if (!o) return nullptr;
            return std::make_unique<ConvertExpr>(v.to_kind, std::move(o), v.range, v.to_bitvector_width);
        }
        case ExprKind::Ternary: {
            auto& v = static_cast<const TernaryExpr&>(e);
            auto c = cloneExpr(*v.condition);
            auto t = cloneExpr(*v.true_expr);
            auto f = cloneExpr(*v.false_expr);
            if (!c || !t || !f) return nullptr;
            return std::make_unique<TernaryExpr>(
                std::move(c), std::move(t), std::move(f), v.range);
        }
        case ExprKind::Lambda:
            return nullptr; // can't clone lambda
        case ExprKind::Pipe: {
            auto& v = static_cast<const PipeExpr&>(e);
            auto l = cloneExpr(*v.lhs);
            auto r = cloneExpr(*v.rhs);
            if (!l || !r) return nullptr;
            return std::make_unique<PipeExpr>(
                std::move(l), std::move(r), v.range);
        }
        case ExprKind::Assignment: {
            auto& v = static_cast<const AssignmentExpr&>(e);
            auto t = cloneExpr(*v.target);
            auto val = cloneExpr(*v.value);
            if (!t || !val) return nullptr;
            return std::make_unique<AssignmentExpr>(
                std::move(t), std::move(val), v.range);
        }
        case ExprKind::Call: {
            auto& v = static_cast<const CallExpr&>(e);
            auto callee = cloneExpr(*v.callee);
            if (!callee) return nullptr;
            std::vector<std::unique_ptr<Expr>> args;
            for (auto& a : v.args) {
                auto ca = cloneExpr(*a);
                if (!ca) return nullptr;
                args.push_back(std::move(ca));
            }
            return std::make_unique<CallExpr>(std::move(callee), std::move(args), v.range);
        }
        case ExprKind::NewExpr: {
            auto& v = static_cast<const NewExpr&>(e);
            std::vector<std::unique_ptr<Expr>> args;
            for (auto& a : v.args) {
                auto ca = cloneExpr(*a);
                if (!ca) return nullptr;
                args.push_back(std::move(ca));
            }
            return std::make_unique<NewExpr>(v.class_name, v.type_args, std::move(args), v.range);
        }
        case ExprKind::NewArrayExpr: {
            auto& v = static_cast<const NewArrayExpr&>(e);
            std::vector<std::unique_ptr<Expr>> dims;
            for (auto& d : v.dimensions) {
                auto cd = cloneExpr(*d);
                if (!cd) return nullptr;
                dims.push_back(std::move(cd));
            }
            return std::make_unique<NewArrayExpr>(v.element_type, std::move(dims), v.range);
        }
        case ExprKind::EnumVariant: {
            auto& v = static_cast<const EnumVariantExpr&>(e);
            std::vector<std::unique_ptr<Expr>> args;
            for (auto& a : v.args) {
                auto ca = cloneExpr(*a);
                if (!ca) return nullptr;
                args.push_back(std::move(ca));
            }
            return std::make_unique<EnumVariantExpr>(
                v.enum_name, v.variant_index, std::move(args), v.range);
        }
        case ExprKind::Range: {
            auto& v = static_cast<const RangeExpr&>(e);
            auto s = cloneExpr(*v.start);
            auto e = cloneExpr(*v.end);
            if (!s || !e) return nullptr;
            return std::make_unique<RangeExpr>(std::move(s), std::move(e), v.range);
        }
        case ExprKind::NamedArg: {
            auto& v = static_cast<const NamedArgExpr&>(e);
            auto val = cloneExpr(*v.value);
            if (!val) return nullptr;
            return std::make_unique<NamedArgExpr>(v.name, std::move(val), v.range);
        }
        default:
            return nullptr; // cannot clone complex expressions
    }
}

std::unique_ptr<Expr> Parser::parseExpr() {
    return parseAssignment();
}

std::unique_ptr<Expr> Parser::parsePipe() {
    auto expr = parseConditional();
    while (match(TokenKind::PipeForward)) {
        auto rhs = parseConditional();
        expr = std::make_unique<PipeExpr>(
            std::move(expr), std::move(rhs), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseAssignment() {
    auto expr = parsePipe();
    if (match(TokenKind::Equal)) {
        auto value = parseAssignment();
        return std::make_unique<AssignmentExpr>(
            std::move(expr), std::move(value), previous().range);
    }
    // Compound assignment: a += b  →  a = a + b
    BinaryOpKind compound_op;
    bool is_compound = true;
    if (match(TokenKind::PlusEqual))        compound_op = BinaryOpKind::Add;
    else if (match(TokenKind::MinusEqual))  compound_op = BinaryOpKind::Sub;
    else if (match(TokenKind::StarEqual))   compound_op = BinaryOpKind::Mul;
    else if (match(TokenKind::SlashEqual))  compound_op = BinaryOpKind::Div;
    else if (match(TokenKind::PercentEqual))compound_op = BinaryOpKind::Mod;
    else is_compound = false;

    if (is_compound) {
        auto rhs = parseAssignment();
        auto lhs_clone = cloneExpr(*expr);
        if (!lhs_clone) {
            diag_.error(expr->range, "cannot use compound assignment on this expression");
            return rhs;
        }
        auto binop = std::make_unique<BinaryOpExpr>(
            std::move(lhs_clone), compound_op, std::move(rhs), previous().range);
        return std::make_unique<AssignmentExpr>(
            std::move(expr), std::move(binop), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseConditional() {
    auto expr = parseCoalesce();
    if (match(TokenKind::Question)) {
        auto true_expr = parseExpr();
        consume(TokenKind::Colon, "expected ':' in ternary expression");
        auto false_expr = parseConditional();
        return std::make_unique<TernaryExpr>(
            std::move(expr), std::move(true_expr), std::move(false_expr),
            previous().range);
    }
    return expr;
}

// a ?? b  →  (a != null ? a : b)（右结合）。lhs 被求值两次（条件 + 真分支）——
// 对变量/字段读取无副作用；副作用调用作 lhs 时需自行提临时变量。
std::unique_ptr<Expr> Parser::parseCoalesce() {
    auto lhs = parseLogicalOr();
    if (match(TokenKind::QuestionQuestion)) {
        auto rhs = parseCoalesce();
        auto null_lit = std::make_unique<NullLiteralExpr>(previous().range);
        auto cond = std::make_unique<BinaryOpExpr>(
            cloneExpr(*lhs), BinaryOpKind::Ne, std::move(null_lit), previous().range);
        return std::make_unique<TernaryExpr>(
            std::move(cond), std::move(lhs), std::move(rhs), previous().range);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseLogicalOr() {
    auto expr = parseLogicalAnd();
    while (match(TokenKind::OrOr)) {
        auto rhs = parseLogicalAnd();
        expr = std::make_unique<BinaryOpExpr>(
            std::move(expr), BinaryOpKind::Or, std::move(rhs), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseLogicalAnd() {
    auto expr = parseBitwiseOr();
    while (match(TokenKind::AndAnd)) {
        auto rhs = parseBitwiseOr();
        expr = std::make_unique<BinaryOpExpr>(
            std::move(expr), BinaryOpKind::And, std::move(rhs), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseBitwiseOr() {
    auto expr = parseBitwiseXor();
    while (match(TokenKind::Pipe)) {
        auto rhs = parseBitwiseXor();
        expr = std::make_unique<BinaryOpExpr>(
            std::move(expr), BinaryOpKind::BitOr, std::move(rhs), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseBitwiseXor() {
    auto expr = parseBitwiseAnd();
    while (match(TokenKind::Caret)) {
        auto rhs = parseBitwiseAnd();
        expr = std::make_unique<BinaryOpExpr>(
            std::move(expr), BinaryOpKind::BitXor, std::move(rhs), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseBitwiseAnd() {
    auto expr = parseEquality();
    while (match(TokenKind::Amp)) {
        auto rhs = parseEquality();
        expr = std::make_unique<BinaryOpExpr>(
            std::move(expr), BinaryOpKind::BitAnd, std::move(rhs), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseEquality() {
    auto expr = parseRelational();
    while (true) {
        if (match(TokenKind::EqualEqual)) {
            auto rhs = parseRelational();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Eq, std::move(rhs), previous().range);
        } else if (match(TokenKind::NotEqual)) {
            auto rhs = parseRelational();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Ne, std::move(rhs), previous().range);
        } else {
            break;
        }
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseRelational() {
    auto expr = parseShift();
    while (true) {
        if (match(TokenKind::Less)) {
            auto rhs = parseShift();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Lt, std::move(rhs), previous().range);
        } else if (match(TokenKind::Greater)) {
            auto rhs = parseShift();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Gt, std::move(rhs), previous().range);
        } else if (match(TokenKind::LessEqual)) {
            auto rhs = parseShift();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Le, std::move(rhs), previous().range);
        } else if (match(TokenKind::GreaterEqual)) {
            auto rhs = parseShift();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Ge, std::move(rhs), previous().range);
        } else {
            break;
        }
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseShift() {
    auto expr = parseAdditive();
    while (true) {
        if (match(TokenKind::LessLess)) {
            auto rhs = parseAdditive();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Shl, std::move(rhs), previous().range);
        } else if (match(TokenKind::GreaterGreater)) {
            auto rhs = parseAdditive();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Shr, std::move(rhs), previous().range);
        } else {
            break;
        }
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseAdditive() {
    auto expr = parseRange();
    while (true) {
        if (match(TokenKind::Plus)) {
            auto rhs = parseMultiplicative();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Add, std::move(rhs), previous().range);
        } else if (match(TokenKind::Minus)) {
            auto rhs = parseMultiplicative();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Sub, std::move(rhs), previous().range);
        } else {
            break;
        }
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseRange() {
    auto expr = parseMultiplicative();
    if (match(TokenKind::DoubleDot)) {
        auto end = parseRange();
        return std::make_unique<RangeExpr>(std::move(expr), std::move(end), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseMultiplicative() {
    auto expr = parseUnary();
    while (true) {
        if (match(TokenKind::Star)) {
            auto rhs = parseUnary();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Mul, std::move(rhs), previous().range);
        } else if (match(TokenKind::Slash)) {
            auto rhs = parseUnary();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Div, std::move(rhs), previous().range);
        } else if (match(TokenKind::Percent)) {
            auto rhs = parseUnary();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Mod, std::move(rhs), previous().range);
        } else {
            break;
        }
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseUnary() {
    // §8.2 @gpu reduce 表达式形式：`double s = @gpu reduce (acc,x)=>... init V over a[lo..hi);`
    // 在表达式上下文识别（@ gpu reduce 三 token）；语句形式由 parseStatement 处理
    // （带 `-> out`）。表达式形式结果 = 归约值。
    if (check(TokenKind::At) && peekNext().kind == TokenKind::Identifier &&
        peekNext().value == "gpu" && tokens_.size() > (size_t)current_ + 2 &&
        tokens_[current_ + 2].kind == TokenKind::Identifier &&
        tokens_[current_ + 2].value == "reduce") {
        SourceRange r = peek().range;
        advance(); // @
        advance(); // gpu
        advance(); // reduce
        auto st = parseGpuReduceStmt(true);
        // 表达式形式必然产出 GpuReduceStmt（expr_form=true 时合成临时输出名）
        auto rd = std::unique_ptr<GpuReduceStmt>(
            static_cast<GpuReduceStmt*>(st.release()));
        return std::make_unique<GpuReduceExpr>(std::move(rd), r);
    }
    // await expr — coroutine suspend with value passing (expression form, C2).
    // The operand is a FULL expression: `await n * 2` == `await (n * 2)`.
    if (match(TokenKind::Keyword_await)) {
        // `await;` / `await,` / `await)` — suspend with no value passed out
        if (check(TokenKind::Semicolon) || check(TokenKind::RightParen) ||
            check(TokenKind::Comma)) {
            return std::make_unique<AwaitExpr>(nullptr, previous().range);
        }
        auto operand = parseExpr();
        // Optional timeout: `long r = await Signal.go timeout 100;` (C10)
        if (check(TokenKind::Identifier) && peek().value == "timeout") {
            advance();
            auto to = parseExpr();
            return std::make_unique<AwaitExpr>(std::move(operand), std::move(to),
                                               previous().range);
        }
        return std::make_unique<AwaitExpr>(std::move(operand), previous().range);
    }
    if (match(TokenKind::Bang)) {
        auto operand = parseUnary();
        return std::make_unique<UnaryOpExpr>(
            UnaryOpKind::Not, std::move(operand), previous().range);
    }
    if (match(TokenKind::Tilde)) {
        // §5.1 一元取反：~x（整型/bitvector/bit 位取反）
        auto operand = parseUnary();
        return std::make_unique<UnaryOpExpr>(
            UnaryOpKind::BitNot, std::move(operand), previous().range);
    }
    if (match(TokenKind::Minus)) {
        auto operand = parseUnary();
        return std::make_unique<UnaryOpExpr>(
            UnaryOpKind::Negate, std::move(operand), previous().range);
    }
    // Prefix ++/--: desugar as assignment `x = x ± N`.
    // Collect ALL consecutive ++/-- first. A naive per-operator desugar that
    // recursively parses the operand and cloneExpr's it nests AssignmentExprs,
    // doubling the tree at every level → exponential AST growth (`----`×25 =
    // 2^26 nodes → OOM, found by fuzz/`内存占用太大挂了`). Summing the delta
    // keeps the AST linear and is semantically equivalent for pure chains
    // (each nested level telescopes onto the base lvalue).
    if (check(TokenKind::PlusPlus) || check(TokenKind::MinusMinus)) {
        int64_t net = 0;
        while (match(TokenKind::PlusPlus)) net += 1;
        while (match(TokenKind::MinusMinus)) net -= 1;
        auto operand = parseUnary();
        if (net == 0) return operand;  // `++--x` etc.: deltas cancel out
        auto delta = std::make_unique<IntegerLiteralExpr>(
            net < 0 ? -net : net, previous().range);
        BinaryOpKind op = net < 0 ? BinaryOpKind::Sub : BinaryOpKind::Add;
        auto binop = std::make_unique<BinaryOpExpr>(
            cloneExpr(*operand), op, std::move(delta), previous().range);
        return std::make_unique<AssignmentExpr>(
            std::move(operand), std::move(binop), previous().range);
    }
    return parsePostfix();
}

std::unique_ptr<Expr> Parser::parsePostfix() {
    auto expr = parsePrimary();

    while (true) {
        // Generic call: foo<int>(args) or ClassName.method<T>(args) — probe with
        // a diagnostic-free token scan (parseType can emit spurious errors on
        // `E < energies[mid]` etc. when the probe fails, so we must not call it
        // during lookahead).
        if ((expr->kind == ExprKind::Identifier || expr->kind == ExprKind::MemberAccess) &&
            check(TokenKind::Less) && scanGenericTypeArgs()) {
            std::vector<TypeNode> targs;
            advance(); // consume '<'
            targs.push_back(parseType());
            while (match(TokenKind::Comma)) {
                targs.push_back(parseType());
            }
            consumeGenericClose("expected '>' after generic arguments");
            auto args = parseCallArgs();
            auto call = std::make_unique<CallExpr>(
                std::move(expr), std::move(args), previous().range);
            call->call_type_args = std::move(targs);
            expr = std::move(call);
            continue;
        }
        if (check(TokenKind::LeftParen)) {
            auto args = parseCallArgs();
            expr = std::make_unique<CallExpr>(
                std::move(expr), std::move(args), previous().range);

        } else if (match(TokenKind::Dot)) {
            // Tuple field access: t.0, t.1 — allow an integer after '.'.
            std::string member;
            if (check(TokenKind::IntegerLiteral)) {
                member = advance().value;
            } else {
                member = parseIdentifier("expected member name");
            }
            expr = std::make_unique<MemberAccessExpr>(
                std::move(expr), member, previous().range);

        } else if (match(TokenKind::QuestionDot)) {
            // Null-safe 访问：a?.f  →  (a != null ? a.f : null)；a?.m(args) 同理。
            // 结果类型须可空（类字段/类返回的方法）。a 被求值两次（条件 + 真分支）。
            std::string member = parseIdentifier("expected member name after '?.'");
            std::unique_ptr<Expr> access = std::make_unique<MemberAccessExpr>(
                cloneExpr(*expr), member, previous().range);
            if (check(TokenKind::LeftParen)) {
                auto args = parseCallArgs();
                access = std::make_unique<CallExpr>(
                    std::move(access), std::move(args), previous().range);
            }
            auto null_lit = std::make_unique<NullLiteralExpr>(previous().range);
            auto cond = std::make_unique<BinaryOpExpr>(
                cloneExpr(*expr), BinaryOpKind::Ne, std::move(null_lit), previous().range);
            expr = std::make_unique<TernaryExpr>(
                std::move(cond), std::move(access),
                std::make_unique<NullLiteralExpr>(previous().range), previous().range);

        } else if (match(TokenKind::LeftBracket)) {
            auto index = parseExpr();
            consume(TokenKind::RightBracket, "expected ']' after index");
            expr = std::make_unique<SubscriptExpr>(
                std::move(expr), std::move(index), previous().range);

        } else if (match(TokenKind::PlusPlus)) {
            // Postfix ++: desugar as a = a + 1
            auto one = std::make_unique<IntegerLiteralExpr>(1, previous().range);
            auto binop = std::make_unique<BinaryOpExpr>(
                cloneExpr(*expr), BinaryOpKind::Add, std::move(one), previous().range);
            expr = std::make_unique<AssignmentExpr>(
                std::move(expr), std::move(binop), previous().range);

        } else if (match(TokenKind::MinusMinus)) {
            // Postfix --: desugar as a = a - 1
            auto one = std::make_unique<IntegerLiteralExpr>(1, previous().range);
            auto binop = std::make_unique<BinaryOpExpr>(
                cloneExpr(*expr), BinaryOpKind::Sub, std::move(one), previous().range);
            expr = std::make_unique<AssignmentExpr>(
                std::move(expr), std::move(binop), previous().range);

        } else {
            break;
        }
    }

    return expr;
}

std::unique_ptr<Expr> Parser::parsePrimary() {
    ParserDepthGuard g(recursion_depth_, 300);
    if (g.exceeded) {
        diag_.error(peek().range, "expression nested too deeply");
        // Skip the rest of the deeply-nested '('...')' group in one shot
        // (balanced-paren consumption) so the parser terminates instead of
        // re-descending through thousands of parens.
        int nest = 0;
        while (!isAtEnd()) {
            TokenKind k = peek().kind;
            if (k == TokenKind::LeftParen) nest++;
            else if (k == TokenKind::RightParen) {
                if (--nest <= 0) { advance(); break; }
            }
            advance();
        }
        return std::make_unique<IdentifierExpr>("__error__", peek().range);
    }
    // bitvector<N>(x) —— 带宽度目标的显式转换（先于通用类型关键字转换处理，
    // 因为 bitvector 后跟 '<' 而非 '('）。
    if (check(TokenKind::Type_bitvector) && current_ + 2 < tokens_.size() &&
        tokens_[current_ + 1].kind == TokenKind::Less) {
        SourceRange r = peek().range;
        advance();                          // bitvector
        consume(TokenKind::Less, "expected '<' after bitvector");
        int bw = 0;
        if (check(TokenKind::IntegerLiteral)) {
            try { bw = (int)parseIntegerLiteralValue(advance().value); }
            catch (...) { bw = 0; }
        } else {
            diag_.error(peek().range, "expected integer width in bitvector<N>");
        }
        consumeGenericClose("expected '>' after bitvector width");
        if (match(TokenKind::LeftParen)) {
            auto op = parseExpr();
            consume(TokenKind::RightParen, "expected ')' after conversion operand");
            return std::make_unique<ConvertExpr>(TypeKind::BitVector, std::move(op), r, bw);
        }
        diag_.error(r, "expected '(' after bitvector<N>");
        return std::make_unique<IdentifierExpr>("__error__", r);
    }
    // 显式类型转换：uint8(x) / long(x) / double(x)（类型关键字后跟 '('）
    if (!isAtEnd()) {
        TypeKind ck = typeTokenToKind(peek().kind);
        if (ck != TypeKind::Void && current_ + 1 < tokens_.size() &&
            tokens_[current_ + 1].kind == TokenKind::LeftParen) {
            SourceRange r = peek().range;
            advance();                 // 类型关键字
            advance();                 // '('
            auto op = parseExpr();
            consume(TokenKind::RightParen, "expected ')' after conversion operand");
            return std::make_unique<ConvertExpr>(ck, std::move(op), r);
        }
    }
    if (match(TokenKind::IntegerLiteral)) {
        int64_t val = parseIntegerLiteralValue(previous().value);
        return std::make_unique<IntegerLiteralExpr>(val, previous().range);
    }
    if (match(TokenKind::LongLiteral)) {
        int64_t val = parseIntegerLiteralValue(previous().value);
        return std::make_unique<IntegerLiteralExpr>(val, previous().range, true);
    }
    if (match(TokenKind::UIntLiteral)) {
        int64_t val = parseIntegerLiteralValue(previous().value);
        return std::make_unique<IntegerLiteralExpr>(val, previous().range, false, true);
    }
    if (match(TokenKind::FloatLiteral)) {
        double val = std::stod(previous().value);
        return std::make_unique<FloatLiteralExpr>(val, previous().range);
    }
    if (match(TokenKind::FloatLiteral32)) {
        double val = std::stod(previous().value);
        return std::make_unique<FloatLiteralExpr>(val, previous().range, true);
    }
    if (match(TokenKind::BoolLiteral) || match(TokenKind::Keyword_true)) {
        return std::make_unique<BoolLiteralExpr>(true, previous().range);
    }
    if (match(TokenKind::Keyword_false)) {
        return std::make_unique<BoolLiteralExpr>(false, previous().range);
    }
    if (match(TokenKind::StringLiteral)) {
        auto range = previous().range;
        auto result = expandDollarInterpolation(previous().value, range, previous().raw);
        // "${expr}" 插值：lexer 在字符串内遇 '$'+'{' 时合成
        //   StringLiteral(prefix) InterpOpen <expr> InterpClose StringLiteral(suffix) ...
        // 这里把它们折叠成 + 拼接（sema/codegen 复用 string + any → string）。
        while (check(TokenKind::InterpOpen)) {
            consume(TokenKind::InterpOpen, "expected interpolation start");
            auto expr = parseExpr();
            consume(TokenKind::InterpClose, "expected '}' to close string interpolation");
            result = std::make_unique<BinaryOpExpr>(std::move(result), BinaryOpKind::Add,
                                                    std::move(expr), previous().range);
            if (check(TokenKind::StringLiteral)) {
                auto tok = advance();
                auto suffix = expandDollarInterpolation(tok.value, tok.range, tok.raw);
                result = std::make_unique<BinaryOpExpr>(std::move(result), BinaryOpKind::Add,
                                                        std::move(suffix), tok.range);
            }
        }
        return result;
    }
    if (match(TokenKind::CharLiteral)) {
        // char literal value is a single-character string
        int64_t val = previous().value.empty() ? 0 : (int64_t)(unsigned char)previous().value[0];
        return std::make_unique<IntegerLiteralExpr>(val, previous().range, false, false, true);
    }
    if (match(TokenKind::NullLiteral) || match(TokenKind::Keyword_null)) {
        return std::make_unique<NullLiteralExpr>(previous().range);
    }
    if (match(TokenKind::Keyword_this)) {
        return std::make_unique<ThisExpr>(previous().range);
    }
    if (match(TokenKind::Keyword_try)) {
        // Expression try: try <expr> catch (e) <expr>  (value on success, fallback on error)
        auto te = parseExpr();
        if (match(TokenKind::Keyword_catch)) {
            consume(TokenKind::LeftParen, "expected '(' after 'catch'");
            std::string cvn = parseIdentifier("expected catch variable name");
            consume(TokenKind::RightParen, "expected ')' after catch variable");
            auto ce = parseExpr();
            return std::make_unique<TryExpr>(std::move(te), cvn, std::move(ce), previous().range);
        }
        diag_.error(peek().range, "expected 'catch' in expression try");
        return std::make_unique<IntegerLiteralExpr>(0, previous().range);
    }
    if (match(TokenKind::Keyword_new)) {
        // new double[n] — dynamic array allocation (parse type manually, avoid parseType which eats [])
        if (peek().kind >= TokenKind::Type_byte && peek().kind <= TokenKind::Type_string) {
            TypeNode elem_type;
            elem_type.range = peek().range;
            auto tk = advance();
            switch (tk.kind) {
                case TokenKind::Type_byte:   elem_type.basic_type = BuiltinType::Byte; break;
                case TokenKind::Type_short:  elem_type.basic_type = BuiltinType::Short; break;
                case TokenKind::Type_int:    elem_type.basic_type = BuiltinType::Int; break;
                case TokenKind::Type_long:   elem_type.basic_type = BuiltinType::Long; break;
                case TokenKind::Type_ubyte:  elem_type.basic_type = BuiltinType::UByte; break;
                case TokenKind::Type_ushort: elem_type.basic_type = BuiltinType::UShort; break;
                case TokenKind::Type_uint:   elem_type.basic_type = BuiltinType::UInt; break;
                case TokenKind::Type_ulong:  elem_type.basic_type = BuiltinType::ULong; break;
                case TokenKind::Type_uint8:  elem_type.basic_type = BuiltinType::UByte; break;
                case TokenKind::Type_uint16: elem_type.basic_type = BuiltinType::UShort; break;
                case TokenKind::Type_uint32: elem_type.basic_type = BuiltinType::UInt; break;
                case TokenKind::Type_uint64: elem_type.basic_type = BuiltinType::ULong; break;
                case TokenKind::Type_int8:   elem_type.basic_type = BuiltinType::Byte; break;
                case TokenKind::Type_int16:  elem_type.basic_type = BuiltinType::Short; break;
                case TokenKind::Type_int32:  elem_type.basic_type = BuiltinType::Int; break;
                case TokenKind::Type_int64:  elem_type.basic_type = BuiltinType::Long; break;
                case TokenKind::Type_char:   elem_type.basic_type = BuiltinType::Char; break;
                case TokenKind::Type_float:  elem_type.basic_type = BuiltinType::Float; break;
                case TokenKind::Type_double: elem_type.basic_type = BuiltinType::Double; break;
                case TokenKind::Type_bool:   elem_type.basic_type = BuiltinType::Bool; break;
                case TokenKind::Type_string: elem_type.basic_type = BuiltinType::String; break;
                default: break;
            }
            std::vector<std::unique_ptr<Expr>> dims;
            while (match(TokenKind::LeftBracket)) {
                auto size_expr = parseExpr();
                consume(TokenKind::RightBracket, "expected ']' after array size");
                dims.push_back(std::move(size_expr));
            }
            if (dims.empty()) {
                diag_.error(previous().range, "expected '[size]' after type in new expression");
                return std::make_unique<NullLiteralExpr>(previous().range);
            }
            return std::make_unique<NewArrayExpr>(std::move(elem_type), std::move(dims), previous().range);
        }
        // new ClassName(args) — class allocation
        std::string class_name = parseIdentifier("expected class name after 'new'");
        // new Type[n] where Type is a generic param or user class (e.g. new T[n],
        // new Foo[n]): identifier followed by '[' — build a dynamic array instead
        // of a class allocation. Purely additive; previously this errored.
        if (check(TokenKind::LeftBracket)) {
            TypeNode elem_type;
            elem_type.range = peek().range;
            elem_type.class_name = class_name;
            std::vector<std::unique_ptr<Expr>> dims;
            while (match(TokenKind::LeftBracket)) {
                auto size_expr = parseExpr();
                consume(TokenKind::RightBracket, "expected ']' after array size");
                dims.push_back(std::move(size_expr));
            }
            if (dims.empty()) {
                diag_.error(previous().range, "expected '[size]' after type in new expression");
                return std::make_unique<NullLiteralExpr>(previous().range);
            }
            return std::make_unique<NewArrayExpr>(std::move(elem_type), std::move(dims), previous().range);
        }
        std::vector<TypeNode> type_args;
        if (match(TokenKind::Less)) {
            type_args = parseTypeArgList();
            consumeGenericClose("expected '>' after generic arguments");
        }
        // new Generic<Arg>[n] — generic class dynamic array (e.g. new Box<int>[2]).
        // The generic args are already parsed; a following '[' makes this an array
        // allocation instead of a class construction.
        if (check(TokenKind::LeftBracket)) {
            TypeNode elem_type;
            elem_type.range = peek().range;
            elem_type.class_name = class_name;
            elem_type.type_args = type_args;
            std::vector<std::unique_ptr<Expr>> dims;
            while (match(TokenKind::LeftBracket)) {
                auto size_expr = parseExpr();
                consume(TokenKind::RightBracket, "expected ']' after array size");
                dims.push_back(std::move(size_expr));
            }
            if (dims.empty()) {
                diag_.error(previous().range, "expected '[size]' after type in new expression");
                return std::make_unique<NullLiteralExpr>(previous().range);
            }
            return std::make_unique<NewArrayExpr>(std::move(elem_type), std::move(dims), previous().range);
        }
        auto args = parseCallArgs();
        return std::make_unique<NewExpr>(class_name, std::move(type_args), std::move(args), previous().range);
    }
    if (check(TokenKind::LeftParen)) {
        // Tuple literal: (a, b, ...) — top-level comma before the matching ')'
        // (checked before consuming '(' so scanTupleLiteral sees the '(').
        if (scanTupleLiteral()) {
            advance(); // consume '('
            std::vector<std::unique_ptr<Expr>> elems;
            if (!check(TokenKind::RightParen)) {
                elems.push_back(parseExpr());
                while (match(TokenKind::Comma)) {
                    if (check(TokenKind::RightParen)) { advance(); break; } // trailing comma
                    elems.push_back(parseExpr());
                }
            }
            consume(TokenKind::RightParen, "expected ')' after tuple literal");
            return std::make_unique<TupleExpr>(std::move(elems), previous().range);
        }
        // consume '('
        advance();
        // Check if this might be a lambda: look ahead for ') =>'
        size_t saved = current_;
        // §8.2：'(' 后紧跟 '@'（如 `(@gpu reduce ...)` 分组）不可能是 lambda
        // （lambda 参数是标识符/类型，不会是 '@'）→ 直接按分组表达式解析。
        if (!isAtEnd() && !(check(TokenKind::At))) {
            for (size_t i = saved; i < tokens_.size(); i++) {
                if (tokens_[i].kind == TokenKind::RightParen && i + 1 < tokens_.size()
                    && tokens_[i + 1].kind == TokenKind::FatArrow) {
                    return parseLambdaExpr();
                }
                if (tokens_[i].kind == TokenKind::RightParen) break;
            }
        }
        // Not a lambda, parse (expr)
        current_ = saved;
        auto expr = parseExpr();
        consume(TokenKind::RightParen, "expected ')' after expression");
        return expr;
    }
    if (check(TokenKind::Identifier) && peek().value == "quote" &&
        peekNext().kind == TokenKind::LeftBrace) {
        // Contextual keyword `quote { ... }` — compile-time AST template (M4).
        // Only recognized when followed by '{'; `char quote = ...` stays a var.
        advance();  // consume 'quote'
        consume(TokenKind::LeftBrace, "expected '{' after 'quote'");
        auto body = parseBlock();
        return std::make_unique<QuoteExpr>(std::move(body), previous().range);
    }
    // M-FN-2 命名 lambda：`fn name(params) => { body }`（body 内 name 自引用递归）。
    if (check(TokenKind::Identifier) && peek().value == "fn" &&
        peekNext().kind == TokenKind::Identifier &&
        peekNext2().kind == TokenKind::LeftParen) {
        advance();  // consume 'fn'
        std::string lname = parseIdentifier("expected lambda name after 'fn'");
        consume(TokenKind::LeftParen, "expected '(' after lambda name");
        auto l = parseLambdaBody();
        static_cast<LambdaExpr*>(l.get())->name = lname;
        return l;
    }
    if (check(TokenKind::Identifier)) {
        // ⚠ Function-argument evaluation order is unspecified in C++: passing
        // `parseIdentifier(...)` and `previous().range` as sibling arguments let
        // the compiler read previous() BEFORE the parse advances, so the
        // identifier got the previous token's range (e.g. the `{` of a block)
        // instead of its own → wrong "undefined symbol" diagnostics. Sequence
        // the parse first, then read the freshly-advanced previous().
        std::string nm = parseIdentifier("expected identifier");
        return std::make_unique<IdentifierExpr>(nm, previous().range);
    }
    if (check(TokenKind::Dollar)) {
        // Macro template parameter: $name → MacroParamExpr placeholder.
        advance();  // consume $
        std::string pname = parseIdentifier("expected macro parameter name after '$'");
        return std::make_unique<MacroParamExpr>(pname, previous().range);
    }

    diag_.error(peek().range,
        std::string("expected expression, got '") + Token::kindName(peek().kind) + "'");
    return std::make_unique<IdentifierExpr>("__error__", peek().range);
}

std::unique_ptr<Expr> Parser::parseLambdaExpr() {
    // Caller has consumed '(' — parse params + body.
    return parseLambdaBody();
}

std::unique_ptr<Expr> Parser::parseLambdaBody() {
    std::vector<ParamDecl> params;
    if (!check(TokenKind::RightParen)) {
        params.push_back(parseParam(false));  // lambda：暂不支持默认值
        while (match(TokenKind::Comma)) {
            params.push_back(parseParam(false));
        }
    }
    consume(TokenKind::RightParen, "expected ')' after parameters");
    consume(TokenKind::FatArrow, "expected '=>' after lambda parameters");
    consume(TokenKind::LeftBrace, "expected '{' for lambda body");
    auto body = parseBlock();
    return std::make_unique<LambdaExpr>(std::move(params), std::move(body), previous().range);
}

bool Parser::isMacroStmtPlaceholder() {
    // current_ is at '$'. A standalone statement placeholder is:
    //   '$' Identifier   followed by  '}' | ';' | EOF
    // (otherwise `$x` is an expression, e.g. `$x = $x + $inc` or `foo($a)`)
    if (!check(TokenKind::Dollar)) return false;
    if (current_ + 1 >= tokens_.size()) return false;
    if (tokens_[current_ + 1].kind != TokenKind::Identifier) return false;
    if (current_ + 2 >= tokens_.size()) return true;  // $param at EOF
    auto nk = tokens_[current_ + 2].kind;
    return nk == TokenKind::RightBrace || nk == TokenKind::Semicolon ||
           nk == TokenKind::Dollar;   // next placeholder (multi-stmt macro body)
}

std::unique_ptr<MacroDecl> Parser::parseMacroDecl() {    auto decl = std::make_unique<MacroDecl>();
    decl->range = previous().range;  // 'macro' token
    decl->name = parseIdentifier("expected macro name after 'macro'");

    consume(TokenKind::LeftParen, "expected '(' after macro name");
    if (!check(TokenKind::RightParen)) {
        do {
            if (!match(TokenKind::Dollar)) {
                diag_.error(peek().range,
                    "expected '$' before macro parameter name");
                break;
            }
            decl->params.push_back(
                parseIdentifier("expected macro parameter name after '$'"));
        } while (match(TokenKind::Comma));
    }
    consume(TokenKind::RightParen, "expected ')' after macro parameters");
    consume(TokenKind::LeftBrace, "expected '{' for macro body");
    decl->body = parseBlock();
    return decl;
}

TypeNode Parser::parseType() {
    TypeNode node;
    node.range = peek().range;

    // Function type: (A, B) -> R (additive; diagnostic-free lookahead)
    if (check(TokenKind::LeftParen) && scanFunctionType()) {
        advance(); // consume '('
        if (!check(TokenKind::RightParen)) {
            node.func_param_types.push_back(parseType());
            while (match(TokenKind::Comma)) {
                node.func_param_types.push_back(parseType());
            }
        }
        consume(TokenKind::RightParen, "expected ')' in function type");
        consume(TokenKind::Arrow, "expected '->' in function type");
        node.func_return_type = std::make_shared<TypeNode>(parseType());
        node.range = peek().range;
        return node;
    }

    // Tuple type: (A, B) — top-level comma, ')' NOT followed by '->'
    // (function types handled above; `(int)` stays a parenthesized single type).
    if (check(TokenKind::LeftParen) && scanTupleType()) {
        advance(); // consume '('
        if (!check(TokenKind::RightParen)) {
            node.func_param_types.push_back(parseType());
            while (match(TokenKind::Comma)) {
                if (check(TokenKind::RightParen)) { advance(); break; } // trailing comma (int,)
                node.func_param_types.push_back(parseType());
            }
        }
        consume(TokenKind::RightParen, "expected ')' in tuple type");
        node.is_tuple = true;
        node.range = peek().range;
        // (A,B)[] — array of tuples
        if (match(TokenKind::LeftBracket)) {
            int arr_size = 0;
            if (check(TokenKind::IntegerLiteral)) {
                auto tok = advance();
                arr_size = std::stoi(tok.value);
            }
            consume(TokenKind::RightBracket, "expected ']' in array type");
            auto elem = std::make_unique<TypeNode>(std::move(node));
            node = TypeNode{};
            node.element_type = std::move(elem);
            node.array_size = arr_size;
            node.range = peek().range;
        }
        // (A,B)? — Option<(A,B)> sugar
        if (match(TokenKind::Question)) {
            TypeNode inner = node;
            node = TypeNode{};
            node.class_name = "Option";
            node.type_args.push_back(inner);
            node.range = inner.range;
        }
        return node;
    }

    if (match(TokenKind::Type_byte))       node.basic_type = BuiltinType::Byte;
    else if (match(TokenKind::Type_short)) node.basic_type = BuiltinType::Short;
    else if (match(TokenKind::Type_int))   node.basic_type = BuiltinType::Int;
    else if (match(TokenKind::Type_long))  node.basic_type = BuiltinType::Long;
    else if (match(TokenKind::Type_ubyte)) node.basic_type = BuiltinType::UByte;
    else if (match(TokenKind::Type_ushort))node.basic_type = BuiltinType::UShort;
    else if (match(TokenKind::Type_uint))  node.basic_type = BuiltinType::UInt;
    else if (match(TokenKind::Type_ulong)) node.basic_type = BuiltinType::ULong;
    else if (match(TokenKind::Type_uint8))  node.basic_type = BuiltinType::UByte;
    else if (match(TokenKind::Type_uint16)) node.basic_type = BuiltinType::UShort;
    else if (match(TokenKind::Type_uint32)) node.basic_type = BuiltinType::UInt;
    else if (match(TokenKind::Type_uint64)) node.basic_type = BuiltinType::ULong;
    else if (match(TokenKind::Type_int8))  node.basic_type = BuiltinType::Byte;
    else if (match(TokenKind::Type_int16)) node.basic_type = BuiltinType::Short;
    else if (match(TokenKind::Type_int32)) node.basic_type = BuiltinType::Int;
    else if (match(TokenKind::Type_int64)) node.basic_type = BuiltinType::Long;
    else if (match(TokenKind::Type_char))  node.basic_type = BuiltinType::Char;
    else if (match(TokenKind::Type_float)) node.basic_type = BuiltinType::Float;
    else if (match(TokenKind::Type_double))node.basic_type = BuiltinType::Double;
    else if (match(TokenKind::Type_float4)) node.basic_type = BuiltinType::Float4;   // §3.6 向量
    else if (match(TokenKind::Type_double2))node.basic_type = BuiltinType::Double2;
    else if (match(TokenKind::Type_int4))   node.basic_type = BuiltinType::Int4;
    else if (match(TokenKind::Type_bool))  node.basic_type = BuiltinType::Bool;
    else if (match(TokenKind::Type_string))node.basic_type = BuiltinType::String;
    else if (match(TokenKind::Type_bit))   node.basic_type = BuiltinType::Bit;
    else if (match(TokenKind::Type_bitvector)) {
        // bitvector<N> — 定长位向量；N 为编译期整数字面量（8/16/32/64）。
        node.basic_type = BuiltinType::BitVector;
        consume(TokenKind::Less, "expected '<' after 'bitvector'");
        if (check(TokenKind::IntegerLiteral)) {
            auto tok = advance();
            try { node.bitvector_width = (int)std::stoll(tok.value); }
            catch (...) { node.bitvector_width = 0; }
        } else {
            diag_.error(peek().range, "expected integer width in bitvector<N>");
        }
        consumeGenericClose("expected '>' after bitvector width");
    }
    else if (match(TokenKind::Keyword_void)) node.basic_type = BuiltinType::Void;
    else if (match(TokenKind::Keyword_var)) {
        // var — type inference marker (actual type resolved in Sema)
        node.basic_type = BuiltinType::Int; // placeholder
        node.is_inferred = true;
    }
    else if (check(TokenKind::Identifier)) {
        node.class_name = parseIdentifier("expected type name");
        // Type alias substitution: `type Name = Type;` — resolve at parse time
        // so every downstream path (sema/codegen, incl. basic_type shortcuts)
        // sees the fully expanded type. Alias must be declared before use.
        if (node.class_name.size() > 0) {
            auto it = aliases_.find(node.class_name);
            if (it != aliases_.end()) node = it->second;
        }
        // Check for generic type arguments: ClassName<Type>
        if (match(TokenKind::Less)) {
            node.type_args = parseTypeArgList();
            consumeGenericClose("expected '>' after generic arguments");
        }
        // Check for qualified name: ClassName::StructName
        if (match(TokenKind::DoubleColon)) {
            std::string nested = parseIdentifier("expected struct name");
            node.class_name = node.class_name + "::" + nested;
        }
    } else {
        diag_.error(peek().range,
            std::string("expected type, got '") + Token::kindName(peek().kind) + "'");
    }

    // Array type: Type[] or Type[size]
    if (match(TokenKind::LeftBracket)) {
        int arr_size = 0;
        // Check for fixed size like Type[10]
        if (check(TokenKind::IntegerLiteral)) {
            auto tok = advance();
            arr_size = std::stoi(tok.value);
            if (arr_size <= 0) {
                // No error — probing case (e.g. arr[0] parsed as type probe)
            }
        }
        consume(TokenKind::RightBracket, "expected ']' in array type");
        auto elem = std::make_unique<TypeNode>(std::move(node));
        node = TypeNode{};
        node.element_type = std::move(elem);
        node.array_size = arr_size;
        node.range = peek().range;
    }

    // Option sugar: Type? ≡ Option<Type> (additive; requires `import option;`)
    // e.g. int? == Option<int>; int[]? == Option<int[]>
    if (match(TokenKind::Question)) {
        TypeNode inner = node;
        node = TypeNode{};
        node.class_name = "Option";
        node.type_args.push_back(inner);
        node.range = inner.range;
    }

    return node;
}

std::string Parser::parseIdentifier(const std::string& error_msg) {
    if (check(TokenKind::Identifier)) {
        auto tok = advance();
        return tok.value;
    }
    diag_.error(peek().range, error_msg);
    return "__error__";
}

bool Parser::isTypeToken(TokenKind k) const {
    if (k >= TokenKind::Type_byte && k <= TokenKind::Type_bitvector) return true;
    if (k == TokenKind::Keyword_void) return true;
    if (k == TokenKind::Keyword_var) return true;
    if (k == TokenKind::Identifier) return true;
    return false;
}

bool Parser::scanGenericTypeArgs() {
    size_t i = current_;
    if (i >= tokens_.size() || tokens_[i].kind != TokenKind::Less) return false;
    i++; // consume '<'
    if (i >= tokens_.size() || tokens_[i].kind == TokenKind::Greater) return false; // <>()
    int depth = 1;
    while (i < tokens_.size()) {
        TokenKind k = tokens_[i].kind;
        if (k == TokenKind::Less) { depth++; i++; continue; }
        if (k == TokenKind::Greater) {
            depth--;
            if (depth == 0) {
                i++;
                return i < tokens_.size() && tokens_[i].kind == TokenKind::LeftParen;
            }
            i++; continue;
        }
        // Tokens that cannot appear inside a MYP type → not a generic call.
        if (k == TokenKind::LeftParen || k == TokenKind::RightParen ||
            k == TokenKind::LeftBrace || k == TokenKind::RightBrace ||
            k == TokenKind::AndAnd || k == TokenKind::OrOr ||
            k == TokenKind::Equal || k == TokenKind::EqualEqual ||
            k == TokenKind::NotEqual || k == TokenKind::Semicolon ||
            k == TokenKind::Colon) {
            return false;
        }
        if (k == TokenKind::LeftBracket) {
            i++;
            // Inside []: only an integer or an immediate ']' is a valid type
            // (`[ident]` is a subscript expression → not a generic call).
            if (i < tokens_.size() && tokens_[i].kind == TokenKind::IntegerLiteral) i++;
            else if (i < tokens_.size() && tokens_[i].kind == TokenKind::RightBracket) {}
            else return false;
            if (i >= tokens_.size() || tokens_[i].kind != TokenKind::RightBracket) return false;
            i++;
            continue;
        }
        i++;
    }
    return false;
}

bool Parser::scanFunctionType() {
    size_t i = current_;
    if (i >= tokens_.size() || tokens_[i].kind != TokenKind::LeftParen) return false;
    i++; // consume '('
    int depth = 1;
    while (i < tokens_.size()) {
        TokenKind k = tokens_[i].kind;
        if (k == TokenKind::LeftParen) { depth++; i++; continue; }
        if (k == TokenKind::RightParen) {
            depth--;
            if (depth == 0) {
                i++;
                return i < tokens_.size() && tokens_[i].kind == TokenKind::Arrow;
            }
            i++; continue;
        }
        if (k == TokenKind::Semicolon) return false; // statement boundary
        i++;
    }
    return false;
}

bool Parser::scanTupleType() {
    size_t i = current_;
    if (i >= tokens_.size() || tokens_[i].kind != TokenKind::LeftParen) return false;
    i++;
    int depth = 1;
    bool saw_comma = false;
    while (i < tokens_.size()) {
        TokenKind k = tokens_[i].kind;
        if (k == TokenKind::LeftParen) { depth++; i++; continue; }
        if (k == TokenKind::RightParen) {
            depth--;
            if (depth == 0) {
                i++;
                if (!saw_comma) return false;
                return !(i < tokens_.size() && tokens_[i].kind == TokenKind::Arrow);
            }
            i++; continue;
        }
        if (depth == 1 && k == TokenKind::Comma) saw_comma = true;
        i++;
    }
    return false;
}

bool Parser::scanTupleLiteral() {
    size_t i = current_;
    if (i >= tokens_.size() || tokens_[i].kind != TokenKind::LeftParen) return false;
    i++;
    int depth = 1;
    bool saw_comma = false;
    while (i < tokens_.size()) {
        TokenKind k = tokens_[i].kind;
        if (k == TokenKind::LeftParen) { depth++; i++; continue; }
        if (k == TokenKind::RightParen) {
            depth--;
            if (depth == 0) {
                i++;
                if (!saw_comma) return false;
                return !(i < tokens_.size() && tokens_[i].kind == TokenKind::FatArrow);
            }
            i++; continue;
        }
        if (depth == 1 && k == TokenKind::Comma) saw_comma = true;
        i++;
    }
    return false;
}

bool Parser::scanDestructureAssign() {
    size_t i = current_;
    if (i >= tokens_.size() || tokens_[i].kind != TokenKind::LeftParen) return false;
    i++;
    int depth = 1;
    bool saw_comma = false;
    while (i < tokens_.size()) {
        TokenKind k = tokens_[i].kind;
        if (k == TokenKind::LeftParen) { depth++; i++; continue; }
        if (k == TokenKind::RightParen) {
            depth--;
            if (depth == 0) {
                i++;
                return saw_comma && i < tokens_.size() &&
                       tokens_[i].kind == TokenKind::Equal;
            }
            i++; continue;
        }
        if (depth == 1 && k == TokenKind::Comma) saw_comma = true;
        i++;
    }
    return false;
}

bool Parser::scanTupleVarDecl() {
    size_t i = current_;
    if (i >= tokens_.size() || tokens_[i].kind != TokenKind::LeftParen) return false;
    i++;
    int depth = 1;
    bool saw_comma = false;
    while (i < tokens_.size()) {
        TokenKind k = tokens_[i].kind;
        if (k == TokenKind::LeftParen) { depth++; i++; continue; }
        if (k == TokenKind::RightParen) {
            depth--;
            if (depth == 0) {
                i++;
                return saw_comma && i < tokens_.size() &&
                       tokens_[i].kind == TokenKind::Identifier;
            }
            i++; continue;
        }
        if (depth == 1 && k == TokenKind::Comma) saw_comma = true;
        i++;
    }
    return false;
}

bool Parser::checkType() const {
    if (isAtEnd()) return false;
    switch (peek().kind) {
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
        case TokenKind::Type_int8:
        case TokenKind::Type_int16:
        case TokenKind::Type_int32:
        case TokenKind::Type_int64:
        case TokenKind::Type_char:
        case TokenKind::Type_float:
        case TokenKind::Type_double:
        case TokenKind::Type_float4:
        case TokenKind::Type_double2:
        case TokenKind::Type_int4:
        case TokenKind::Type_bool:
        case TokenKind::Type_string:
        case TokenKind::Type_bit:
        case TokenKind::Type_bitvector:
        case TokenKind::Identifier:
        case TokenKind::Keyword_void:
        case TokenKind::Keyword_var:
            return true;
        default:
            return false;
    }
}

std::vector<std::string> Parser::parseTypeParamList() {
    std::vector<std::string> params;
    do {
        params.push_back(parseIdentifier("expected type parameter name"));
    } while (match(TokenKind::Comma));
    // Optional constraint clause: `<T where T : Interface>` or `<T : Interface>`.
    // The parsed constraints are stored into the caller's ClassDecl via the
    // out-parameter style: this function returns names; constraints are handled
    // by parseClass which calls this then parses the optional where-clause.
    return params;
}

bool Parser::parseTypeParamConstraints(ClassDecl& cls) {
    if (!match(TokenKind::Keyword_where)) return true;
    do {
        std::string param = parseIdentifier("expected type parameter in where clause");
        consume(TokenKind::Colon, "expected ':' after type parameter in where clause");
        std::string iface = parseIdentifier("expected interface name in where clause");
        // Validate the param is one of the declared type params.
        bool found = false;
        for (auto& tp : cls.type_params) if (tp == param) { found = true; break; }
        if (!found) {
            diag_.error(previous().range,
                "where clause references unknown type parameter '" + param + "'");
            return false;
        }
        cls.type_param_constraints[param] = iface;
    } while (match(TokenKind::Comma));
    return true;
}

// §9 泛型函数 where 约束：fn<T where T : Trait>(...)。与类的 where 子句同构；
// Trait 可以是内置数值 trait（Numeric/Integer/Float/Ordered）或接口名。
bool Parser::parseGenericWhereClause(std::vector<std::string>& type_params,
                                     std::unordered_map<std::string, std::string>& out) {
    if (!match(TokenKind::Keyword_where)) return true;
    do {
        std::string param = parseIdentifier("expected type parameter in where clause");
        consume(TokenKind::Colon, "expected ':' after type parameter in where clause");
        std::string trait = parseIdentifier("expected trait or interface name in where clause");
        bool found = false;
        for (auto& tp : type_params) if (tp == param) { found = true; break; }
        if (!found) {
            diag_.error(previous().range,
                "where clause references unknown type parameter '" + param + "'");
            return false;
        }
        out[param] = trait;
    } while (match(TokenKind::Comma));
    return true;
}

std::vector<TypeNode> Parser::parseTypeArgList() {
    std::vector<TypeNode> args;
    do {
        args.push_back(parseType());
    } while (match(TokenKind::Comma));
    return args;
}

bool Parser::isGenericIdentifier() {
    // Check if the current token sequence looks like an identifier followed by <
    if (!check(TokenKind::Identifier)) return false;
    size_t saved = current_;
    advance();
    bool result = check(TokenKind::Less);
    current_ = saved;
    return result;
}

std::unique_ptr<FFIDecl> Parser::parseFFIDecl() {
    auto decl = std::make_unique<FFIDecl>();
    decl->range = previous().range;
    decl->return_type = parseType();
    decl->name = parseIdentifier("expected function name");
    consume(TokenKind::LeftParen, "expected '('");
    if (!check(TokenKind::RightParen)) {
        decl->params.push_back(parseParam());
        while (match(TokenKind::Comma)) {
            decl->params.push_back(parseParam());
        }
    }
    consume(TokenKind::RightParen, "expected ')' after parameters");
    consume(TokenKind::Semicolon, "expected ';' after FFI declaration");
    return decl;
}

std::unique_ptr<EnumDecl> Parser::parseEnumDecl() {
    auto decl = std::make_unique<EnumDecl>();
    decl->range = previous().range;
    decl->name = parseIdentifier("expected enum name");

    consume(TokenKind::LeftBrace, "expected '{' after enum name");

    int vi = 0;
    while (!check(TokenKind::RightBrace) && !isAtEnd()) {
        size_t before = current_;
        EnumVariant variant;
        variant.range = peek().range;
        variant.name = parseIdentifier("expected variant name");

        // Optional data fields: Variant(type name, ...)
        if (match(TokenKind::LeftParen)) {
            if (!check(TokenKind::RightParen)) {
                variant.params = parseParamList(false);  // 枚举数据字段：无默认值
            }
            consume(TokenKind::RightParen, "expected ')' after variant parameters");
        }

        consume(TokenKind::Semicolon, "expected ';' after variant");
        decl->variants.push_back(std::move(variant));
        vi++;
        // Guarantee forward progress: if neither parseIdentifier nor consume
        // consumed anything this iteration (both failed on an unexpected token
        // like '.', '1' or '"'), skip one token so we can't infinite-loop.
        if (current_ == before) advance();
    }

    consume(TokenKind::RightBrace, "expected '}' after enum body");
    return decl;
}

std::unique_ptr<TypeAliasDecl> Parser::parseTypeAlias() {
    auto decl = std::make_unique<TypeAliasDecl>();
    decl->range = peek().range;

    advance(); // consume 'type'
    decl->name = parseIdentifier("expected alias name after 'type'");
    consume(TokenKind::Equal, "expected '=' in type alias");
    decl->alias_type = parseType();
    consume(TokenKind::Semicolon, "expected ';' after type alias");
    // Register for parse-time substitution (before subsequent decls).
    aliases_[decl->name] = decl->alias_type;
    return decl;
}

// "$name" 插值：把字符串值中每个 "$name" 展开为 Identifier，并与字面片段拼成
// BinaryOp(+) 链。无 '$' 或 '$' 后无合法标识符名时返回纯 StringLiteralExpr。
// （与 selfhost parser.myp 的字符串插值逐字节镜像。）
std::unique_ptr<Expr> Parser::expandDollarInterpolation(const std::string& val,
                                                        SourceRange range, bool raw) {
    // 三引号原始字符串：$ 为字面量，不做 $name 展开。
    if (raw || val.find('$') == std::string::npos) {
        return std::make_unique<StringLiteralExpr>(val, range);
    }
    std::unique_ptr<Expr> result;
    size_t pos = 0;
    while (pos < val.size()) {
        auto dpos = val.find('$', pos);
        if (dpos == std::string::npos) {
            auto part = val.substr(pos);
            auto lit = std::make_unique<StringLiteralExpr>(part, range);
            if (result)
                result = std::make_unique<BinaryOpExpr>(
                    std::move(result), BinaryOpKind::Add, std::move(lit), range);
            else
                result = std::move(lit);
            break;
        }
        if (dpos > pos) {
            auto part = val.substr(pos, dpos - pos);
            auto lit = std::make_unique<StringLiteralExpr>(part, range);
            if (result)
                result = std::make_unique<BinaryOpExpr>(
                    std::move(result), BinaryOpKind::Add, std::move(lit), range);
            else
                result = std::move(lit);
        }
        pos = dpos + 1;
        size_t end = pos;
        while (end < val.size() && (std::isalnum(static_cast<unsigned char>(val[end]))
                                    || val[end] == '_')) end++;
        if (end > pos) {
            auto var_name = val.substr(pos, end - pos);
            auto var_expr = std::make_unique<IdentifierExpr>(var_name, range);
            if (result)
                result = std::make_unique<BinaryOpExpr>(
                    std::move(result), BinaryOpKind::Add, std::move(var_expr), range);
            else
                result = std::move(var_expr);
        }
        pos = end;
    }
    if (result) return result;
    return std::make_unique<StringLiteralExpr>(val, range);
}

}  // namespace {ns}
