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
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mylang {

namespace {

struct EvalValue {
    enum Kind { Int, Double, Bool, Str, Ast, Null } kind = Null;
    int64_t i = 0;
    double d = 0.0;
    bool b = false;
    std::string s;
    // M4: AST value (list of statements, shared so EvalValue stays copyable).
    std::shared_ptr<std::vector<std::unique_ptr<Stmt>>> stmts;

    static EvalValue ofInt(int64_t v) { EvalValue r; r.kind = Int; r.i = v; return r; }
    static EvalValue ofDouble(double v) { EvalValue r; r.kind = Double; r.d = v; return r; }
    static EvalValue ofBool(bool v) { EvalValue r; r.kind = Bool; r.b = v; return r; }
    static EvalValue ofStr(std::string v) { EvalValue r; r.kind = Str; r.s = std::move(v); return r; }
    static EvalValue ofAst(std::vector<std::unique_ptr<Stmt>> v) {
        EvalValue r; r.kind = Ast;
        r.stmts = std::make_shared<std::vector<std::unique_ptr<Stmt>>>(std::move(v));
        return r;
    }
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

    // M4: execute a @macro proc-macro function; returns generated statements.
    bool runProcMacro(FuncDecl& fn,
                      const std::vector<std::unique_ptr<Expr>>& args,
                      std::vector<std::unique_ptr<Stmt>>& out) {
        if (!fn.body) {
            diag_.error(fn.range, "proc-macro '" + fn.name + "' has no body");
            return false;
        }
        if (args.size() != fn.params.size()) {
            diag_.error(fn.range, "proc-macro '" + fn.name + "' expects " +
                std::to_string(fn.params.size()) + " argument(s), got " +
                std::to_string(args.size()));
            return false;
        }
        // Evaluate arguments in the (empty) caller environment, then bind.
        std::vector<EvalValue> argVals;
        for (auto& a : args) {
            auto v = evalExpr(*a);
            if (!v) return false;
            argVals.push_back(*v);
        }
        locals_.clear();
        for (size_t i = 0; i < fn.params.size(); i++)
            locals_[fn.params[i].name] = argVals[i];
        bool savedBreak = break_, savedCont = continue_;
        break_ = continue_ = false;
        auto result = evalBlockStatements(*fn.body);
        break_ = savedBreak;
        continue_ = savedCont;
        if (!result || result->kind != EvalValue::Ast) {
            diag_.error(fn.range,
                "proc-macro '" + fn.name + "' must return a quote {...} (StmtList/Stmt/Expr)");
            return false;
        }
        out = std::move(*result->stmts);
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
        case ExprKind::Convert: {
            auto& c = static_cast<const ConvertExpr&>(e);
            auto v = evalExpr(*c.operand);
            if (!v) return std::nullopt;
            // 常量求值：int ↔ float / 整数间截断（窄→宽按位，宽→窄截断）
            bool is_fp = (c.to_kind == TypeKind::Float || c.to_kind == TypeKind::Double);
            if (is_fp) {
                double d = (v->kind == EvalValue::Double) ? v->d : (double)v->i;
                if (c.to_kind == TypeKind::Float) return EvalValue::ofDouble(d);
                return EvalValue::ofDouble(d);
            }
            int64_t iv = (v->kind == EvalValue::Double) ? (int64_t)v->d : v->i;
            return EvalValue::ofInt(iv);
        }
        case ExprKind::BinaryOp: {
            auto& b = static_cast<const BinaryOpExpr&>(e);
            return evalBinary(b);
        }
        case ExprKind::Assignment: {
            auto& a = static_cast<const AssignmentExpr&>(e);
            // Only identifier targets are supported at eval time.
            if (a.target->kind != ExprKind::Identifier) {
                error(a.range, "compile-time evaluation: assignment target must be a variable");
                return std::nullopt;
            }
            auto& id = static_cast<const IdentifierExpr&>(*a.target);
            auto v = evalExpr(*a.value);
            if (!v) return std::nullopt;
            locals_[id.name] = *v;
            return v;
        }
        case ExprKind::Quote: {
            auto& q = static_cast<const QuoteExpr&>(e);
            return evalQuote(q);
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

        // M4: StmtList + StmtList concatenation.
        if (b.op == BinaryOpKind::Add && a.kind == EvalValue::Ast && c.kind == EvalValue::Ast) {
            auto out = std::make_shared<std::vector<std::unique_ptr<Stmt>>>();
            for (auto& s : *a.stmts) out->push_back(cloneStmtI(*s));
            for (auto& s : *c.stmts) out->push_back(cloneStmtI(*s));
            return EvalValue::ofAst(std::move(*out));
        }

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

        // Find @eval / @macro function.
        FuncDecl* fn = nullptr;
        for (auto& f : tu_.functions) {
            if ((f.has_eval || f.has_proc_macro) && f.name == callee) { fn = &f; break; }
        }
        if (!fn) {
            error(c.range, "compile-time evaluation: '" + callee + "' is not an @eval/@macro function");
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

    // ---- M4: quote{...} evaluation & AST cloning with interpolation ----

    /// `quote { ... }` → an AST value (list of statements). `$param`
    /// placeholders are interpolated from the current locals: numeric/string
    /// values become literals, AST values are inlined.
    std::optional<EvalValue> evalQuote(const QuoteExpr& q) {
        std::vector<std::unique_ptr<Stmt>> out;
        for (auto& st : q.body->statements) {
            if (!st) continue;
            // Statement-position interpolation: `$x` as a standalone statement
            // whose value is an AST list → inline the statements.
            if (st->kind == StmtKind::ExprStmt) {
                auto& es = static_cast<const ExprStmt&>(*st);
                if (es.expression && es.expression->kind == ExprKind::MacroParam) {
                    auto& p = static_cast<const MacroParamExpr&>(*es.expression);
                    auto it = locals_.find(p.name);
                    if (it != locals_.end() && it->second.kind == EvalValue::Ast) {
                        for (auto& ss : *it->second.stmts)
                            out.push_back(cloneStmtI(*ss));
                        continue;
                    }
                }
            }
            out.push_back(cloneStmtI(*st));
        }
        return EvalValue::ofAst(std::move(out));
    }

    /// Clone a statement; interpolate `$param` from locals (M4 quote).
    std::unique_ptr<Stmt> cloneStmtI(const Stmt& s) {
        switch (s.kind) {
        case StmtKind::Block: {
            auto& v = static_cast<const BlockStmt&>(s);
            std::vector<std::unique_ptr<Stmt>> stmts;
            for (auto& st : v.statements) stmts.push_back(cloneStmtI(*st));
            return std::make_unique<BlockStmt>(std::move(stmts), v.range);
        }
        case StmtKind::VarDeclStmt: {
            auto& v = static_cast<const VarDeclStmt&>(s);
            std::vector<VarDecl> decls;
            for (auto& d : v.decls) {
                VarDecl nd = d;
                // `$name` variable-name interpolation (M4 quote).
                if (!nd.name.empty() && nd.name[0] == '$') {
                    auto it = locals_.find(nd.name.substr(1));
                    if (it == locals_.end() || it->second.kind != EvalValue::Str) {
                        error(nd.range, "quote: '$" + nd.name.substr(1) +
                            "' must be a string to use as a variable name");
                        nd.name = "__error__";
                    } else {
                        nd.name = it->second.s;
                    }
                }
                if (d.init_expr) nd.init_expr = cloneExprI(*d.init_expr);
                decls.push_back(std::move(nd));
            }
            return std::make_unique<VarDeclStmt>(std::move(decls), v.range);
        }
        case StmtKind::ExprStmt: {
            auto& v = static_cast<const ExprStmt&>(s);
            return std::make_unique<ExprStmt>(cloneExprI(*v.expression), v.range);
        }
        case StmtKind::IfStmt: {
            auto& v = static_cast<const IfStmt&>(s);
            return std::make_unique<IfStmt>(
                cloneExprI(*v.condition),
                v.then_block ? cloneStmtI(*v.then_block) : nullptr,
                v.else_block ? cloneStmtI(*v.else_block) : nullptr, v.range);
        }
        case StmtKind::WhileStmt: {
            auto& v = static_cast<const WhileStmt&>(s);
            return std::make_unique<WhileStmt>(cloneExprI(*v.condition),
                                               v.body ? cloneStmtI(*v.body) : nullptr, v.range);
        }
        case StmtKind::ForStmt: {
            auto& v = static_cast<const ForStmt&>(s);
            return std::make_unique<ForStmt>(
                v.init ? cloneStmtI(*v.init) : nullptr,
                v.condition ? cloneExprI(*v.condition) : nullptr,
                v.step ? cloneExprI(*v.step) : nullptr,
                v.body ? cloneStmtI(*v.body) : nullptr, v.range, v.parallel, v.gpu);
        }
        case StmtKind::ForInStmt: {
            auto& v = static_cast<const ForInStmt&>(s);
            return std::make_unique<ForInStmt>(
                v.var_name, v.var_type, v.has_type,
                v.iterable ? cloneExprI(*v.iterable) : nullptr,
                v.body ? cloneStmtI(*v.body) : nullptr, v.range);
        }
        case StmtKind::ReturnStmt: {
            auto& v = static_cast<const ReturnStmt&>(s);
            return std::make_unique<ReturnStmt>(
                v.value ? cloneExprI(*v.value) : nullptr, v.range);
        }
        case StmtKind::BreakStmt: return std::make_unique<BreakStmt>(s.range);
        case StmtKind::ContinueStmt: return std::make_unique<ContinueStmt>(s.range);
        case StmtKind::AwaitStmt: {
            auto& v = static_cast<const AwaitStmt&>(s);
            return std::make_unique<AwaitStmt>(
                v.expr ? cloneExprI(*v.expr) : nullptr,
                v.timeout ? cloneExprI(*v.timeout) : nullptr, v.range);
        }
        case StmtKind::TryStmt: {
            auto& v = static_cast<const TryStmt&>(s);
            auto tb = v.try_block ? cloneStmtI(*v.try_block) : nullptr;
            std::vector<CatchClause> cs;
            for (auto& c : v.catches) {
                CatchClause nc;
                nc.var_name = c.var_name;
                nc.var_type = c.var_type;
                if (c.block)
                    nc.block = std::unique_ptr<BlockStmt>(
                        static_cast<BlockStmt*>(cloneStmtI(*c.block).release()));
                cs.push_back(std::move(nc));
            }
            auto fb = v.finally_block ? cloneStmtI(*v.finally_block) : nullptr;
            return std::make_unique<TryStmt>(
                tb ? std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(tb.release())) : nullptr,
                std::move(cs),
                fb ? std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(fb.release())) : nullptr,
                v.range);
        }
        case StmtKind::ThrowStmt: {
            auto& v = static_cast<const ThrowStmt&>(s);
            return std::make_unique<ThrowStmt>(cloneExprI(*v.expr), v.range);
        }
        default:
            error(s.range, "quote: statement construct not supported");
            return std::make_unique<BlockStmt>(std::vector<std::unique_ptr<Stmt>>{}, s.range);
        }
    }

    /// Clone an expression; interpolate `$param` from locals (M4 quote).
    std::unique_ptr<Expr> cloneExprI(const Expr& e) {
        switch (e.kind) {
        case ExprKind::IntegerLiteral: {
            auto& v = static_cast<const IntegerLiteralExpr&>(e);
            return std::make_unique<IntegerLiteralExpr>(v.value, v.range, v.is_long);
        }
        case ExprKind::FloatLiteral: {
            auto& v = static_cast<const FloatLiteralExpr&>(e);
            return std::make_unique<FloatLiteralExpr>(v.value, v.range);
        }
        case ExprKind::BoolLiteral: {
            auto& v = static_cast<const BoolLiteralExpr&>(e);
            return std::make_unique<BoolLiteralExpr>(v.value, v.range);
        }
        case ExprKind::StringLiteral: {
            auto& v = static_cast<const StringLiteralExpr&>(e);
            return std::make_unique<StringLiteralExpr>(v.value, v.range);
        }
        case ExprKind::NullLiteral:
            return std::make_unique<NullLiteralExpr>(e.range);
        case ExprKind::Identifier: {
            auto& v = static_cast<const IdentifierExpr&>(e);
            return std::make_unique<IdentifierExpr>(v.name, v.range);
        }
        case ExprKind::MacroParam: {
            auto& p = static_cast<const MacroParamExpr&>(e);
            auto it = locals_.find(p.name);
            if (it == locals_.end()) {
                error(e.range, "quote: unknown interpolation variable '$" + p.name + "'");
                return std::make_unique<IdentifierExpr>("__error__", e.range);
            }
            const EvalValue& v = it->second;
            switch (v.kind) {
            case EvalValue::Int:    return std::make_unique<IntegerLiteralExpr>(v.i, e.range);
            case EvalValue::Double: return std::make_unique<FloatLiteralExpr>(v.d, e.range);
            case EvalValue::Bool:   return std::make_unique<BoolLiteralExpr>(v.b, e.range);
            case EvalValue::Str:    return std::make_unique<StringLiteralExpr>(v.s, e.range);
            case EvalValue::Ast:
                // Expression position: require the AST to be a single expr-stmt.
                if (v.stmts->size() == 1 && (*v.stmts)[0]->kind == StmtKind::ExprStmt) {
                    auto& es = static_cast<const ExprStmt&>(*(*v.stmts)[0]);
                    if (es.expression) return cloneExprI(*es.expression);
                }
                error(e.range, "quote: '$" + p.name + "' is not a single expression");
                return std::make_unique<IdentifierExpr>("__error__", e.range);
            default:
                error(e.range, "quote: cannot interpolate '$" + p.name + "'");
                return std::make_unique<IdentifierExpr>("__error__", e.range);
            }
        }
        case ExprKind::BinaryOp: {
            auto& v = static_cast<const BinaryOpExpr&>(e);
            return std::make_unique<BinaryOpExpr>(
                cloneExprI(*v.lhs), v.op, cloneExprI(*v.rhs), v.range);
        }
        case ExprKind::UnaryOp: {
            auto& v = static_cast<const UnaryOpExpr&>(e);
            return std::make_unique<UnaryOpExpr>(v.op, cloneExprI(*v.operand), v.range);
        }
        case ExprKind::Convert: {
            auto& v = static_cast<const ConvertExpr&>(e);
            return std::make_unique<ConvertExpr>(v.to_kind, cloneExprI(*v.operand), v.range);
        }
        case ExprKind::Call: {
            auto& v = static_cast<const CallExpr&>(e);
            std::vector<std::unique_ptr<Expr>> a;
            for (auto& arg : v.args) a.push_back(cloneExprI(*arg));
            return std::make_unique<CallExpr>(cloneExprI(*v.callee), std::move(a), v.range);
        }
        case ExprKind::MemberAccess: {
            auto& v = static_cast<const MemberAccessExpr&>(e);
            return std::make_unique<MemberAccessExpr>(cloneExprI(*v.object), v.member_name, v.range);
        }
        case ExprKind::Subscript: {
            auto& v = static_cast<const SubscriptExpr&>(e);
            return std::make_unique<SubscriptExpr>(cloneExprI(*v.array), cloneExprI(*v.index), v.range);
        }
        case ExprKind::NewExpr: {
            auto& v = static_cast<const NewExpr&>(e);
            std::vector<std::unique_ptr<Expr>> a;
            for (auto& arg : v.args) a.push_back(cloneExprI(*arg));
            return std::make_unique<NewExpr>(v.class_name, v.type_args, std::move(a), v.range);
        }
        case ExprKind::NewArrayExpr: {
            auto& v = static_cast<const NewArrayExpr&>(e);
            std::vector<std::unique_ptr<Expr>> d;
            for (auto& dim : v.dimensions) d.push_back(cloneExprI(*dim));
            return std::make_unique<NewArrayExpr>(v.element_type, std::move(d), v.range);
        }
        case ExprKind::ThisExpr:
            return std::make_unique<ThisExpr>(e.range);
        case ExprKind::Assignment: {
            auto& v = static_cast<const AssignmentExpr&>(e);
            // `$name = ...` — interpolate a string as an identifier target.
            std::unique_ptr<Expr> tgt;
            if (v.target->kind == ExprKind::MacroParam) {
                auto& p = static_cast<const MacroParamExpr&>(*v.target);
                auto it = locals_.find(p.name);
                if (it != locals_.end() && it->second.kind == EvalValue::Str) {
                    tgt = std::make_unique<IdentifierExpr>(it->second.s, v.target->range);
                } else {
                    tgt = cloneExprI(*v.target);
                }
            } else {
                tgt = cloneExprI(*v.target);
            }
            return std::make_unique<AssignmentExpr>(std::move(tgt), cloneExprI(*v.value), v.range);
        }
        case ExprKind::Ternary: {
            auto& v = static_cast<const TernaryExpr&>(e);
            return std::make_unique<TernaryExpr>(
                cloneExprI(*v.condition), cloneExprI(*v.true_expr), cloneExprI(*v.false_expr), v.range);
        }
        case ExprKind::Range: {
            auto& v = static_cast<const RangeExpr&>(e);
            return std::make_unique<RangeExpr>(cloneExprI(*v.start), cloneExprI(*v.end), v.range);
        }
        case ExprKind::NamedArg: {
            auto& v = static_cast<const NamedArgExpr&>(e);
            return std::make_unique<NamedArgExpr>(v.name, cloneExprI(*v.value), v.range);
        }
        case ExprKind::EnumVariant: {
            auto& v = static_cast<const EnumVariantExpr&>(e);
            std::vector<std::unique_ptr<Expr>> a;
            for (auto& arg : v.args) a.push_back(cloneExprI(*arg));
            return std::make_unique<EnumVariantExpr>(v.enum_name, v.variant_index, std::move(a), v.range);
        }
        case ExprKind::Try: {
            auto& v = static_cast<const TryExpr&>(e);
            return std::make_unique<TryExpr>(cloneExprI(*v.try_expr), v.catch_var_name,
                                             cloneExprI(*v.catch_expr), v.range);
        }
        default:
            error(e.range, "quote: expression construct not supported");
            return std::make_unique<IdentifierExpr>("__error__", e.range);
        }
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

bool evalProcMacro(TranslationUnit& tu, DiagnosticEngine& diag, FuncDecl& fn,
                   const std::vector<std::unique_ptr<Expr>>& args,
                   std::vector<std::unique_ptr<Stmt>>& out) {
    EvalInterpreter eval(tu, diag);
    return eval.runProcMacro(fn, args, out);
}

} // namespace mylang
