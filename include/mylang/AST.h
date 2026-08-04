#ifndef MYLANG_AST_H
#define MYLANG_AST_H

#include "SourceLocation.h"
#include "Token.h"

#include <memory>
#include <string>
#include <vector>

namespace mylang {

// Forward declarations
class Expr;
class Stmt;
class Decl;
struct FuncDecl;

// ---- Types ----
enum class BuiltinType {
    Byte, Short, Int, Long,
    UByte, UShort, UInt, ULong,
    Char, Float, Double, Bool, String, Void,
};

struct TypeNode {
    BuiltinType basic_type = BuiltinType::Int;
    std::string class_name;
    std::vector<TypeNode> type_args; // generic arguments
    bool is_generic_param = false;   // true if this is a type param reference   // non-empty if user-defined type
    std::shared_ptr<TypeNode> element_type; // non-null if array type
    int array_size = 0;       // >0 if fixed-size array like Type[10]
    SourceRange range;

    TypeNode() = default;
    TypeNode(const TypeNode& other)
        : basic_type(other.basic_type), class_name(other.class_name),
          type_args(other.type_args), is_generic_param(other.is_generic_param),
          range(other.range), array_size(other.array_size),
          is_inferred(other.is_inferred),
          element_type(other.element_type ? std::make_shared<TypeNode>(*other.element_type) : nullptr) {}
    TypeNode& operator=(const TypeNode& other) {
        if (this != &other) {
            basic_type = other.basic_type;
            class_name = other.class_name;
            type_args = other.type_args;
            is_generic_param = other.is_generic_param;
            range = other.range;
            array_size = other.array_size;
            is_inferred = other.is_inferred;
            element_type = other.element_type ? std::make_shared<TypeNode>(*other.element_type) : nullptr;
        }
        return *this;
    }

    bool isArray() const { return element_type != nullptr; }
    bool isClass() const { return !class_name.empty(); }
    bool is_inferred = false; // true if declared with 'var'
};

// ---- Declarations ----
struct ParamDecl {
    std::string name;
    TypeNode type;
    bool is_ref = false;
    SourceRange range;
};

struct VarDecl {
    std::string name;
    TypeNode type;
    std::shared_ptr<Expr> init_expr;
    SourceRange range;
    bool is_const = false;
    bool has_thread_annotation = false;
    bool has_threadpool_annotation = false;
};

struct ActionDecl {
    std::string name;
    TypeNode return_type;
    std::vector<ParamDecl> params;
    std::shared_ptr<Stmt> body;  // nullptr if declaration only
    SourceRange range;
    bool has_startup = false;
    bool has_test = false;
    bool has_coro = false;
    bool has_region = false;  // @region: 调用作用域为内存 region（自动回收）
};

struct EventDecl {
    std::string name;
    std::vector<ParamDecl> params;
    SourceRange range;
};

struct PropertyDecl {
    PropertyDecl() = default;
    PropertyDecl(PropertyDecl&&) = default;
    PropertyDecl& operator=(PropertyDecl&&) = default;

    std::string name;
    TypeNode type;
    std::shared_ptr<Expr> init_expr;
    SourceRange range;
    bool is_const = false;
};

// ---- Struct ----
struct StructDecl {
    std::string name;
    std::string parent_class; // non-empty if nested inside a class
    std::vector<PropertyDecl> properties;
    std::vector<FuncDecl> functions; // struct methods
    SourceRange range;
};

// ---- Class ----
struct ClassDecl {
    std::string name;
    std::vector<std::string> type_params; // generic type params
    std::vector<ActionDecl> actions;
    std::vector<ActionDecl> static_actions; // static: section
    std::vector<EventDecl> events;
    std::vector<PropertyDecl> properties;
    std::vector<FuncDecl> functions; // internal methods (function: section)
    std::vector<StructDecl> structs; // nested structs
    bool is_static = false; // class has only static: methods
    bool is_generic_inst = false; // monomorphized instance of a generic template
    std::string interface_class_name; // non-empty if "interface class X;" declared
    SourceRange range;
};

struct InterfaceDecl {
    std::string name;
    std::vector<ActionDecl> actions;
    std::vector<EventDecl> events;
    SourceRange range;
};

// ---- Enum ----
struct EnumVariant {
    std::string name;
    std::vector<ParamDecl> params; // data fields (empty for simple variants)
    SourceRange range;
};

struct EnumDecl {
    std::string name;
    std::vector<EnumVariant> variants;
    SourceRange range;
};

// ---- FFI ----
struct FFIDecl {
    std::string name;
    TypeNode return_type;
    std::vector<ParamDecl> params;
    SourceRange range;
};

// ---- Mapping ----
struct LambdaExpr; // forward decl for MappingNode
struct MappingNode {
    std::string source_name;  // instance or class name, or function name if is_function
    std::string member_name;  // event or action name (empty if is_function)
    bool is_function = false; // true if this node is a file-level function
    bool is_lambda = false;   // true if this node is a lambda expression
    std::shared_ptr<LambdaExpr> lambda; // non-null if is_lambda
    bool is_transformer = false; // true if this is delay(ms)/throttle(ms)
    int transformer_kind = 0;    // 0=delay, 1=throttle
    int transformer_param = 0;   // ms value
    SourceRange range;

    MappingNode() = default;
    MappingNode(const MappingNode& other)
        : source_name(other.source_name), member_name(other.member_name),
          is_function(other.is_function), is_lambda(other.is_lambda),
          lambda(other.lambda), is_transformer(other.is_transformer),
          transformer_kind(other.transformer_kind),
          transformer_param(other.transformer_param),
          range(other.range) {}
    MappingNode& operator=(const MappingNode& other) {
        source_name = other.source_name;
        member_name = other.member_name;
        is_function = other.is_function;
        is_lambda = other.is_lambda;
        lambda = other.lambda;
        is_transformer = other.is_transformer;
        transformer_kind = other.transformer_kind;
        transformer_param = other.transformer_param;
        range = other.range;
        return *this;
    }
};

struct MappingChain {
    std::vector<MappingNode> nodes;
    std::shared_ptr<Expr> where_expr; // non-null if 'where expr' clause
    SourceRange range;
};

struct MappingDecl {
    std::vector<MappingChain> chains;
    bool has_scope = false;  // true if @scope annotation: auto-unregister on scope exit
    SourceRange range;
};

// ---- Expressions ----
enum class ExprKind {
    IntegerLiteral,
    FloatLiteral,
    BoolLiteral,
    StringLiteral,
    NullLiteral,
    Identifier,
    BinaryOp,
    UnaryOp,
    Call,
    MemberAccess,
    Subscript,
    NewExpr,
    NewArrayExpr,
    ThisExpr,
    Assignment,
    Ternary,
    Range,
    EnumVariant,
    Lambda,
    Pipe,
};

struct Expr {
    ExprKind kind;
    SourceRange range;
    TypeNode* type = nullptr;  // resolved during sema

    Expr(ExprKind k, SourceRange r) : kind(k), range(r) {}
    virtual ~Expr() = default;
};

struct IntegerLiteralExpr : Expr {
    int64_t value;
    bool is_long;  // L suffix: 42L → long
    IntegerLiteralExpr(int64_t v, SourceRange r, bool l = false)
        : Expr(ExprKind::IntegerLiteral, r), value(v), is_long(l) {}
};

struct FloatLiteralExpr : Expr {
    double value;
    FloatLiteralExpr(double v, SourceRange r)
        : Expr(ExprKind::FloatLiteral, r), value(v) {}
};

struct BoolLiteralExpr : Expr {
    bool value;
    BoolLiteralExpr(bool v, SourceRange r)
        : Expr(ExprKind::BoolLiteral, r), value(v) {}
};

struct StringLiteralExpr : Expr {
    std::string value;
    StringLiteralExpr(std::string v, SourceRange r)
        : Expr(ExprKind::StringLiteral, r), value(std::move(v)) {}
};

struct NullLiteralExpr : Expr {
    NullLiteralExpr(SourceRange r)
        : Expr(ExprKind::NullLiteral, r) {}
};

struct IdentifierExpr : Expr {
    std::string name;
    IdentifierExpr(std::string n, SourceRange r)
        : Expr(ExprKind::Identifier, r), name(std::move(n)) {}
};

enum class BinaryOpKind {
    Add, Sub, Mul, Div, Mod,
    Eq, Ne, Lt, Gt, Le, Ge,
    And, Or,
    BitAnd, BitOr, BitXor, Shl, Shr,
};

// Operator overloading resolution result (set by Sema, consumed by CodeGen).
// Non-null means this binary op dispatches to a user-defined operator.
//   kind == "struct_method": call struct_<struct_key>_<method>(lhs, rhs...)
//   kind == "function"     : call top-level function <func_name>(lhs, rhs)
struct OperatorCall {
    std::string kind;       // "struct_method" | "function"
    std::string struct_key; // struct type key (for struct_method)
    std::string method;     // method name (for struct_method)
    std::string func_name;  // function name (for function)
};

struct BinaryOpExpr : Expr {
    std::unique_ptr<Expr> lhs;
    BinaryOpKind op;
    std::unique_ptr<Expr> rhs;
    std::shared_ptr<OperatorCall> op_call;  // set by Sema if operator overloaded
    BinaryOpExpr(std::unique_ptr<Expr> l, BinaryOpKind o, std::unique_ptr<Expr> r, SourceRange range_)
        : Expr(ExprKind::BinaryOp, range_), lhs(std::move(l)), op(o), rhs(std::move(r)) {}
};

enum class UnaryOpKind {
    Negate, Not,
};

struct UnaryOpExpr : Expr {
    UnaryOpKind op;
    std::unique_ptr<Expr> operand;
    UnaryOpExpr(UnaryOpKind o, std::unique_ptr<Expr> opnd, SourceRange range_)
        : Expr(ExprKind::UnaryOp, range_), op(o), operand(std::move(opnd)) {}
};

struct CallExpr : Expr {
    std::unique_ptr<Expr> callee;
    std::vector<std::unique_ptr<Expr>> args;
    CallExpr(std::unique_ptr<Expr> c, std::vector<std::unique_ptr<Expr>> a, SourceRange range_)
        : Expr(ExprKind::Call, range_), callee(std::move(c)), args(std::move(a)) {}
};

struct MemberAccessExpr : Expr {
    std::unique_ptr<Expr> object;
    std::string member_name;
    MemberAccessExpr(std::unique_ptr<Expr> obj, std::string mem, SourceRange range_)
        : Expr(ExprKind::MemberAccess, range_), object(std::move(obj)), member_name(std::move(mem)) {}
};

struct SubscriptExpr : Expr {
    std::unique_ptr<Expr> array;
    std::unique_ptr<Expr> index;
    SubscriptExpr(std::unique_ptr<Expr> arr, std::unique_ptr<Expr> idx, SourceRange range_)
        : Expr(ExprKind::Subscript, range_), array(std::move(arr)), index(std::move(idx)) {}
};

struct NewExpr : Expr {
    std::string class_name;
    std::vector<TypeNode> type_args;
    std::vector<std::unique_ptr<Expr>> args;
    NewExpr(std::string cn, std::vector<TypeNode> ta, std::vector<std::unique_ptr<Expr>> a, SourceRange range_)
        : Expr(ExprKind::NewExpr, range_), class_name(std::move(cn)), type_args(std::move(ta)), args(std::move(a)) {}
};

// new double[n] or new double[nx][ny][nz]
struct NewArrayExpr : Expr {
    TypeNode element_type;
    std::vector<std::unique_ptr<Expr>> dimensions;  // [n], [ny], [nz]
    NewArrayExpr(TypeNode et, std::vector<std::unique_ptr<Expr>> dims, SourceRange range_)
        : Expr(ExprKind::NewArrayExpr, range_), element_type(std::move(et)), dimensions(std::move(dims)) {}
};

struct ThisExpr : Expr {
    ThisExpr(SourceRange r)
        : Expr(ExprKind::ThisExpr, r) {}
};

struct AssignmentExpr : Expr {
    std::unique_ptr<Expr> target;
    std::unique_ptr<Expr> value;
    AssignmentExpr(std::unique_ptr<Expr> t, std::unique_ptr<Expr> v, SourceRange range_)
        : Expr(ExprKind::Assignment, range_), target(std::move(t)), value(std::move(v)) {}
};

struct TernaryExpr : Expr {
    std::unique_ptr<Expr> condition;
    std::unique_ptr<Expr> true_expr;
    std::unique_ptr<Expr> false_expr;
    TernaryExpr(std::unique_ptr<Expr> c, std::unique_ptr<Expr> t,
                std::unique_ptr<Expr> f, SourceRange range_)
        : Expr(ExprKind::Ternary, range_),
          condition(std::move(c)), true_expr(std::move(t)), false_expr(std::move(f)) {}
};

struct RangeExpr : Expr {
    std::unique_ptr<Expr> start;
    std::unique_ptr<Expr> end;
    RangeExpr(std::unique_ptr<Expr> s, std::unique_ptr<Expr> e, SourceRange range_)
        : Expr(ExprKind::Range, range_), start(std::move(s)), end(std::move(e)) {}
};

struct LambdaExpr : Expr {
    std::vector<ParamDecl> params;
    std::shared_ptr<Stmt> body;
    std::string hidden_class_name;
    LambdaExpr(std::vector<ParamDecl> p, std::shared_ptr<Stmt> b, SourceRange r)
        : Expr(ExprKind::Lambda, r), params(std::move(p)), body(std::move(b)) {}
};

// Pipeline: lhs |> op  — applies an operator component to lhs (left-assoc).
// Semantics: A |> Op1 |> Op2 == Op2.transform(Op1.transform(A)).
// Resolved by Sema: target_kind="class" (rhs is a class name, instantiate) or
// "instance" (rhs is an operator instance). The called method is `transform`.
struct PipeExpr : Expr {
    std::unique_ptr<Expr> lhs;
    std::unique_ptr<Expr> rhs;      // operator: class name or instance expression
    std::string target_kind;        // "class" | "instance" (set by Sema)
    std::string class_name;         // class owning the transform method
    std::string method = "transform";
    PipeExpr(std::unique_ptr<Expr> l, std::unique_ptr<Expr> r, SourceRange range_)
        : Expr(ExprKind::Pipe, range_), lhs(std::move(l)), rhs(std::move(r)) {}
};

struct EnumVariantExpr : Expr {
    std::string enum_name;
    int variant_index;
    std::vector<std::unique_ptr<Expr>> args;
    EnumVariantExpr(std::string en, int vi, std::vector<std::unique_ptr<Expr>> a, SourceRange r)
        : Expr(ExprKind::EnumVariant, r), enum_name(std::move(en)), variant_index(vi), args(std::move(a)) {}
};

// ---- Statements ----
enum class StmtKind {
    Block,
    VarDeclStmt,
    ExprStmt,
    IfStmt,
    WhileStmt,
    ForStmt,
    ReturnStmt,
    BreakStmt,
    ContinueStmt,
    AwaitStmt,
    MappingStmt,
    MatchStmt,
    TryStmt,
};

struct Stmt {
    StmtKind kind;
    SourceRange range;
    Stmt(StmtKind k, SourceRange r) : kind(k), range(r) {}
    virtual ~Stmt() = default;
};

struct BlockStmt : Stmt {
    std::vector<std::unique_ptr<Stmt>> statements;
    BlockStmt(std::vector<std::unique_ptr<Stmt>> stmts, SourceRange r)
        : Stmt(StmtKind::Block, r), statements(std::move(stmts)) {}
};

struct TryStmt : Stmt {
    std::unique_ptr<BlockStmt> try_block;
    std::string catch_var_name;
    std::string catch_var_type;
    std::unique_ptr<BlockStmt> catch_block;
    std::unique_ptr<BlockStmt> finally_block;
    TryStmt(std::unique_ptr<BlockStmt> tb, const std::string& cvn,
            const std::string& cvt, std::unique_ptr<BlockStmt> cb,
            std::unique_ptr<BlockStmt> fb, SourceRange r)
        : Stmt(StmtKind::TryStmt, r), try_block(std::move(tb)),
          catch_var_name(cvn), catch_var_type(cvt),
          catch_block(std::move(cb)), finally_block(std::move(fb)) {}
};

struct VarDeclStmt : Stmt {
    std::vector<VarDecl> decls;
    VarDeclStmt() : Stmt(StmtKind::VarDeclStmt, {}) {}
    explicit VarDeclStmt(VarDecl d)
        : Stmt(StmtKind::VarDeclStmt, d.range) { decls.push_back(std::move(d)); }
    VarDeclStmt(std::vector<VarDecl> d, SourceRange r)
        : Stmt(StmtKind::VarDeclStmt, r), decls(std::move(d)) {}
};

struct ExprStmt : Stmt {
    std::unique_ptr<Expr> expression;
    ExprStmt(std::unique_ptr<Expr> e, SourceRange r)
        : Stmt(StmtKind::ExprStmt, r), expression(std::move(e)) {}
};

struct IfStmt : Stmt {
    std::unique_ptr<Expr> condition;
    std::unique_ptr<Stmt> then_block;
    std::unique_ptr<Stmt> else_block;
    IfStmt(std::unique_ptr<Expr> cond, std::unique_ptr<Stmt> then_s,
           std::unique_ptr<Stmt> else_s, SourceRange r)
        : Stmt(StmtKind::IfStmt, r), condition(std::move(cond)),
          then_block(std::move(then_s)), else_block(std::move(else_s)) {}
};

struct WhileStmt : Stmt {
    std::unique_ptr<Expr> condition;
    std::unique_ptr<Stmt> body;
    WhileStmt(std::unique_ptr<Expr> cond, std::unique_ptr<Stmt> b, SourceRange r)
        : Stmt(StmtKind::WhileStmt, r), condition(std::move(cond)), body(std::move(b)) {}
};

struct ForStmt : Stmt {
    std::unique_ptr<Stmt> init;
    std::unique_ptr<Expr> condition;
    std::unique_ptr<Expr> step;
    std::unique_ptr<Stmt> body;
    bool parallel;  // @parallel for
    bool gpu;       // @gpu for
    ForStmt(std::unique_ptr<Stmt> i, std::unique_ptr<Expr> cond,
            std::unique_ptr<Expr> s, std::unique_ptr<Stmt> b, SourceRange r, bool par = false, bool g = false)
        : Stmt(StmtKind::ForStmt, r), init(std::move(i)), condition(std::move(cond)),
          step(std::move(s)), body(std::move(b)), parallel(par), gpu(g) {}
};

struct ReturnStmt : Stmt {
    std::unique_ptr<Expr> value;
    ReturnStmt(std::unique_ptr<Expr> v, SourceRange r)
        : Stmt(StmtKind::ReturnStmt, r), value(std::move(v)) {}
};

struct BreakStmt : Stmt {
    BreakStmt(SourceRange r) : Stmt(StmtKind::BreakStmt, r) {}
};

struct ContinueStmt : Stmt {
    ContinueStmt(SourceRange r) : Stmt(StmtKind::ContinueStmt, r) {}
};

struct AwaitStmt : Stmt {
    std::unique_ptr<Expr> expr;
    AwaitStmt(std::unique_ptr<Expr> e, SourceRange r)
        : Stmt(StmtKind::AwaitStmt, r), expr(std::move(e)) {}
};

struct MappingStmt : Stmt {
    MappingDecl decl;
    MappingStmt(MappingDecl d, SourceRange r)
        : Stmt(StmtKind::MappingStmt, r), decl(std::move(d)) {}
};

struct MatchArm {
    std::string enum_name;
    std::string variant_name;  // name of the variant (used during Sema resolution)
    int variant_index;
    std::vector<std::string> bindings; // variable names for destructured data
    std::shared_ptr<Stmt> body;
    SourceRange range;
};

struct MatchStmt : Stmt {
    std::unique_ptr<Expr> subject;
    std::vector<MatchArm> arms;
    MatchStmt(std::unique_ptr<Expr> s, std::vector<MatchArm> a, SourceRange r)
        : Stmt(StmtKind::MatchStmt, r), subject(std::move(s)), arms(std::move(a)) {}
};

// ---- Top-level Declaration ----
struct FuncDecl {
    std::string name;
    TypeNode return_type;
    std::vector<ParamDecl> params;
    std::unique_ptr<BlockStmt> body;
    SourceRange range;
    bool has_test = false;
    bool has_region = false;  // @region: 调用作用域为内存 region（自动回收）
    std::string op_symbol;  // non-empty if this is an operator (@op("..."))
};

struct ImportDecl {
    std::string module_name;  // module name for stdlib imports
    std::string file_path;    // file path for path-based imports (e.g. "./other.myp")
    bool is_path = false;     // true if import uses string path syntax
    SourceRange range;
};

/// Complete translation unit.
struct TranslationUnit {
    std::vector<ImportDecl> imports;
    std::vector<StructDecl> structs;     // file-level structs
    std::vector<ClassDecl> classes;
    std::vector<InterfaceDecl> interfaces;
    std::vector<MappingDecl> mappings;
    std::vector<FuncDecl> functions;
    std::vector<EnumDecl> enums;
    std::vector<FFIDecl> ffis;
};

} // namespace mylang

#endif // MYLANG_AST_H
