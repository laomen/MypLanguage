// src/codegen/escape_analysis.cpp — 逃逸分析实现（第一版，2026-08-28）
//
// 见 escape_analysis.h。核心：对候选变量 v（`let v = new T()`），遍历函数体，
// 用 exprEscapes/stmtEscapes 判定 v 是否逃逸。
#include "mylang/AST.h"
#include "escape_analysis.h"

#include <map>

namespace mylang {

// 前向声明（stmtEscapes <-> exprStmtMentions 互相递归）
static bool stmtEscapes(const Stmt* s, const std::string& v);
static bool exprStmtMentions(const Stmt* s, const std::string& v);

// ---- 表达式级逃逸判定 ----
// v 只允许作为 MemberAccess 的 object（读/写字段）；其他位置逃逸。
// 方法调用 v.method()（Call 分支）判逃逸。
static bool exprEscapes(const Expr* e, const std::string& v) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::Identifier: {
            auto& id = static_cast<const IdentifierExpr&>(*e);
            return id.name == v;  // 裸引用 v → 逃逸（被当作值使用/传递）
        }
        case ExprKind::MemberAccess: {
            auto& ma = static_cast<const MemberAccessExpr&>(*e);
            // v.field：最内层 object 是 v → 允许（方法调用由 Call 分支已拦截）
            const Expr* o = ma.object.get();
            while (o && o->kind == ExprKind::MemberAccess)
                o = static_cast<const MemberAccessExpr*>(o)->object.get();
            if (o && o->kind == ExprKind::Identifier &&
                static_cast<const IdentifierExpr*>(o)->name == v)
                return false;
            return exprEscapes(ma.object.get(), v);
        }
        case ExprKind::Call: {
            auto& c = static_cast<const CallExpr&>(*e);
            // 方法调用 v.method() 允许（receiver 位置，MemberAccess object 分支处理）。
            // v 作为普通实参 f(v) → 逃逸（函数可能持有）。
            for (auto& a : c.args)
                if (exprEscapes(a.get(), v)) return true;
            return exprEscapes(c.callee.get(), v);
        }
        case ExprKind::Assignment: {
            auto& a = static_cast<const AssignmentExpr&>(*e);
            return exprEscapes(a.target.get(), v) || exprEscapes(a.value.get(), v);
        }
        case ExprKind::BinaryOp: {
            auto& b = static_cast<const BinaryOpExpr&>(*e);
            return exprEscapes(b.lhs.get(), v) || exprEscapes(b.rhs.get(), v);
        }
        case ExprKind::UnaryOp: {
            auto& u = static_cast<const UnaryOpExpr&>(*e);
            return exprEscapes(u.operand.get(), v);
        }
        case ExprKind::Convert: {
            auto& c = static_cast<const ConvertExpr&>(*e);
            return exprEscapes(c.operand.get(), v);
        }
        case ExprKind::Subscript: {
            auto& s = static_cast<const SubscriptExpr&>(*e);
            // 数组元素访问 v[i]：array 是 v 本身 → 允许（第二版数组逃逸分析）。
            if (s.array && s.array->kind == ExprKind::Identifier &&
                static_cast<const IdentifierExpr*>(s.array.get())->name == v)
                return exprEscapes(s.index.get(), v);
            return exprEscapes(s.array.get(), v) || exprEscapes(s.index.get(), v);
        }
        case ExprKind::NewExpr: {
            auto& n = static_cast<const NewExpr&>(*e);
            for (auto& a : n.args)
                if (exprEscapes(a.get(), v)) return true;
            return false;
        }
        case ExprKind::NewArrayExpr: {
            auto& n = static_cast<const NewArrayExpr&>(*e);
            for (auto& d : n.dimensions)
                if (exprEscapes(d.get(), v)) return true;
            return false;
        }
        case ExprKind::Ternary: {
            auto& t = static_cast<const TernaryExpr&>(*e);
            return exprEscapes(t.condition.get(), v) ||
                   exprEscapes(t.true_expr.get(), v) ||
                   exprEscapes(t.false_expr.get(), v);
        }
        case ExprKind::Range: {
            auto& r = static_cast<const RangeExpr&>(*e);
            return exprEscapes(r.start.get(), v) || exprEscapes(r.end.get(), v);
        }
        case ExprKind::EnumVariant: {
            auto& ev = static_cast<const EnumVariantExpr&>(*e);
            for (auto& a : ev.args)
                if (exprEscapes(a.get(), v)) return true;
            return false;
        }
        case ExprKind::TupleExpr: {
            auto& t = static_cast<const TupleExpr&>(*e);
            for (auto& el : t.elements)
                if (exprEscapes(el.get(), v)) return true;
            return false;
        }
        case ExprKind::NamedArg: {
            auto& n = static_cast<const NamedArgExpr&>(*e);
            return exprEscapes(n.value.get(), v);
        }
        case ExprKind::Pipe: {
            auto& p = static_cast<const PipeExpr&>(*e);
            return exprEscapes(p.lhs.get(), v) || exprEscapes(p.rhs.get(), v);
        }
        case ExprKind::Try: {
            auto& t = static_cast<const TryExpr&>(*e);
            return exprEscapes(t.try_expr.get(), v) || exprEscapes(t.catch_expr.get(), v);
        }
        case ExprKind::Await: {
            auto& a = static_cast<const AwaitExpr&>(*e);
            if (a.operand && exprEscapes(a.operand.get(), v)) return true;
            if (a.timeout && exprEscapes(a.timeout.get(), v)) return true;
            return false;
        }
        case ExprKind::Lambda:
            return true;  // v 出现在 lambda 内 → 逃逸（保守）
        case ExprKind::ThisExpr:
        case ExprKind::IntegerLiteral:
        case ExprKind::FloatLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::StringLiteral:
        case ExprKind::NullLiteral:
        case ExprKind::MacroParam:
        case ExprKind::Quote:
        case ExprKind::GpuReduceExpr:
            return false;  // 不含 v 的引用
    }
    return false;
}

// ---- 表达式"提及"判定（任意位置出现 Identifier==v）——复杂语句保守用 ----
static bool exprMentions(const Expr* e, const std::string& v) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::Identifier:
            return static_cast<const IdentifierExpr&>(*e).name == v;
        case ExprKind::MemberAccess:
            return exprMentions(static_cast<const MemberAccessExpr&>(*e).object.get(), v);
        case ExprKind::Call: {
            auto& c = static_cast<const CallExpr&>(*e);
            if (exprMentions(c.callee.get(), v)) return true;
            for (auto& a : c.args)
                if (exprMentions(a.get(), v)) return true;
            return false;
        }
        case ExprKind::Assignment: {
            auto& a = static_cast<const AssignmentExpr&>(*e);
            return exprMentions(a.target.get(), v) || exprMentions(a.value.get(), v);
        }
        case ExprKind::BinaryOp: {
            auto& b = static_cast<const BinaryOpExpr&>(*e);
            return exprMentions(b.lhs.get(), v) || exprMentions(b.rhs.get(), v);
        }
        case ExprKind::UnaryOp:
            return exprMentions(static_cast<const UnaryOpExpr&>(*e).operand.get(), v);
        case ExprKind::Convert:
            return exprMentions(static_cast<const ConvertExpr&>(*e).operand.get(), v);
        case ExprKind::Subscript: {
            auto& s = static_cast<const SubscriptExpr&>(*e);
            return exprMentions(s.array.get(), v) || exprMentions(s.index.get(), v);
        }
        case ExprKind::NewExpr: {
            auto& n = static_cast<const NewExpr&>(*e);
            for (auto& a : n.args)
                if (exprMentions(a.get(), v)) return true;
            return false;
        }
        case ExprKind::NewArrayExpr: {
            auto& n = static_cast<const NewArrayExpr&>(*e);
            for (auto& d : n.dimensions)
                if (exprMentions(d.get(), v)) return true;
            return false;
        }
        case ExprKind::Ternary: {
            auto& t = static_cast<const TernaryExpr&>(*e);
            return exprMentions(t.condition.get(), v) ||
                   exprMentions(t.true_expr.get(), v) ||
                   exprMentions(t.false_expr.get(), v);
        }
        case ExprKind::Range: {
            auto& r = static_cast<const RangeExpr&>(*e);
            return exprMentions(r.start.get(), v) || exprMentions(r.end.get(), v);
        }
        case ExprKind::EnumVariant: {
            auto& ev = static_cast<const EnumVariantExpr&>(*e);
            for (auto& a : ev.args)
                if (exprMentions(a.get(), v)) return true;
            return false;
        }
        case ExprKind::TupleExpr: {
            auto& t = static_cast<const TupleExpr&>(*e);
            for (auto& el : t.elements)
                if (exprMentions(el.get(), v)) return true;
            return false;
        }
        case ExprKind::NamedArg:
            return exprMentions(static_cast<const NamedArgExpr&>(*e).value.get(), v);
        case ExprKind::Pipe: {
            auto& p = static_cast<const PipeExpr&>(*e);
            return exprMentions(p.lhs.get(), v) || exprMentions(p.rhs.get(), v);
        }
        case ExprKind::Try: {
            auto& t = static_cast<const TryExpr&>(*e);
            return exprMentions(t.try_expr.get(), v) || exprMentions(t.catch_expr.get(), v);
        }
        case ExprKind::Await: {
            auto& a = static_cast<const AwaitExpr&>(*e);
            if (a.operand && exprMentions(a.operand.get(), v)) return true;
            if (a.timeout && exprMentions(a.timeout.get(), v)) return true;
            return false;
        }
        case ExprKind::Lambda:
            return true;  // 提及即保守逃逸
        case ExprKind::GpuReduceExpr:
            return true;
        default:
            return false;
    }
}

// ---- 语句级逃逸判定 ----
static bool stmtEscapes(const Stmt* s, const std::string& v) {
    if (!s) return false;
    switch (s->kind) {
        case StmtKind::Block: {
            auto& b = static_cast<const BlockStmt&>(*s);
            for (auto& st : b.statements)
                if (stmtEscapes(st.get(), v)) return true;
            return false;
        }
        case StmtKind::VarDeclStmt: {
            auto& vd = static_cast<const VarDeclStmt&>(*s);
            for (auto& d : vd.decls) {
                // v 自己的声明跳过（init 是 new，不含 v 引用）；其他变量 init 提及 v → 逃逸
                if (d.name != v && d.init_expr && exprEscapes(d.init_expr.get(), v))
                    return true;
            }
            return false;
        }
        case StmtKind::DestructureStmt:
            return true;  // 保守：解构赋值可能转移引用
        case StmtKind::ExprStmt: {
            auto& e = static_cast<const ExprStmt&>(*s);
            return exprEscapes(e.expression.get(), v);
        }
        case StmtKind::IfStmt: {
            auto& i = static_cast<const IfStmt&>(*s);
            if (i.condition && exprEscapes(i.condition.get(), v)) return true;
            if (i.then_block && stmtEscapes(i.then_block.get(), v)) return true;
            if (i.else_block && stmtEscapes(i.else_block.get(), v)) return true;
            return false;
        }
        case StmtKind::WhileStmt: {
            auto& w = static_cast<const WhileStmt&>(*s);
            if (w.condition && exprEscapes(w.condition.get(), v)) return true;
            if (w.body && stmtEscapes(w.body.get(), v)) return true;
            return false;
        }
        case StmtKind::ForStmt: {
            auto& f = static_cast<const ForStmt&>(*s);
            if (f.init && stmtEscapes(f.init.get(), v)) return true;
            if (f.condition && exprEscapes(f.condition.get(), v)) return true;
            if (f.step && exprEscapes(f.step.get(), v)) return true;
            if (f.body && stmtEscapes(f.body.get(), v)) return true;
            return false;
        }
        case StmtKind::ForInStmt: {
            auto& f = static_cast<const ForInStmt&>(*s);
            if (f.iterable && exprEscapes(f.iterable.get(), v)) return true;
            if (f.body && stmtEscapes(f.body.get(), v)) return true;
            return false;
        }
        case StmtKind::ReturnStmt: {
            auto& r = static_cast<const ReturnStmt&>(*s);
            return r.value ? exprEscapes(r.value.get(), v) : false;  // return v → 逃逸
        }
        case StmtKind::TryStmt: {
            auto& t = static_cast<const TryStmt&>(*s);
            if (t.try_block && stmtEscapes(t.try_block.get(), v)) return true;
            for (auto& c : t.catches)
                if (c.block && stmtEscapes(c.block.get(), v)) return true;
            if (t.finally_block && stmtEscapes(t.finally_block.get(), v)) return true;
            return false;
        }
        case StmtKind::ThrowStmt:
        case StmtKind::AwaitStmt:
        case StmtKind::MappingStmt:
        case StmtKind::MatchStmt:
        case StmtKind::NonlocalStmt:
        case StmtKind::GpuTileStmt:
        case StmtKind::GpuReduceStmt:
        case StmtKind::GpuScanStmt:
        case StmtKind::GpuScatterStmt:
            // 保守：复杂语句里只要提及 v 就逃逸（异常/并发/模式匹配可能转移引用）
            return exprStmtMentions(s, v);
        case StmtKind::BreakStmt:
        case StmtKind::ContinueStmt:
            return false;
    }
    return false;
}

// 复杂语句的"提及"判定（stmtEscapes 用）
static bool exprStmtMentions(const Stmt* s, const std::string& v) {
    switch (s->kind) {
        case StmtKind::ThrowStmt: {
            auto& t = static_cast<const ThrowStmt&>(*s);
            return exprMentions(t.expr.get(), v);
        }
        case StmtKind::AwaitStmt: {
            auto& a = static_cast<const AwaitStmt&>(*s);
            if (a.expr && exprMentions(a.expr.get(), v)) return true;
            if (a.timeout && exprMentions(a.timeout.get(), v)) return true;
            return false;
        }
        case StmtKind::MatchStmt: {
            auto& m = static_cast<const MatchStmt&>(*s);
            if (exprMentions(m.subject.get(), v)) return true;
            for (auto& arm : m.arms)
                if (arm.body && stmtEscapes(arm.body.get(), v)) return true;
            return false;
        }
        case StmtKind::GpuReduceStmt: {
            auto& g = static_cast<const GpuReduceStmt&>(*s);
            if (g.init_expr && exprMentions(g.init_expr.get(), v)) return true;
            if (g.begin_expr && exprMentions(g.begin_expr.get(), v)) return true;
            if (g.end_expr && exprMentions(g.end_expr.get(), v)) return true;
            return false;
        }
        case StmtKind::GpuScanStmt: {
            auto& g = static_cast<const GpuScanStmt&>(*s);
            if (g.init_expr && exprMentions(g.init_expr.get(), v)) return true;
            if (g.begin_expr && exprMentions(g.begin_expr.get(), v)) return true;
            if (g.end_expr && exprMentions(g.end_expr.get(), v)) return true;
            return false;
        }
        case StmtKind::GpuScatterStmt: {
            auto& g = static_cast<const GpuScatterStmt&>(*s);
            if (g.a_begin && exprMentions(g.a_begin.get(), v)) return true;
            if (g.a_end && exprMentions(g.a_end.get(), v)) return true;
            if (g.idx_begin && exprMentions(g.idx_begin.get(), v)) return true;
            if (g.idx_end && exprMentions(g.idx_end.get(), v)) return true;
            return false;
        }
        default:
            return true;  // Mapping/Nonlocal/GpuTile：保守逃逸
    }
}

// ---- 收集候选：VarDecl + NewExpr 初始化的局部变量 ----
static void collectCandidates(const Stmt* s, std::map<std::string, const VarDecl*>& out) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Block: {
            auto& b = static_cast<const BlockStmt&>(*s);
            for (auto& st : b.statements) collectCandidates(st.get(), out);
            return;
        }
        case StmtKind::VarDeclStmt: {
            auto& vd = static_cast<const VarDeclStmt&>(*s);
            for (auto& d : vd.decls) {
                if (d.has_thread_annotation || d.has_threadpool_annotation) continue;
                if (d.is_const) continue;
                // 类对象候选（new T()）；数组候选（new T[N]，生成时 isStackArrayCandidate
                // 过滤——维度常量 + 元素非 ARC）。
                if (d.init_expr && (d.init_expr->kind == ExprKind::NewExpr ||
                                    d.init_expr->kind == ExprKind::NewArrayExpr))
                    out[d.name] = &d;
            }
            return;
        }
        case StmtKind::IfStmt: {
            auto& i = static_cast<const IfStmt&>(*s);
            collectCandidates(i.then_block.get(), out);
            collectCandidates(i.else_block.get(), out);
            return;
        }
        case StmtKind::WhileStmt: {
            collectCandidates(static_cast<const WhileStmt&>(*s).body.get(), out);
            return;
        }
        case StmtKind::ForStmt: {
            auto& f = static_cast<const ForStmt&>(*s);
            collectCandidates(f.init.get(), out);
            collectCandidates(f.body.get(), out);
            return;
        }
        case StmtKind::ForInStmt: {
            collectCandidates(static_cast<const ForInStmt&>(*s).body.get(), out);
            return;
        }
        case StmtKind::TryStmt: {
            auto& t = static_cast<const TryStmt&>(*s);
            collectCandidates(t.try_block.get(), out);
            for (auto& c : t.catches) collectCandidates(c.block.get(), out);
            collectCandidates(t.finally_block.get(), out);
            return;
        }
        case StmtKind::MatchStmt: {
            auto& m = static_cast<const MatchStmt&>(*s);
            for (auto& arm : m.arms) collectCandidates(arm.body.get(), out);
            return;
        }
        default:
            return;
    }
}

// ---- 分配内省检测（与 selfhost codegen.myp 对拍）----
// 函数体调用 Memory.liveObjectCount[ByType] → 观测 arena 堆对象数量 → 栈上化
// 会使计数断言失真 → 整函数不栈上化（保守；回归 arc/cyclecollect/mem_diag/
// weak_multi_sub 依赖）。
static bool exprHasLiveCall(const Expr* e);
static bool stmtHasLiveCall(const Stmt* s);

static bool callIsLiveCount(const Expr* c) {
    if (!c) return false;
    if (c->kind == ExprKind::Identifier)
        return static_cast<const IdentifierExpr*>(c)->name == "liveObjectCount";
    if (c->kind == ExprKind::MemberAccess) {
        auto& m = static_cast<const MemberAccessExpr&>(*c);
        return m.member_name == "liveObjectCount" ||
               m.member_name == "liveObjectCountByType";
    }
    return false;
}

static bool exprHasLiveCall(const Expr* e) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::Call: {
            auto& c = static_cast<const CallExpr&>(*e);
            if (callIsLiveCount(c.callee.get())) return true;
            for (auto& a : c.args)
                if (exprHasLiveCall(a.get())) return true;
            return exprHasLiveCall(c.callee.get());
        }
        case ExprKind::MemberAccess:
            return exprHasLiveCall(static_cast<const MemberAccessExpr&>(*e).object.get());
        case ExprKind::Assignment: {
            auto& a = static_cast<const AssignmentExpr&>(*e);
            return exprHasLiveCall(a.target.get()) || exprHasLiveCall(a.value.get());
        }
        case ExprKind::BinaryOp: {
            auto& b = static_cast<const BinaryOpExpr&>(*e);
            return exprHasLiveCall(b.lhs.get()) || exprHasLiveCall(b.rhs.get());
        }
        case ExprKind::UnaryOp:
            return exprHasLiveCall(static_cast<const UnaryOpExpr&>(*e).operand.get());
        case ExprKind::Convert:
            return exprHasLiveCall(static_cast<const ConvertExpr&>(*e).operand.get());
        case ExprKind::Subscript: {
            auto& s = static_cast<const SubscriptExpr&>(*e);
            return exprHasLiveCall(s.array.get()) || exprHasLiveCall(s.index.get());
        }
        case ExprKind::NewExpr: {
            auto& n = static_cast<const NewExpr&>(*e);
            for (auto& a : n.args)
                if (exprHasLiveCall(a.get())) return true;
            return false;
        }
        case ExprKind::NewArrayExpr: {
            auto& n = static_cast<const NewArrayExpr&>(*e);
            for (auto& d : n.dimensions)
                if (exprHasLiveCall(d.get())) return true;
            return false;
        }
        case ExprKind::Ternary: {
            auto& t = static_cast<const TernaryExpr&>(*e);
            return exprHasLiveCall(t.condition.get()) ||
                   exprHasLiveCall(t.true_expr.get()) ||
                   exprHasLiveCall(t.false_expr.get());
        }
        case ExprKind::Range: {
            auto& r = static_cast<const RangeExpr&>(*e);
            return exprHasLiveCall(r.start.get()) || exprHasLiveCall(r.end.get());
        }
        case ExprKind::TupleExpr: {
            auto& t = static_cast<const TupleExpr&>(*e);
            for (auto& el : t.elements)
                if (exprHasLiveCall(el.get())) return true;
            return false;
        }
        case ExprKind::Try: {
            auto& t = static_cast<const TryExpr&>(*e);
            return exprHasLiveCall(t.try_expr.get()) || exprHasLiveCall(t.catch_expr.get());
        }
        case ExprKind::Pipe: {
            auto& p = static_cast<const PipeExpr&>(*e);
            return exprHasLiveCall(p.lhs.get()) || exprHasLiveCall(p.rhs.get());
        }
        case ExprKind::EnumVariant: {
            auto& ev = static_cast<const EnumVariantExpr&>(*e);
            for (auto& a : ev.args)
                if (exprHasLiveCall(a.get())) return true;
            return false;
        }
        case ExprKind::Lambda:
            return stmtHasLiveCall(static_cast<const LambdaExpr&>(*e).body.get());
        default:
            return false;
    }
}

static bool stmtHasLiveCall(const Stmt* s) {
    if (!s) return false;
    switch (s->kind) {
        case StmtKind::Block: {
            auto& b = static_cast<const BlockStmt&>(*s);
            for (auto& st : b.statements)
                if (stmtHasLiveCall(st.get())) return true;
            return false;
        }
        case StmtKind::VarDeclStmt: {
            auto& vd = static_cast<const VarDeclStmt&>(*s);
            for (auto& d : vd.decls)
                if (d.init_expr && exprHasLiveCall(d.init_expr.get())) return true;
            return false;
        }
        case StmtKind::ExprStmt:
            return exprHasLiveCall(static_cast<const ExprStmt&>(*s).expression.get());
        case StmtKind::IfStmt: {
            auto& i = static_cast<const IfStmt&>(*s);
            if (i.condition && exprHasLiveCall(i.condition.get())) return true;
            if (i.then_block && stmtHasLiveCall(i.then_block.get())) return true;
            if (i.else_block && stmtHasLiveCall(i.else_block.get())) return true;
            return false;
        }
        case StmtKind::WhileStmt: {
            auto& w = static_cast<const WhileStmt&>(*s);
            if (w.condition && exprHasLiveCall(w.condition.get())) return true;
            if (w.body && stmtHasLiveCall(w.body.get())) return true;
            return false;
        }
        case StmtKind::ForStmt: {
            auto& f = static_cast<const ForStmt&>(*s);
            if (f.init && stmtHasLiveCall(f.init.get())) return true;
            if (f.condition && exprHasLiveCall(f.condition.get())) return true;
            if (f.step && exprHasLiveCall(f.step.get())) return true;
            if (f.body && stmtHasLiveCall(f.body.get())) return true;
            return false;
        }
        case StmtKind::ForInStmt: {
            auto& f = static_cast<const ForInStmt&>(*s);
            if (f.iterable && exprHasLiveCall(f.iterable.get())) return true;
            if (f.body && stmtHasLiveCall(f.body.get())) return true;
            return false;
        }
        case StmtKind::ReturnStmt: {
            auto& r = static_cast<const ReturnStmt&>(*s);
            return r.value ? exprHasLiveCall(r.value.get()) : false;
        }
        case StmtKind::TryStmt: {
            auto& t = static_cast<const TryStmt&>(*s);
            if (t.try_block && stmtHasLiveCall(t.try_block.get())) return true;
            for (auto& c : t.catches)
                if (c.block && stmtHasLiveCall(c.block.get())) return true;
            if (t.finally_block && stmtHasLiveCall(t.finally_block.get())) return true;
            return false;
        }
        case StmtKind::ThrowStmt:
            return exprHasLiveCall(static_cast<const ThrowStmt&>(*s).expr.get());
        case StmtKind::AwaitStmt: {
            auto& a = static_cast<const AwaitStmt&>(*s);
            if (a.expr && exprHasLiveCall(a.expr.get())) return true;
            if (a.timeout && exprHasLiveCall(a.timeout.get())) return true;
            return false;
        }
        case StmtKind::MatchStmt: {
            auto& m = static_cast<const MatchStmt&>(*s);
            if (m.subject && exprHasLiveCall(m.subject.get())) return true;
            for (auto& arm : m.arms)
                if (arm.body && stmtHasLiveCall(arm.body.get())) return true;
            return false;
        }
        default:
            return false;
    }
}

// ---- 主入口 ----
std::set<std::string> analyzeEscapeStackVars(const BlockStmt* body) {
    std::set<std::string> result;
    if (!body) return result;
    // 分配内省函数（Memory.liveObjectCount[ByType]）→ 不栈上化（见上）。
    if (stmtHasLiveCall(body)) return result;
    // 1. 收集候选（VarDecl + NewExpr 初始化）
    std::map<std::string, const VarDecl*> candidates;
    collectCandidates(body, candidates);
    if (candidates.empty()) return result;
    // 2. 对每个候选，遍历整个函数体检查是否逃逸
    for (auto& kv : candidates) {
        if (!stmtEscapes(body, kv.first)) result.insert(kv.first);
    }
    return result;
}

}  // namespace mylang
