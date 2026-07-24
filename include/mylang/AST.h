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
    std::string class_name;   // non-empty if user-defined type
    std::shared_ptr<TypeNode> element_type; // non-null if array type
    int array_size = 0;       // >0 if fixed-size array like Type[10]
    SourceRange range;

    TypeNode() = default;
    TypeNode(const TypeNode& other)
        : basic_type(other.basic_type), class_name(other.class_name),
          range(other.range), array_size(other.array_size),
          is_inferred(other.is_inferred),
          element_type(other.element_type ? std::make_shared<TypeNode>(*other.element_type) : nullptr) {}
    TypeNode& operator=(const TypeNode& other) {
        if (this != &other) {
            basic_type = other.basic_type;
            class_name = other.class_name;
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
    SourceRange range;
};

struct VarDecl {
    std::string name;
    TypeNode type;
    std::shared_ptr<Expr> init_expr;
    SourceRange range;
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
    std::vector<ActionDecl> actions;
    std::vector<ActionDecl> static_actions; // static: section
    std::vector<EventDecl> events;
    std::vector<PropertyDecl> properties;
    std::vector<FuncDecl> functions; // internal methods (function: section)
    std::vector<StructDecl> structs; // nested structs
    bool is_static = false; // class has only static: methods
    std::string interface_class_name; // non-empty if "interface class X;" declared
    SourceRange range;
};

struct InterfaceDecl {
    std::string name;
    std::vector<ActionDecl> actions;
    std::vector<EventDecl> events;
    SourceRange range;
};

// ---- Mapping ----
struct MappingNode {
    std::string source_name;  // instance or class name
    std::string member_name;  // event or action name
    SourceRange range;
};

struct MappingChain {
    std::vector<MappingNode> nodes;
    SourceRange range;
};

struct MappingDecl {
    std::vector<MappingChain> chains;
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
    ThisExpr,
    Assignment,
    Ternary,
    Range,
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
    IntegerLiteralExpr(int64_t v, SourceRange r)
        : Expr(ExprKind::IntegerLiteral, r), value(v) {}
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
};

struct BinaryOpExpr : Expr {
    std::unique_ptr<Expr> lhs;
    BinaryOpKind op;
    std::unique_ptr<Expr> rhs;
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
    std::vector<std::unique_ptr<Expr>> args;
    NewExpr(std::string cn, std::vector<std::unique_ptr<Expr>> a, SourceRange range_)
        : Expr(ExprKind::NewExpr, range_), class_name(std::move(cn)), args(std::move(a)) {}
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
    MappingStmt,
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

struct VarDeclStmt : Stmt {
    VarDecl decl;
    VarDeclStmt(VarDecl d, SourceRange r)
        : Stmt(StmtKind::VarDeclStmt, r), decl(std::move(d)) {}
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
    std::unique_ptr<VarDeclStmt> init;
    std::unique_ptr<Expr> condition;
    std::unique_ptr<Expr> step;
    std::unique_ptr<Stmt> body;
    ForStmt(std::unique_ptr<VarDeclStmt> i, std::unique_ptr<Expr> cond,
            std::unique_ptr<Expr> s, std::unique_ptr<Stmt> b, SourceRange r)
        : Stmt(StmtKind::ForStmt, r), init(std::move(i)), condition(std::move(cond)),
          step(std::move(s)), body(std::move(b)) {}
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

struct MappingStmt : Stmt {
    MappingDecl decl;
    MappingStmt(MappingDecl d, SourceRange r)
        : Stmt(StmtKind::MappingStmt, r), decl(std::move(d)) {}
};

// ---- Top-level Declaration ----
struct FuncDecl {
    std::string name;
    TypeNode return_type;
    std::vector<ParamDecl> params;
    std::unique_ptr<BlockStmt> body;
    SourceRange range;
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
};

} // namespace mylang

#endif // MYLANG_AST_H
