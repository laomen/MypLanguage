#ifndef MYLANG_SEMA_H
#define MYLANG_SEMA_H

#include "AST.h"
#include "DiagnosticEngine.h"
#include "SymbolTable.h"
#include "Type.h"

#include <unordered_map>
#include <functional>
#include <set>

namespace mylang {

/// Semantic analyzer: name resolution + type checking.
class Sema {
public:
    Sema(DiagnosticEngine& diag);

    /// Analyze the translation unit. Returns true if no errors.
    bool analyze(TranslationUnit& tu);

private:
    struct StmtResult {};

    // ---- Pass 1: Collect declarations ----
    void visitTranslationUnit(TranslationUnit& tu);
    void visitClassDecl(ClassDecl& decl);
    void visitStructDecl(StructDecl& decl);
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
    TypeInfo visitCall(CallExpr& expr);
    TypeInfo visitMemberAccess(MemberAccessExpr& expr);
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

    TypeInfo substituteTypeParams(const TypeNode& node,
                                  const std::vector<std::string>& type_params,
                                  const std::vector<TypeInfo>& type_args);
    std::string typeName(const TypeInfo& type) const;
    bool typesCompatible(const TypeInfo& lhs, const TypeInfo& rhs) const;

    // ---- Helpers ----
    void error(const SourceRange& range, const std::string& msg);
    bool expectBool(const TypeInfo& type, const SourceRange& range);
    bool expectNumeric(const TypeInfo& type, const SourceRange& range);

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
    int in_catch_depth_ = 0;         // >0 inside a catch block (for `throw;` rethrow)
    bool in_coro_method_ = false;    // true while checking an @coro action body (await allowed)
    std::string current_class_name_;
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
    std::vector<std::string> current_func_type_params_; // type params of current top-level function
    TypeInfo resolveGenericCall(CallExpr& expr, const std::string& name, int tu_index);
    TypeNode TypeNodeFromTypeInfo(const TypeInfo& t);
    void inferLambdaReturn(Stmt& stmt, TypeNode& out, bool& found);
    void collectLambdaLocals(Stmt& stmt, std::set<std::string>& locals);
    void collectExprLocals(Expr& e, std::set<std::string>& locals);
    void collectLambdaCaptures(Stmt& stmt, const std::set<std::string>& locals,
                               const std::vector<std::string>& params,
                               std::vector<std::string>& out);
    void collectExprCaptures(Expr& e, const std::set<std::string>& locals,
                             const std::vector<std::string>& params,
                             std::vector<std::string>& out);

};

} // namespace mylang

#endif // MYLANG_SEMA_H
