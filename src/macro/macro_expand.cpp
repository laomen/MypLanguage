// M3 — declarative macro expansion.
//
// `macro name($a, $b) { <template body> }` defines a compile-time AST template.
// A call `name(arg1, arg2)` in statement position is expanded by cloning the
// template body and replacing each `$param` MacroParamExpr with a deep copy of
// the corresponding argument AST. Nested macro calls inside the body are
// expanded iteratively (depth-limited). Runs after parsing, before sema.

#include "mylang/Macro.h"
#include "mylang/DiagnosticEngine.h"
#include "mylang/Eval.h"
#include "mylang/Lexer.h"
#include "mylang/Parser.h"
#include "mylang/SourceLocation.h"

#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

namespace mylang {

namespace {

// Captured macro arguments: param name -> argument AST (owned clones).
using MacroArgs = std::unordered_map<std::string, std::unique_ptr<Expr>>;

class MacroExpander {
public:
    MacroExpander(TranslationUnit& tu, DiagnosticEngine& diag)
        : tu_(tu), diag_(diag) {
        for (auto& m : tu_.macros) macros_[m.name] = &m;
        for (auto& fn : tu_.functions)
            if (fn.has_proc_macro) proc_macros_[fn.name] = &fn;
    }

    bool run() {
        for (auto& fn : tu_.functions)
            if (fn.body) expandBlock(*fn.body, 0);
        for (auto& cls : tu_.classes) {
            for (auto& a : cls.actions)
                if (a.body) expandBlock(static_cast<BlockStmt&>(*a.body), 0);
            for (auto& a : cls.static_actions)
                if (a.body) expandBlock(static_cast<BlockStmt&>(*a.body), 0);
            for (auto& f : cls.functions) if (f.body) expandBlock(*f.body, 0);
        }
        return !diag_.hasErrors();
    }

private:
    static constexpr int kMaxDepth = 100;

    TranslationUnit& tu_;
    DiagnosticEngine& diag_;
    std::unordered_map<std::string, MacroDecl*> macros_;
    std::unordered_map<std::string, FuncDecl*> proc_macros_;  // M4 @macro

    static BlockStmt& asBlock(Stmt& s) { return static_cast<BlockStmt&>(s); }

    void error(const SourceRange& r, const std::string& msg) { diag_.error(r, msg); }

    // ---- Deep-clone an expression; substitute $param from args if present ----
    std::unique_ptr<Expr> cloneExpr(const Expr& e, const MacroArgs& args) {
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
        case ExprKind::NullLiteral:
            return std::make_unique<NullLiteralExpr>(e.range);
        case ExprKind::Identifier: {
            auto& v = static_cast<const IdentifierExpr&>(e);
            return std::make_unique<IdentifierExpr>(v.name, v.range);
        }
        case ExprKind::MacroParam: {
            auto& v = static_cast<const MacroParamExpr&>(e);
            auto it = args.find(v.name);
            if (it != args.end()) {
                // Substitute with a deep copy of the captured argument.
                return cloneExpr(*it->second, {});
            }
            error(v.range, "undefined macro parameter '$" + v.name + "'");
            return std::make_unique<IdentifierExpr>("__error__", v.range);
        }
        case ExprKind::BinaryOp: {
            auto& v = static_cast<const BinaryOpExpr&>(e);
            return std::make_unique<BinaryOpExpr>(
                cloneExpr(*v.lhs, args), v.op, cloneExpr(*v.rhs, args), v.range);
        }
        case ExprKind::UnaryOp: {
            auto& v = static_cast<const UnaryOpExpr&>(e);
            return std::make_unique<UnaryOpExpr>(v.op, cloneExpr(*v.operand, args), v.range);
        }
        case ExprKind::Convert: {
            auto& v = static_cast<const ConvertExpr&>(e);
            return std::make_unique<ConvertExpr>(v.to_kind, cloneExpr(*v.operand, args), v.range);
        }
        case ExprKind::Call: {
            auto& v = static_cast<const CallExpr&>(e);
            std::vector<std::unique_ptr<Expr>> a;
            for (auto& arg : v.args) a.push_back(cloneExpr(*arg, args));
            return std::make_unique<CallExpr>(cloneExpr(*v.callee, args), std::move(a), v.range);
        }
        case ExprKind::MemberAccess: {
            auto& v = static_cast<const MemberAccessExpr&>(e);
            return std::make_unique<MemberAccessExpr>(cloneExpr(*v.object, args), v.member_name, v.range);
        }
        case ExprKind::Subscript: {
            auto& v = static_cast<const SubscriptExpr&>(e);
            return std::make_unique<SubscriptExpr>(cloneExpr(*v.array, args), cloneExpr(*v.index, args), v.range);
        }
        case ExprKind::NewExpr: {
            auto& v = static_cast<const NewExpr&>(e);
            std::vector<std::unique_ptr<Expr>> a;
            for (auto& arg : v.args) a.push_back(cloneExpr(*arg, args));
            return std::make_unique<NewExpr>(v.class_name, v.type_args, std::move(a), v.range);
        }
        case ExprKind::NewArrayExpr: {
            auto& v = static_cast<const NewArrayExpr&>(e);
            std::vector<std::unique_ptr<Expr>> d;
            for (auto& dim : v.dimensions) d.push_back(cloneExpr(*dim, args));
            return std::make_unique<NewArrayExpr>(v.element_type, std::move(d), v.range);
        }
        case ExprKind::ThisExpr:
            return std::make_unique<ThisExpr>(e.range);
        case ExprKind::Assignment: {
            auto& v = static_cast<const AssignmentExpr&>(e);
            return std::make_unique<AssignmentExpr>(cloneExpr(*v.target, args), cloneExpr(*v.value, args), v.range);
        }
        case ExprKind::Ternary: {
            auto& v = static_cast<const TernaryExpr&>(e);
            return std::make_unique<TernaryExpr>(
                cloneExpr(*v.condition, args), cloneExpr(*v.true_expr, args),
                cloneExpr(*v.false_expr, args), v.range);
        }
        case ExprKind::Range: {
            auto& v = static_cast<const RangeExpr&>(e);
            return std::make_unique<RangeExpr>(cloneExpr(*v.start, args), cloneExpr(*v.end, args), v.range);
        }
        case ExprKind::NamedArg: {
            auto& v = static_cast<const NamedArgExpr&>(e);
            return std::make_unique<NamedArgExpr>(v.name, cloneExpr(*v.value, args), v.range);
        }
        case ExprKind::EnumVariant: {
            auto& v = static_cast<const EnumVariantExpr&>(e);
            std::vector<std::unique_ptr<Expr>> a;
            for (auto& arg : v.args) a.push_back(cloneExpr(*arg, args));
            return std::make_unique<EnumVariantExpr>(v.enum_name, v.variant_index, std::move(a), v.range);
        }
        case ExprKind::Try: {
            auto& v = static_cast<const TryExpr&>(e);
            return std::make_unique<TryExpr>(cloneExpr(*v.try_expr, args), v.catch_var_name,
                                             cloneExpr(*v.catch_expr, args), v.range);
        }
        case ExprKind::Lambda:
        case ExprKind::Pipe:
        case ExprKind::Await:
        default:
            error(e.range, "macro body: expression construct not supported in macro expansion");
            return std::make_unique<IdentifierExpr>("__error__", e.range);
        }
    }

    // ---- Deep-clone a statement; substitute $param from args if present ----
    std::unique_ptr<Stmt> cloneStmt(const Stmt& s, const MacroArgs& args) {
        switch (s.kind) {
        case StmtKind::Block: {
            auto& v = static_cast<const BlockStmt&>(s);
            std::vector<std::unique_ptr<Stmt>> stmts;
            for (auto& st : v.statements) stmts.push_back(cloneStmt(*st, args));
            return std::make_unique<BlockStmt>(std::move(stmts), v.range);
        }
        case StmtKind::VarDeclStmt: {
            auto& v = static_cast<const VarDeclStmt&>(s);
            std::vector<VarDecl> decls;
            for (auto& d : v.decls) {
                VarDecl nd = d;
                if (d.init_expr) nd.init_expr = cloneExpr(*d.init_expr, args);
                decls.push_back(std::move(nd));
            }
            return std::make_unique<VarDeclStmt>(std::move(decls), v.range);
        }
        case StmtKind::ExprStmt: {
            auto& v = static_cast<const ExprStmt&>(s);
            return std::make_unique<ExprStmt>(cloneExpr(*v.expression, args), v.range);
        }
        case StmtKind::IfStmt: {
            auto& v = static_cast<const IfStmt&>(s);
            return std::make_unique<IfStmt>(
                cloneExpr(*v.condition, args),
                v.then_block ? cloneStmt(*v.then_block, args) : nullptr,
                v.else_block ? cloneStmt(*v.else_block, args) : nullptr, v.range);
        }
        case StmtKind::WhileStmt: {
            auto& v = static_cast<const WhileStmt&>(s);
            return std::make_unique<WhileStmt>(cloneExpr(*v.condition, args),
                                               v.body ? cloneStmt(*v.body, args) : nullptr, v.range);
        }
        case StmtKind::ForStmt: {
            auto& v = static_cast<const ForStmt&>(s);
            return std::make_unique<ForStmt>(
                v.init ? cloneStmt(*v.init, args) : nullptr,
                v.condition ? cloneExpr(*v.condition, args) : nullptr,
                v.step ? cloneExpr(*v.step, args) : nullptr,
                v.body ? cloneStmt(*v.body, args) : nullptr, v.range, v.parallel, v.gpu, v.resident);
        }
        case StmtKind::ForInStmt: {
            auto& v = static_cast<const ForInStmt&>(s);
            return std::make_unique<ForInStmt>(
                v.var_name, v.var_type, v.has_type,
                v.iterable ? cloneExpr(*v.iterable, args) : nullptr,
                v.body ? cloneStmt(*v.body, args) : nullptr, v.range);
        }
        case StmtKind::ReturnStmt: {
            auto& v = static_cast<const ReturnStmt&>(s);
            return std::make_unique<ReturnStmt>(
                v.value ? cloneExpr(*v.value, args) : nullptr, v.range);
        }
        case StmtKind::BreakStmt:
            return std::make_unique<BreakStmt>(s.range);
        case StmtKind::ContinueStmt:
            return std::make_unique<ContinueStmt>(s.range);
        case StmtKind::AwaitStmt: {
            auto& v = static_cast<const AwaitStmt&>(s);
            return std::make_unique<AwaitStmt>(
                v.expr ? cloneExpr(*v.expr, args) : nullptr,
                v.timeout ? cloneExpr(*v.timeout, args) : nullptr, v.range);
        }
        case StmtKind::TryStmt: {
            auto& v = static_cast<const TryStmt&>(s);
            auto tb = v.try_block ? cloneStmt(*v.try_block, args) : nullptr;
            std::vector<CatchClause> cs;
            for (auto& c : v.catches) {
                CatchClause nc;
                nc.var_name = c.var_name;
                nc.var_type = c.var_type;
                if (c.block) nc.block = std::unique_ptr<BlockStmt>(
                    static_cast<BlockStmt*>(cloneStmt(*c.block, args).release()));
                cs.push_back(std::move(nc));
            }
            auto fb = v.finally_block ? cloneStmt(*v.finally_block, args) : nullptr;
            return std::make_unique<TryStmt>(
                tb ? std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(tb.release())) : nullptr,
                std::move(cs),
                fb ? std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(fb.release())) : nullptr,
                v.range);
        }
        case StmtKind::ThrowStmt: {
            auto& v = static_cast<const ThrowStmt&>(s);
            return std::make_unique<ThrowStmt>(cloneExpr(*v.expr, args), v.range);
        }
        case StmtKind::MappingStmt:
        case StmtKind::MatchStmt:
        default:
            error(s.range, "macro body: statement construct not supported in macro expansion");
            return std::make_unique<BlockStmt>(std::vector<std::unique_ptr<Stmt>>{}, s.range);
        }
    }

    // ---- Recursively expand macro calls inside nested statements ----
    void expandNested(Stmt& s, int depth) {
        switch (s.kind) {
        case StmtKind::Block: expandBlock(static_cast<BlockStmt&>(s), depth); break;
        case StmtKind::IfStmt: {
            auto& v = static_cast<IfStmt&>(s);
            if (v.then_block) expandNested(*v.then_block, depth);
            if (v.else_block) expandNested(*v.else_block, depth);
            break;
        }
        case StmtKind::WhileStmt: {
            auto& v = static_cast<WhileStmt&>(s);
            if (v.body) expandNested(*v.body, depth);
            break;
        }
        case StmtKind::ForStmt: {
            auto& v = static_cast<ForStmt&>(s);
            if (v.init) expandNested(*v.init, depth);
            if (v.body) expandNested(*v.body, depth);
            break;
        }
        case StmtKind::ForInStmt: {
            auto& v = static_cast<ForInStmt&>(s);
            if (v.body) expandNested(*v.body, depth);
            break;
        }
        case StmtKind::TryStmt: {
            auto& v = static_cast<TryStmt&>(s);
            if (v.try_block) expandBlock(*v.try_block, depth);
            for (auto& c : v.catches) if (c.block) expandBlock(*c.block, depth);
            if (v.finally_block) expandBlock(*v.finally_block, depth);
            break;
        }
        default: break;
        }
    }

    // ---- Expand macro calls in a block; emits expanded statements in place ----
    void expandBlock(BlockStmt& block, int depth) {
        if (depth > kMaxDepth) {
            error(block.range, "macro expansion depth limit exceeded (nested macros?)");
            return;
        }
        std::vector<std::unique_ptr<Stmt>> out;
        for (auto& st : block.statements) {
            auto* es = dynamic_cast<ExprStmt*>(st.get());
            CallExpr* call = es ? dynamic_cast<CallExpr*>(es->expression.get()) : nullptr;
            IdentifierExpr* callee =
                call && call->callee && call->callee->kind == ExprKind::Identifier
                    ? static_cast<IdentifierExpr*>(call->callee.get()) : nullptr;
            if (callee) {
                auto mit = macros_.find(callee->name);
                if (mit != macros_.end()) {
                    MacroDecl& m = *mit->second;
                    if (call->args.size() != m.params.size()) {
                        error(call->range, "macro '" + m.name + "' expects " +
                            std::to_string(m.params.size()) + " argument(s), got " +
                            std::to_string(call->args.size()));
                        continue;
                    }
                    // Build captured args (deep clones of the call arguments).
                    MacroArgs args;
                    for (size_t i = 0; i < m.params.size(); i++)
                        args[m.params[i]] = cloneExpr(*call->args[i], {});
                    // Clone the template body with substitutions applied.
                    auto body = cloneStmt(*m.body, args);
                    auto& blk = static_cast<BlockStmt&>(*body);
                    // Expand nested macros inside the instantiated body.
                    expandBlock(blk, depth + 1);
                    for (auto& s : blk.statements) out.push_back(std::move(s));
                    continue;
                }
                // M4: @macro proc-macro call — execute at compile time.
                auto pit = proc_macros_.find(callee->name);
                if (pit != proc_macros_.end()) {
                    FuncDecl& pfn = *pit->second;
                    std::vector<std::unique_ptr<Stmt>> gen;
                    if (!evalProcMacro(tu_, diag_, pfn, call->args, gen)) continue;
                    // Expand any nested M3 macros inside the generated code.
                    BlockStmt tmp(std::move(gen), call->range);
                    expandBlock(tmp, depth + 1);
                    for (auto& s : tmp.statements) out.push_back(std::move(s));
                    continue;
                }
            }
            expandNested(*st, depth);
            out.push_back(std::move(st));
        }
        block.statements = std::move(out);
    }
};

} // namespace

bool expandMacros(TranslationUnit& tu, DiagnosticEngine& diag) {
    MacroExpander expander(tu, diag);
    return expander.run();
}

// ==============================
// @derive(X) — 类级派生（v1：@derive(Json) → 自动生成 toJson/fromJson）
// ==============================

namespace {

// 属性类型 → JSON 字段的 toJson 表达式片段（MYP 源码文本）与 fromJson 赋值语句。
// 返回 false = 类型不支持（err 填充原因）。
bool jsonFieldExpr(const TypeNode& t, const std::string& name,
                   std::string& to_expr, std::string& from_stmt, std::string& err) {
    if (t.element_type || !t.class_name.empty() || t.is_tuple || t.isFunction()) {
        err = "array/class/struct/tuple/function";
        return false;
    }
    switch (t.basic_type) {
        case BuiltinType::Int:
        case BuiltinType::Long:
        case BuiltinType::Short:
        case BuiltinType::Byte:
        case BuiltinType::UInt:
        case BuiltinType::ULong:
        case BuiltinType::UShort:
        case BuiltinType::UByte:
            to_expr = "\"\\\"" + name + "\\\":\" + " + name;
            from_stmt = name + " = p.getInt(\"" + name + "\");";
            return true;
        case BuiltinType::Double:
        case BuiltinType::Float:
            to_expr = "\"\\\"" + name + "\\\":\" + " + name;
            from_stmt = name + " = p.getDouble(\"" + name + "\");";
            return true;
        case BuiltinType::Bool:
            to_expr = "\"\\\"" + name + "\\\":\" + (" + name + " ? \"true\" : \"false\")";
            from_stmt = name + " = p.getBool(\"" + name + "\") != 0;";
            return true;
        case BuiltinType::String:
            to_expr = "\"\\\"" + name + "\\\":\\\"\" + Json.escape(" + name + ") + \"\\\"\"";
            from_stmt = name + " = p.getString(\"" + name + "\");";
            return true;
        default:
            err = "type not supported";
            return false;
    }
}

} // namespace

bool expandDerives(TranslationUnit& tu, DiagnosticEngine& diag) {
    for (auto& cls : tu.classes) {
        if (cls.derive.empty()) continue;
        if (cls.derive != "Json") {
            diag.error(cls.range, "unsupported derive macro '@derive(" + cls.derive +
                                 ")' (supported: Json)");
            cls.derive.clear();
            continue;
        }
        if (!cls.type_params.empty()) {
            diag.error(cls.range,
                "@derive(Json) on generic classes is not yet supported "
                "(v1 supports non-generic classes)");
            cls.derive.clear();
            continue;
        }

        // 构造合成源码（toJson / fromJson 两个方法，复用既有 MYP parser 生成 AST）。
        std::ostringstream s;
        s << "class __MYP_DERIVE_" << cls.name << " {\n"
          << "    action:\n"
          << "        string toJson() {\n"
          << "            string s = \"{\";\n";
        std::vector<std::string> from_stmts;
        size_t n = cls.properties.size();
        bool bad = false;
        for (size_t i = 0; i < n; ++i) {
            const auto& p = cls.properties[i];
            std::string te, fs, err;
            if (!jsonFieldExpr(p.type, p.name, te, fs, err)) {
                diag.error(p.range, "property '" + p.name + "' of class '" + cls.name +
                                    "' has a type not supported by @derive(Json) "
                                    "(v1: int/long/double/float/bool/string)");
                bad = true;
                break;
            }
            s << "            s = s + " << te;
            if (i + 1 < n) s << " + \",\"";
            s << ";\n";
            from_stmts.push_back(fs);
        }
        if (bad) { cls.derive.clear(); continue; }
        s << "            s = s + \"}\";\n"
          << "            return s;\n"
          << "        }\n"
          << "        void fromJson(string j) {\n"
          << "            Json p = new Json(j);\n";
        for (auto& fs : from_stmts) s << "            " << fs << "\n";
        s << "            p.free();\n"
          << "        }\n"
          << "}\n";

        // 重新 lex + parse 合成源码，抽出两个方法注入目标类。
        SourceManager sm;
        sm.loadString(s.str(), "<@derive>");
        DiagnosticEngine sub_diag(sm);
        Lexer lexer(sm, sub_diag);
        auto toks = lexer.tokenize();
        Parser parser(toks, sub_diag);
        auto sub = parser.parse();
        if (sub_diag.hasErrors() || !sub || sub->classes.empty()) {
            diag.error(cls.range, "@derive(Json) failed to generate methods for class '" +
                                  cls.name + "' (internal error)");
            cls.derive.clear();
            continue;
        }
        auto& sub_cls = sub->classes[0];
        for (auto& a : sub_cls.actions) cls.actions.push_back(std::move(a));
        cls.derive.clear(); // 已展开
    }
    return !diag.hasErrors();
}

// ---- Debug: compact AST dump after expansion (--macro-expand) ----

namespace {

void dumpExpr(const Expr& e, int indent);
void dumpStmt(const Stmt& s, int indent) {
    std::string pad(indent, ' ');
    auto name = [&](const char* n) { std::cout << pad << n << "\n"; };
    switch (s.kind) {
    case StmtKind::Block:
        name("block");
        for (auto& st : static_cast<const BlockStmt&>(s).statements)
            if (st) dumpStmt(*st, indent + 2);
        break;
    case StmtKind::VarDeclStmt:
        name("var-decl");
        for (auto& d : static_cast<const VarDeclStmt&>(s).decls) {
            std::cout << pad << "  " << d.name << "\n";
            if (d.init_expr) dumpExpr(*d.init_expr, indent + 4);
        }
        break;
    case StmtKind::ExprStmt:
        name("expr-stmt");
        if (static_cast<const ExprStmt&>(s).expression)
            dumpExpr(*static_cast<const ExprStmt&>(s).expression, indent + 2);
        break;
    case StmtKind::IfStmt: {
        auto& v = static_cast<const IfStmt&>(s);
        name("if");
        dumpExpr(*v.condition, indent + 2);
        if (v.then_block) dumpStmt(*v.then_block, indent + 2);
        if (v.else_block) dumpStmt(*v.else_block, indent + 2);
        break;
    }
    case StmtKind::WhileStmt: {
        auto& v = static_cast<const WhileStmt&>(s);
        name("while");
        dumpExpr(*v.condition, indent + 2);
        if (v.body) dumpStmt(*v.body, indent + 2);
        break;
    }
    case StmtKind::ForStmt: {
        auto& v = static_cast<const ForStmt&>(s);
        name("for");
        if (v.init) dumpStmt(*v.init, indent + 2);
        if (v.condition) dumpExpr(*v.condition, indent + 2);
        if (v.step) dumpExpr(*v.step, indent + 2);
        if (v.body) dumpStmt(*v.body, indent + 2);
        break;
    }
    case StmtKind::ForInStmt: {
        auto& v = static_cast<const ForInStmt&>(s);
        name("for-in");
        dumpExpr(*v.iterable, indent + 2);
        if (v.body) dumpStmt(*v.body, indent + 2);
        break;
    }
    case StmtKind::ReturnStmt:
        name("return");
        if (static_cast<const ReturnStmt&>(s).value)
            dumpExpr(*static_cast<const ReturnStmt&>(s).value, indent + 2);
        break;
    case StmtKind::BreakStmt: name("break"); break;
    case StmtKind::ContinueStmt: name("continue"); break;
    case StmtKind::ThrowStmt: {
        auto& v = static_cast<const ThrowStmt&>(s);
        name("throw");
        if (v.expr) dumpExpr(*v.expr, indent + 2);
        break;
    }
    default: name("stmt"); break;
    }
}

void dumpExpr(const Expr& e, int indent) {
    std::string pad(indent, ' ');
    auto name = [&](const char* n) { std::cout << pad << n << "\n"; };
    switch (e.kind) {
    case ExprKind::IntegerLiteral:
        std::cout << pad << "int " << static_cast<const IntegerLiteralExpr&>(e).value << "\n";
        break;
    case ExprKind::FloatLiteral:
        std::cout << pad << "double " << static_cast<const FloatLiteralExpr&>(e).value << "\n";
        break;
    case ExprKind::BoolLiteral:
        std::cout << pad << "bool " << (static_cast<const BoolLiteralExpr&>(e).value ? "true" : "false") << "\n";
        break;
    case ExprKind::StringLiteral:
        std::cout << pad << "string \"" << static_cast<const StringLiteralExpr&>(e).value << "\"\n";
        break;
    case ExprKind::Identifier:
        std::cout << pad << "ident " << static_cast<const IdentifierExpr&>(e).name << "\n";
        break;
    case ExprKind::BinaryOp: {
        auto& v = static_cast<const BinaryOpExpr&>(e);
        name("binop");
        dumpExpr(*v.lhs, indent + 2);
        dumpExpr(*v.rhs, indent + 2);
        break;
    }
    case ExprKind::UnaryOp:
        name("unop");
        dumpExpr(*static_cast<const UnaryOpExpr&>(e).operand, indent + 2);
        break;
    case ExprKind::Convert:
        name("convert");
        dumpExpr(*static_cast<const ConvertExpr&>(e).operand, indent + 2);
        break;
    case ExprKind::Call: {
        auto& v = static_cast<const CallExpr&>(e);
        std::cout << pad << "call\n";
        dumpExpr(*v.callee, indent + 2);
        for (auto& a : v.args) dumpExpr(*a, indent + 2);
        break;
    }
    case ExprKind::MemberAccess: {
        auto& v = static_cast<const MemberAccessExpr&>(e);
        std::cout << pad << "member ." << v.member_name << "\n";
        dumpExpr(*v.object, indent + 2);
        break;
    }
    case ExprKind::Assignment:
        name("assign");
        dumpExpr(*static_cast<const AssignmentExpr&>(e).target, indent + 2);
        dumpExpr(*static_cast<const AssignmentExpr&>(e).value, indent + 2);
        break;
    default: name("expr"); break;
    }
}

} // namespace

void dumpMacroExpandedAST(TranslationUnit& tu) {
    for (auto& fn : tu.functions) {
        std::cout << "[function " << fn.name << "]\n";
        if (fn.body) dumpStmt(*fn.body, 0);
    }
    for (auto& cls : tu.classes) {
        for (auto& a : cls.actions) {
            std::cout << "[action " << cls.name << "." << a.name << "]\n";
            if (a.body) dumpStmt(*a.body, 0);
        }
        for (auto& a : cls.static_actions) {
            std::cout << "[static " << cls.name << "." << a.name << "]\n";
            if (a.body) dumpStmt(*a.body, 0);
        }
    }
}

} // namespace mylang
