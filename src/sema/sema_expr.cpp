// expression — part of the Sema implementation, split from src/sema/sema.cpp.
// Pure refactor: member definitions moved out of a large monolith;
// no behavior change. See sema.h for the class.

#include "mylang/Sema.h"

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace mylang {

// RAII recursion-depth guard for visitExpr — pathologically deep ASTs (e.g. a
// 20k-long `1+1+1+...` chain) would otherwise overflow the stack.
struct SemaDepthGuard {
    int& depth;
    bool exceeded;
    SemaDepthGuard(int& d, int max) : depth(d), exceeded(false) {
        if (++depth > max) exceeded = true;
    }
    ~SemaDepthGuard() { --depth; }
};

TypeInfo Sema::visitExpr(Expr& expr) {
    SemaDepthGuard g(recursion_depth_, 300);
    if (g.exceeded) {
        error(expr.range, "expression nested too deeply");
        return TypeInfo();
    }
    TypeInfo result;
    switch (expr.kind) {
        case ExprKind::IntegerLiteral:
            result = visitIntegerLiteral(static_cast<IntegerLiteralExpr&>(expr));
            break;
        case ExprKind::FloatLiteral:
            result = visitFloatLiteral(static_cast<FloatLiteralExpr&>(expr));
            break;
        case ExprKind::BoolLiteral:
            result = visitBoolLiteral(static_cast<BoolLiteralExpr&>(expr));
            break;
        case ExprKind::StringLiteral:
            result = visitStringLiteral(static_cast<StringLiteralExpr&>(expr));
            break;
        case ExprKind::NullLiteral:
            result = visitNullLiteral(static_cast<NullLiteralExpr&>(expr));
            break;
        case ExprKind::Identifier:
            result = visitIdentifier(static_cast<IdentifierExpr&>(expr));
            break;
        case ExprKind::BinaryOp:
            result = visitBinaryOp(static_cast<BinaryOpExpr&>(expr));
            break;
        case ExprKind::UnaryOp:
            result = visitUnaryOp(static_cast<UnaryOpExpr&>(expr));
            break;
        case ExprKind::Convert:
            result = visitConvert(static_cast<ConvertExpr&>(expr));
            break;
        case ExprKind::Call:
            result = visitCall(static_cast<CallExpr&>(expr));
            break;
        case ExprKind::MemberAccess:
            result = visitMemberAccess(static_cast<MemberAccessExpr&>(expr));
            break;
        case ExprKind::Subscript:
            result = visitSubscript(static_cast<SubscriptExpr&>(expr));
            break;
        case ExprKind::NewExpr:
            result = visitNewExpr(static_cast<NewExpr&>(expr));
            break;
        case ExprKind::NewArrayExpr:
            result = visitNewArrayExpr(static_cast<NewArrayExpr&>(expr));
            break;
        case ExprKind::ThisExpr:
            result = visitThisExpr(static_cast<ThisExpr&>(expr));
            break;
        case ExprKind::Assignment:
            result = visitAssignment(static_cast<AssignmentExpr&>(expr));
            break;
        case ExprKind::Ternary:
            result = visitTernary(static_cast<TernaryExpr&>(expr));
            break;
        case ExprKind::Range:
            result = visitRange(static_cast<RangeExpr&>(expr));
            break;
        case ExprKind::Lambda:
            result = visitLambda(static_cast<LambdaExpr&>(expr));
            break;
        case ExprKind::Pipe:
            result = visitPipe(static_cast<PipeExpr&>(expr));
            break;
        case ExprKind::EnumVariant:
            result = visitEnumVariant(static_cast<EnumVariantExpr&>(expr));
            break;
        case ExprKind::Try:
            result = visitTryExpr(static_cast<TryExpr&>(expr));
            break;
        case ExprKind::TupleExpr: {
            auto& te = static_cast<TupleExpr&>(expr);
            TypeInfo tt(TypeKind::Tuple);
            for (auto& el : te.elements)
                tt.tuple_types.push_back(visitExpr(*el));
            result = tt;
            break;
        }
        case ExprKind::NamedArg: {
            // 防御：正常路径中命名实参在 visitCall/构造器解析时已被重排消除；
            // 残留时按实参值类型处理。
            auto& na = static_cast<NamedArgExpr&>(expr);
            result = visitExpr(*na.value);
            break;
        }
        case ExprKind::Await: {
            if (!in_coro_method_) {
                error(expr.range, "'await' is only allowed inside an '@coro' method");
            }
            auto& ae = static_cast<AwaitExpr&>(expr);
            // await expr 默认求值为 resume 传入值 → long；形态3（@async 调用）
            // 的 await 值为调用返回类型（见下）。
            TypeInfo await_type(TypeKind::Long);
            if (ae.operand) {
                // await ClassName.eventName — block on an event (C4).
                // Recognized here (not in visitMemberAccess) so a bare
                // `ClassName.eventName` outside await keeps existing semantics.
                bool is_event_ref = false;
                if (ae.operand->kind == ExprKind::MemberAccess) {
                    auto& ma = static_cast<MemberAccessExpr&>(*ae.operand);
                    if (ma.object->kind == ExprKind::Identifier) {
                        auto& oid = static_cast<IdentifierExpr&>(*ma.object);
                        if (current_tu_) {
                            for (auto& cls : current_tu_->classes) {
                                if (cls.name == oid.name) {
                                    for (auto& ev : cls.events)
                                        if (ev.name == ma.member_name) { is_event_ref = true; break; }
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!is_event_ref) {
                    TypeInfo op_ti = visitExpr(*ae.operand);
                    // §五-5 形态3: await <@async 调用> — 类型 = 调用返回类型
                    // （普通 await expr 仍是 resume 传入值 → long）
                    if (ae.operand->kind == ExprKind::Call) {
                        auto& call = static_cast<const CallExpr&>(*ae.operand);
                        if (call.callee && isAsyncCallee(call.callee.get()))
                            await_type = op_ti;
                    }
                }
            }
            if (ae.timeout) {
                TypeInfo tt = visitExpr(*ae.timeout);
                if (!expectNumeric(tt, ae.timeout->range))
                    error(ae.timeout->range, "await timeout must be numeric (ms)");
            }
            // await expr evaluates to the value passed in by resume → long
            // (§五-5: 形态3 @async 调用 → 调用返回类型，见上)
            result = await_type;
            break;
        }
    }
    expr.resolved_kind = result.kind;
    return result;
}

TypeInfo Sema::visitTryExpr(TryExpr& expr) {
    // Type-check the try expression (the value on success).
    auto t = visitExpr(*expr.try_expr);
    // Type-check the catch expression (the fallback value on error).
    auto f = visitExpr(*expr.catch_expr);
    if (!typesCompatible(t, f)) {
        error(expr.range, "try/catch expressions have incompatible types: '" +
              typeName(t) + "' and '" + typeName(f) + "'");
        return t;
    }
    return t;
}

TypeInfo Sema::visitIntegerLiteral(IntegerLiteralExpr& expr) {
    auto val = expr.value;
    // 字符字面量 'A' → char（u8 语义，§4.1）
    if (expr.is_char) return TypeInfo(TypeKind::Char);
    // u suffix → 无符号：按值定宽（同有符号字面量：小值→ubyte/ushort，大→uint/ulong）
    if (expr.is_unsigned) {
        if (val >= 0 && val <= 0xFF) return TypeInfo(TypeKind::UByte);
        if (val <= 0xFFFF)           return TypeInfo(TypeKind::UShort);
        if (val <= 0xFFFFFFFFLL)     return TypeInfo(TypeKind::UInt);
        return TypeInfo(TypeKind::ULong);
    }
    // L suffix forces long type
    if (expr.is_long) return TypeInfo(TypeKind::Long);
    TypeInfo result;
    if (val >= -128 && val <= 127)           result = TypeInfo(TypeKind::Byte);
    else if (val >= -32768 && val <= 32767)  result = TypeInfo(TypeKind::Short);
    else if (val >= -2147483648LL && val <= 2147483647LL) result = TypeInfo(TypeKind::Int);
    else                                      result = TypeInfo(TypeKind::Long);
    return result;
}

TypeInfo Sema::visitFloatLiteral(FloatLiteralExpr& expr) {
    return expr.is_f32 ? TypeInfo(TypeKind::Float) : TypeInfo(TypeKind::Double);
}

TypeInfo Sema::visitBoolLiteral(BoolLiteralExpr& expr) {
    return TypeInfo(TypeKind::Bool);
}

TypeInfo Sema::visitStringLiteral(StringLiteralExpr& expr) {
    return TypeInfo(TypeKind::String);
}

TypeInfo Sema::visitNullLiteral(NullLiteralExpr& expr) {
    return TypeInfo(TypeKind::Null);
}

TypeInfo Sema::visitIdentifier(IdentifierExpr& expr) {
    auto* type = symbol_table_.lookup(expr.name);
    if (type) return *type;

    // Check for @static class names — they're accessible as identifiers
    if (current_tu_) {
        for (auto& cls : current_tu_->classes) {
            if (cls.is_static && cls.name == expr.name) {
                TypeInfo class_type(TypeKind::Class);
                class_type.class_name = cls.name;
                return class_type;
            }
        }
    }

    if (in_class_method_) {
        auto member = current_class_member_types_.find(expr.name);
        if (member != current_class_member_types_.end()) return member->second;
    }

    error(expr.range, "undefined symbol '" + expr.name + "'");
    return TypeInfo(TypeKind::Void);
}

TypeInfo Sema::visitBinaryOp(BinaryOpExpr& expr) {
    auto lhs_type = visitExpr(*expr.lhs);
    auto rhs_type = visitExpr(*expr.rhs);

    // Operator overloading: dispatch to a user-defined operator when the
    // operands are not handled by builtin semantics.
    //   Order: 1) struct-internal operator (this = left operand)
    //          2) external @op function matching (lhs_type, rhs_type)
    // Builtin numeric/string/bool is handled first in each case below.
    auto resolveOperator = [&]() -> TypeInfo {
        if (!current_tu_) return TypeInfo(TypeKind::Void);
        std::string sym;
        switch (expr.op) {
            case BinaryOpKind::Add: sym = "+"; break;
            case BinaryOpKind::Sub: sym = "-"; break;
            case BinaryOpKind::Mul: sym = "*"; break;
            case BinaryOpKind::Div: sym = "/"; break;
            case BinaryOpKind::Mod: sym = "%"; break;
            case BinaryOpKind::Eq:  sym = "=="; break;
            case BinaryOpKind::Ne:  sym = "!="; break;
            case BinaryOpKind::Lt:  sym = "<"; break;
            case BinaryOpKind::Gt:  sym = ">"; break;
            case BinaryOpKind::Le:  sym = "<="; break;
            case BinaryOpKind::Ge:  sym = ">="; break;
            default: return TypeInfo(TypeKind::Void);
        }
        // 1) struct-internal operator (this = left operand, 1 param = right)
        if (lhs_type.kind == TypeKind::Struct) {
            for (auto& st : current_tu_->structs) {
                std::string skey = st.parent_class.empty()
                    ? st.name : st.parent_class + "::" + st.name;
                if (skey != lhs_type.class_name) continue;
                for (auto& m : st.functions) {
                    if (m.op_symbol != sym) continue;
                    if (m.params.size() != 1) continue;
                    auto ptype = typeNodeToTypeInfo(m.params[0].type);
                    if (typesCompatible(ptype, rhs_type)) {
                        expr.op_call = std::make_shared<OperatorCall>();
                        expr.op_call->kind = "struct_method";
                        expr.op_call->struct_key = skey;
                        expr.op_call->method = m.name;
                        return typeNodeToTypeInfo(m.return_type);
                    }
                }
            }
        }
        // 2) external @op function matching (lhs_type, rhs_type)
        for (auto& f : current_tu_->functions) {
            if (f.op_symbol != sym) continue;
            if (f.params.size() != 2) continue;
            auto p0 = typeNodeToTypeInfo(f.params[0].type);
            auto p1 = typeNodeToTypeInfo(f.params[1].type);
            if (typesCompatible(p0, lhs_type) && typesCompatible(p1, rhs_type)) {
                expr.op_call = std::make_shared<OperatorCall>();
                expr.op_call->kind = "function";
                expr.op_call->func_name = f.name;
                return typeNodeToTypeInfo(f.return_type);
            }
        }
        return TypeInfo(TypeKind::Void);
    };

    switch (expr.op) {
        case BinaryOpKind::Add: {
            // String concatenation: string + any → string
            if (lhs_type.kind == TypeKind::String || rhs_type.kind == TypeKind::String)
                return TypeInfo(TypeKind::String);
            if (lhs_type.kind == TypeKind::Void || rhs_type.kind == TypeKind::Void)
                return TypeInfo(TypeKind::Int);  // cascading error recovery
            TypeInfo op_ret = resolveOperator();
            if (op_ret.kind != TypeKind::Void) return op_ret;
            if (!expectNumeric(lhs_type, expr.lhs->range) ||
                !expectNumeric(rhs_type, expr.rhs->range)) {
                return TypeInfo(TypeKind::Int);
            }
            goto numeric_common;
        }
        case BinaryOpKind::Sub:
        case BinaryOpKind::Mul: case BinaryOpKind::Div: case BinaryOpKind::Mod: {
            if (lhs_type.kind == TypeKind::Void || rhs_type.kind == TypeKind::Void)
                return TypeInfo(TypeKind::Int);  // cascading error recovery
            TypeInfo op_ret = resolveOperator();
            if (op_ret.kind != TypeKind::Void) return op_ret;
            if (!expectNumeric(lhs_type, expr.lhs->range) ||
                !expectNumeric(rhs_type, expr.rhs->range)) {
                return TypeInfo(TypeKind::Int);
            }
            goto numeric_common;
        }

        case BinaryOpKind::Eq: case BinaryOpKind::Ne: {
            TypeInfo op_ret = resolveOperator();
            if (op_ret.kind != TypeKind::Void) return op_ret;
            return TypeInfo(TypeKind::Bool);
        }

        case BinaryOpKind::Lt: case BinaryOpKind::Gt:
        case BinaryOpKind::Le: case BinaryOpKind::Ge: {
            TypeInfo op_ret = resolveOperator();
            if (op_ret.kind != TypeKind::Void) return op_ret;
            // P1 §6.4：string 词法比较（strcmp），消除 "只能用 Str.cmp" 的不对称
            if (lhs_type.kind == TypeKind::String || rhs_type.kind == TypeKind::String) {
                if (lhs_type.kind != TypeKind::String || rhs_type.kind != TypeKind::String)
                    error(expr.range, "string comparison requires both operands to be strings");
                return TypeInfo(TypeKind::Bool);
            }
            if (!expectNumeric(lhs_type, expr.lhs->range))
                return TypeInfo(TypeKind::Bool);
            // uint32：无符号比较（codegen 选 ULT/UGT/ULE/UGE）。
            // 决定规则同 numeric_common：只要出现 ULong 就是无符号；否则只要
            // 出现 Long/Double/Float 就按有符号；否则有任一无符号即无符号。
            expr.lhs_unsigned = isUnsignedKind(lhs_type.kind);
            expr.rhs_unsigned = isUnsignedKind(rhs_type.kind);
            bool uc = expr.lhs_unsigned || expr.rhs_unsigned;
            if (lhs_type.kind == TypeKind::Long || rhs_type.kind == TypeKind::Long ||
                lhs_type.kind == TypeKind::Double || rhs_type.kind == TypeKind::Double ||
                lhs_type.kind == TypeKind::Float || rhs_type.kind == TypeKind::Float)
                uc = false;
            if (lhs_type.kind == TypeKind::ULong || rhs_type.kind == TypeKind::ULong)
                uc = true;
            expr.result_unsigned = uc;
            return TypeInfo(TypeKind::Bool);
        }

        case BinaryOpKind::And: case BinaryOpKind::Or:
            expectBool(lhs_type, expr.lhs->range);
            expectBool(rhs_type, expr.rhs->range);
            return TypeInfo(TypeKind::Bool);

        case BinaryOpKind::BitAnd: case BinaryOpKind::BitOr:
        case BinaryOpKind::BitXor: case BinaryOpKind::Shl:
        case BinaryOpKind::Shr:
            if (!expectNumeric(lhs_type, expr.lhs->range) ||
                !expectNumeric(rhs_type, expr.rhs->range)) {
                return TypeInfo(TypeKind::Int);
            }
            expr.lhs_unsigned = isUnsignedKind(lhs_type.kind);
            expr.rhs_unsigned = isUnsignedKind(rhs_type.kind);
            // uint32：位运算/移位在无符号上保持无符号（uint >> n 是逻辑右移）。
            if (lhs_type.kind == TypeKind::ULong || rhs_type.kind == TypeKind::ULong) {
                expr.result_unsigned = true;
                return TypeInfo(TypeKind::ULong);
            }
            if (lhs_type.kind == TypeKind::Long || rhs_type.kind == TypeKind::Long) {
                expr.result_unsigned = false;
                return TypeInfo(TypeKind::Long);
            }
            if (lhs_type.kind == TypeKind::UInt || rhs_type.kind == TypeKind::UInt) {
                expr.result_unsigned = true;
                return TypeInfo(TypeKind::UInt);
            }
            if (lhs_type.kind == TypeKind::UByte || rhs_type.kind == TypeKind::UByte ||
                lhs_type.kind == TypeKind::UShort || rhs_type.kind == TypeKind::UShort) {
                expr.result_unsigned = true;
                return TypeInfo(TypeKind::UInt);
            }
            return TypeInfo(TypeKind::Int);
    }
    return TypeInfo(TypeKind::Void);

numeric_common:
    expr.lhs_unsigned = isUnsignedKind(lhs_type.kind);
    expr.rhs_unsigned = isUnsignedKind(rhs_type.kind);
    if (lhs_type.kind == TypeKind::Double || rhs_type.kind == TypeKind::Double)
        return TypeInfo(TypeKind::Double);
    if (lhs_type.kind == TypeKind::Float || rhs_type.kind == TypeKind::Float)
        return TypeInfo(TypeKind::Float);
    if (lhs_type.kind == TypeKind::ULong || rhs_type.kind == TypeKind::ULong) {
        expr.result_unsigned = true;
        return TypeInfo(TypeKind::ULong);
    }
    if (lhs_type.kind == TypeKind::Long || rhs_type.kind == TypeKind::Long) {
        expr.result_unsigned = false;
        return TypeInfo(TypeKind::Long);
    }
    if (lhs_type.kind == TypeKind::UInt || rhs_type.kind == TypeKind::UInt) {
        expr.result_unsigned = true;
        return TypeInfo(TypeKind::UInt);
    }
    if (lhs_type.kind == TypeKind::Int || rhs_type.kind == TypeKind::Int) {
        expr.result_unsigned = false;
        return TypeInfo(TypeKind::Int);
    }
    // UByte/UShort（及 Byte/Short）→ UInt
    if (lhs_type.kind == TypeKind::UByte || rhs_type.kind == TypeKind::UByte ||
        lhs_type.kind == TypeKind::UShort || rhs_type.kind == TypeKind::UShort) {
        expr.result_unsigned = true;
        return TypeInfo(TypeKind::UInt);
    }
    return TypeInfo(TypeKind::Int);
}

TypeInfo Sema::visitUnaryOp(UnaryOpExpr& expr) {
    auto operand_type = visitExpr(*expr.operand);
    switch (expr.op) {
        case UnaryOpKind::Negate:
            if (!expectNumeric(operand_type, expr.operand->range))
                return TypeInfo(TypeKind::Int);
            return operand_type;
        case UnaryOpKind::Not:
            expectBool(operand_type, expr.operand->range);
            return TypeInfo(TypeKind::Bool);
    }
    return TypeInfo(TypeKind::Void);
}

TypeInfo Sema::visitConvert(ConvertExpr& expr) {
    auto ot = visitExpr(*expr.operand);
    // P1 D6（docs/type_system_design §3.4）：bool 入显式转换链。int(b)/long(b)/
    // double(b)/float(b) 与 bool(n)/bool(f) 都是显式 cast；隐式 bool↔整型仍禁
    // （expectBool 严格）。string 仍不入 T(x)（那是 parse，§6.2）。
    bool src_ok = isNumericKind(ot.kind) || ot.kind == TypeKind::Bool;
    bool dst_ok = isNumericKind(expr.to_kind) || expr.to_kind == TypeKind::Bool;
    if (!src_ok || !dst_ok) {
        error(expr.range, "cannot convert '" + typeName(ot) + "' to '" +
              typeName(TypeInfo(expr.to_kind)) +
              "' (conversion operand and target must be numeric or bool)");
    }
    return TypeInfo(expr.to_kind);
}

// P0 bitcast<T,U>(x)：位保持重解释（docs/type_system_design §5.2）。与 cast（改值）
// 相反，bitcast 保持位不变（float↔uint 等）。语法：bitcast<U>(x)（源从操作数推断）
// 或 bitcast<T,U>(x)（显式源）。要求源/目标同宽。
TypeInfo Sema::visitBitcast(CallExpr& expr) {
    if (expr.args.size() != 1) {
        error(expr.range, "bitcast takes exactly one argument");
        return TypeInfo(TypeKind::Void);
    }
    if (expr.call_type_args.size() < 1 || expr.call_type_args.size() > 2) {
        error(expr.range, "bitcast requires one or two type arguments: bitcast<U>(x) or bitcast<T,U>(x)");
        return TypeInfo(TypeKind::Void);
    }
    auto operand_ti = visitExpr(*expr.args[0]);
    TypeInfo target_ti = typeNodeToTypeInfo(expr.call_type_args.back());
    TypeInfo source_ti = (expr.call_type_args.size() == 2)
        ? typeNodeToTypeInfo(expr.call_type_args[0])
        : operand_ti;
    if (expr.call_type_args.size() == 2 && !typesCompatible(source_ti, operand_ti)) {
        error(expr.range, "bitcast source type '" + typeName(source_ti) +
              "' does not match operand type '" + typeName(operand_ti) + "'");
        return TypeInfo(TypeKind::Void);
    }
    auto bitcast_width = [](TypeKind k) -> int {
        switch (k) {
            case TypeKind::Byte: case TypeKind::UByte: case TypeKind::Char: return 8;
            case TypeKind::Short: case TypeKind::UShort: return 16;
            case TypeKind::Int: case TypeKind::UInt: case TypeKind::Float: return 32;
            case TypeKind::Long: case TypeKind::ULong: case TypeKind::Double: return 64;
            default: return 0;
        }
    };
    int sw = bitcast_width(source_ti.kind), tw = bitcast_width(target_ti.kind);
    if (sw == 0 || tw == 0) {
        error(expr.range, "bitcast requires numeric source and target types (integer/float/char)");
        return TypeInfo(TypeKind::Void);
    }
    if (sw != tw) {
        error(expr.range, "bitcast requires source and target of the same width (" +
              std::to_string(sw) + " vs " + std::to_string(tw) + " bits)");
        return TypeInfo(TypeKind::Void);
    }
    expr.resolved_kind = target_ti.kind;
    return target_ti;
}

// P1 bytes(s) / str(bytes)（docs §6.3）：string ↔ ubyte[] 互转。
// 仅当实参类型匹配时作为内建（bytes 收 string；str 收 ubyte[] 动态数组），否则
// 返回 Void 让调用方落到普通函数解析（用户同名函数不受影响）。
TypeInfo Sema::visitBytesStr(CallExpr& expr, const std::string& name) {
    if (expr.args.size() != 1) {
        error(expr.range, name + " takes exactly one argument");
        return TypeInfo(TypeKind::Void);
    }
    auto ot = visitExpr(*expr.args[0]);
    if (name == "bytes") {
        if (ot.kind != TypeKind::String) return TypeInfo(TypeKind::Void);  // 非 string → 普通解析
        TypeInfo rt(TypeKind::Array);
        rt.element_type = std::make_shared<TypeInfo>(TypeKind::UByte);
        expr.resolved_kind = TypeKind::Array;
        return rt;
    }
    // str(bytes)：收 ubyte[]（或 u8 元素的定长数组）
    bool is_ubyte_arr = (ot.kind == TypeKind::Array && ot.element_type &&
                         ot.element_type->kind == TypeKind::UByte);
    if (!is_ubyte_arr) return TypeInfo(TypeKind::Void);   // 非 ubyte[] → 普通解析
    expr.resolved_kind = TypeKind::String;
    return TypeInfo(TypeKind::String);
}

// P2 parse* 全族（docs §6.2）：统一 strtol/strtoull/strtod 语义（带符号与基数，
// 0x 前缀支持），失败回 0。parseInt/Long → 有符号；parseUint/Ulong → 无符号；
// parseFloat/Double → 浮点。仅当实参是 string 时作为内建。
TypeInfo Sema::visitParse(CallExpr& expr, const std::string& name) {
    if (expr.args.size() != 1) {
        error(expr.range, name + " takes exactly one argument");
        return TypeInfo(TypeKind::Void);
    }
    auto ot = visitExpr(*expr.args[0]);
    if (ot.kind != TypeKind::String) {
        error(expr.range, name + " expects a string argument");
        return TypeInfo(TypeKind::Void);
    }
    TypeKind ret;
    if (name == "parseInt")       ret = TypeKind::Int;
    else if (name == "parseLong") ret = TypeKind::Long;
    else if (name == "parseUint") ret = TypeKind::UInt;
    else if (name == "parseUlong")ret = TypeKind::ULong;
    else if (name == "parseFloat")ret = TypeKind::Float;
    else                          ret = TypeKind::Double;
    expr.resolved_kind = ret;
    return TypeInfo(ret);
}

TypeInfo Sema::resolveGenericCall(CallExpr& expr, const std::string& name, int tu_index) {
    if (!current_tu_ || tu_index < 0) return TypeInfo(TypeKind::Void);
    FuncDecl& templ = current_tu_->functions[tu_index];

    // 1) Visit args to get their types.
    std::vector<TypeInfo> arg_types;
    for (auto& a : expr.args) arg_types.push_back(visitExpr(*a));

    // 2) Resolve concrete type args: explicit <T1,...> else infer from args.
    std::vector<TypeNode> concrete;
    bool explicit_given = expr.call_type_args.size() > 0;
    if (explicit_given && expr.call_type_args.size() != templ.type_params.size()) {
        error(expr.range, "generic function '" + name + "' expects " +
            std::to_string(templ.type_params.size()) + " type argument(s), got " +
            std::to_string(expr.call_type_args.size()));
        return TypeInfo(TypeKind::Void);
    }
    for (size_t ti = 0; ti < templ.type_params.size(); ti++) {
        if (explicit_given) {
            concrete.push_back(expr.call_type_args[ti]);
            continue;
        }
        // Infer: find the first param whose type is exactly this type param.
        bool found = false;
        for (size_t pi = 0; pi < templ.params.size() && pi < arg_types.size(); pi++) {
            const TypeNode& ptn = templ.params[pi].type;
            if (ptn.isClass() && ptn.class_name == templ.type_params[ti] &&
                ptn.type_args.empty() && !ptn.isArray()) {
                concrete.push_back(TypeNodeFromTypeInfo(arg_types[pi]));
                found = true;
                break;
            }
            // tp[] — infer from array element type
            if (ptn.isArray() && ptn.element_type &&
                ptn.element_type->isClass() &&
                ptn.element_type->class_name == templ.type_params[ti]) {
                if (arg_types[pi].kind == TypeKind::Array && arg_types[pi].element_type) {
                    concrete.push_back(TypeNodeFromTypeInfo(*arg_types[pi].element_type));
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            error(expr.range, "cannot infer type parameter '" + templ.type_params[ti] +
                "' for generic function '" + name + "' (pass explicit args: " +
                name + "<...>(...))");
            return TypeInfo(TypeKind::Void);
        }
    }

    // 3) Mangled instance name; reuse if already instantiated.
    std::string mangled = name;
    for (auto& c : concrete)
        mangled += "_" + typeName(typeNodeToTypeInfo(c));
    mangled += "_inst";

    // 4) Find or clone the instance.
    const FuncDecl* inst_ptr = nullptr;
    for (auto& f : current_tu_->functions)
        if (f.name == mangled) { inst_ptr = &f; break; }
    if (!inst_ptr) {
        FuncDecl inst;
        inst.name = mangled;
        inst.is_generic_inst = true;
        inst.type_params = templ.type_params;
        inst.inst_type_args = concrete;
        inst.range = templ.range;
        inst.has_test = templ.has_test;
        inst.has_region = templ.has_region;
        inst.has_coro = templ.has_coro;
        inst.op_symbol = templ.op_symbol;
        inst.return_type = substituteTypeNode(templ.return_type, templ.type_params, concrete);
        for (auto& p : templ.params) {
            ParamDecl np;
            np.name = p.name;
            np.type = substituteTypeNode(p.type, templ.type_params, concrete);
            np.is_ref = p.is_ref;
            np.default_expr = p.default_expr;
            np.range = p.range;
            inst.params.push_back(std::move(np));
        }
        inst.body = templ.body; // shared body (codegen resolves T per-inst)
        // §四-2 × 泛型：泛型模板体在 sema 被跳过 → ForInStmt 注解从未计算。单态化
        // 时用具体参数类型重注解共享 body（iterable 为参数标识符的情形）。注意：
        // 共享 body 意味着多实例同函数时注解以最后一次为准（多实例 for-in 暂不支持）。
        if (inst.body)
            annotateForInsInStmt(*inst.body, inst.params);
        current_tu_->functions.push_back(std::move(inst));
        inst_ptr = &current_tu_->functions.back();
    }

    // 5) Type-check args against the instance signature; set the call target.
    //    §四-1：实参数量不足/含命名实参时先规范化（默认补齐 + 命名重排）。
    TypeInfo rt = typeNodeToTypeInfo(inst_ptr->return_type);
    bool has_named = false;
    for (auto& a : expr.args) if (a->kind == ExprKind::NamedArg) { has_named = true; break; }
    if (expr.args.size() != inst_ptr->params.size() || has_named) {
        if (!normalizeArgsToParamDecls(expr.args, inst_ptr->params, expr.range)) {
            error(expr.range, "expected " + std::to_string(inst_ptr->params.size()) +
                " arguments, got " + std::to_string(expr.args.size()));
            return TypeInfo(TypeKind::Void);
        }
        arg_types.clear();
        for (auto& a : expr.args) arg_types.push_back(visitExpr(*a));
    }
    for (size_t i = 0; i < expr.args.size(); i++) {
        TypeInfo pt = typeNodeToTypeInfo(inst_ptr->params[i].type);
        if (!typesCompatible(pt, arg_types[i])) {
            error(expr.args[i]->range, "argument " + std::to_string(i + 1) +
                ": expected '" + typeName(pt) + "', got '" + typeName(arg_types[i]) + "'");
        }
    }
    expr.resolved_call_name = mangled;
    return rt;
}

TypeInfo Sema::resolveGenericStaticCall(CallExpr& expr, const std::string& cls_name,
                                        const std::string& method, int class_index,
                                        int action_index) {
    if (!current_tu_ || class_index < 0 || action_index < 0) return TypeInfo(TypeKind::Void);
    ActionDecl& templ = current_tu_->classes[class_index].static_actions[action_index];

    // 1) Visit args to get their types.
    std::vector<TypeInfo> arg_types;
    for (auto& a : expr.args) arg_types.push_back(visitExpr(*a));

    // 2) Resolve concrete type args: explicit <T1,...> else infer from args.
    std::vector<TypeNode> concrete;
    bool explicit_given = expr.call_type_args.size() > 0;
    if (explicit_given && expr.call_type_args.size() != templ.type_params.size()) {
        error(expr.range, "generic static method '" + cls_name + "." + method + "' expects " +
            std::to_string(templ.type_params.size()) + " type argument(s), got " +
            std::to_string(expr.call_type_args.size()));
        return TypeInfo(TypeKind::Void);
    }
    for (size_t ti = 0; ti < templ.type_params.size(); ti++) {
        if (explicit_given) { concrete.push_back(expr.call_type_args[ti]); continue; }
        bool found = false;
        for (size_t pi = 0; pi < templ.params.size() && pi < arg_types.size(); pi++) {
            const TypeNode& ptn = templ.params[pi].type;
            if (ptn.isClass() && ptn.class_name == templ.type_params[ti] &&
                ptn.type_args.empty() && !ptn.isArray()) {
                concrete.push_back(TypeNodeFromTypeInfo(arg_types[pi]));
                found = true;
                break;
            }
            if (ptn.isArray() && ptn.element_type &&
                ptn.element_type->isClass() &&
                ptn.element_type->class_name == templ.type_params[ti]) {
                if (arg_types[pi].kind == TypeKind::Array && arg_types[pi].element_type) {
                    concrete.push_back(TypeNodeFromTypeInfo(*arg_types[pi].element_type));
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            error(expr.range, "cannot infer type parameter '" + templ.type_params[ti] +
                "' for generic static method '" + cls_name + "." + method +
                "' (pass explicit args: " + cls_name + "." + method + "<...>(...))");
            return TypeInfo(TypeKind::Void);
        }
    }

    // 3) Mangled instance name (distinct prefix avoids collision with generic
    //    functions named ClassName_method).
    std::string mangled = "__gs_" + cls_name + "_" + method;
    for (auto& c : concrete)
        mangled += "_" + typeName(typeNodeToTypeInfo(c));
    mangled += "_inst";

    // 4) Find or clone the instance.
    const FuncDecl* inst_ptr = nullptr;
    for (auto& f : current_tu_->functions)
        if (f.name == mangled) { inst_ptr = &f; break; }
    if (!inst_ptr) {
        FuncDecl inst;
        inst.name = mangled;
        inst.is_generic_inst = true;
        inst.type_params = templ.type_params;
        inst.inst_type_args = concrete;
        inst.range = templ.range;
        inst.has_region = templ.has_region;
        inst.has_coro = templ.has_coro;
        inst.return_type = substituteTypeNode(templ.return_type, templ.type_params, concrete);
        for (auto& p : templ.params) {
            ParamDecl np;
            np.name = p.name;
            np.type = substituteTypeNode(p.type, templ.type_params, concrete);
            np.is_ref = false;
            np.default_expr = p.default_expr;
            np.range = p.range;
            inst.params.push_back(std::move(np));
        }
        inst.body = std::static_pointer_cast<BlockStmt>(templ.body); // shared body (codegen resolves T per-inst)
        current_tu_->functions.push_back(std::move(inst));
        inst_ptr = &current_tu_->functions.back();
    }

    // 5) Type-check args against the instance signature; set the call target.
    //    §四-1：实参数量不足/含命名实参时先规范化（默认补齐 + 命名重排）。
    TypeInfo rt = typeNodeToTypeInfo(inst_ptr->return_type);
    bool has_named = false;
    for (auto& a : expr.args) if (a->kind == ExprKind::NamedArg) { has_named = true; break; }
    if (expr.args.size() != inst_ptr->params.size() || has_named) {
        if (!normalizeArgsToParamDecls(expr.args, inst_ptr->params, expr.range)) {
            error(expr.range, "expected " + std::to_string(inst_ptr->params.size()) +
                " arguments, got " + std::to_string(expr.args.size()));
            return TypeInfo(TypeKind::Void);
        }
        arg_types.clear();
        for (auto& a : expr.args) arg_types.push_back(visitExpr(*a));
    }
    for (size_t i = 0; i < expr.args.size(); i++) {
        TypeInfo pt = typeNodeToTypeInfo(inst_ptr->params[i].type);
        if (!typesCompatible(pt, arg_types[i])) {
            error(expr.args[i]->range, "argument " + std::to_string(i + 1) +
                ": expected '" + typeName(pt) + "', got '" + typeName(arg_types[i]) + "'");
        }
    }
    expr.resolved_call_name = mangled;
    return rt;
}

TypeNode Sema::TypeNodeFromTypeInfo(const TypeInfo& t) {
    TypeNode n;
    switch (t.kind) {
        case TypeKind::Int:    n.basic_type = BuiltinType::Int; break;
        case TypeKind::Float:  n.basic_type = BuiltinType::Float; break;
        case TypeKind::Double: n.basic_type = BuiltinType::Double; break;
        case TypeKind::Bool:   n.basic_type = BuiltinType::Bool; break;
        case TypeKind::Byte:   n.basic_type = BuiltinType::Byte; break;
        case TypeKind::Short:  n.basic_type = BuiltinType::Short; break;
        case TypeKind::Long:   n.basic_type = BuiltinType::Long; break;
        case TypeKind::Char:   n.basic_type = BuiltinType::Char; break;
        case TypeKind::String: n.basic_type = BuiltinType::String; break;
        case TypeKind::UInt:   n.basic_type = BuiltinType::UInt; break;
        case TypeKind::UByte:  n.basic_type = BuiltinType::UByte; break;
        case TypeKind::UShort: n.basic_type = BuiltinType::UShort; break;
        case TypeKind::ULong:  n.basic_type = BuiltinType::ULong; break;
        case TypeKind::Class:  n.class_name = t.class_name; break;
        case TypeKind::Interface: n.class_name = t.class_name; break;
        case TypeKind::Struct: n.class_name = t.class_name; break;
        case TypeKind::Enum:   n.class_name = t.class_name; break;
        case TypeKind::Array:
            if (t.element_type) {
                n.element_type = std::make_shared<TypeNode>(TypeNodeFromTypeInfo(*t.element_type));
                n.array_size = t.array_size;
            }
            break;
        case TypeKind::Slice:
            if (t.element_type) {
                n.class_name = "slice";
                n.type_args.push_back(TypeNodeFromTypeInfo(*t.element_type));
            }
            break;
        case TypeKind::Function:
            n.is_tuple = false;
            for (auto& p : t.param_types)
                n.func_param_types.push_back(TypeNodeFromTypeInfo(p));
            if (t.return_type)
                n.func_return_type = std::make_shared<TypeNode>(TypeNodeFromTypeInfo(*t.return_type));
            break;
        case TypeKind::Tuple:
            n.is_tuple = true;
            for (auto& p : t.tuple_types)
                n.func_param_types.push_back(TypeNodeFromTypeInfo(p));
            break;
        default: n.basic_type = BuiltinType::Int; break;
    }
    return n;
}

static std::unique_ptr<Expr> cloneExpr(const Expr& e) {
    switch (e.kind) {
        case ExprKind::IntegerLiteral: {
            auto& v = static_cast<const IntegerLiteralExpr&>(e);
            return std::make_unique<IntegerLiteralExpr>(v.value, v.range, v.is_long);
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
            auto o = cloneExpr(*v.object);
            if (!o) return nullptr;
            return std::make_unique<MemberAccessExpr>(std::move(o), v.member_name, v.range);
        }
        case ExprKind::Subscript: {
            auto& v = static_cast<const SubscriptExpr&>(e);
            auto arr = cloneExpr(*v.array);
            auto idx = cloneExpr(*v.index);
            if (!arr || !idx) return nullptr;
            return std::make_unique<SubscriptExpr>(std::move(arr), std::move(idx), v.range);
        }
        case ExprKind::BinaryOp: {
            auto& v = static_cast<const BinaryOpExpr&>(e);
            auto l = cloneExpr(*v.lhs);
            auto r = cloneExpr(*v.rhs);
            if (!l || !r) return nullptr;
            return std::make_unique<BinaryOpExpr>(std::move(l), v.op, std::move(r), v.range);
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
            return std::make_unique<ConvertExpr>(v.to_kind, std::move(o), v.range);
        }
        case ExprKind::Ternary: {
            auto& v = static_cast<const TernaryExpr&>(e);
            auto c = cloneExpr(*v.condition);
            auto t = cloneExpr(*v.true_expr);
            auto f = cloneExpr(*v.false_expr);
            if (!c || !t || !f) return nullptr;
            return std::make_unique<TernaryExpr>(std::move(c), std::move(t), std::move(f), v.range);
        }
        case ExprKind::Pipe: {
            auto& v = static_cast<const PipeExpr&>(e);
            auto l = cloneExpr(*v.lhs);
            auto r = cloneExpr(*v.rhs);
            if (!l || !r) return nullptr;
            return std::make_unique<PipeExpr>(std::move(l), std::move(r), v.range);
        }
        case ExprKind::Assignment: {
            auto& v = static_cast<const AssignmentExpr&>(e);
            auto t = cloneExpr(*v.target);
            auto val = cloneExpr(*v.value);
            if (!t || !val) return nullptr;
            return std::make_unique<AssignmentExpr>(std::move(t), std::move(val), v.range);
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
            auto e2 = cloneExpr(*v.end);
            if (!s || !e2) return nullptr;
            return std::make_unique<RangeExpr>(std::move(s), std::move(e2), v.range);
        }
        default:
            return nullptr; // lambda/await/try 等复杂表达式不可作默认值
    }
}

void Sema::populateFuncTypeMeta(TypeInfo& ft, const std::vector<ParamDecl>& params) {
    for (auto& p : params) {
        ft.param_names.push_back(p.name);
        ft.param_has_default.push_back(p.default_expr != nullptr);
        ft.param_default_exprs.push_back(p.default_expr.get());
    }
}

static std::string namedArgName(const Expr& a, const std::vector<ParamDecl>& params) {
    if (a.kind != ExprKind::Assignment) return "";
    auto& as = static_cast<const AssignmentExpr&>(a);
    if (as.target->kind != ExprKind::Identifier) return "";
    const std::string& name = static_cast<const IdentifierExpr&>(*as.target).name;
    for (auto& p : params)
        if (p.name == name) return name;
    return "";
}

static std::optional<std::vector<int>> planArgMapping(
    const std::vector<std::unique_ptr<Expr>>& args,
    const std::vector<ParamDecl>& params) {
    size_t nparam = params.size();
    std::vector<int> plan(nparam, -1);
    size_t positional = 0;
    std::vector<std::pair<std::string, size_t>> named;
    for (size_t ai = 0; ai < args.size(); ai++) {
        std::string nn = namedArgName(*args[ai], params);
        if (args[ai]->kind == ExprKind::NamedArg)
            named.emplace_back(static_cast<NamedArgExpr&>(*args[ai]).name, ai);
        else if (!nn.empty())
            named.emplace_back(nn, ai);
        else
            positional++;
    }
    if (positional > nparam) return std::nullopt;
    size_t pi = 0;
    for (size_t ai = 0; ai < args.size(); ai++) {
        // 跳过命名实参（NamedArgExpr 或命名赋值实参），只给位置实参占位
        if (args[ai]->kind == ExprKind::NamedArg) continue;
        if (!namedArgName(*args[ai], params).empty()) continue;
        plan[pi++] = (int)ai;
    }
    for (auto& np : named) {
        bool found = false;
        for (size_t i = 0; i < nparam; i++) {
            if (params[i].name == np.first) {
                if (plan[i] >= 0) return std::nullopt;  // 位置+命名重叠
                plan[i] = (int)np.second;
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
    }
    for (size_t i = 0; i < named.size(); i++)
        for (size_t j = i + 1; j < named.size(); j++)
            if (named[i].first == named[j].first) return std::nullopt;
    for (size_t i = 0; i < nparam; i++)
        if (plan[i] < 0 && !params[i].default_expr) return std::nullopt;  // 必填缺失
    return plan;
}

static bool fitsArgsToParamDecls(const std::vector<std::unique_ptr<Expr>>& args,
                                 const std::vector<ParamDecl>& params) {
    size_t nparam = params.size();
    size_t positional = 0;
    std::vector<std::pair<std::string, const Expr*>> named;
    for (auto& a : args) {
        std::string nn = namedArgName(*a, params);
        if (a->kind == ExprKind::NamedArg) {
            auto& na = static_cast<NamedArgExpr&>(*a);
            named.emplace_back(na.name, na.value.get());
        } else if (!nn.empty()) {
            named.emplace_back(nn, static_cast<const AssignmentExpr&>(*a).value.get());
        } else {
            positional++;
        }
    }
    if (positional > nparam) return false;
    for (auto& np : named) {
        bool found = false;
        for (auto& p : params)
            if (p.name == np.first) { found = true; break; }
        if (!found) return false;
    }
    for (size_t i = 0; i < named.size(); i++)
        for (size_t j = i + 1; j < named.size(); j++)
            if (named[i].first == named[j].first) return false;
    // provided[i]: 形参 i 是否被位置或命名实参提供
    std::vector<bool> provided(nparam, false);
    for (size_t i = 0; i < positional && i < nparam; i++) provided[i] = true;
    for (auto& np : named) {
        for (size_t i = 0; i < nparam; i++) {
            if (params[i].name == np.first) {
                if (provided[i]) return false;  // 位置+命名重叠
                provided[i] = true;
            }
        }
    }
    for (size_t i = 0; i < nparam; i++)
        if (!provided[i] && !params[i].default_expr) return false;  // 必填缺失
    return true;
}

bool Sema::normalizeArgsToParamDecls(std::vector<std::unique_ptr<Expr>>& args,
                                     const std::vector<ParamDecl>& params,
                                     const SourceRange& call_range) {
    size_t nparam = params.size();
    if (nparam == 0) {
        if (!args.empty()) {
            error(call_range, "call takes no arguments, but " +
                  std::to_string(args.size()) + " given");
            return false;
        }
        return true;
    }
    // §四-1：把「赋值实参 `name = value` 且 name 是形参名」重解释为命名实参
    //（parser 按赋值表达式解析，为兼容宏的赋值实参形态）。
    for (auto& a : args) {
        std::string nn = namedArgName(*a, params);
        if (!nn.empty()) {
            auto& as = static_cast<AssignmentExpr&>(*a);
            a = std::make_unique<NamedArgExpr>(nn, std::move(as.value), a->range);
        }
    }
    if (!fitsArgsToParamDecls(args, params)) {
        // 给出更具体的错误
        size_t positional = 0;
        for (auto& a : args)
            if (a->kind != ExprKind::NamedArg) positional++;
        if (positional > nparam) {
            error(call_range, "too many arguments: expected at most " +
                  std::to_string(nparam) + ", got " + std::to_string(args.size()));
        } else {
            std::vector<std::pair<std::string, const Expr*>> named;
            for (auto& a : args)
                if (a->kind == ExprKind::NamedArg)
                    named.emplace_back(static_cast<NamedArgExpr&>(*a).name,
                                       static_cast<NamedArgExpr&>(*a).value.get());
            bool any_named = !named.empty();
            if (any_named) {
                // 未知命名实参
                for (auto& np : named) {
                    bool found = false;
                    for (auto& p : params)
                        if (p.name == np.first) { found = true; break; }
                    if (!found) {
                        error(call_range, "unknown named argument '" + np.first + "'");
                        return false;
                    }
                }
                // 位置 + 命名重叠（前 positional 个形参已被位置实参占据）
                for (size_t i = 0; i < positional && i < nparam; i++) {
                    for (auto& np : named) {
                        if (np.first == params[i].name) {
                            error(call_range, "parameter '" + params[i].name +
                                  "' given both by position and by name");
                            return false;
                        }
                    }
                }
            }
            // 必填缺失
            std::vector<bool> provided(nparam, false);
            for (size_t i = 0; i < positional && i < nparam; i++) provided[i] = true;
            for (auto& np : named)
                for (size_t i = 0; i < nparam; i++)
                    if (params[i].name == np.first) provided[i] = true;
            for (size_t i = 0; i < nparam; i++) {
                if (!provided[i] && !params[i].default_expr) {
                    error(call_range, "missing required argument '" + params[i].name + "'");
                    return false;
                }
            }
            if (!any_named)
                error(call_range, "expected " + std::to_string(nparam) +
                      " arguments, got " + std::to_string(args.size()));
        }
        return false;
    }

    // 解析位置/命名实参
    std::vector<std::unique_ptr<Expr>> positional;
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> named;
    for (auto& a : args) {
        if (a->kind == ExprKind::NamedArg) {
            auto& na = static_cast<NamedArgExpr&>(*a);
            named.emplace_back(na.name, std::move(na.value));
        } else {
            positional.push_back(std::move(a));
        }
    }
    std::vector<int> name_to_idx(nparam, -1);
    for (size_t i = 0; i < nparam; i++)
        for (size_t j = 0; j < named.size(); j++)
            if (named[j].first == params[i].name) name_to_idx[i] = (int)j;

    std::vector<bool> provided(nparam, false);
    std::vector<int> arg_of_param(nparam, -1);
    for (size_t i = 0; i < positional.size(); i++) { provided[i] = true; arg_of_param[i] = (int)i; }
    for (size_t i = 0; i < nparam; i++) {
        if (name_to_idx[i] >= 0) {
            provided[i] = true;
            arg_of_param[i] = (int)(positional.size() + name_to_idx[i]);
        }
    }

    std::vector<std::unique_ptr<Expr>> out;
    for (size_t i = 0; i < nparam; i++) {
        if (provided[i]) {
            int src = arg_of_param[i];
            if (src < (int)positional.size())
                out.push_back(std::move(positional[src]));
            else {
                int ni = src - (int)positional.size();
                out.push_back(std::move(named[ni].second));
            }
        } else {
            auto cl = params[i].default_expr ? cloneExpr(*params[i].default_expr) : nullptr;
            if (!cl) {
                error(call_range, "cannot use default value for parameter '" +
                      params[i].name + "' (non-constant expression)");
                return false;
            }
            out.push_back(std::move(cl));
        }
    }
    args = std::move(out);
    return true;
}

bool Sema::normalizeCallArgs(std::vector<std::unique_ptr<Expr>>& args,
                             const TypeInfo& ft, const SourceRange& call_range) {
    size_t nparam = ft.param_types.size();
    if (nparam == 0) {
        if (!args.empty()) {
            error(call_range, "call takes no arguments, but " +
                  std::to_string(args.size()) + " given");
            return false;
        }
        return true;
    }
    // 无默认/命名元数据（如函数值类型）：严格按数量匹配
    if (ft.param_names.empty() || ft.param_names.size() != nparam) {
        if (args.size() != nparam) {
            error(call_range, "expected " + std::to_string(nparam) +
                  " arguments, got " + std::to_string(args.size()));
            return false;
        }
        for (auto& a : args)
            if (a->kind == ExprKind::NamedArg) {
                error(a->range, "named argument '" +
                      static_cast<NamedArgExpr&>(*a).name + "' not applicable here");
                return false;
            }
        return true;
    }
    std::vector<ParamDecl> pds;
    pds.reserve(nparam);
    for (size_t i = 0; i < nparam; i++) {
        ParamDecl pd;
        pd.name = ft.param_names[i];
        // 借用指针包装（no-op deleter）；default_expr 生命周期 = 整个 TU
        pd.default_expr = ft.param_default_exprs[i]
            ? std::shared_ptr<Expr>(ft.param_default_exprs[i], [](Expr*) {})
            : nullptr;
        pds.push_back(std::move(pd));
    }
    return normalizeArgsToParamDecls(args, pds, call_range);
}

void Sema::checkParamDefaults(const std::vector<ParamDecl>& params) {
    for (auto& p : params) {
        if (!p.default_expr) continue;
        TypeInfo dt = visitExpr(*p.default_expr);
        TypeInfo pt = typeNodeToTypeInfo(p.type);
        if (!typesCompatible(pt, dt)) {
            error(p.default_expr->range, "default value for parameter '" + p.name +
                "' has type '" + typeName(dt) + "', expected '" + typeName(pt) + "'");
        }
    }
}

TypeInfo Sema::visitCall(CallExpr& expr) {
    // §五-5: an @async function/static method may only be called from an @coro
    // context (it suspends the fiber via park primitives — meaningless/unsafe
    // elsewhere). Runtime falls back to blocking, but sema rejects at compile time.
    if (!in_coro_method_ && isAsyncCallee(expr.callee.get()))
        error(expr.range, "'@async' function can only be awaited inside an '@coro' method");

    // Recursive self-call inside an @coro method: calling a @coro method SPAWNS a
    // new coroutine and returns a HANDLE (long), not the method's return value —
    // so `deep(n-1) + 1` silently computes on a handle (garbage, e.g. always 2).
    // Reject with a clear message; the recursion belongs in a plain function.
    // A self-call whose result is DISCARDED as a statement (`deep(n-1);`) is the
    // spawn-chain form and stays allowed (tests/coro_stack).
    bool top_discarded = in_discarded_stmt_expr_;
    in_discarded_stmt_expr_ = false;  // nested calls in args are value uses
    if (in_coro_method_ && !current_method_name_.empty() && current_tu_) {
        bool self_call = false;
        if (expr.callee->kind == ExprKind::Identifier) {
            auto& ci = static_cast<const IdentifierExpr&>(*expr.callee);
            if (ci.name == current_method_name_) self_call = true;
        } else if (expr.callee->kind == ExprKind::MemberAccess) {
            auto& ma = static_cast<const MemberAccessExpr&>(*expr.callee);
            if (ma.object->kind == ExprKind::ThisExpr &&
                ma.member_name == current_method_name_) self_call = true;
        }
        if (self_call && !top_discarded) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name != current_class_name_) continue;
                for (auto& a : cls.actions) {
                    if (a.name == current_method_name_ && a.has_coro) {
                        error(expr.range,
                            "recursive call to '@coro' method '" + current_method_name_ +
                            "' is not supported: an '@coro' call spawns a new coroutine and "
                            "returns a handle, not the method's return value. Move the "
                            "recursion into a plain function and call it from the '@coro' body");
                        return TypeInfo(TypeKind::Void);
                    }
                }
            }
        }
    }

    // P0 bitcast<T,U>(x)：位保持重解释（docs/type_system_design §5.2）。必须是
    // 带显式类型实参的调用（bitcast<uint>(x) / bitcast<float,uint>(x)），且不是
    // 用户声明的函数——在此提前拦截，避免落到 generic_functions_ 解析。
    if (expr.callee->kind == ExprKind::Identifier) {
        auto& bc_id = static_cast<const IdentifierExpr&>(*expr.callee);
        if (bc_id.name == "bitcast" && !expr.call_type_args.empty())
            return visitBitcast(expr);
        // P1 bytes(s) / str(bytes)（docs §6.3）：string ↔ ubyte[]。仅当实参类型
        // 匹配时才拦截（bytes 收 string、str 收 ubyte[]），用户同名函数不冲突。
        if (bc_id.name == "bytes" || bc_id.name == "str")
            return visitBytesStr(expr, bc_id.name);
        // P2 parse* 全族（docs §6.2）：parseInt/Long/Uint/Ulong/Float/Double(s)
        if (bc_id.name == "parseInt" || bc_id.name == "parseLong" ||
            bc_id.name == "parseUint" || bc_id.name == "parseUlong" ||
            bc_id.name == "parseFloat" || bc_id.name == "parseDouble")
            return visitParse(expr, bc_id.name);
    }

    // Generic static method call: StaticClass.genericMethod<...>(...) or inferred.
    if (expr.callee->kind == ExprKind::MemberAccess) {        auto& gma = static_cast<MemberAccessExpr&>(*expr.callee);
        if (gma.object->kind == ExprKind::Identifier) {
            auto& gso = static_cast<IdentifierExpr&>(*gma.object);
            std::string gkey = gso.name + "::" + gma.member_name;
            auto git = generic_static_methods_.find(gkey);
            if (git != generic_static_methods_.end())
                return resolveGenericStaticCall(expr, gso.name, gma.member_name,
                                                git->second.class_index,
                                                git->second.action_index);
        }
    }
    // Generic function call: foo<int>(...) or foo(x) with inference.
    if (expr.callee->kind == ExprKind::Identifier) {
        auto& gid = static_cast<const IdentifierExpr&>(*expr.callee);
        auto gfit = generic_functions_.find(gid.name);
        if (gfit != generic_functions_.end())
            return resolveGenericCall(expr, gid.name, gfit->second.tu_index);
    }
    // Struct 函数式构造：StructName(args) — 用构造器创建 struct 值
    if (expr.callee->kind == ExprKind::Identifier) {
        auto& id = static_cast<const IdentifierExpr&>(*expr.callee);
        if (resolveStructConstruction(expr, id.name)) {
            TypeInfo rt(TypeKind::Struct);
            rt.class_name = expr.resolved_struct_type;
            return rt;
        }
    }

    // Slice built-in method call: a.size() / a.length() → int, a.data() → T[].
    // visitMemberAccess resolves the bare field (a.length / a.size) to the
    // value itself; here the call form is intercepted so `a.size()` keeps
    // working even though the callee's MemberAccess now types as int.
    if (expr.callee->kind == ExprKind::MemberAccess) {
        auto& sma = static_cast<MemberAccessExpr&>(*expr.callee);
        if (sma.object->kind == ExprKind::Identifier) {
            TypeInfo oti = visitExpr(*sma.object);
            if (oti.kind == TypeKind::Slice) {
                if (sma.member_name == "size" || sma.member_name == "length") {
                    if (!expr.args.empty())
                        error(expr.range, "slice size()/length() takes no arguments");
                    return TypeInfo(TypeKind::Int);
                }
                if (sma.member_name == "data") {
                    if (!expr.args.empty())
                        error(expr.range, "slice data() takes no arguments");
                    TypeInfo at(TypeKind::Array);
                    at.element_type = oti.element_type;
                    return at;
                }
            }
        }
    }

    auto callee_type = visitExpr(*expr.callee);

    if (callee_type.kind != TypeKind::Function) {
        auto* ident = dynamic_cast<IdentifierExpr*>(expr.callee.get());
        std::string name = ident ? ident->name : "expression";
        error(expr.range, "'" + name + "' is not callable");
        return TypeInfo(TypeKind::Void);
    }
    // §四-1：规范实参（命名实参重排 + 默认值补齐），失败已报错
    if (!normalizeCallArgs(expr.args, callee_type, expr.range))
        return TypeInfo(TypeKind::Void);

    // M-FN-2 named lambda self-recursion: `name(args)` inside the lambda body
    // routes to the lambda's own tramp (codegen detects the `__self` marker).
    if (!lambda_self_name_.empty() && expr.callee->kind == ExprKind::Identifier) {
        auto& sid = static_cast<const IdentifierExpr&>(*expr.callee);
        if (sid.name == lambda_self_name_)
            expr.resolved_call_name = lambda_self_class_ + "__self";
    }

    for (size_t i = 0; i < expr.args.size(); ++i) {
        TypeInfo arg_type;
        if (expr.args[i]->kind == ExprKind::Lambda &&
            callee_type.param_types[i].kind == TypeKind::Function) {
            arg_type = visitLambda(static_cast<LambdaExpr&>(*expr.args[i]),
                                   &callee_type.param_types[i]);
        } else {
            arg_type = visitExpr(*expr.args[i]);
        }
        if (!typesCompatible(callee_type.param_types[i], arg_type)) {
            error(expr.args[i]->range, "argument " + std::to_string(i + 1) +
                  ": expected '" + typeName(callee_type.param_types[i]) +
                  "', got '" + typeName(arg_type) + "'");
        }
    }

    // A call to a top-level @coro function returns a coroutine handle (long),
    // not the function's declared return type (which is delivered via the
    // coroutine's result slot and read with Coro.result(handle)).
    auto* ident2 = dynamic_cast<IdentifierExpr*>(expr.callee.get());
    if (ident2 && current_tu_) {
        for (auto& f : current_tu_->functions) {
            if (f.has_coro && f.name == ident2->name)
                return TypeInfo(TypeKind::Long);
        }
    }

    if (callee_type.return_type) return *callee_type.return_type;
    return TypeInfo(TypeKind::Void);
}

bool Sema::resolveStructConstruction(CallExpr& expr, const std::string& name) {
    // 查找 struct（文件级或类内嵌套），取类型 key
    const StructDecl* st = nullptr;
    std::string type_key;
    if (current_tu_) {
        for (auto& s : current_tu_->structs) {
            std::string key = s.parent_class.empty()
                ? s.name : s.parent_class + "::" + s.name;
            if (s.name == name) { st = &s; type_key = key; break; }
        }
        if (!st) {
            for (auto& cls : current_tu_->classes) {
                for (auto& s : cls.structs) {
                    if (s.name == name) { st = &s; type_key = cls.name + "::" + s.name; break; }
                }
                if (st) break;
            }
        }
    }
    if (!st) return false;

    // 收集构造器候选（struct 方法中 has_constructor）
    struct Cand { std::string cname; const std::vector<ParamDecl>* params; };
    std::vector<Cand> cands;
    for (auto& f : st->functions) if (f.has_constructor) cands.push_back({f.name, &f.params});
    if (cands.empty()) return false;  // 无构造器 → 不是函数式构造，保持普通调用解析

    // 类型检查并收集实参类型（命名实参形态 `name = value` → 只 visit 值）
    std::vector<TypeInfo> arg_types;
    for (auto& a : expr.args) {
        if (a->kind == ExprKind::Assignment) {
            arg_types.push_back(visitExpr(*static_cast<AssignmentExpr&>(*a).value));
        } else {
            arg_types.push_back(visitExpr(*a));
        }
    }

    int best = -1; int best_score = 1 << 30; bool ambiguous = false;
    for (size_t ci = 0; ci < cands.size(); ci++) {
        auto& params = *cands[ci].params;
        // §四-1：默认值/命名实参 → 校验 + 映射后按形参类型打分
        auto plan = planArgMapping(expr.args, params);
        if (!plan) continue;
        int promos = 0; bool ok = true;
        for (size_t i = 0; i < (*plan).size(); i++) {
            if ((*plan)[i] < 0) continue;  // 缺省 → 完全匹配
            TypeInfo pt = typeNodeToTypeInfo(params[i].type);
            const TypeInfo& at = arg_types[(*plan)[i]];
            if (typesCompatible(pt, at)) {
                if (pt.kind != at.kind) promos++;
            } else { ok = false; break; }
        }
        if (!ok) continue;
        if (best < 0 || promos < best_score) { best = (int)ci; best_score = promos; ambiguous = false; }
        else if (promos == best_score) ambiguous = true;
    }

    if (best < 0) {
        std::string argstr;
        for (size_t i = 0; i < arg_types.size(); i++) {
            if (i) argstr += ", ";
            argstr += typeName(arg_types[i]);
        }
        error(expr.range, "no matching constructor for '" + name + "(" + argstr + ")'");
        return true;  // 已报错，视为构造调用处理
    }
    if (ambiguous) {
        error(expr.range, "ambiguous constructor call for '" + name + "'");
        return true;
    }
    auto& c = cands[best];
    // §四-1：实参规范化为完整有序列表（命名重排 + 默认补齐）
    normalizeArgsToParamDecls(expr.args, *c.params, expr.range);
    expr.resolved_struct_type = type_key;
    expr.resolved_struct_ctor = "struct_" +
        constructorMangledName(type_key, c.cname, *c.params);
    return true;
}

TypeInfo Sema::visitMemberAccess(MemberAccessExpr& expr) {
    auto obj_type = visitExpr(*expr.object);

    // Record the object's resolved class so codegen can emit field access on a
    // method-call result (`obj.method().field`) — that case has no variable
    // name for the var_class_map_ lookup and previously fell through codegen,
    // dropping the field (LLVM verify: param type mismatch).
    expr.resolved_object_class = obj_type.class_name;

    // Tuple field access: t.0, t.1 — numeric member name on a tuple value.
    if (obj_type.kind == TypeKind::Tuple && !expr.member_name.empty()) {
        bool all_digits = std::all_of(expr.member_name.begin(), expr.member_name.end(),
            [](unsigned char c) { return std::isdigit(c); });
        if (all_digits) {
            unsigned idx = (unsigned)std::stoul(expr.member_name);
            if (idx < obj_type.tuple_types.size())
                return obj_type.tuple_types[idx];
            error(expr.range, "tuple index " + expr.member_name + " out of range (" +
                std::to_string(obj_type.tuple_types.size()) + " elements)");
            return TypeInfo(TypeKind::Void);
        }
    }

    // Slice member access: .size()/.length() → int, .data() → T[]
    // Field form (a.length / a.size / a.data) resolves to the value itself;
    // the call form a.size() is handled in visitCall (slice built-in method).
    if (obj_type.kind == TypeKind::Slice) {
        if (expr.member_name == "size" || expr.member_name == "length") {
            TypeInfo ti(TypeKind::Int);
            return ti;
        }
        if (expr.member_name == "data") {
            TypeInfo at(TypeKind::Array);
            at.element_type = obj_type.element_type;
            return at;
        }
        error(expr.range, "slice has no member '" + expr.member_name +
            "' (expected size/length/data)");
        return TypeInfo(TypeKind::Void);
    }

    // Interface member access — look up method in interface declaration
    if (obj_type.kind == TypeKind::Interface) {
        auto interface_it = interface_member_types_.find(obj_type.class_name);
        if (interface_it != interface_member_types_.end()) {
            auto member_it = interface_it->second.find(expr.member_name);
            if (member_it != interface_it->second.end())
                return member_it->second;
            error(expr.range, "interface '" + obj_type.class_name +
                "' has no member '" + expr.member_name + "'");
            return TypeInfo(TypeKind::Void);
        }
        error(expr.range, "unknown interface '" + obj_type.class_name + "'");
        return TypeInfo(TypeKind::Void);
    }

    // Struct member access — fields are PUBLIC
    if (obj_type.kind == TypeKind::Struct) {
        auto struct_it = struct_member_types_.find(obj_type.class_name);
        if (struct_it == struct_member_types_.end()) {
            error(expr.range, "unknown struct '" + obj_type.class_name + "'");
            return TypeInfo(TypeKind::Void);
        }
        auto member_it = struct_it->second.find(expr.member_name);
        if (member_it != struct_it->second.end()) return member_it->second;
        error(expr.range, "struct '" + obj_type.class_name +
              "' has no member '" + expr.member_name + "'");
        return TypeInfo(TypeKind::Void);
    }

    // Enum variant access — Color.Red
    if (obj_type.kind == TypeKind::Enum) {
        // Find the enum and variant
        auto eit = enum_info_.find(obj_type.class_name);
        if (eit == enum_info_.end()) {
            error(expr.range, "unknown enum '" + obj_type.class_name + "'");
            return TypeInfo(TypeKind::Void);
        }
        for (size_t vi = 0; vi < eit->second.variants.size(); vi++) {
            if (eit->second.variants[vi].name == expr.member_name) {
                // Return function type if variant has data, else just the enum type
                if (eit->second.variants[vi].params.empty()) {
                    return obj_type;
                } else {
                    // Variant with data: return a function type (constructs the variant)
                    TypeInfo ft(TypeKind::Function);
                    ft.return_type = std::make_shared<TypeInfo>(obj_type);
                    for (auto& p : eit->second.variants[vi].params) {
                        ft.param_types.push_back(typeNodeToTypeInfo(p.type));
                    }
                    return ft;
                }
            }
        }
        error(expr.range, "enum '" + obj_type.class_name +
              "' has no variant '" + expr.member_name + "'");
        return TypeInfo(TypeKind::Void);
    }

    if (obj_type.kind != TypeKind::Class) {
        error(expr.range, "cannot access member of non-class type '" +
              typeName(obj_type) + "'");
        return TypeInfo(TypeKind::Void);
    }

    if (auto* class_decl = findClassDecl(obj_type.class_name)) {
        auto& cls = *class_decl;
        // Properties — accessible from anywhere
        for (auto& prop : cls.properties) {
            if (prop.name == expr.member_name) {
                return typeNodeToTypeInfo(prop.type);
            }
        }
        for (auto& action : cls.actions) {
            if (action.name == expr.member_name) {
                // @startup methods on @thread instances are auto-invoked
                // in the worker thread; a manual call re-runs the entry
                // logic and double-executes (SIGSEGV at runtime). Reject
                // the pattern at compile time. NOTE: @startup on plain
                // instances is still callable (mypc run relies on it).
                if (action.has_startup &&
                    expr.object->kind == ExprKind::Identifier) {
                    auto& oid = static_cast<IdentifierExpr&>(*expr.object);
                    if (thread_annotated_vars_.count(oid.name))
                        error(expr.range, "cannot manually call '@startup' method '" +
                              action.name + "' on a @thread instance "
                              "(auto-invoked in the worker thread)");
                }
                TypeInfo func_type(TypeKind::Function);
                if (action.has_coro) {
                    // @coro method call → returns coroutine handle (long)
                    func_type.return_type = std::make_shared<TypeInfo>(TypeKind::Long);
                } else {
                    func_type.return_type = std::make_shared<TypeInfo>(
                        typeNodeToTypeInfo(action.return_type));
                }
                for (auto& p : action.params)
                    func_type.param_types.push_back(typeNodeToTypeInfo(p.type));
                populateFuncTypeMeta(func_type, action.params);
                return func_type;
            }
        }
        // Static actions: accessible from anywhere (no 'this' needed)
        for (auto& action : cls.static_actions) {
            if (action.name == expr.member_name) {
                TypeInfo func_type(TypeKind::Function);
                func_type.return_type = std::make_shared<TypeInfo>(
                    typeNodeToTypeInfo(action.return_type));
                for (auto& p : action.params)
                    func_type.param_types.push_back(typeNodeToTypeInfo(p.type));
                populateFuncTypeMeta(func_type, action.params);
                return func_type;
            }
        }
        // Also check function: section (visible inside class, not from outside)
        for (auto& func : cls.functions) {
            if (func.name == expr.member_name) {
                bool is_this = expr.object->kind == ExprKind::ThisExpr;
                if (!is_this) {
                    error(expr.range, "cannot access function '" + func.name +
                          "' from outside class '" + cls.name + "'");
                    return TypeInfo(TypeKind::Void);
                }
                TypeInfo ft(TypeKind::Function);
                ft.return_type = std::make_shared<TypeInfo>(
                    typeNodeToTypeInfo(func.return_type));
                for (auto& p : func.params)
                    ft.param_types.push_back(typeNodeToTypeInfo(p.type));
                populateFuncTypeMeta(ft, func.params);
                return ft;
            }
        }
        for (auto& event : cls.events) {
            if (event.name == expr.member_name) {
                TypeInfo event_type(TypeKind::Function);
                event_type.return_type = std::make_shared<TypeInfo>(TypeKind::Void);
                for (auto& p : event.params) {
                    event_type.param_types.push_back(typeNodeToTypeInfo(p.type));
                    event_type.param_is_ref.push_back(false);
                }
                return event_type;
            }
        }
        error(expr.range, "class '" + obj_type.class_name +
              "' has no member '" + expr.member_name + "'");
        return TypeInfo(TypeKind::Void);
    }
    error(expr.range, "unknown class '" + obj_type.class_name + "'");
    return TypeInfo(TypeKind::Void);
}

TypeInfo Sema::visitSubscript(SubscriptExpr& expr) {
    auto arr_type = visitExpr(*expr.array);
    auto idx_type = visitExpr(*expr.index);
    // P1 D10（docs §6.3）：string 下标 s[i] : char —— 取第 i 个字符（字符码 u8）
    if (arr_type.kind == TypeKind::String) {
        if (!expectNumeric(idx_type, expr.index->range))
            return TypeInfo(TypeKind::Void);
        return TypeInfo(TypeKind::Char);
    }
    if (arr_type.kind != TypeKind::Array && arr_type.kind != TypeKind::Slice) {
        error(expr.range, "cannot index non-array type '" + typeName(arr_type) + "'");
        return TypeInfo(TypeKind::Void);
    }
    if (!expectNumeric(idx_type, expr.index->range))
        return TypeInfo(TypeKind::Void);
    if (arr_type.element_type) return *arr_type.element_type;
    return TypeInfo(TypeKind::Void);
}

TypeInfo Sema::visitNewExpr(NewExpr& expr) {
    // Built-in slice<T>(n): allocate n elements, return { data, len }
    if (expr.class_name == "slice") {
        if (expr.type_args.size() != 1) {
            error(expr.range, "slice requires exactly one type argument: slice<T>");
            return TypeInfo(TypeKind::Void);
        }
        if (expr.args.size() != 1) {
            error(expr.range, "slice requires exactly one size argument: new slice<T>(n)");
            return TypeInfo(TypeKind::Void);
        }
        auto sz = visitExpr(*expr.args[0]);
        if (sz.kind != TypeKind::Int && sz.kind != TypeKind::Long
            && sz.kind != TypeKind::Short && sz.kind != TypeKind::Byte) {
            error(expr.args[0]->range, "slice size must be an integer expression");
        }
        TypeInfo result(TypeKind::Slice);
        result.element_type = std::make_shared<TypeInfo>(
            typeNodeToTypeInfo(expr.type_args[0]));
        return result;
    }
    // Check if this is a generic instantiation
    if (!expr.type_args.empty()) {
        // Construct a temporary TypeNode to trigger monomorphization
        TypeNode tn;
        tn.class_name = expr.class_name;
        tn.type_args = expr.type_args;
        TypeInfo inst_type = typeNodeToTypeInfo(tn);
        // 在实例类上解析构造器（实例名与 codegen 的 mangling 一致：Box_int_inst）
        std::string inst_name = expr.class_name;
        for (auto& ta : expr.type_args) { inst_name += "_"; inst_name += mangleTypeNode(ta); }
        inst_name += "_inst";
        resolveNewConstructor(expr, inst_name);
        return inst_type;
    }

    bool found = false;
    if (current_tu_) {
        for (auto& cls : current_tu_->classes) {
            if (cls.name == expr.class_name) { found = true; break; }
        }
    }
    if (!found)
        error(expr.range, "unknown class '" + expr.class_name + "'");

    resolveNewConstructor(expr, expr.class_name);

    TypeInfo result(TypeKind::Class);
    result.class_name = expr.class_name;
    return result;
}

void Sema::resolveNewConstructor(NewExpr& expr, const std::string& cls_name) {
    const ClassDecl* cls = nullptr;
    if (current_tu_) {
        for (auto& c : current_tu_->classes)
            if (c.name == cls_name) { cls = &c; break; }
    }
    if (!cls) return;

    // 收集构造器候选（action: 与 function: 段）
    struct Cand { std::string name; const std::vector<ParamDecl>* params; };
    std::vector<Cand> cands;
    for (auto& a : cls->actions) if (a.has_constructor) cands.push_back({a.name, &a.params});
    for (auto& f : cls->functions) if (f.has_constructor) cands.push_back({f.name, &f.params});
    if (cands.empty()) return;  // 无构造器 → legacy @startup init / 默认

    // 类型检查并收集实参类型（此时才 visit——避免影响无构造器的 legacy 类）。
    // 命名实参形态 `name = value`（parser 解析为赋值表达式）→ 只 visit 值，
    // 不 visit 目标标识符（它可能是未定义的形参名）。
    std::vector<TypeInfo> arg_types;
    for (auto& a : expr.args) {
        if (a->kind == ExprKind::Assignment) {
            arg_types.push_back(visitExpr(*static_cast<AssignmentExpr&>(*a).value));
        } else {
            arg_types.push_back(visitExpr(*a));
        }
    }

    int best = -1;
    int best_score = 1 << 30;
    bool ambiguous = false;
    for (size_t ci = 0; ci < cands.size(); ci++) {
        auto& params = *cands[ci].params;
        // §四-1：支持默认值/命名实参——先校验实参适配形参（含缺省补齐），
        // 得到「形参 i ← 实参 j」映射后再按形参类型打分。
        auto plan = planArgMapping(expr.args, params);
        if (!plan) continue;
        int promos = 0; bool ok = true;
        for (size_t i = 0; i < (*plan).size(); i++) {
            if ((*plan)[i] < 0) continue;  // 缺省 → 跳过（视为完全匹配）
            TypeInfo pt = typeNodeToTypeInfo(params[i].type);
            const TypeInfo& at = arg_types[(*plan)[i]];
            if (typesCompatible(pt, at)) {
                if (pt.kind != at.kind) promos++;
            } else { ok = false; break; }
        }
        if (!ok) continue;
        if (best < 0 || promos < best_score) { best = (int)ci; best_score = promos; ambiguous = false; }
        else if (promos == best_score) ambiguous = true;
    }

    if (best < 0) {
        std::string argstr;
        for (size_t i = 0; i < arg_types.size(); i++) {
            if (i) argstr += ", ";
            argstr += typeName(arg_types[i]);
        }
        error(expr.range, "no matching constructor for '" + cls_name + "(" + argstr + ")'");
        return;
    }
    if (ambiguous) {
        error(expr.range, "ambiguous constructor call for '" + cls_name + "'");
        return;
    }
    auto& c = cands[best];
    // §四-1：把实参规范化为完整有序列表（命名重排 + 默认补齐），供 codegen 直接使用
    normalizeArgsToParamDecls(expr.args, *c.params, expr.range);
    expr.resolved_ctor = constructorMangledName(cls_name, c.name, *c.params);
}

TypeInfo Sema::visitNewArrayExpr(NewArrayExpr& expr) {
    // Type-check dimensions: must be integer expressions
    for (auto& dim : expr.dimensions) {
        auto dt = visitExpr(*dim);
        if (dt.kind != TypeKind::Int && dt.kind != TypeKind::Long
            && dt.kind != TypeKind::Short && dt.kind != TypeKind::Byte) {
            error(dim->range, "array size must be an integer expression");
        }
    }
    // Return array type
    TypeInfo result(TypeKind::Array);
    result.element_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(expr.element_type));
    return result;
}

TypeInfo Sema::visitThisExpr(ThisExpr& expr) {
    if (!in_class_method_ && !in_struct_method_) {
        error(expr.range, "'this' can only be used inside a class action");
        return TypeInfo(TypeKind::Void);
    }
    if (in_struct_method_) {
        TypeInfo result(TypeKind::Struct);
        result.class_name = current_struct_type_key_;
        return result;
    }
    TypeInfo result(TypeKind::Class);
    result.class_name = current_class_name_;
    return result;
}

TypeInfo Sema::visitRange(RangeExpr& expr) {
    auto start_type = visitExpr(*expr.start);
    auto end_type = visitExpr(*expr.end);
    if (!expectNumeric(start_type, expr.start->range) ||
        !expectNumeric(end_type, expr.end->range)) {
        return TypeInfo(TypeKind::Int);
    }
    return TypeInfo(TypeKind::Int); // Range evaluates to int
}

TypeInfo Sema::visitEnumVariant(EnumVariantExpr& expr) {
    // Enum variants are represented as i32 constants
    TypeInfo result(TypeKind::Int);
    return result;
}

TypeInfo Sema::visitTernary(TernaryExpr& expr) {
    auto cond_type = visitExpr(*expr.condition);
    expectBool(cond_type, expr.condition->range);
    auto true_type = visitExpr(*expr.true_expr);
    auto false_type = visitExpr(*expr.false_expr);
    // Numeric branches: unify to the wider type (e.g. `x > 0 ? 1 : x` — the
    // literal 1 defaults to byte, x is int → int). Mirrors binary-op promotion;
    // codegen casts both branches to the common type.
    if (isNumericKind(true_type.kind) && isNumericKind(false_type.kind)) {
        TypeKind common = commonNumericKind(true_type.kind, false_type.kind);
        if (common != TypeKind::Void) {
            if (common != true_type.kind)
                true_type = TypeInfo(common);
            return true_type;
        }
    }
    if (!typesCompatible(true_type, false_type)) {
        error(expr.range, "ternary branches have incompatible types: '" +
              typeName(true_type) + "' and '" + typeName(false_type) + "'");
        return true_type;
    }
    return true_type;
}

TypeInfo Sema::visitAssignment(AssignmentExpr& expr) {
    // Target must be an assignable lvalue: variable, field, or array element.
    // (Rejects e.g. `--5`, `5 = 1`, `f() = x` — previously crashed codegen.)
    switch (expr.target->kind) {
        case ExprKind::Identifier:
        case ExprKind::MemberAccess:
        case ExprKind::Subscript:
            break;  // assignable
        default:
            error(expr.range,
                "cannot assign to this expression (target must be a variable, "
                "field, or array element)");
            return TypeInfo(TypeKind::Void);
    }

    // Check if target is a const property
    auto checkConstProperty = [&](const std::string& prop_name) {
        if (!current_class_name_.empty() && current_tu_) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name != current_class_name_) continue;
                for (auto& p : cls.properties) {
                    if (p.name == prop_name && p.is_const) {
                        error(expr.range, "cannot assign to const property '" + prop_name + "'");
                        return true;
                    }
                }
            }
        }
        return false;
    };

    if (expr.target->kind == ExprKind::Identifier) {
        auto& id = static_cast<const IdentifierExpr&>(*expr.target);
        if (checkConstProperty(id.name)) return TypeInfo(TypeKind::Void);
    } else if (expr.target->kind == ExprKind::MemberAccess) {
        auto& ma = static_cast<const MemberAccessExpr&>(*expr.target);
        if (ma.object->kind == ExprKind::ThisExpr) {
            if (checkConstProperty(ma.member_name)) return TypeInfo(TypeKind::Void);
        }
    }
    auto target_type = visitExpr(*expr.target);
    auto value_type = visitExpr(*expr.value);
    // Skip cascading error when target is unknown (already reported)
    if (target_type.kind != TypeKind::Void && !typesCompatible(target_type, value_type)) {
        error(expr.range, "cannot assign value of type '" +
              typeName(value_type) + "' to variable of type '" +
              typeName(target_type) + "'");
    }
    return target_type;
}

const TypeAliasDecl* Sema::findAlias(const std::string& name) const {
    if (!current_tu_) return nullptr;
    for (auto& a : current_tu_->type_aliases)
        if (a.name == name) return &a;
    return nullptr;
}

TypeInfo Sema::typeNodeToTypeInfo(const TypeNode& node, int alias_depth) {
    // Type alias expansion: `type Name = Type;` — resolve Name to its aliased
    // type (recursively; depth-limited to catch cyclic aliases).
    if (node.isClass() && node.type_args.empty()) {
        if (auto* alias = findAlias(node.class_name)) {
            if (alias_depth > 32) {
                error(node.range, "recursive type alias '" + node.class_name + "'");
                return TypeInfo(TypeKind::Int);
            }
            return typeNodeToTypeInfo(alias->alias_type, alias_depth + 1);
        }
    }
    // Function type: (A, B) -> R
    if (node.isFunction()) {
        TypeInfo ft(TypeKind::Function);
        ft.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(*node.func_return_type));
        for (auto& p : node.func_param_types)
            ft.param_types.push_back(typeNodeToTypeInfo(p));
        return ft;
    }
    // Tuple type: (A, B)
    if (node.isTuple()) {
        TypeInfo tt(TypeKind::Tuple);
        for (auto& p : node.func_param_types)
            tt.tuple_types.push_back(typeNodeToTypeInfo(p));
        return tt;
    }
    if (node.isArray()) {
        TypeInfo arr_type(TypeKind::Array);
        arr_type.array_size = node.array_size;
        if (node.element_type)
            arr_type.element_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(*node.element_type));
        return arr_type;
    }
    if (node.isClass()) {
        // 关联类型：X::Item — X 为实现接口的类（或其类型参数），Item 为绑定类型（§三-5）
        {
            auto pos = node.class_name.find("::");
            if (pos != std::string::npos) {
                std::string owner = node.class_name.substr(0, pos);
                std::string member = node.class_name.substr(pos + 2);
                if (current_tu_) {
                    for (auto& cls : current_tu_->classes) {
                        if (cls.name != owner) continue;
                        auto bit = cls.associated_type_bindings.find(member);
                        if (bit != cls.associated_type_bindings.end())
                            return typeNodeToTypeInfo(bit->second);
                    }
                }
                // owner 是类型参数（T）→ 约束接口的关联类型 → 抽象 Assoc；
                // 无约束 → 占位符 Int（codegen 单态化时经 current_type_params_
                // 解析到具体类绑定）。
                for (auto& tp : current_func_type_params_)
                    if (tp == owner) return TypeInfo(TypeKind::Int);
                if (!current_class_name_.empty())
                    for (auto& cls : current_tu_->classes)
                        if (cls.name == current_class_name_)
                            for (auto& tp : cls.type_params) {
                                if (tp != owner) continue;
                                auto cit = cls.type_param_constraints.find(tp);
                                if (cit != cls.type_param_constraints.end())
                                    return TypeInfo(TypeKind::Assoc);
                                return TypeInfo(TypeKind::Int);
                            }
            }
        }
        // Built-in slice<T>: { T* data; int64 len } fat pointer
        if (node.class_name == "slice" && node.type_args.size() == 1) {
            TypeInfo st(TypeKind::Slice);
            st.element_type = std::make_shared<TypeInfo>(
                typeNodeToTypeInfo(node.type_args[0]));
            return st;
        }
        std::string lookup_name = node.class_name;

        // Check if this is an interface type
        if (current_tu_) {
            for (auto& ifd : current_tu_->interfaces) {
                if (ifd.name == lookup_name) {
                    TypeInfo if_type(TypeKind::Interface);
                    if_type.class_name = lookup_name;
                    return if_type;
                }
            }
        }

        // Check if this is a generic class instantiation
        if (!node.type_args.empty() && generic_classes_.count(node.class_name)) {
            // If any type argument is a generic type-param placeholder (e.g.
            // `new Option<R>()` inside a generic template, or `Box<T>` inside a
            // generic class method), defer monomorphization. The placeholder Int
            // is only for name-mangling; instantiating now would bake the
            // type-param NAME into members (ctor param = `R`), which later
            // resolves to void outside the template scope. The concrete
            // instance (e.g. Option_int_inst) is created at the call site.
            bool has_placeholder = false;
            for (auto& a : node.type_args) {
                std::string nm = a.class_name;
                for (auto& tp : current_func_type_params_)
                    if (tp == nm) { has_placeholder = true; break; }
                if (has_placeholder) break;
                if (!current_class_name_.empty()) {
                    for (auto& cls : current_tu_->classes)
                        if (cls.name == current_class_name_)
                            for (auto& tp : cls.type_params)
                                if (tp == nm) { has_placeholder = true; break; }
                }
                if (has_placeholder) break;
            }
            if (has_placeholder) {
                TypeInfo result(TypeKind::Class);
                result.class_name = node.class_name;
                return result;
            }

            // Monomorphization: create a concrete class with type params substituted
            std::string mangled = node.class_name;
            for (auto& a : node.type_args)
                mangled += "_" + typeName(typeNodeToTypeInfo(a));
            mangled += "_inst";
            // Check if already instantiated
            TypeInfo result(TypeKind::Class);
            result.class_name = mangled;
            if (symbol_table_.lookup(mangled)) return result;

            // Find and clone the generic class
            auto& gen = generic_classes_[node.class_name];
            if (gen.tu_index < 0 || !current_tu_) return result;

            ClassDecl& original = current_tu_->classes[gen.tu_index];

            // ---- M2: enforce generic type-param constraints (where T : I) ----
            // Before monomorphizing, verify each constrained type argument
            // implements the required interface.
            for (auto& [tp, iface] : original.type_param_constraints) {
                size_t ti = original.type_params.size();
                for (size_t k = 0; k < original.type_params.size(); k++)
                    if (original.type_params[k] == tp) { ti = k; break; }
                if (ti >= node.type_args.size()) continue;
                const TypeNode& arg = node.type_args[ti];
                bool ok = false;
                if (!arg.class_name.empty()) {
                    for (auto& c : current_tu_->classes) {
                        if (c.name == arg.class_name) {
                            // The concrete class must declare the interface via
                            // `interface class X;` (interface_class_name).
                            if (c.interface_class_name == iface) { ok = true; }
                            // Also allow if the arg IS the interface itself.
                            for (auto& ifd : current_tu_->interfaces)
                                if (ifd.name == arg.class_name && iface == arg.class_name)
                                    ok = true;
                            break;
                        }
                    }
                }
                if (!ok) {
                    error(node.range, "type argument '" + typeName(typeNodeToTypeInfo(arg)) +
                        "' does not satisfy constraint '" + tp + " : " + iface +
                        "' for generic class '" + node.class_name + "'");
                    return result;
                }
            }

            ClassDecl inst;
            inst.name = mangled;
            inst.is_generic_inst = true;
            inst.type_params = original.type_params;
            inst.inst_type_args = node.type_args; // keep concrete args for codegen
            inst.range = original.range;

            // 关联类型绑定：类型实参代入（如 type Item = T; → Item = int）
            for (auto& [name, bt] : original.associated_type_bindings)
                inst.associated_type_bindings[name] =
                    substituteTypeNode(bt, original.type_params, node.type_args);

            // Substitute properties
            for (auto& prop : original.properties) {
                PropertyDecl p;
                p.name = prop.name;
                p.type = substituteTypeNode(prop.type, original.type_params, node.type_args);
                p.range = prop.range;
                inst.properties.push_back(std::move(p));
            }

            // Substitute actions (share body)
            for (auto& action : original.actions) {
                ActionDecl a;
                a.name = action.name;
                a.return_type = substituteTypeNode(action.return_type, original.type_params, node.type_args);
                for (auto& param : action.params) {
                    ParamDecl p;
                    p.name = param.name;
                    p.type = substituteTypeNode(param.type, original.type_params, node.type_args);
                    p.range = param.range;
                    a.params.push_back(std::move(p));
                }
                a.body = action.body; // share body
                a.has_startup = action.has_startup;
                a.has_test = action.has_test;
                a.has_constructor = action.has_constructor;
                a.range = action.range;
                inst.actions.push_back(std::move(a));
            }

            // Copy static actions
            for (auto& action : original.static_actions) {
                ActionDecl a;
                a.name = action.name;
                a.return_type = substituteTypeNode(action.return_type, original.type_params, node.type_args);
                for (auto& param : action.params) {
                    ParamDecl p;
                    p.name = param.name;
                    p.type = substituteTypeNode(param.type, original.type_params, node.type_args);
                    p.range = param.range;
                    a.params.push_back(std::move(p));
                }
                a.type_params = action.type_params;
                a.body = action.body;
                a.range = action.range;
                inst.static_actions.push_back(std::move(a));
            }

            // Copy events
            for (auto& ev : original.events) {
                EventDecl e;
                e.name = ev.name;
                for (auto& param : ev.params) {
                    ParamDecl p;
                    p.name = param.name;
                    p.type = substituteTypeNode(param.type, original.type_params, node.type_args);
                    p.range = param.range;
                    e.params.push_back(std::move(p));
                }
                e.range = ev.range;
                inst.events.push_back(std::move(e));
            }

            // Copy function: section (share body — type-checking of insts is
            // skipped, but codegen needs the body; generic type params inside
            // the body (e.g. `new T[n]`) are resolved by codegen's type-param
            // map, not by textual substitution).
            for (auto& func : original.functions) {
                FuncDecl f;
                f.name = func.name;
                f.return_type = substituteTypeNode(func.return_type, original.type_params, node.type_args);
                for (auto& param : func.params) {
                    ParamDecl p;
                    p.name = param.name;
                    p.type = substituteTypeNode(param.type, original.type_params, node.type_args);
                    p.range = param.range;
                    f.params.push_back(std::move(p));
                }
                f.body = func.body; // share body (codegen resolves T per-inst)
                f.has_constructor = func.has_constructor;
                f.range = func.range;
                inst.functions.push_back(std::move(f));
            }

            // Register and add to TU — visitClassDecl will declare the type
            current_tu_->classes.push_back(std::move(inst));
            class_indices_[current_tu_->classes.back().name] =
                current_tu_->classes.size() - 1;
            visitClassDecl(*current_tu_, current_tu_->classes.size() - 1);
            // Now it's registered; look up the result
            auto* lookup_result = symbol_table_.lookup(mangled);
            if (lookup_result) result = *lookup_result;
            return result;
        }

        auto* existing = symbol_table_.lookup(lookup_name);
        if (existing) return *existing;
        if (!current_class_name_.empty()) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name != current_class_name_) continue;
                for (auto& tp : cls.type_params) {
                    if (tp != lookup_name) continue;
                    // 约束类型参数（where T : I，§三-5）→ 解析为接口类型，使
                    // T 上的方法调用 / T::Item 可在模板体中静态检查（运行时单态化
                    // 到具体类）。
                    auto cit = cls.type_param_constraints.find(tp);
                    if (cit != cls.type_param_constraints.end()) {
                        TypeInfo if_type(TypeKind::Interface);
                        if_type.class_name = cit->second;
                        return if_type;
                    }
                    TypeInfo tp_type(TypeKind::Int);
                    return tp_type;
                }
            }
        }
        // Generic function type param placeholder (T → Int, like classes)
        for (auto& tp : current_func_type_params_) {
            if (tp == lookup_name) {
                TypeInfo tp_type(TypeKind::Int);
                return tp_type;
            }
        }

        // 抽象关联类型（§三-5）：标识符匹配某接口的 `type Item;` 声明 → Assoc
        //（静态检查与任意类型兼容；运行时经实现类绑定为具体类型）。
        if (current_tu_) {
            for (auto& ifd : current_tu_->interfaces)
                for (auto& at : ifd.associated_types)
                    if (at == lookup_name) return TypeInfo(TypeKind::Assoc);
        }

        // Check if this is a generic class instantiation
        if (!node.type_args.empty() && generic_classes_.count(node.class_name)) {
            // For now, return a simple class type (actual monomorphization TBD)
            TypeInfo result(TypeKind::Class);
            result.class_name = node.class_name;
            return result;
        }
        // Check if this is an enum type
        if (existing && (existing->kind == TypeKind::Enum)) {
            return *existing;
        }

        // Check if this is a struct type by looking up the symbol table
        if (existing && existing->kind == TypeKind::Struct) {
            TypeInfo st_type(TypeKind::Struct);
            st_type.class_name = lookup_name;
            return st_type;
        }
        // Verify class exists
        bool class_found = false;
        if (current_tu_) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name == node.class_name) { class_found = true; break; }
            }
        }
        if (!class_found) {
            // Undefined type name (e.g. a typo `propertyvoid` / `UnknownType`).
            // Previously fell back to Void in sema while codegen resolved the
            // same name to Int → divergent function signatures → LLVM verify
            // "return type does not match". Report a clean error instead.
            // Exception: macro AST types (StmtList/Stmt/Expr) are valid inside
            // @macro bodies but not registered classes — keep the Void fallback.
            bool is_macro_ast =
                node.class_name == "StmtList" || node.class_name == "Stmt" ||
                node.class_name == "Expr";
            if (!is_macro_ast)
                error(node.range, "unknown type '" + node.class_name + "'");
            TypeInfo err_type(TypeKind::Void);
            return err_type;
        }
        TypeInfo cls_type(TypeKind::Class);
        cls_type.class_name = node.class_name;
        return cls_type;
    }
    switch (node.basic_type) {
        case BuiltinType::Byte:   return TypeInfo(TypeKind::Byte);
        case BuiltinType::Short:  return TypeInfo(TypeKind::Short);
        case BuiltinType::Int:    return TypeInfo(TypeKind::Int);
        case BuiltinType::Long:   return TypeInfo(TypeKind::Long);
        case BuiltinType::UByte:  return TypeInfo(TypeKind::UByte);
        case BuiltinType::UShort: return TypeInfo(TypeKind::UShort);
        case BuiltinType::UInt:   return TypeInfo(TypeKind::UInt);
        case BuiltinType::ULong:  return TypeInfo(TypeKind::ULong);
        case BuiltinType::Char:   return TypeInfo(TypeKind::Char);
        case BuiltinType::Float:  return TypeInfo(TypeKind::Float);
        case BuiltinType::Double: return TypeInfo(TypeKind::Double);
        case BuiltinType::Bool:   return TypeInfo(TypeKind::Bool);
        case BuiltinType::String: return TypeInfo(TypeKind::String);
        case BuiltinType::Void:   return TypeInfo(TypeKind::Void);
    }
    return TypeInfo(TypeKind::Void);
}

std::string Sema::typeName(const TypeInfo& type) const {
    switch (type.kind) {
        case TypeKind::Byte:   return "byte";
        case TypeKind::Short:  return "short";
        case TypeKind::Int:    return "int";
        case TypeKind::Long:   return "long";
        case TypeKind::UByte:  return "ubyte";
        case TypeKind::UShort: return "ushort";
        case TypeKind::UInt:   return "uint";
        case TypeKind::ULong:  return "ulong";
        case TypeKind::Char:   return "char";
        case TypeKind::Float:  return "float";
        case TypeKind::Double: return "double";
        case TypeKind::Bool:   return "bool";
        case TypeKind::String: return "string";
        case TypeKind::Void:   return "void";
        case TypeKind::Null:   return "null";
        case TypeKind::Class:  return type.class_name;
        case TypeKind::Struct: return type.class_name;
        case TypeKind::Interface: return type.class_name;
        case TypeKind::Array:
            if (type.element_type) return typeName(*type.element_type) + "[]";
            return "array";
        case TypeKind::Slice:
            if (type.element_type) return "slice<" + typeName(*type.element_type) + ">";
            return "slice";
        case TypeKind::Function: {
            std::string s = "(";
            for (size_t i = 0; i < type.param_types.size(); i++) {
                if (i) s += ", ";
                s += typeName(type.param_types[i]);
            }
            s += ") -> ";
            s += type.return_type ? typeName(*type.return_type) : "void";
            return s;
        }
        case TypeKind::Tuple: {
            std::string s = "(";
            for (size_t i = 0; i < type.tuple_types.size(); i++) {
                if (i) s += ", ";
                s += typeName(type.tuple_types[i]);
            }
            s += ")";
            return s;
        }
        case TypeKind::Assoc:
            return "assoc";
    }
    return "unknown";
}

bool Sema::typesCompatible(const TypeInfo& lhs, const TypeInfo& rhs) const {
    if (lhs.kind == rhs.kind) {
        if (lhs.kind == TypeKind::Class) return lhs.class_name == rhs.class_name;
        if (lhs.kind == TypeKind::Array) {
            if (lhs.element_type && rhs.element_type)
                return typesCompatible(*lhs.element_type, *rhs.element_type);
            return !lhs.element_type && !rhs.element_type;
        }
        if (lhs.kind == TypeKind::Slice) {
            if (lhs.element_type && rhs.element_type)
                return typesCompatible(*lhs.element_type, *rhs.element_type);
            return !lhs.element_type && !rhs.element_type;
        }
        if (lhs.kind == TypeKind::Function) {
            // Structural: same param count, each param compatible, return compatible
            if (lhs.param_types.size() != rhs.param_types.size()) return false;
            for (size_t i = 0; i < lhs.param_types.size(); i++)
                if (!typesCompatible(lhs.param_types[i], rhs.param_types[i])) return false;
            if (lhs.return_type && rhs.return_type)
                return typesCompatible(*lhs.return_type, *rhs.return_type);
            return !lhs.return_type && !rhs.return_type;
        }
        if (lhs.kind == TypeKind::Tuple) {
            // Structural: same element count, each element compatible
            if (lhs.tuple_types.size() != rhs.tuple_types.size()) return false;
            for (size_t i = 0; i < lhs.tuple_types.size(); i++)
                if (!typesCompatible(lhs.tuple_types[i], rhs.tuple_types[i])) return false;
            return true;
        }
        return true;
    }
    // 抽象关联类型（§三-5）：Assoc 与任意类型兼容（双向通配）
    if (lhs.kind == TypeKind::Assoc || rhs.kind == TypeKind::Assoc) return true;
    // Implicit numeric promotion: Int ↔ Long are compatible
    if ((lhs.kind == TypeKind::Int && rhs.kind == TypeKind::Long) ||
        (lhs.kind == TypeKind::Long && rhs.kind == TypeKind::Int))
        return true;
    // Interface compatibility: rhs (concrete class) implements lhs (interface)
    if (lhs.kind == TypeKind::Interface && rhs.kind == TypeKind::Class) {
        if (!current_tu_) return false;
        for (auto& cls : current_tu_->classes) {
            if (cls.name != rhs.class_name) continue;
            if (cls.interface_class_name == lhs.class_name) return true;
        }
        return false;
    }
    if (rhs.kind == TypeKind::Null && lhs.kind == TypeKind::Class) return true;

    // P1（docs/type_system_design §3.2 隐式转换格）：无损/同符号拓宽才隐式。
    // 移除：char↔byte 互换（char=u8，§4.1）、Int→UInt/Long→UInt、Int→Float/Long→Float、
    //       i64/u64→f64（2026-08-12 决定：i8/i16/i32 与 u8/u16/u32→f64 精确保留隐式）。
    // 保留：同符号整型拓宽（SExt/ZExt）、i8/i16/i32→f64、u8/u16/u32→f64、f32→f64、
    //       无损跨符号小整型→大整型（UByte→Int/Long 等值域内可全表示）。
    auto promotes = [](TypeKind from, TypeKind to) -> bool {
        if (from == to) return true;
        switch (from) {
            case TypeKind::Byte:   // i8：同符号拓宽 + f64（f32/uint 显式）
                return to == TypeKind::Short || to == TypeKind::Int ||
                       to == TypeKind::Long || to == TypeKind::Double;
            case TypeKind::Char:   // char = u8 语义（§4.1）：u8 族 + i32/i64/f64（ZExt）
                return to == TypeKind::UByte || to == TypeKind::UShort ||
                       to == TypeKind::UInt || to == TypeKind::ULong ||
                       to == TypeKind::Int || to == TypeKind::Long ||
                       to == TypeKind::Double;
            case TypeKind::Short:  // i16
                return to == TypeKind::Int || to == TypeKind::Long || to == TypeKind::Double;
            case TypeKind::Int:    // i32（Int→Float/UInt 显式）
                return to == TypeKind::Long || to == TypeKind::Double;
            case TypeKind::Long:   // i64：f64 改显式（决定）
                return false;
            case TypeKind::Float:  // f32
                return to == TypeKind::Double;
            case TypeKind::UByte:  // u8
                return to == TypeKind::UShort || to == TypeKind::UInt ||
                       to == TypeKind::ULong || to == TypeKind::Int ||
                       to == TypeKind::Long || to == TypeKind::Double;
            case TypeKind::UShort: // u16
                return to == TypeKind::UInt || to == TypeKind::ULong ||
                       to == TypeKind::Int || to == TypeKind::Long || to == TypeKind::Double;
            case TypeKind::UInt:   // u32
                return to == TypeKind::ULong || to == TypeKind::Long || to == TypeKind::Double;
            case TypeKind::ULong:  // u64：无隐式（f64 改显式）
                return false;
            default: return false;
        }
    };
    return promotes(rhs.kind, lhs.kind);
}

void Sema::collectExprLocals(Expr& e, std::set<std::string>& locals) {
    switch (e.kind) {
        case ExprKind::BinaryOp: {
            auto& b = static_cast<BinaryOpExpr&>(e);
            collectExprLocals(*b.lhs, locals); collectExprLocals(*b.rhs, locals); break;
        }
        case ExprKind::UnaryOp:
            collectExprLocals(*static_cast<UnaryOpExpr&>(e).operand, locals); break;
        case ExprKind::Convert:
            collectExprLocals(*static_cast<ConvertExpr&>(e).operand, locals); break;
        case ExprKind::Call: {
            auto& c = static_cast<CallExpr&>(e);
            collectExprLocals(*c.callee, locals);
            for (auto& a : c.args) collectExprLocals(*a, locals);
            break;
        }
        case ExprKind::MemberAccess:
            collectExprLocals(*static_cast<MemberAccessExpr&>(e).object, locals); break;
        case ExprKind::Subscript: {
            auto& s = static_cast<SubscriptExpr&>(e);
            collectExprLocals(*s.array, locals); collectExprLocals(*s.index, locals); break;
        }
        case ExprKind::NewExpr:
            for (auto& a : static_cast<NewExpr&>(e).args) collectExprLocals(*a, locals);
            break;
        case ExprKind::NewArrayExpr:
            for (auto& d : static_cast<NewArrayExpr&>(e).dimensions) collectExprLocals(*d, locals);
            break;
        case ExprKind::Assignment: {
            auto& as = static_cast<AssignmentExpr&>(e);
            collectExprLocals(*as.target, locals); collectExprLocals(*as.value, locals); break;
        }
        case ExprKind::Ternary: {
            auto& t = static_cast<TernaryExpr&>(e);
            collectExprLocals(*t.condition, locals); collectExprLocals(*t.true_expr, locals);
            collectExprLocals(*t.false_expr, locals); break;
        }
        case ExprKind::Range: {
            auto& r = static_cast<RangeExpr&>(e);
            collectExprLocals(*r.start, locals); collectExprLocals(*r.end, locals); break;
        }
        case ExprKind::Lambda: {
            // Nested lambda: its params/locals belong to the inner scope — skip.
            break;
        }
        case ExprKind::Pipe: {
            auto& p = static_cast<PipeExpr&>(e);
            if (p.lhs) collectExprLocals(*p.lhs, locals);
            if (p.rhs) collectExprLocals(*p.rhs, locals);
            break;
        }
        case ExprKind::Try: {
            auto& t = static_cast<TryExpr&>(e);
            if (t.try_expr) collectExprLocals(*t.try_expr, locals);
            if (t.catch_expr) collectExprLocals(*t.catch_expr, locals);
            break;
        }
        case ExprKind::TupleExpr:
            for (auto& el : static_cast<TupleExpr&>(e).elements) collectExprLocals(*el, locals);
            break;
        case ExprKind::Await:
            if (static_cast<AwaitExpr&>(e).operand) collectExprLocals(*static_cast<AwaitExpr&>(e).operand, locals);
            break;
        default: break;
    }
}

void Sema::collectLambdaLocals(Stmt& stmt, std::set<std::string>& locals) {
    switch (stmt.kind) {
        case StmtKind::Block:
            for (auto& s : static_cast<BlockStmt&>(stmt).statements)
                if (s) collectLambdaLocals(*s, locals);
            break;
        case StmtKind::VarDeclStmt:
            for (auto& d : static_cast<VarDeclStmt&>(stmt).decls) {
                locals.insert(d.name);
                if (d.init_expr) collectExprLocals(*d.init_expr, locals);
            }
            break;
        case StmtKind::ExprStmt:
            if (static_cast<ExprStmt&>(stmt).expression)
                collectExprLocals(*static_cast<ExprStmt&>(stmt).expression, locals);
            break;
        case StmtKind::IfStmt: {
            auto& i = static_cast<IfStmt&>(stmt);
            if (i.condition) collectExprLocals(*i.condition, locals);
            if (i.then_block) collectLambdaLocals(*i.then_block, locals);
            if (i.else_block) collectLambdaLocals(*i.else_block, locals);
            break;
        }
        case StmtKind::WhileStmt: {
            auto& w = static_cast<WhileStmt&>(stmt);
            if (w.condition) collectExprLocals(*w.condition, locals);
            if (w.body) collectLambdaLocals(*w.body, locals);
            break;
        }
        case StmtKind::ForStmt: {
            auto& f = static_cast<ForStmt&>(stmt);
            if (f.init) collectLambdaLocals(*f.init, locals);
            if (f.condition) collectExprLocals(*f.condition, locals);
            if (f.step) collectExprLocals(*f.step, locals);
            if (f.body) collectLambdaLocals(*f.body, locals);
            break;
        }
        case StmtKind::ForInStmt: {
            auto& f = static_cast<ForInStmt&>(stmt);
            locals.insert(f.var_name);  // loop var — lambda-local
            if (f.body) collectLambdaLocals(*f.body, locals);
            break;
        }
        case StmtKind::ReturnStmt:
            if (static_cast<ReturnStmt&>(stmt).value)
                collectExprLocals(*static_cast<ReturnStmt&>(stmt).value, locals);
            break;
        case StmtKind::DestructureStmt: {
            auto& ds = static_cast<DestructureStmt&>(stmt);
            // Declaration leaves are locals (shadowed by the tuple element types).
            std::function<void(const DestructureTarget&)> bind =
                [&](const DestructureTarget& t) {
                    if (!t.name.empty()) { locals.insert(t.name); return; }
                    for (auto& c : t.elements) bind(c);
                };
            for (auto& c : ds.target.elements) bind(c);
            if (ds.value) collectExprLocals(*ds.value, locals);
            break;
        }
        case StmtKind::MatchStmt:
            for (auto& a : static_cast<MatchStmt&>(stmt).arms) {
                for (auto& b : a.bindings) locals.insert(b);
                if (a.body) collectLambdaLocals(*a.body, locals);
            }
            break;
        default: break;
    }
}

bool Sema::isGlobalName(const std::string& name) const {
    if (generic_functions_.count(name)) return true;
    if (!current_tu_) return false;
    for (auto& f : current_tu_->functions)
        if (f.name == name) return true;
    for (auto& c : current_tu_->classes)
        if (c.name == name) return true;
    for (auto& i : current_tu_->interfaces)
        if (i.name == name) return true;
    for (auto& s : current_tu_->structs)
        if (s.name == name) return true;
    for (auto& e : current_tu_->enums)
        if (e.name == name) return true;
    return false;
}

bool Sema::isAsyncCallee(const Expr* callee) const {
    if (!callee || !current_tu_) return false;
    if (callee->kind == ExprKind::Identifier) {
        auto& id = static_cast<const IdentifierExpr&>(*callee);
        for (auto& f : current_tu_->functions)
            if (f.name == id.name && f.has_async) return true;
        return false;
    }
    if (callee->kind == ExprKind::MemberAccess) {
        auto& ma = static_cast<const MemberAccessExpr&>(*callee);
        if (ma.object && ma.object->kind == ExprKind::Identifier) {
            auto& oid = static_cast<const IdentifierExpr&>(*ma.object);
            for (auto& cls : current_tu_->classes) {
                if (cls.name != oid.name) continue;   // Class.method (static)
                for (auto& a : cls.actions)
                    if (a.name == ma.member_name && a.has_async) return true;
                for (auto& a : cls.static_actions)
                    if (a.name == ma.member_name && a.has_async) return true;
            }
            // 实例接收者：obj.method — 解析变量的类类型后查该类 @async 方法
            const TypeInfo* t = symbol_table_.lookup(oid.name);
            if (t && t->kind == TypeKind::Class) {
                for (auto& cls : current_tu_->classes) {
                    if (cls.name != t->class_name) continue;
                    for (auto& a : cls.actions)
                        if (a.name == ma.member_name && a.has_async) return true;
                }
            }
        }
    }
    return false;
}

void Sema::collectExprCaptures(Expr& e, const std::set<std::string>& locals,
                               const std::vector<std::string>& params,
                               std::vector<std::string>& out) {
    switch (e.kind) {
        case ExprKind::Identifier: {
            auto& id = static_cast<IdentifierExpr&>(e);
            if (id.name == "this" || id.name == "true" || id.name == "false") break;
            if (std::find(params.begin(), params.end(), id.name) != params.end()) break;
            if (locals.count(id.name)) break;
            // 全局函数/泛型函数/类/接口/struct/枚举名：不是外层局部变量，无需捕获
            //（否则 lambda 内调用顶层函数被误捕 → 合成捕获声明与函数名重复）。
            if (isGlobalName(id.name)) break;
            if (symbol_table_.lookup(id.name))
                if (std::find(out.begin(), out.end(), id.name) == out.end())
                    out.push_back(id.name);
            break;
        }
        case ExprKind::BinaryOp: {
            auto& b = static_cast<BinaryOpExpr&>(e);
            collectExprCaptures(*b.lhs, locals, params, out);
            collectExprCaptures(*b.rhs, locals, params, out); break;
        }
        case ExprKind::UnaryOp:
            collectExprCaptures(*static_cast<UnaryOpExpr&>(e).operand, locals, params, out); break;
        case ExprKind::Call: {
            auto& c = static_cast<CallExpr&>(e);
            collectExprCaptures(*c.callee, locals, params, out);
            for (auto& a : c.args) collectExprCaptures(*a, locals, params, out);
            break;
        }
        case ExprKind::MemberAccess:
            collectExprCaptures(*static_cast<MemberAccessExpr&>(e).object, locals, params, out); break;
        case ExprKind::Subscript: {
            auto& s = static_cast<SubscriptExpr&>(e);
            collectExprCaptures(*s.array, locals, params, out);
            collectExprCaptures(*s.index, locals, params, out); break;
        }
        case ExprKind::NewExpr:
            for (auto& a : static_cast<NewExpr&>(e).args) collectExprCaptures(*a, locals, params, out);
            break;
        case ExprKind::NewArrayExpr:
            for (auto& d : static_cast<NewArrayExpr&>(e).dimensions) collectExprCaptures(*d, locals, params, out);
            break;
        case ExprKind::Assignment: {
            auto& as = static_cast<AssignmentExpr&>(e);
            collectExprCaptures(*as.target, locals, params, out);
            collectExprCaptures(*as.value, locals, params, out); break;
        }
        case ExprKind::Ternary: {
            auto& t = static_cast<TernaryExpr&>(e);
            collectExprCaptures(*t.condition, locals, params, out);
            collectExprCaptures(*t.true_expr, locals, params, out);
            collectExprCaptures(*t.false_expr, locals, params, out); break;
        }
        case ExprKind::Range: {
            auto& r = static_cast<RangeExpr&>(e);
            collectExprCaptures(*r.start, locals, params, out);
            collectExprCaptures(*r.end, locals, params, out); break;
        }
        case ExprKind::Lambda: {
            auto& l = static_cast<LambdaExpr&>(e);
            if (l.body) collectLambdaCaptures(*l.body, locals, params, out);
            break;
        }
        case ExprKind::Pipe: {
            auto& p = static_cast<PipeExpr&>(e);
            if (p.lhs) collectExprCaptures(*p.lhs, locals, params, out);
            if (p.rhs) collectExprCaptures(*p.rhs, locals, params, out);
            break;
        }
        case ExprKind::Try: {
            auto& t = static_cast<TryExpr&>(e);
            if (t.try_expr) collectExprCaptures(*t.try_expr, locals, params, out);
            if (t.catch_expr) collectExprCaptures(*t.catch_expr, locals, params, out);
            break;
        }
        case ExprKind::TupleExpr:
            for (auto& el : static_cast<TupleExpr&>(e).elements) collectExprCaptures(*el, locals, params, out);
            break;
        case ExprKind::Await:
            if (static_cast<AwaitExpr&>(e).operand) collectExprCaptures(*static_cast<AwaitExpr&>(e).operand, locals, params, out);
            break;
        default: break;
    }
}

void Sema::collectLambdaCaptures(Stmt& stmt, const std::set<std::string>& locals,
                                 const std::vector<std::string>& params,
                                 std::vector<std::string>& out) {
    switch (stmt.kind) {
        case StmtKind::Block:
            for (auto& s : static_cast<BlockStmt&>(stmt).statements)
                if (s) collectLambdaCaptures(*s, locals, params, out);
            break;
        case StmtKind::VarDeclStmt:
            for (auto& d : static_cast<VarDeclStmt&>(stmt).decls)
                if (d.init_expr) collectExprCaptures(*d.init_expr, locals, params, out);
            break;
        case StmtKind::ExprStmt:
            if (static_cast<ExprStmt&>(stmt).expression)
                collectExprCaptures(*static_cast<ExprStmt&>(stmt).expression, locals, params, out);
            break;
        case StmtKind::IfStmt: {
            auto& i = static_cast<IfStmt&>(stmt);
            if (i.condition) collectExprCaptures(*i.condition, locals, params, out);
            if (i.then_block) collectLambdaCaptures(*i.then_block, locals, params, out);
            if (i.else_block) collectLambdaCaptures(*i.else_block, locals, params, out);
            break;
        }
        case StmtKind::WhileStmt: {
            auto& w = static_cast<WhileStmt&>(stmt);
            if (w.condition) collectExprCaptures(*w.condition, locals, params, out);
            if (w.body) collectLambdaCaptures(*w.body, locals, params, out);
            break;
        }
        case StmtKind::ForStmt: {
            auto& f = static_cast<ForStmt&>(stmt);
            if (f.init) collectLambdaCaptures(*f.init, locals, params, out);
            if (f.condition) collectExprCaptures(*f.condition, locals, params, out);
            if (f.step) collectExprCaptures(*f.step, locals, params, out);
            if (f.body) collectLambdaCaptures(*f.body, locals, params, out);
            break;
        }
        case StmtKind::ForInStmt: {
            auto& f = static_cast<ForInStmt&>(stmt);
            if (f.iterable) collectExprCaptures(*f.iterable, locals, params, out);
            if (f.body) collectLambdaCaptures(*f.body, locals, params, out);
            break;
        }
        case StmtKind::ReturnStmt:
            if (static_cast<ReturnStmt&>(stmt).value)
                collectExprCaptures(*static_cast<ReturnStmt&>(stmt).value, locals, params, out);
            break;
        case StmtKind::DestructureStmt: {
            auto& ds = static_cast<DestructureStmt&>(stmt);
            if (ds.value) collectExprCaptures(*ds.value, locals, params, out);
            break;
        }
        case StmtKind::MatchStmt:
            for (auto& a : static_cast<MatchStmt&>(stmt).arms)
                if (a.body) collectLambdaCaptures(*a.body, locals, params, out);
            break;
        default: break;
    }
}

// ---- M-FN-2 nonlocal：收集当前 lambda body 内的 `nonlocal name;`（不进入嵌套
// lambda —— 嵌套 lambda 的 nonlocal 属于它自己）。----
void Sema::collectExprNonlocal(Expr& e, std::set<std::string>& out) {
    if (e.kind == ExprKind::Lambda) return;  // 跳过嵌套 lambda body
    switch (e.kind) {
        case ExprKind::Assignment: {
            auto& a = static_cast<AssignmentExpr&>(e);
            collectExprNonlocal(*a.target, out);
            collectExprNonlocal(*a.value, out);
            break;
        }
        case ExprKind::BinaryOp: {
            auto& b = static_cast<BinaryOpExpr&>(e);
            collectExprNonlocal(*b.lhs, out);
            collectExprNonlocal(*b.rhs, out);
            break;
        }
        case ExprKind::UnaryOp:
            collectExprNonlocal(*static_cast<UnaryOpExpr&>(e).operand, out);
            break;
        case ExprKind::Call: {
            auto& c = static_cast<CallExpr&>(e);
            collectExprNonlocal(*c.callee, out);
            for (auto& a : c.args) collectExprNonlocal(*a, out);
            break;
        }
        case ExprKind::MemberAccess:
            collectExprNonlocal(*static_cast<MemberAccessExpr&>(e).object, out);
            break;
        case ExprKind::Subscript: {
            auto& s = static_cast<SubscriptExpr&>(e);
            collectExprNonlocal(*s.array, out);
            collectExprNonlocal(*s.index, out);
            break;
        }
        case ExprKind::NewExpr:
            for (auto& a : static_cast<NewExpr&>(e).args) collectExprNonlocal(*a, out);
            break;
        case ExprKind::NewArrayExpr:
            for (auto& d : static_cast<NewArrayExpr&>(e).dimensions) collectExprNonlocal(*d, out);
            break;
        case ExprKind::Ternary: {
            auto& t = static_cast<TernaryExpr&>(e);
            collectExprNonlocal(*t.condition, out);
            collectExprNonlocal(*t.true_expr, out);
            collectExprNonlocal(*t.false_expr, out);
            break;
        }
        case ExprKind::TupleExpr:
            for (auto& el : static_cast<TupleExpr&>(e).elements) collectExprNonlocal(*el, out);
            break;
        default: break;
    }
}

void Sema::collectLambdaNonlocal(Stmt& stmt, std::set<std::string>& out) {
    switch (stmt.kind) {
        case StmtKind::NonlocalStmt:
            for (auto& n : static_cast<NonlocalStmt&>(stmt).names) out.insert(n);
            break;
        case StmtKind::Block:
            for (auto& s : static_cast<BlockStmt&>(stmt).statements)
                if (s) collectLambdaNonlocal(*s, out);
            break;
        case StmtKind::VarDeclStmt:
            for (auto& d : static_cast<VarDeclStmt&>(stmt).decls)
                if (d.init_expr) collectExprNonlocal(*d.init_expr, out);
            break;
        case StmtKind::ExprStmt:
            if (static_cast<ExprStmt&>(stmt).expression)
                collectExprNonlocal(*static_cast<ExprStmt&>(stmt).expression, out);
            break;
        case StmtKind::IfStmt: {
            auto& i = static_cast<IfStmt&>(stmt);
            if (i.then_block) collectLambdaNonlocal(*i.then_block, out);
            if (i.else_block) collectLambdaNonlocal(*i.else_block, out);
            break;
        }
        case StmtKind::WhileStmt:
            if (static_cast<WhileStmt&>(stmt).body)
                collectLambdaNonlocal(*static_cast<WhileStmt&>(stmt).body, out);
            break;
        case StmtKind::ForStmt: {
            auto& f = static_cast<ForStmt&>(stmt);
            if (f.init) collectLambdaNonlocal(*f.init, out);
            if (f.body) collectLambdaNonlocal(*f.body, out);
            break;
        }
        case StmtKind::ForInStmt:
            if (static_cast<ForInStmt&>(stmt).body)
                collectLambdaNonlocal(*static_cast<ForInStmt&>(stmt).body, out);
            break;
        case StmtKind::ReturnStmt:
            if (static_cast<ReturnStmt&>(stmt).value)
                collectExprNonlocal(*static_cast<ReturnStmt&>(stmt).value, out);
            break;
        case StmtKind::DestructureStmt:
            if (static_cast<DestructureStmt&>(stmt).value)
                collectExprNonlocal(*static_cast<DestructureStmt&>(stmt).value, out);
            break;
        case StmtKind::MatchStmt:
            for (auto& a : static_cast<MatchStmt&>(stmt).arms)
                if (a.body) collectLambdaNonlocal(*a.body, out);
            break;
        default: break;
    }
}

TypeInfo Sema::visitLambda(LambdaExpr& expr, const TypeInfo* expected_fn) {
    // Create a hidden class: __lambda_N
    std::string cls_name = "__lambda_" + std::to_string(lambda_counter_++);
    expr.hidden_class_name = cls_name;

    // ---- M-FN-2: capture analysis (outer locals referenced in the body) ----
    std::vector<std::string> params;
    for (auto& p : expr.params) params.push_back(p.name);
    std::set<std::string> locals;
    for (auto& p : expr.params) locals.insert(p.name);
    // 命名 lambda：`name` 自引用走 this，不进捕获集（否则被当外层变量捕获）。
    if (!expr.name.empty()) locals.insert(expr.name);
    if (expr.body) collectLambdaLocals(*expr.body, locals);
    std::vector<std::string> captures;
    if (expr.body) collectLambdaCaptures(*expr.body, locals, params, captures);

    // ---- M-FN-2 nonlocal：预扫描 `nonlocal name;`（按引用捕获外层函数变量）----
    std::set<std::string> nonlocal_names;
    if (expr.body) collectLambdaNonlocal(*expr.body, nonlocal_names);
    if (!nonlocal_names.empty() && in_lambda_body_ > 0) {
        error(expr.range, "nonlocal inside a nested lambda is not supported yet");
        nonlocal_names.clear();
    }
    if (!nonlocal_names.empty() && in_struct_method_) {
        error(expr.range, "nonlocal inside a struct method's lambda is not supported yet");
        nonlocal_names.clear();
    }
    // 保证每个 nonlocal 名都在捕获集里（只写不读也需共享 cell）。
    for (auto& n : nonlocal_names)
        if (std::find(captures.begin(), captures.end(), n) == captures.end())
            captures.push_back(n);

    ClassDecl cls;
    cls.name = cls_name;
    cls.range = expr.range;

    // Capture slots as hidden-class properties: cap_0, cap_1, ...
    // nonlocal 捕获的槽类型 = cell 类引用（__cell_N），外层变量提升为堆 cell。
    std::vector<TypeNode> cap_types;
    for (size_t i = 0; i < captures.size(); i++) {
        const std::string& cname = captures[i];
        bool is_nl = nonlocal_names.count(cname) != 0;
        auto* sym = symbol_table_.lookup(cname);
        PropertyDecl prop;
        std::string slot = "cap_" + std::to_string(i);
        TypeNode cty;
        if (is_nl) {
            // 校验：解析到外层函数变量 + 非 lambda 参数 + 标量类型（v1）
            bool is_param = std::find(params.begin(), params.end(), cname) != params.end();
            if (!sym) {
                error(expr.range, "nonlocal '" + cname + "' does not resolve to an outer variable");
                is_nl = false;
            } else if (is_param) {
                error(expr.range, "cannot 'nonlocal' a lambda parameter '" + cname + "'");
                is_nl = false;
            } else if (isGlobalName(cname) || sym->kind == TypeKind::Class ||
                       sym->kind == TypeKind::Interface || sym->kind == TypeKind::Struct ||
                       sym->kind == TypeKind::Enum || sym->kind == TypeKind::Function ||
                       sym->kind == TypeKind::Slice || sym->kind == TypeKind::String) {
                error(expr.range, "nonlocal '" + cname + "' must be a scalar-typed outer local (v1), got '" +
                      typeName(*sym) + "'");
                is_nl = false;
            }
        }
        if (is_nl) {
            // 创建/复用 cell 类（每标量类型一个 __cell_N，单属性 v:T）
            std::string tkey = typeName(*sym);
            auto cit = cell_class_by_type_.find(tkey);
            std::string ccell;
            if (cit == cell_class_by_type_.end()) {
                ccell = "__cell_" + std::to_string(cell_counter_++);
                ClassDecl cell;
                cell.name = ccell;
                cell.range = expr.range;
                PropertyDecl cv;
                cv.name = "v";
                cv.type = TypeNodeFromTypeInfo(*sym);
                cv.range = expr.range;
                cell.properties.push_back(std::move(cv));
                if (current_tu_) {
                    current_tu_->classes.push_back(std::move(cell));
                    class_indices_[current_tu_->classes.back().name] =
                        current_tu_->classes.size() - 1;
                }
                cell_class_by_type_[tkey] = ccell;
            } else {
                ccell = cit->second;
            }
            cty.class_name = ccell;                 // 槽类型 = cell 类引用
            expr.nonlocal_cells.push_back(ccell);   // 平行于 capture_names
            current_func_nonlocal_vars_.insert(cname);  // 传播给外层函数/action
            current_func_nonlocal_cell_class_[cname] = ccell;
            cls.nonlocal_slots.push_back(ClassDecl::NonlocalSlot{slot, cname, ccell});
        } else {
            cty = sym ? TypeNodeFromTypeInfo(*sym) : TypeNode();
            expr.nonlocal_cells.push_back("");
        }
        prop.name = slot;
        prop.type = cty;
        prop.range = expr.range;
        cls.properties.push_back(std::move(prop));
        expr.capture_names.push_back(cname);
        expr.capture_slots.push_back(slot);
        cap_types.push_back(cty);
    }

    ActionDecl action;
    action.name = "__call";
    action.return_type = TypeNode(); // default Int (will be inferred from body)
    if (!expr.params.empty()) {
        // Use the last param's type as return type (default void)
        action.params = expr.params;
    }
    action.body = expr.body;
    action.range = expr.range;

    // Prepend capture copies to the __call body: `T name = this.cap_i;` so the
    // original body's references to `name` resolve to the local copy.
    // nonlocal 捕获跳过（共享 cell：codegen 在 __call 开头注入 cell 属性 GEP 别名，
    // 读写直接打到外层变量所在堆 cell）。
    if (!captures.empty()) {
        if (auto* block = dynamic_cast<BlockStmt*>(action.body.get())) {
            std::vector<std::unique_ptr<Stmt>> pre;
            for (size_t i = 0; i < captures.size(); i++) {
                if (i < expr.nonlocal_cells.size() && !expr.nonlocal_cells[i].empty()) continue;
                VarDecl d;
                d.name = captures[i];
                d.type = cap_types[i];
                d.range = expr.range;
                auto ma = std::make_unique<MemberAccessExpr>(
                    std::make_unique<ThisExpr>(expr.range),
                    "cap_" + std::to_string(i), expr.range);
                d.init_expr = std::move(ma);
                std::vector<VarDecl> decls;
                decls.push_back(std::move(d));
                auto st = std::make_unique<VarDeclStmt>(std::move(decls), expr.range);
                pre.push_back(std::move(st));
            }
            block->statements.insert(block->statements.begin(),
                std::make_move_iterator(pre.begin()), std::make_move_iterator(pre.end()));
        }
    }

    // Determine __call return type: contextual expected type (M-FN-2, e.g.
    // `(int) -> int f = (x) => ...` / passed to a `(T)->R` param) takes
    // priority — this avoids inferring from body expressions that reference
    // body-local variables (not yet in scope during inference). Otherwise fall
    // back to inferring from the first `return expr;` (bare return / none ⇒ void).
    TypeNode ret_ty;
    if (expected_fn && expected_fn->kind == TypeKind::Function && expected_fn->return_type) {
        ret_ty = TypeNodeFromTypeInfo(*expected_fn->return_type);
    } else {
        symbol_table_.enterScope();
        for (auto& p : expr.params)
            symbol_table_.declare(p.name, typeNodeToTypeInfo(p.type));
        bool found = false;
        inferLambdaReturn(*expr.body, ret_ty, found);
        symbol_table_.leaveScope();
    }
    action.return_type = ret_ty;
    cls.actions.push_back(std::move(action));

    // Register class and add to TU (named lambda: record self-name on the class
    // so the main-loop __call body visit can bind the self-reference).
    if (current_tu_) {
        cls.lambda_name = expr.name;
        current_tu_->classes.push_back(std::move(cls));
        class_indices_[current_tu_->classes.back().name] =
            current_tu_->classes.size() - 1;
        visitClassDecl(*current_tu_, current_tu_->classes.size() - 1);
    }

    // First-class function value: the lambda's type is its function type.
    TypeInfo ft(TypeKind::Function);
    ft.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(action.return_type));
    for (auto& p : expr.params) ft.param_types.push_back(typeNodeToTypeInfo(p.type));
    return ft;
}

void Sema::inferLambdaReturn(Stmt& stmt, TypeNode& out, bool& found) {
    if (found) return;
    switch (stmt.kind) {
        case StmtKind::Block: {
            auto& b = static_cast<BlockStmt&>(stmt);
            for (auto& s : b.statements) {
                if (s) { inferLambdaReturn(*s, out, found); if (found) return; }
            }
            break;
        }
        case StmtKind::IfStmt: {
            auto& i = static_cast<IfStmt&>(stmt);
            if (i.then_block) { inferLambdaReturn(*i.then_block, out, found); if (found) return; }
            if (i.else_block) { inferLambdaReturn(*i.else_block, out, found); if (found) return; }
            break;
        }
        case StmtKind::WhileStmt: {
            auto& w = static_cast<WhileStmt&>(stmt);
            if (w.body) { inferLambdaReturn(*w.body, out, found); if (found) return; }
            break;
        }
        case StmtKind::ForStmt: {
            auto& f = static_cast<ForStmt&>(stmt);
            if (f.body) { inferLambdaReturn(*f.body, out, found); if (found) return; }
            break;
        }
        case StmtKind::ForInStmt: {
            auto& f = static_cast<ForInStmt&>(stmt);
            if (f.body) { inferLambdaReturn(*f.body, out, found); if (found) return; }
            break;
        }
        case StmtKind::ReturnStmt: {
            auto& r = static_cast<ReturnStmt&>(stmt);
            if (r.value) {
                TypeInfo t = visitExpr(*r.value);
                out = TypeNodeFromTypeInfo(t);
            } else {
                TypeNode vt;
                vt.basic_type = BuiltinType::Void;
                out = vt;
            }
            found = true;
            break;
        }
        default: break;
    }
}

TypeInfo Sema::visitPipe(PipeExpr& expr) {
    auto lhs_type = visitExpr(*expr.lhs);

    // Resolve the operator on the RHS: class name or instance variable.
    auto findTransform = [&](const ClassDecl& cls) -> const ActionDecl* {
        for (auto& a : cls.actions)
            if (a.name == "transform" && a.params.size() == 1) return &a;
        for (auto& f : cls.functions)
            if (f.name == "transform" && f.params.size() == 1) return nullptr;  // functions not supported as pipe target v1
        return nullptr;
    };

    if (expr.rhs->kind == ExprKind::Identifier) {
        auto& id = static_cast<IdentifierExpr&>(*expr.rhs);
        // 1) Class name → instantiate
        if (current_tu_) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name != id.name) continue;
                if (auto* a = findTransform(cls)) {
                    auto ptype = typeNodeToTypeInfo(a->params[0].type);
                    if (!typesCompatible(ptype, lhs_type)) {
                        error(expr.range, "pipe: cannot apply '" + id.name +
                              ".transform' to operand of type '" + typeName(lhs_type) + "'");
                        return TypeInfo(TypeKind::Void);
                    }
                    expr.target_kind = "class";
                    expr.class_name = cls.name;
                    expr.method = "transform";
                    return typeNodeToTypeInfo(a->return_type);
                }
            }
        }
        // 2) Instance variable → reuse
        if (auto* sym = symbol_table_.lookup(id.name)) {
            if (sym->kind == TypeKind::Class && current_tu_) {
                for (auto& cls : current_tu_->classes) {
                    if (cls.name != sym->class_name) continue;
                    if (auto* a = findTransform(cls)) {
                        auto ptype = typeNodeToTypeInfo(a->params[0].type);
                        if (!typesCompatible(ptype, lhs_type)) {
                            error(expr.range, "pipe: cannot apply '" + id.name +
                                  ".transform' to operand of type '" + typeName(lhs_type) + "'");
                            return TypeInfo(TypeKind::Void);
                        }
                        expr.target_kind = "instance";
                        expr.class_name = cls.name;
                        expr.method = "transform";
                        return typeNodeToTypeInfo(a->return_type);
                    }
                }
            }
        }
    }

    error(expr.range, "pipe '|>' requires an operator component with a single-argument 'transform' method");
    return TypeInfo(TypeKind::Void);
}

}  // namespace {ns}
