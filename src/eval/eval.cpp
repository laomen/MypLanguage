// M1 — @eval compile-time evaluation (lightweight MYP interpreter).
//
// Implements the pure-function subset of MYP for compile-time evaluation:
//   * scalar literals (int/long/double/float/bool/string)
//   * arithmetic / comparison / logical / bitwise operators
//   * if / while / for / block / return / break / continue
//   * recursion and mutual calls among @eval functions
//   * references to earlier top-level consts
// Forbidden at eval time: `new`, member access, non-@eval calls, arrays (V1),
// threads, I/O — encountering one reports a compile-time diagnostic.

#include "mylang/Eval.h"
#include "mylang/DiagnosticEngine.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace mylang {

namespace {

struct EvalValue {
    enum Kind { Int, Double, Bool, Str, Null } kind = Null;
    int64_t i = 0;
    double d = 0.0;
    bool b = false;
    std::string s;

    static EvalValue ofInt(int64_t v) { EvalValue r; r.kind = Int; r.i = v; return r; }
    static EvalValue ofDouble(double v) { EvalValue r; r.kind = Double; r.d = v; return r; }
    static EvalValue ofBool(bool v) { EvalValue r; r.kind = Bool; r.b = v; return r; }
    static EvalValue ofStr(std::string v) { EvalValue r; r.kind = Str; r.s = std::move(v); return r; }
};

class EvalInterpreter {
public:
    EvalInterpreter(TranslationUnit& tu, DiagnosticEngine& diag)
        : tu_(tu), diag_(diag) {}

    // Top-level entry: evaluate all foldable const declarations.
    bool run() {
        for (auto& fn : tu_.functions) {
            if (!fn.is_const_decl) continue;
            if (!fn.body || fn.body->statements.empty()) continue;
            auto* ret = dynamic_cast<ReturnStmt*>(fn.body->statements[0].get());
            if (!ret || !ret->value) continue;
            if (const_values_.count(fn.name)) continue;

            locals_.clear();
            auto val = evalExpr(*ret->value);
            if (!val) {
                // Non-foldable initializer: keep existing runtime behavior.
                continue;
            }
            // Record the const value (usable by later consts / @eval fns).
            const_values_[fn.name] = *val;

            // Rewrite body to `return <literal>;` so codegen emits a constant.
            std::unique_ptr<Expr> lit = makeLiteral(*val, fn.return_type);
            if (!lit) {
                diag_.error(fn.range, "cannot materialize const '" + fn.name +
                    "' as a compile-time literal");
                return false;
            }
            fn.body->statements.clear();
            fn.body->statements.push_back(
                std::make_unique<ReturnStmt>(std::move(lit), fn.range));
        }
        return !diag_.hasErrors();
    }

private:
    TranslationUnit& tu_;
    DiagnosticEngine& diag_;
    std::unordered_map<std::string, EvalValue> locals_;
    std::unordered_map<std::string, EvalValue> const_values_;
    int depth_ = 0;
    bool break_ = false;
    bool continue_ = false;

    static constexpr int kMaxDepth = 100000;

    void error(const SourceRange& r, const std::string& msg) {
        diag_.error(r, msg);
    }

    // ---- Expression evaluation (std::nullopt => not foldable / error) ----

    std::optional<EvalValue> evalExpr(const Expr& e) {
        switch (e.kind) {
        case ExprKind::IntegerLiteral: {
            auto& v = static_cast<const IntegerLiteralExpr&>(e);
            return EvalValue::ofInt(v.value);
        }
        case ExprKind::FloatLiteral: {
            auto& v = static_cast<const FloatLiteralExpr&>(e);
            return EvalValue::ofDouble(v.value);
        }
        case ExprKind::BoolLiteral: {
            auto& v = static_cast<const BoolLiteralExpr&>(e);
            return EvalValue::ofBool(v.value);
        }
        case ExprKind::StringLiteral: {
            auto& v = static_cast<const StringLiteralExpr&>(e);
            return EvalValue::ofStr(v.value);
        }
        case ExprKind::NullLiteral:
            return EvalValue::ofInt(0);
        case ExprKind::Identifier: {
            auto& id = static_cast<const IdentifierExpr&>(e);
            auto it = locals_.find(id.name);
            if (it != locals_.end()) return it->second;
            auto cit = const_values_.find(id.name);
            if (cit != const_values_.end()) return cit->second;
            error(e.range, "compile-time evaluation: unknown identifier '" + id.name + "'");
            return std::nullopt;
        }
        case ExprKind::UnaryOp: {
            auto& u = static_cast<const UnaryOpExpr&>(e);
            auto v = evalExpr(*u.operand);
            if (!v) return std::nullopt;
            if (u.op == UnaryOpKind::Not) {
                return EvalValue::ofBool(!toBool(*v));
            }
            // Negate
            if (v->kind == EvalValue::Double) return EvalValue::ofDouble(-v->d);
            return EvalValue::ofInt(-v->i);
        }
        case ExprKind::BinaryOp: {
            auto& b = static_cast<const BinaryOpExpr&>(e);
            return evalBinary(b);
        }
        case ExprKind::Call: {
            auto& c = static_cast<const CallExpr&>(e);
            return evalCall(c);
        }
        default:
            error(e.range, "compile-time evaluation: construct not supported in @eval context");
            return std::nullopt;
        }
    }

    static bool toBool(const EvalValue& v) {
        switch (v.kind) {
        case EvalValue::Bool: return v.b;
        case EvalValue::Int:  return v.i != 0;
        case EvalValue::Double: return v.d != 0.0;
        default: return false;
        }
    }

    std::optional<EvalValue> evalBinary(const BinaryOpExpr& b) {
        auto l = evalExpr(*b.lhs);
        auto r = evalExpr(*b.rhs);
        if (!l || !r) return std::nullopt;
        const EvalValue& a = *l;
        const EvalValue& c = *r;

        // Comparison / logical produce bool.
        switch (b.op) {
        case BinaryOpKind::Eq:
            return EvalValue::ofBool(eq(a, c));
        case BinaryOpKind::Ne:
            return EvalValue::ofBool(!eq(a, c));
        case BinaryOpKind::Lt: return EvalValue::ofBool(lt(a, c));
        case BinaryOpKind::Gt: return EvalValue::ofBool(lt(c, a));
        case BinaryOpKind::Le: return EvalValue::ofBool(!lt(c, a));
        case BinaryOpKind::Ge: return EvalValue::ofBool(!lt(a, c));
        case BinaryOpKind::And: return EvalValue::ofBool(toBool(a) && toBool(c));
        case BinaryOpKind::Or:  return EvalValue::ofBool(toBool(a) || toBool(c));
        default: break;
        }

        // Arithmetic / bitwise.
        if (a.kind == EvalValue::Double || c.kind == EvalValue::Double) {
            double x = a.kind == EvalValue::Double ? a.d : (double)a.i;
            double y = c.kind == EvalValue::Double ? c.d : (double)c.i;
            switch (b.op) {
            case BinaryOpKind::Add: return EvalValue::ofDouble(x + y);
            case BinaryOpKind::Sub: return EvalValue::ofDouble(x - y);
            case BinaryOpKind::Mul: return EvalValue::ofDouble(x * y);
            case BinaryOpKind::Div:
                if (y == 0.0) { error(b.range, "compile-time division by zero"); return std::nullopt; }
                return EvalValue::ofDouble(x / y);
            default:
                error(b.range, "compile-time evaluation: operator not supported on double");
                return std::nullopt;
            }
        }
        int64_t x = a.i, y = c.i;
        switch (b.op) {
        case BinaryOpKind::Add: return EvalValue::ofInt(x + y);
        case BinaryOpKind::Sub: return EvalValue::ofInt(x - y);
        case BinaryOpKind::Mul: return EvalValue::ofInt(x * y);
        case BinaryOpKind::Div:
            if (y == 0) { error(b.range, "compile-time division by zero"); return std::nullopt; }
            return EvalValue::ofInt(x / y);
        case BinaryOpKind::Mod:
            if (y == 0) { error(b.range, "compile-time modulo by zero"); return std::nullopt; }
            return EvalValue::ofInt(x % y);
        case BinaryOpKind::BitAnd: return EvalValue::ofInt(x & y);
        case BinaryOpKind::BitOr:  return EvalValue::ofInt(x | y);
        case BinaryOpKind::BitXor: return EvalValue::ofInt(x ^ y);
        case BinaryOpKind::Shl:    return EvalValue::ofInt(x << (y & 63));
        case BinaryOpKind::Shr:    return EvalValue::ofInt(x >> (y & 63));
        default: break;
        }
        error(b.range, "compile-time evaluation: unsupported operator");
        return std::nullopt;
    }

    static bool eq(const EvalValue& a, const EvalValue& c) {
        if (a.kind == EvalValue::Double || c.kind == EvalValue::Double)
            return (a.kind == EvalValue::Double ? a.d : (double)a.i) ==
                   (c.kind == EvalValue::Double ? c.d : (double)c.i);
        if (a.kind == EvalValue::Bool || c.kind == EvalValue::Bool)
            return toBool(a) == toBool(c);
        return a.i == c.i;
    }

    static bool lt(const EvalValue& a, const EvalValue& c) {
        if (a.kind == EvalValue::Double || c.kind == EvalValue::Double)
            return (a.kind == EvalValue::Double ? a.d : (double)a.i) <
                   (c.kind == EvalValue::Double ? c.d : (double)c.i);
        return a.i < c.i;
    }

    // ---- Call expression ----

    std::optional<EvalValue> evalCall(const CallExpr& c) {
        // Callee must be an identifier naming an @eval function or a const.
        if (c.callee->kind != ExprKind::Identifier) {
            error(c.range, "compile-time evaluation: can only call @eval functions");
            return std::nullopt;
        }
        std::string callee = static_cast<const IdentifierExpr&>(*c.callee).name;

        // Zero-arg call of a const value: const K();  -> K
        auto cit = const_values_.find(callee);
        if (cit != const_values_.end() && c.args.empty())
            return cit->second;

        // Find @eval function.
        FuncDecl* fn = nullptr;
        for (auto& f : tu_.functions) {
            if (f.has_eval && f.name == callee) { fn = &f; break; }
        }
        if (!fn) {
            error(c.range, "compile-time evaluation: '" + callee + "' is not an @eval function");
            return std::nullopt;
        }
        if (c.args.size() != fn->params.size()) {
            error(c.range, "compile-time evaluation: wrong argument count for '" + callee + "'");
            return std::nullopt;
        }
        if (++depth_ > kMaxDepth) {
            error(c.range, "compile-time evaluation: recursion depth exceeded in '" + callee + "'");
            return std::nullopt;
        }

        // Evaluate arguments in the CALLER's environment first, then bind
        // them into the callee's fresh locals (recursive calls need the
        // caller's locals still visible while evaluating arguments).
        std::vector<EvalValue> argVals;
        for (auto& a : c.args) {
            auto v = evalExpr(*a);
            if (!v) return std::nullopt;
            argVals.push_back(*v);
        }

        // Bind args, save/restore locals around the call.
        std::unordered_map<std::string, EvalValue> saved = std::move(locals_);
        locals_.clear();
        for (size_t i = 0; i < fn->params.size(); i++)
            locals_[fn->params[i].name] = argVals[i];
        bool savedBreak = break_, savedCont = continue_;
        break_ = continue_ = false;
        auto result = evalBlockStatements(*fn->body);
        locals_ = std::move(saved);
        break_ = savedBreak;
        continue_ = savedCont;
        depth_--;
        return result;
    }

    // ---- Statements (returns the function's return value when a return
    //      statement is hit; std::nullopt otherwise) ----

    std::optional<EvalValue> evalBlockStatements(const BlockStmt& block) {
        for (auto& st : block.statements) {
            if (!st) continue;
            auto r = evalStmt(*st);
            if (r) return r;  // return/break/continue signal
        }
        return std::nullopt;
    }

    std::optional<EvalValue> evalStmt(const Stmt& s) {
        switch (s.kind) {
        case StmtKind::Block:
            return evalBlockStatements(static_cast<const BlockStmt&>(s));
        case StmtKind::ReturnStmt: {
            auto& r = static_cast<const ReturnStmt&>(s);
            if (!r.value) return EvalValue::ofInt(0);
            return evalExpr(*r.value);
        }
        case StmtKind::VarDeclStmt: {
            auto& vds = static_cast<const VarDeclStmt&>(s);
            for (auto& d : vds.decls) {
                if (d.init_expr) {
                    auto v = evalExpr(*d.init_expr);
                    if (!v) return std::nullopt;
                    locals_[d.name] = *v;
                } else {
                    locals_[d.name] = EvalValue::ofInt(0);
                }
            }
            return std::nullopt;
        }
        case StmtKind::ExprStmt: {
            auto& es = static_cast<const ExprStmt&>(s);
            if (es.expression) {
                if (!evalExpr(*es.expression)) return std::nullopt;
            }
            return std::nullopt;
        }
        case StmtKind::IfStmt: {
            auto& is = static_cast<const IfStmt&>(s);
            auto cond = evalExpr(*is.condition);
            if (!cond) return std::nullopt;
            if (toBool(*cond)) {
                if (is.then_block) return evalStmt(*is.then_block);
            } else if (is.else_block) {
                return evalStmt(*is.else_block);
            }
            return std::nullopt;
        }
        case StmtKind::WhileStmt: {
            auto& ws = static_cast<const WhileStmt&>(s);
            while (true) {
                auto cond = evalExpr(*ws.condition);
                if (!cond) return std::nullopt;
                if (!toBool(*cond)) break;
                auto r = evalStmt(*ws.body);
                if (r) {
                    if (break_) { break_ = false; return std::nullopt; }
                    if (continue_) { continue_ = false; continue; }
                    return r;  // return
                }
            }
            return std::nullopt;
        }
        case StmtKind::ForStmt: {
            auto& fs = static_cast<const ForStmt&>(s);
            if (fs.init) { auto r = evalStmt(*fs.init); if (r) return r; }
            while (true) {
                if (fs.condition) {
                    auto cond = evalExpr(*fs.condition);
                    if (!cond) return std::nullopt;
                    if (!toBool(*cond)) break;
                }
                auto r = evalStmt(*fs.body);
                if (r) {
                    if (break_) { break_ = false; break; }
                    if (continue_) { continue_ = false; }
                    else { return r; }
                }
                if (fs.step) { auto r2 = evalExpr(*fs.step); if (!r2) return std::nullopt; }
            }
            return std::nullopt;
        }
        case StmtKind::BreakStmt:
            break_ = true;
            return EvalValue::ofInt(1);  // signal (break/continue markers set)
        case StmtKind::ContinueStmt:
            continue_ = true;
            return EvalValue::ofInt(1);
        default:
            error(s.range, "compile-time evaluation: statement not supported in @eval function");
            return std::nullopt;
        }
    }

    // ---- Literal materialization for rewritten const body ----

    std::unique_ptr<Expr> makeLiteral(const EvalValue& v, const TypeNode& t) {
        switch (v.kind) {
        case EvalValue::Bool: return std::make_unique<BoolLiteralExpr>(v.b, SourceRange{});
        case EvalValue::Double: return std::make_unique<FloatLiteralExpr>(v.d, SourceRange{});
        case EvalValue::Str: return std::make_unique<StringLiteralExpr>(v.s, SourceRange{});
        case EvalValue::Int:
        default:
            return std::make_unique<IntegerLiteralExpr>(v.i, SourceRange{});
        }
    }
};

} // namespace

bool evaluateCompileTimeConstants(TranslationUnit& tu, DiagnosticEngine& diag) {
    EvalInterpreter eval(tu, diag);
    return eval.run();
}

} // namespace mylang
