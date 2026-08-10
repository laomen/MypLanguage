#ifndef MYLANG_AST_H
#define MYLANG_AST_H

#include "SourceLocation.h"
#include "Token.h"
#include "Type.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mylang {

struct Expr;   // 前置声明：ParamDecl::default_expr（定义见本文件 Expr）
struct Stmt;

// Forward declarations
class Expr;
class Stmt;
class Decl;
struct FuncDecl;
struct BlockStmt;

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
    // Function type: (A, B) -> R  (func_return_type non-null ⇒ function type)
    // Tuple type:    (A, B)       (is_tuple ⇒ tuple; func_param_types = element types)
    std::vector<TypeNode> func_param_types;
    std::shared_ptr<TypeNode> func_return_type;
    bool is_tuple = false;   // true if this is a tuple type (int, string)
    SourceRange range;

    TypeNode() = default;
    TypeNode(const TypeNode& other)
        : basic_type(other.basic_type), class_name(other.class_name),
          type_args(other.type_args), is_generic_param(other.is_generic_param),
          range(other.range), array_size(other.array_size),
          is_inferred(other.is_inferred),
          is_tuple(other.is_tuple),
          func_param_types(other.func_param_types),
          func_return_type(other.func_return_type ? std::make_shared<TypeNode>(*other.func_return_type) : nullptr),
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
            is_tuple = other.is_tuple;
            func_param_types = other.func_param_types;
            func_return_type = other.func_return_type ? std::make_shared<TypeNode>(*other.func_return_type) : nullptr;
            element_type = other.element_type ? std::make_shared<TypeNode>(*other.element_type) : nullptr;
        }
        return *this;
    }

    bool isArray() const { return element_type != nullptr; }
    bool isClass() const { return !class_name.empty(); }
    bool isFunction() const { return func_return_type != nullptr; }
    bool isTuple() const { return is_tuple; }
    bool is_inferred = false; // true if declared with 'var'
};

// 类型 → mangled 名（泛型实例名与构造器重载名共用；sema 与 codegen 必须保持一致）
inline std::string mangleTypeNode(const TypeNode& t) {
    if (t.element_type) return mangleTypeNode(*t.element_type) + "_arr";
    if (!t.class_name.empty()) {
        std::string s = t.class_name;
        for (auto& ta : t.type_args) { s += "_"; s += mangleTypeNode(ta); }
        return s;
    }
    switch (t.basic_type) {
        case BuiltinType::Byte: return "byte";
        case BuiltinType::Short: return "short";
        case BuiltinType::Int: return "int";
        case BuiltinType::Long: return "long";
        case BuiltinType::UByte: return "ubyte";
        case BuiltinType::UShort: return "ushort";
        case BuiltinType::UInt: return "uint";
        case BuiltinType::ULong: return "ulong";
        case BuiltinType::Char: return "char";
        case BuiltinType::Float: return "float";
        case BuiltinType::Double: return "double";
        case BuiltinType::Bool: return "bool";
        case BuiltinType::String: return "string";
        case BuiltinType::Void: return "void";
    }
    return "unknown";
}

// ---- Declarations ----
struct ParamDecl {
    std::string name;
    TypeNode type;
    bool is_ref = false;
    // §四-1 默认参数：`void f(int a, int b = 10)` —— 调用时可省略（sema 克隆到实参）。
    // shared_ptr：ParamDecl 在向量中可拷贝（含泛型实例克隆），默认表达式只读共享。
    std::shared_ptr<Expr> default_expr;
    SourceRange range;
};

// 构造器 mangled 名：Class_Action_<paramtypes>（重载唯一；sema 与 codegen 共用）
inline std::string constructorMangledName(const std::string& cls_name,
                                          const std::string& action_name,
                                          const std::vector<ParamDecl>& params) {
    std::string s = cls_name + "_" + action_name;
    for (auto& p : params) { s += "_"; s += mangleTypeNode(p.type); }
    return s;
}

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
    // Generic static method: List.map<T, U>(...) — type params on a static action.
    std::vector<std::string> type_params;
    std::shared_ptr<Stmt> body;  // nullptr if declaration only
    SourceRange range;
    bool has_startup = false;
    bool has_constructor = false;  // @constructor 或 函数名==类名：构造器（new 时调用）
    bool has_test = false;
    bool has_coro = false;
    bool has_async = false;  // @async: 可挂起的异步 IO 方法（await 形态3）
    bool has_region = false;  // @region: 调用作用域为内存 region（自动回收）
    int coro_stack_kb = 0;    // @coro(stack=N): coroutine stack size in KB (0 = default 128KB)
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
    // Generic type-param constraints: type_param -> interface name (where T : I)
    std::unordered_map<std::string, std::string> type_param_constraints;
    std::vector<ActionDecl> actions;
    std::vector<ActionDecl> static_actions; // static: section
    std::vector<EventDecl> events;
    std::vector<PropertyDecl> properties;
    std::vector<FuncDecl> functions; // internal methods (function: section)
    std::vector<StructDecl> structs; // nested structs
    bool is_static = false; // class has only static: methods
    bool is_generic_inst = false; // monomorphized instance of a generic template
    std::vector<TypeNode> inst_type_args; // concrete args of a generic instance
    std::string interface_class_name; // non-empty if "interface class X;" declared
    // M-FN-2 named lambda: the lambda's own name (empty for normal classes), so
    // the __call body visit can bind the self-reference.
    std::string lambda_name;
    // 关联类型绑定：type Item = int;  —— 实现接口的关联类型（§三-5）
    std::unordered_map<std::string, TypeNode> associated_type_bindings;
    SourceRange range;
};

struct InterfaceDecl {
    std::string name;
    std::vector<ActionDecl> actions;
    std::vector<EventDecl> events;
    // 关联类型声明：type Item;  —— 实现类须绑定（§三-5）
    std::vector<std::string> associated_types;
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
    Try,
    Await,
    TupleExpr,
    NamedArg,   // §四-1 命名实参：f(name = value)
    MacroParam,
    Quote,
    Convert,    // 显式类型转换：uint8(x) / long(x) / double(x) ...
};

struct Expr {
    ExprKind kind;
    SourceRange range;
    TypeNode* type = nullptr;  // resolved during sema
    // §uint32：sema 解析出的表达式类型（默认 Int = 未设置/有符号回退）。
    // codegen 用它判断无符号实参/初始化值的 ZExt 加宽。
    TypeKind resolved_kind = TypeKind::Int;

    Expr(ExprKind k, SourceRange r) : kind(k), range(r) {}
    virtual ~Expr() = default;
};

struct IntegerLiteralExpr : Expr {
    int64_t value;
    bool is_long;      // L suffix: 42L → long
    bool is_unsigned;  // u suffix: 42u / 0xFFu → uint/ulong（无符号字面量）
    IntegerLiteralExpr(int64_t v, SourceRange r, bool l = false, bool u = false)
        : Expr(ExprKind::IntegerLiteral, r), value(v), is_long(l), is_unsigned(u) {}
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

/// §四-1 命名实参：`f(name = value)`。sema 在 visitCall 中按参数名重排到对应位置。
struct NamedArgExpr : Expr {
    std::string name;
    std::unique_ptr<Expr> value;
    NamedArgExpr(std::string n, std::unique_ptr<Expr> v, SourceRange r)
        : Expr(ExprKind::NamedArg, r), name(std::move(n)), value(std::move(v)) {}
};

/// A macro template parameter placeholder `$name` inside a macro body.
/// Replaced with the captured argument AST during macro expansion.
struct MacroParamExpr : Expr {
    std::string name;
    MacroParamExpr(std::string n, SourceRange r)
        : Expr(ExprKind::MacroParam, r), name(std::move(n)) {}
};

/// `quote { ... }` — compile-time AST template (M4 proc-macro).
/// Holds the quoted block; evaluated by the interpreter to build AST values.
struct QuoteExpr : Expr {
    std::unique_ptr<BlockStmt> body;
    QuoteExpr(std::unique_ptr<BlockStmt> b, SourceRange r)
        : Expr(ExprKind::Quote, r), body(std::move(b)) {}
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
    // §uint32：sema 解析出的无符号性（供 codegen 选 UDiv/URem/LShr/无符号比较与
    // ZExt 加宽；默认全 false = 现有有符号行为）。
    bool lhs_unsigned = false;    // 左操作数源类型为无符号
    bool rhs_unsigned = false;    // 右操作数源类型为无符号
    bool result_unsigned = false; // 公共运算类型为无符号
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

// 显式类型转换：`uint8(x)` / `byte(x)` / `long(x)` / `double(x)` ...
// 类型关键字后跟 '(' 解析为转换（宽→窄截断，窄→宽按源符号扩展，int↔float 转换）。
struct ConvertExpr : Expr {
    TypeKind to_kind;                     // 目标内置类型
    std::unique_ptr<Expr> operand;
    ConvertExpr(TypeKind k, std::unique_ptr<Expr> opnd, SourceRange range_)
        : Expr(ExprKind::Convert, range_), to_kind(k), operand(std::move(opnd)) {}
};

struct CallExpr : Expr {
    std::unique_ptr<Expr> callee;
    std::vector<std::unique_ptr<Expr>> args;
    std::vector<TypeNode> call_type_args;   // generic call: foo<int>(...) (explicit)
    std::string resolved_call_name;          // sema：泛型函数单态化后的 mangled 目标名
    std::string resolved_struct_type;  // sema：函数式构造的 struct 类型 key（空=普通调用）
    std::string resolved_struct_ctor;  // sema：函数式构造的构造器 mangled 名
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
    std::string resolved_ctor;  // sema 解析出的匹配构造器 mangled 名（空=无构造器，走 legacy/default）
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
    // M-FN-2 named lambda `fn name(...) => {...}`: name is bound to the lambda
    // itself inside the body (recursion via the hidden class's own tramp).
    std::string name;
    // M-FN-2 closure capture (by value): outer local names + hidden-class slot names.
    std::vector<std::string> capture_names;  // outer variable names (ordered)
    std::vector<std::string> capture_slots;  // hidden class property names ("cap_i")
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

// Expression try: try <expr> catch (e) <expr>  — value on success, fallback on error
struct TryExpr : Expr {
    std::unique_ptr<Expr> try_expr;
    std::string catch_var_name;
    std::unique_ptr<Expr> catch_expr;
    TryExpr(std::unique_ptr<Expr> te, std::string cvn, std::unique_ptr<Expr> ce, SourceRange r)
        : Expr(ExprKind::Try, r), try_expr(std::move(te)),
          catch_var_name(std::move(cvn)), catch_expr(std::move(ce)) {}
};

// Tuple literal: (a, b, ...) — at least 2 elements (top-level comma).
struct TupleExpr : Expr {
    std::vector<std::unique_ptr<Expr>> elements;
    TupleExpr(std::vector<std::unique_ptr<Expr>> elems, SourceRange r)
        : Expr(ExprKind::TupleExpr, r), elements(std::move(elems)) {}
};

// Destructuring target tree: (A a, B b) or ((int p, int q), int z)
struct DestructureTarget {
    std::string name;                              // non-empty ⇒ leaf (identifier)
    TypeNode type;                                 // declared type for leaf (declaration destructure)
    bool has_type = false;                         // true if a type annotation was parsed
    std::vector<DestructureTarget> elements;       // non-empty ⇒ nested tuple
    SourceRange range;
};

enum class StmtKind {
    Block,
    VarDeclStmt,
    DestructureStmt,
    ExprStmt,
    IfStmt,
    WhileStmt,
    ForStmt,
    ForInStmt,
    ReturnStmt,
    BreakStmt,
    ContinueStmt,
    AwaitStmt,
    MappingStmt,
    MatchStmt,
    TryStmt,
    ThrowStmt,
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

struct CatchClause {
    std::string var_name;
    std::string var_type;   // "" = catch-all (matches anything, var is string msg)
    std::unique_ptr<BlockStmt> block;
};

struct TryStmt : Stmt {
    std::unique_ptr<BlockStmt> try_block;
    std::vector<CatchClause> catches;
    std::unique_ptr<BlockStmt> finally_block;
    TryStmt(std::unique_ptr<BlockStmt> tb, std::vector<CatchClause> cs,
            std::unique_ptr<BlockStmt> fb, SourceRange r)
        : Stmt(StmtKind::TryStmt, r), try_block(std::move(tb)),
          catches(std::move(cs)), finally_block(std::move(fb)) {}
};

// throw expr; — string shortcut or typed exception object
struct ThrowStmt : Stmt {
    std::unique_ptr<Expr> expr;
    std::string throw_type;   // "string" | class name (set by Sema)
    ThrowStmt(std::unique_ptr<Expr> e, SourceRange r)
        : Stmt(StmtKind::ThrowStmt, r), expr(std::move(e)) {}
};

struct VarDeclStmt : Stmt {
    std::vector<VarDecl> decls;
    VarDeclStmt() : Stmt(StmtKind::VarDeclStmt, {}) {}
    explicit VarDeclStmt(VarDecl d)
        : Stmt(StmtKind::VarDeclStmt, d.range) { decls.push_back(std::move(d)); }
    VarDeclStmt(std::vector<VarDecl> d, SourceRange r)
        : Stmt(StmtKind::VarDeclStmt, r), decls(std::move(d)) {}
};

// Destructuring statement: (A a, B b) = expr;  or  (a, b) = expr;  (assignment)
struct DestructureStmt : Stmt {
    DestructureTarget target;
    std::unique_ptr<Expr> value;
    bool is_decl;   // true = declare new vars ((A a, B b) = ...); false = assign existing
    DestructureStmt(DestructureTarget t, std::unique_ptr<Expr> v, bool decl, SourceRange r)
        : Stmt(StmtKind::DestructureStmt, r), target(std::move(t)),
          value(std::move(v)), is_decl(decl) {}
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

// for (x in coll) { ... } — 集合迭代（§四-2，additive）。
// sema 解析 iter_kind 并注解：
//   0 = class 迭代（需 size() + get(int)），size_fn/get_fn 为解析后的函数名
//   1 = 固定数组 T[N]（编译期长度）
//   2 = slice<T>（运行时长度，用 s.size()）
struct ForInStmt : Stmt {
    std::string var_name;         // 循环变量名
    TypeNode var_type;            // 显式元素类型（has_type=false 时推断）
    bool has_type;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Stmt> body;
    TypeInfo elem_type;           // sema 解析的元素类型
    int iter_kind = 0;            // 0=class, 1=fixed array, 2=slice
    std::string class_name;       // class 迭代：实例类名（mangled）
    std::string size_fn;          // class/slice：size 函数名
    std::string get_fn;           // class：get 函数名
    int array_size = -1;          // 固定数组长度（iter_kind==1）
    ForInStmt(std::string vn, TypeNode vt, bool ht,
              std::unique_ptr<Expr> it, std::unique_ptr<Stmt> b, SourceRange r)
        : Stmt(StmtKind::ForInStmt, r), var_name(std::move(vn)), var_type(std::move(vt)),
          has_type(ht), iterable(std::move(it)), body(std::move(b)) {}
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

// await expr — expression form (C2): suspends, passes `expr` value out to the
// scheduler, and evaluates to the value passed in by __myp_coro_resume.
// Used e.g. `int v = await n * 2;`
struct AwaitStmt : Stmt {
    std::unique_ptr<Expr> expr;
    std::unique_ptr<Expr> timeout;   // nullable: `await Signal.go timeout 100;` (C10)
    AwaitStmt(std::unique_ptr<Expr> e, SourceRange r)
        : Stmt(StmtKind::AwaitStmt, r), expr(std::move(e)) {}
    AwaitStmt(std::unique_ptr<Expr> e, std::unique_ptr<Expr> to, SourceRange r)
        : Stmt(StmtKind::AwaitStmt, r), expr(std::move(e)), timeout(std::move(to)) {}
    AwaitStmt(SourceRange r)  // await; — simple suspend (no value passing)
        : Stmt(StmtKind::AwaitStmt, r), expr(nullptr) {}
};

// await expr — expression form (C2): suspends, passes `expr` value out to the
// scheduler, and evaluates to the value passed in by __myp_coro_resume.
// Used e.g. `int v = await n * 2;`
struct AwaitExpr : Expr {
    std::unique_ptr<Expr> operand;  // may be null → await with no value
    std::unique_ptr<Expr> timeout;  // nullable: `await Signal.go timeout 100` (C10)
    AwaitExpr(std::unique_ptr<Expr> op, SourceRange r)
        : Expr(ExprKind::Await, r), operand(std::move(op)) {}
    AwaitExpr(std::unique_ptr<Expr> op, std::unique_ptr<Expr> to, SourceRange r)
        : Expr(ExprKind::Await, r), operand(std::move(op)), timeout(std::move(to)) {}
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
    std::shared_ptr<BlockStmt> body; // shared: generic insts share the template body
    SourceRange range;
    std::vector<std::string> type_params;    // generic function template params <T,U>
    std::vector<TypeNode> inst_type_args;    // monomorphized instance concrete args
    bool is_generic_inst = false;            // monomorphized instance (not a template)
    bool has_test = false;
    bool is_auto_main = false;               // `mypc run`: 编译器注入的合成 main（豁免 main() 限制）
    bool has_region = false;  // @region: 调用作用域为内存 region（自动回收）
    bool has_coro = false;    // @coro: 顶层协程函数（C 系列）
    bool has_async = false;   // @async: 顶层异步 IO 函数（await 形态3）
    int coro_stack_kb = 0;    // @coro(stack=N) — 协程栈大小 KB（0=默认）
    std::string op_symbol;  // non-empty if this is an operator (@op("..."))
    bool has_eval = false;  // @eval: 编译期求值（纯函数）
    bool has_proc_macro = false; // @macro: 过程宏函数（编译期 AST 生成，M4）
    bool has_constructor = false; // @constructor 或 函数名==类名：构造器（new 时调用）
    bool is_const_decl = false; // top-level `const T name = expr` (a value, not a callable)
};

struct ImportDecl {
    std::string module_name;  // module name for stdlib imports
    std::string file_path;    // file path for path-based imports (e.g. "./other.myp")
    bool is_path = false;     // true if import uses string path syntax
    SourceRange range;
};

/// A top-level declarative macro: `macro name($a, $b) { <template body> }`.
/// The body is a normal MYP block whose `$param` occurrences are
/// MacroParamExpr placeholders; expansion substitutes captured argument ASTs.
struct MacroDecl {
    std::string name;
    std::vector<std::string> params;
    std::unique_ptr<BlockStmt> body;
    SourceRange range;
};

// Type alias: `type Name = Type;` (contextual keyword — only recognized in
// top-level `type <Id> = <Type>;` form, so `type` stays usable as identifier).
struct TypeAliasDecl {
    std::string name;
    TypeNode alias_type;
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
    std::vector<MacroDecl> macros;       // declarative macros (M3)
    std::vector<TypeAliasDecl> type_aliases; // type Name = Type; (contextual keyword)
};

} // namespace mylang

#endif // MYLANG_AST_H
