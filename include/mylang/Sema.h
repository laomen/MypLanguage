#ifndef MYLANG_SEMA_H
#define MYLANG_SEMA_H

#include "AST.h"
#include "DiagnosticEngine.h"
#include "SymbolTable.h"
#include "Type.h"

#include <unordered_map>
#include <functional>
#include <set>
#include <map>

namespace mylang {

/// Semantic analyzer: name resolution + type checking.
class Sema {
public:
    Sema(DiagnosticEngine& diag);

    /// Analyze the translation unit. Returns true if no errors.
    bool analyze(TranslationUnit& tu);

    /// `mypc run`: enable auto-generated main for single-class files with
    /// @startup (inject a synthetic main when none is defined).
    void setAutoMain(bool v) { auto_main_ = v; }

private:
    struct StmtResult {};

    // ---- Pass 1: Collect declarations ----
    void visitTranslationUnit(TranslationUnit& tu);
    void visitClassDecl(TranslationUnit& tu, size_t ci);
    void visitStructDecl(StructDecl& decl);
    void declareStructName(StructDecl& decl);
    void visitBitfieldDecl(BitfieldDecl& decl);
    void declareBitfieldName(BitfieldDecl& decl);
    void visitInterfaceDecl(InterfaceDecl& decl);
    void visitFuncDecl(FuncDecl& decl);
    void visitEnumDecl(EnumDecl& decl);

    // ---- Pass 2: Type-check bodies ----
    void visitFuncBody(FuncDecl& decl);
    void visitActionBody(ActionDecl& decl);
    StmtResult visitStmt(Stmt& stmt);
    StmtResult visitBlock(BlockStmt& stmt);
    StmtResult visitVarDecl(VarDecl& decl);
    StmtResult visitIfStmt(IfStmt& stmt);
    StmtResult visitWhileStmt(WhileStmt& stmt);
    StmtResult visitForStmt(ForStmt& stmt);
    StmtResult visitForInStmt(ForInStmt& stmt);
    // §3.2 @gpu tile（共享内存协作 kernel）：校验维度/上限/grid，声明共享数组。
    StmtResult visitGpuTileStmt(GpuTileStmt& stmt);
    // §8.2 @gpu reduce（声明式归约）：out = fold(init, a[lo..hi))。校验 op 返回
    // 类型 = 数组元素 = init = acc；提取 return 表达式到 stmt.op_expr。
    StmtResult visitGpuReduceStmt(GpuReduceStmt& stmt);
    // §8.2 @gpu reduce 表达式形式（GpuReduceExpr）：声明合成临时标量后复用
    // visitGpuReduceStmt 校验；表达式结果类型 = 数组元素类型。
    TypeInfo visitGpuReduceExpr(GpuReduceExpr& expr);
    // §8.3 @gpu scan（声明式前缀和）：b[lo+i] = init∘a[lo]∘…。同 reduce 校验。
    StmtResult visitGpuScanStmt(GpuScanStmt& stmt);
    // §8.4 @gpu scatter（声明式散点）：b[idx[lo_i+p]] = a[lo_a+p]。校验 a/b 同
    // 元素类型 T[]、idx 整型[]、两区间整界；冲突模式合法。
    StmtResult visitGpuScatterStmt(GpuScatterStmt& stmt);
    // §四-2 × 泛型：ForInStmt 注解逻辑（正常访问 + 单态化重注解共用）。
    bool annotateForInStmt(ForInStmt& stmt, const TypeInfo& it_type);
    void annotateForInsInStmt(Stmt& s, const std::vector<ParamDecl>& params);
    StmtResult visitReturnStmt(ReturnStmt& stmt);
    StmtResult visitMatchStmt(MatchStmt& stmt);
    StmtResult visitTryStmt(TryStmt& stmt);
    StmtResult visitThrowStmt(ThrowStmt& stmt);

    // ---- Expression type checking ----
    TypeInfo visitExpr(Expr& expr);
    TypeInfo visitIntegerLiteral(IntegerLiteralExpr& expr);
    TypeInfo visitFloatLiteral(FloatLiteralExpr& expr);
    TypeInfo visitBoolLiteral(BoolLiteralExpr& expr);
    TypeInfo visitStringLiteral(StringLiteralExpr& expr);
    TypeInfo visitNullLiteral(NullLiteralExpr& expr);
    TypeInfo visitIdentifier(IdentifierExpr& expr);
    TypeInfo visitBinaryOp(BinaryOpExpr& expr);
    TypeInfo visitUnaryOp(UnaryOpExpr& expr);
    TypeInfo visitConvert(ConvertExpr& expr);
    TypeInfo visitCall(CallExpr& expr);
    TypeInfo visitBitcast(CallExpr& expr);
    TypeInfo visitBytesStr(CallExpr& expr, const std::string& name);
    TypeInfo visitBytesOf(CallExpr& expr);
    TypeInfo visitParse(CallExpr& expr, const std::string& name);
    // §6.2 P4 parseIntOpt：返回 (value:int, ok:bool) 元组，区分合法 0 与解析失败。
    TypeInfo visitParseOpt(CallExpr& expr);
    TypeInfo visitBitOps(CallExpr& expr, const std::string& name);
    // §9.5 多态数学 intrinsic（__myp_math_*）：一元实数函数/abs/trunc 按实参
    // 类型定返回类型（f32→f32、f64→f64；整型原样返回供泛型体 Int 占位符检查，
    // 实例化由 where T : Float/Numeric 约束把关）。
    TypeInfo visitMathIntrinsic(CallExpr& expr, const std::string& name);
    // §3.6 向量打包访问：load4(float[] a, long i) : float4 —— 16B 打包读
    // a[4i..4i+3]；store4(float[] a, long i, float4 v) —— 打包写。
    TypeInfo visitVec4Access(CallExpr& expr, const std::string& name);
    // §4.2 P3 checked 溢出变体：checkedAdd/checkedMul 返回 (value, overflow:bool)
    // 元组——@llvm.sadd/smul.with.overflow 直映（有符号整型，按实参类型选位宽）。
    TypeInfo visitCheckedOp(CallExpr& expr, const std::string& name);
    // §3.1 kernel 执行上下文：@gpu for body 内 `kernel.gid/bx/tx/bd/gx`（long）
    // 与 `kernel.sync()`（void）隐式可见（保留标识符，仅 @gpu for body 内解析）。
    // §3.1 kernel 执行上下文（@gpu for / @gpu tile body 内）
    bool in_gpu_for_ = false;
    // §3.3 发散控制流深度（kernel 上下文内 if/while body）：>0 时 kernel.sync()
    // 位于线程相关分支 → 警告（aligned barrier 需 uniform 控制流，否则死锁）。
    int in_gpu_divergent_ = 0;
    TypeInfo visitMemberAccess(MemberAccessExpr& expr);
    // §四-1：填充 Function TypeInfo 的参数元数据（名/默认值），与 param_types 对齐
    void populateFuncTypeMeta(TypeInfo& ft, const std::vector<ParamDecl>& params);
    // §四-1：把实参（含命名实参 + 缺省默认值）规范化为与形参一一对应的有序列表
    bool normalizeCallArgs(std::vector<std::unique_ptr<Expr>>& args,
                           const TypeInfo& ft, const SourceRange& call_range);
    // §四-1：按 ParamDecl 列表规范化实参（构造器/struct 构造用）
    bool normalizeArgsToParamDecls(std::vector<std::unique_ptr<Expr>>& args,
                                   const std::vector<ParamDecl>& params,
                                   const SourceRange& call_range);
    // §四-1：声明期校验每个带默认值的形参默认表达式类型
    void checkParamDefaults(const std::vector<ParamDecl>& params);
    TypeInfo visitSubscript(SubscriptExpr& expr);
    TypeInfo visitNewExpr(NewExpr& expr);
    void resolveNewConstructor(NewExpr& expr, const std::string& cls_name);
    bool resolveStructConstruction(CallExpr& expr, const std::string& name);
    TypeInfo visitNewArrayExpr(NewArrayExpr& expr);
    TypeInfo visitThisExpr(ThisExpr& expr);
    TypeInfo visitAssignment(AssignmentExpr& expr);
    TypeInfo visitTernary(TernaryExpr& expr);
    TypeInfo visitTryExpr(TryExpr& expr);
    TypeInfo visitRange(RangeExpr& expr);
    TypeInfo visitEnumVariant(EnumVariantExpr& expr);

    // ---- Type utilities ----
    TypeInfo typeNodeToTypeInfo(const TypeNode& node, int alias_depth = 0);
    const TypeAliasDecl* findAlias(const std::string& name) const;
    TypeNode substituteTypeNode(const TypeNode& node,
                                const std::vector<std::string>& type_params,
                                const std::vector<TypeNode>& type_args) const;
    TypeInfo visitLambda(LambdaExpr& expr, const TypeInfo* expected_fn = nullptr);
    TypeInfo visitPipe(PipeExpr& expr);
    void visitFFI(FFIDecl& decl);
    int lambda_counter_ = 0;
    // M-FN-2 named lambda self-reference: while visiting a `fn name(...)` lambda's
    // __call body, calls to `name` resolve to the lambda's own tramp (this).
    std::string lambda_self_name_;
    std::string lambda_self_class_;
    // ---- M-FN-2 nonlocal（按引用捕获）----
    // >0 = 正在类型检查某个 lambda 的 __call body（用于"nonlocal 仅 lambda 内"校验）
    int in_lambda_body_ = 0;
    // 当前函数/action 的 nonlocal 捕获 accumulator（visitLambda 时填入，函数体
    // 结束赋给 decl.nonlocal_captures，供 codegen 序言把变量提升为 cell）。
    std::set<std::string> current_func_nonlocal_vars_;
    // nonlocal 变量名 → cell 类（平行于 current_func_nonlocal_vars_）。
    std::map<std::string, std::string> current_func_nonlocal_cell_class_;
    // visitStmt 可能触发单态化重分配 tu.functions/classes → decl/action 引用悬垂。
    // 先在 visitFuncBody/action 体内把 accumulator 拷到下面成员（安全），再由调用方
    // 按索引重取后赋给 decl.nonlocal_captures。
    std::set<std::string> last_func_nonlocal_vars_;
    std::map<std::string, std::string> last_func_nonlocal_cell_class_;
    // 已合成的 cell 类：类型名 → __cell_N（每个标量类型复用同一个 cell 类）。
    std::map<std::string, std::string> cell_class_by_type_;
    int cell_counter_ = 0;
    void collectLambdaNonlocal(Stmt& stmt, std::set<std::string>& out);
    void collectExprNonlocal(Expr& e, std::set<std::string>& out);
    StmtResult visitNonlocalStmt(NonlocalStmt& stmt);

    TypeInfo substituteTypeParams(const TypeNode& node,
                                  const std::vector<std::string>& type_params,
                                  const std::vector<TypeInfo>& type_args);
    std::string typeName(const TypeInfo& type) const;
    bool typesCompatible(const TypeInfo& lhs, const TypeInfo& rhs) const;
    // 实参类型 from → 形参类型 to 是否为「有损窄化」（typesCompatible 已通过前提下）：
    // Long→Int、Double→Float、Double→Int、Long→Short 等位宽收窄，或浮点→整型。
    // 用于参数传递处报「第 k 个实参是 long 但形参是 int」类错误，捕获漏参/换位。
    bool isNarrowing(const TypeInfo& from, const TypeInfo& to) const;
    bool isNumericKind(TypeKind k) const;
    bool isUnsignedKind(TypeKind k) const;
    TypeKind commonNumericKind(TypeKind a, TypeKind b) const;
    // §9 内置数值 trait 约束：类型 t 是否满足 trait（Numeric/Integer/Float/Ordered，
    // 或接口名 → 类实现检查）。
    bool satisfiesTraitConstraint(const TypeInfo& t, const std::string& trait) const;
    // Conservative "does this statement guarantee control never falls off its
    // end" (return/throw/infinite-loop/all-arms-return). Used to flag non-void
    // functions missing a return at the end.
    bool stmtGuaranteesTermination(const Stmt& s) const;
    // Error if the current (non-void) function's body can fall off the end.
    void checkMissingReturn(const SourceRange& range, const Stmt& body);

    // ---- Helpers ----
    void error(const SourceRange& range, const std::string& msg);
    bool expectBool(const TypeInfo& type, const SourceRange& range);
    bool expectNumeric(const TypeInfo& type, const SourceRange& range);
    // Builds current_class_member_types_ for class tu.classes[ci]. Takes the TU
    // + index (NOT a ClassDecl&) because resolving generic member types may
    // monomorphize (tu.classes reallocates) → a held reference would dangle.
    void buildCurrentClassMemberTypes(TranslationUnit& tu, size_t ci);
    ClassDecl* findClassDecl(const std::string& name);
    const ClassDecl* findClassDecl(const std::string& name) const;
    // O(1) 按名查顶层函数（含单态化实例；名称唯一）。避免 isGlobalName/isAsyncCallee/
    // resolveGenericCall 每调用线性扫全部函数（P7 规模 O(N²) 根因）。
    FuncDecl* findFunctionDecl(const std::string& name);
    const FuncDecl* findFunctionDecl(const std::string& name) const;
    // 顶层函数 push 后登记索引（findFunctionDecl 用）——供单态化/合成 main 调用。
    void indexFunction(const std::string& name, size_t index);
    // O(1) 按裸名查 struct（文件级优先，再嵌套）；type_key 输出限定 key（"S" 或 "C::S"）。
    const StructDecl* findStructDecl(const std::string& name, std::string* type_key) const;

    // ---- Mapping cycle detection ----
    void checkMappingCycles(const MappingDecl& decl);
    void checkMappingTypes(const MappingDecl& decl);

    // ---- Struct method type-checking ----
    void checkStructMethods(const StructDecl& decl);

    // ---- Interface validation ----
    void checkInterfaceImpl(const ClassDecl& cls);

    // ---- Built-in modules ----
    void registerIntrinsics();

    // ---- State ----
    DiagnosticEngine& diag_;
    mutable SymbolTable symbol_table_;
    TranslationUnit* current_tu_ = nullptr;

    // Current function return type (for return checking)
    TypeInfo current_return_type_;

    // Whether we're currently inside a loop (for break/continue checking)
    bool in_loop_ = false;

    // Whether we're currently inside a class action/method (for 'this')
    bool in_class_method_ = false;
    bool in_struct_method_ = false;  // Whether inside a struct method
    bool in_main_function_ = false;
    // BUG-050: 正在解析调用表达式 `f(...)` 的 callee —— 裸 const 标识符折叠仅对
    // 非 callee 引用生效（`CAP()` 仍按可调用 Function 解析，不被折叠成值类型）。
    bool in_call_callee_ = false;
    int in_catch_depth_ = 0;         // >0 inside a catch block (for `throw;` rethrow)
    bool in_coro_method_ = false;    // true while checking an @coro action body (await allowed)
    // Current action name (for the @coro recursive-self-call diagnostic).
    std::string current_method_name_;
    // True while visiting a statement-level call whose result is discarded
    // (e.g. `deep(n-1);`) — such @coro self-calls spawn a chain and are allowed.
    bool in_discarded_stmt_expr_ = false;
    // §五-5: is `callee` an @async-annotated function/static method? (only
    // await-able inside an @coro context)
    bool isAsyncCallee(const Expr* callee) const;
    std::string current_class_name_;
    std::unordered_map<std::string, TypeInfo> current_class_member_types_;
    std::unordered_map<std::string, size_t> class_indices_;
    // 顶层函数名 → 索引（含单态化实例）。functions 在 sema 期间会 push_back
    // 单态化实例——vector 重分配不失效索引，但必须在每次 push 后登记新名。
    std::unordered_map<std::string, size_t> function_indices_;
    // 按裸名索引 struct（文件级优先，再嵌套），供 resolveStructConstruction 等
    // O(1) 查找——避免每方法调用线性扫全部类（P6 规模 O(N²) 根因）。value =
    // { StructDecl*, type_key }。struct 在 sema 期间不增长（仅 classes.push_back
    // 单态化），analyze() 一次建全即可。
    std::unordered_map<std::string, std::pair<const StructDecl*, std::string>> struct_by_name_;
    std::unordered_map<std::string, std::unordered_map<std::string, TypeInfo>>
        struct_member_types_;
    // ---- Bitfield tracking (§5.1) ----
    struct BitfieldFieldInfo { int offset = 0; int width = 1; };
    std::unordered_map<std::string, std::unordered_map<std::string, BitfieldFieldInfo>>
        bitfield_layout_;
    std::unordered_map<std::string, std::unordered_map<std::string, TypeInfo>>
        bitfield_member_types_;
    std::unordered_map<std::string, int> bitfield_bits_;
    std::unordered_map<std::string, std::unordered_map<std::string, TypeInfo>>
        interface_member_types_;
    std::string current_struct_type_key_;  // Qualified type key for the current struct

    // ---- Enum info tracking ----
    struct EnumInfo {
        std::vector<EnumVariant> variants;
    };
    std::unordered_map<std::string, EnumInfo> enum_info_;

    // ---- Generic class tracking ----
    struct GenericInfo {
        int tu_index = -1; // index into current_tu_->classes
    };
    std::unordered_map<std::string, GenericInfo> generic_classes_;

    // ---- Generic function tracking ----
    struct GenericFuncInfo {
        int tu_index = -1; // index into current_tu_->functions
    };
    std::unordered_map<std::string, GenericFuncInfo> generic_functions_;
    // 是否存在任何带 @op 的外部函数（analyze() 一次建好）。P7 规模修复：visitBinaryOp
    // 对每个 `s + 1` 都线性扫全部函数找 @op 匹配（N 个单态化函数 → O(N²)）；
    // 无 @op 函数时直接跳过。
    bool any_op_functions_ = false;
    // 是否存在任何带 @coro 的顶层函数（analyze() 一次建好）。P7 规模修复：visitCall
    // 对每个标识符调用都线性扫全部函数找 @coro 返回长句柄（N 次调用 × N 个函数 = O(N²)）。
    bool any_coro_functions_ = false;
    std::vector<std::string> current_func_type_params_; // type params of current top-level function
    TypeInfo resolveGenericCall(CallExpr& expr, const std::string& name, int tu_index);

    // ---- Generic static method tracking (List.map<T,U>(...)) ----
    struct GenericStaticMethodInfo {
        int class_index = -1;  // index into current_tu_->classes
        int action_index = -1; // index into classes[class_index].static_actions
    };
    std::unordered_map<std::string, GenericStaticMethodInfo> generic_static_methods_;
    TypeInfo resolveGenericStaticCall(CallExpr& expr, const std::string& cls_name,
                                      const std::string& method, int class_index, int action_index);
    TypeNode TypeNodeFromTypeInfo(const TypeInfo& t);

    // ---- `mypc run` 自动 main（单类文件带 @startup，无 main 时注入合成 main）----
    bool auto_main_ = false;
    void injectAutoMainIfNeeded(TranslationUnit& tu);
    void inferLambdaReturn(Stmt& stmt, TypeNode& out, bool& found);
    void collectLambdaLocals(Stmt& stmt, std::set<std::string>& locals);
    void collectExprLocals(Expr& e, std::set<std::string>& locals);
    void collectLambdaCaptures(Stmt& stmt, const std::set<std::string>& locals,
                               const std::vector<std::string>& params,
                               std::vector<std::string>& out);
    void collectExprCaptures(Expr& e, const std::set<std::string>& locals,
                             const std::vector<std::string>& params,
                             std::vector<std::string>& out);
    // §五-3：标识符是否为全局函数/泛型函数/类/接口/struct/枚举名（lambda 捕获
    // 分析跳过——它们不是外层局部变量）。
    bool isGlobalName(const std::string& name) const;

    // Recursion depth for deep-nesting stack-overflow defense (guarded in
    // visitExpr; see SemaDepthGuard in sema_expr.cpp).
    int recursion_depth_ = 0;

    // ---- Recursive struct detection (infinite by-value size) ----
    // struct key -> struct-type field names embedded by value; and the decl
    // range for diagnostics. A cycle in this graph means an infinitely-sized
    // struct (e.g. `struct S { S next; }`), which would otherwise reach codegen
    // and fail with a cryptic "Code generation failed".
    std::unordered_map<std::string, std::vector<std::string>> struct_byval_edges_;
    std::unordered_map<std::string, SourceRange> struct_decl_ranges_;
    // Struct names already declared in Phase A (name-only registration). Used so
    // visitStructDecl's field validation is order-independent: a struct may
    // reference a struct declared later in the same merged translation unit.
    std::set<std::string> declared_struct_names_;
    void detectStructRecursion();

    // Names of local variables declared with `@thread` (auto-starting worker
    // threads). Manual calls to their `@startup` method are rejected at
    // compile time because the runtime auto-invokes it in the worker thread.
    std::set<std::string> thread_annotated_vars_;

};

} // namespace mylang

#endif // MYLANG_SEMA_H
