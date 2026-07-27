#ifndef MYLANG_CODEGEN_H
#define MYLANG_CODEGEN_H

#include "AST.h"
#include "DiagnosticEngine.h"
#include "Type.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace mylang {

/// LLVM IR code generator.
class CodeGen {
public:
    CodeGen(DiagnosticEngine& diag);
    ~CodeGen();

    std::string generate(TranslationUnit& tu, const std::string& output_filename, int opt_level = 0);

private:
    // ---- Module & Builder ----
    llvm::LLVMContext ctx_;
    llvm::IRBuilder<> builder_;
    std::unique_ptr<llvm::Module> module_;

    // ---- Variable scope stack ----
    std::vector<std::unordered_map<std::string, llvm::Value*>> named_values_;

    // ---- Class struct tracking ----
    // Maps class name → LLVM struct type
    std::unordered_map<std::string, llvm::StructType*> class_structs_;
    // Maps class name → property name → index in struct
    std::unordered_map<std::string, std::unordered_map<std::string, unsigned>> property_indices_;

    // ---- Current function ----
    llvm::Function* current_function_ = nullptr;

    // ---- Loop context (for break/continue) ----
    struct LoopContext {
        llvm::BasicBlock* continue_bb;
        llvm::BasicBlock* break_bb;
    };
    std::vector<LoopContext> loop_context_;

    // ---- Current class context (for bare method calls) ----
    std::string current_class_name_;

    // ---- Struct type tracking ----
    // Maps struct name (e.g. "Vec2" or "Sensor::Config") → LLVM struct type
    std::unordered_map<std::string, llvm::StructType*> struct_types_;
    // Maps struct name → field name → index
    std::unordered_map<std::string, std::unordered_map<std::string, unsigned>> struct_field_indices_;

    // ---- @thread handle tracking (for cleanup at main exit) ----
    std::vector<std::string> thread_handle_names_;
    bool in_main_ = false;

    // ---- Runtime function declarations ----
    // ---- Runtime function declarations ----
    llvm::Function* runtime_print_ = nullptr;
    llvm::Function* runtime_println_ = nullptr;
    llvm::Function* runtime_print_int_ = nullptr;
    llvm::Function* runtime_print_long_ = nullptr;
    llvm::Function* runtime_print_float_ = nullptr;
    llvm::Function* runtime_print_bool_ = nullptr;
    llvm::Function* runtime_alloc_ = nullptr;
    llvm::Function* runtime_free_ = nullptr;
    llvm::Function* runtime_now_ms_ = nullptr;
    llvm::Function* runtime_sleep_ms_ = nullptr;
    // Event system
    llvm::Function* runtime_event_register_ = nullptr;
    llvm::Function* runtime_event_fire_ = nullptr;
    llvm::Function* runtime_event_process_all_ = nullptr;
    // Thread system
    llvm::Function* runtime_thread_create_ = nullptr;
    llvm::Function* runtime_thread_run_loop_ = nullptr;
    llvm::Function* runtime_thread_stop_ = nullptr;
    llvm::Function* runtime_thread_destroy_ = nullptr;
    llvm::Function* runtime_thread_assoc_instance_ = nullptr;

    // ---- Math runtime functions ----
    llvm::Function* runtime_math_sqrt_ = nullptr;
    llvm::Function* runtime_math_abs_ = nullptr;
    llvm::Function* runtime_math_floor_ = nullptr;
    llvm::Function* runtime_math_ceil_ = nullptr;
    llvm::Function* runtime_math_sin_ = nullptr;
    llvm::Function* runtime_math_cos_ = nullptr;
    llvm::Function* runtime_math_tan_ = nullptr;
    llvm::Function* runtime_math_exp_ = nullptr;
    llvm::Function* runtime_math_log_ = nullptr;
    llvm::Function* runtime_math_pow_ = nullptr;
    llvm::Function* runtime_math_abs_int_ = nullptr;

    // ---- File I/O runtime functions ----
    llvm::Function* runtime_io_fopen_ = nullptr;
    llvm::Function* runtime_io_fclose_ = nullptr;
    llvm::Function* runtime_io_read_line_ = nullptr;
    llvm::Function* runtime_io_write_ = nullptr;
    llvm::Function* runtime_io_write_line_ = nullptr;
    llvm::Function* runtime_io_has_next_ = nullptr;
    llvm::Function* runtime_io_read_byte_ = nullptr;
    llvm::Function* runtime_io_read_i32be_ = nullptr;
    llvm::Function* runtime_io_seek_ = nullptr;    llvm::Function* runtime_io_write_byte_ = nullptr;
    llvm::Function* runtime_io_write_i32be_ = nullptr;
    llvm::Function* runtime_io_write_double_ = nullptr;
    llvm::Function* runtime_io_read_double_ = nullptr;
    // ---- Read line from stdin ----
    llvm::Function* runtime_read_line_ = nullptr;

    // ---- String comparison ----
    llvm::Function* runtime_str_eq_ = nullptr;

    // ---- Non-blocking keyboard ----
    llvm::Function* runtime_kbhit_ = nullptr;
    llvm::Function* runtime_getch_ = nullptr;
    llvm::Function* runtime_flush_ = nullptr;

    // ---- String to double ----
    llvm::Function* runtime_atof_ = nullptr;

    // ---- Timer runtime functions ----
    llvm::Function* runtime_timer_create_ = nullptr;

    // ---- Memory runtime functions ----
    llvm::Function* runtime_free_all_ = nullptr;

    // ---- Init function ----
    llvm::Function* init_func_ = nullptr;

    // ---- Error handling runtime functions ----
    llvm::Function* runtime_setjmp_ = nullptr;
    llvm::Function* runtime_longjmp_ = nullptr;
    llvm::Function* runtime_throw_ = nullptr;
    llvm::Function* runtime_get_error_ = nullptr;
    llvm::StructType* jmp_buf_type_ = nullptr;
    llvm::GlobalVariable* global_jmp_buf_ = nullptr;

    // ---- Test framework runtime functions ----
    llvm::Function* runtime_assert_ = nullptr;
    llvm::Function* runtime_assert_eq_ = nullptr;
    llvm::Function* runtime_assert_str_eq_ = nullptr;
    llvm::Function* runtime_test_report_ = nullptr;

    // ---- Global class instance refs (for mapping handler lookup) ----
    std::unordered_map<std::string, llvm::GlobalVariable*> class_instance_globals_;

    // ---- Variable name → class name map (for method resolution) ----
    std::unordered_map<std::string, std::string> var_class_map_;
    /// Track element types for local array variables (for subscript codegen).
    std::unordered_map<std::string, llvm::Type*> array_elem_types_;

    // ---- Global event ID map: "ClassName::eventName" -> int ----
    std::unordered_map<std::string, int> event_id_map_;

    // ---- Static action tracking: "ClassName_action" -> true/false ----
    std::unordered_map<std::string, bool> is_static_action_;

    // ---- Class-related methods ----
    void buildClassStructTypes(TranslationUnit& tu);
    llvm::StructType* getClassStruct(const std::string& name);
    bool getPropertyIndex(const std::string& class_name, const std::string& prop_name, unsigned& idx);
    llvm::Type* getPropertyType(const ClassDecl& cls, const std::string& prop_name);
    const ClassDecl* findClass(const std::string& name);

    // ---- Struct-related methods ----
    void buildStructTypes(TranslationUnit& tu);
    llvm::StructType* getStructType(const std::string& name);
    bool getStructFieldIndex(const std::string& struct_name, const std::string& field_name, unsigned& idx);
    llvm::Type* getStructFieldType(const StructDecl& st, const std::string& field_name);

    // ---- Type mapping ----
    llvm::Type* getLLVMType(const TypeInfo& type);
    /// Convert a TypeNode (from AST) to TypeInfo, preserving array info.
    TypeInfo typeNodeToCodegenType(const TypeNode& node);
    llvm::Type* typeNodeToLLVMType(const TypeNode& tn);

    // ---- Symbol table helpers ----
    void pushScope();
    void popScope();
    void setNamedValue(const std::string& name, llvm::Value* alloca);
    llvm::Value* getNamedValue(const std::string& name);
    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func,
                                              llvm::Type* type,
                                              const std::string& name);

    // ---- Top-level generation ----
    void generateTranslationUnit(TranslationUnit& tu);
    void createClassActionDecl(const ClassDecl& cls, const ActionDecl& action);
    void createStaticActionDecl(const ClassDecl& cls, const ActionDecl& action);
    void generateClass(const ClassDecl& decl);
    void generateClassAction(const ClassDecl& cls, const ActionDecl& action);
    void generateStaticAction(const ClassDecl& cls, const ActionDecl& action);
    void generateClassFunction(const ClassDecl& cls, const FuncDecl& fn_decl);
    void generateEventFire(const ClassDecl& cls, const EventDecl& ev, int event_id);
    void generateStructMethods(const StructDecl& st);
    void declareStructMethods(const StructDecl& st);
    void generateFuncDecl(const FuncDecl& decl);
    void generateMappingDecl(const MappingDecl& decl, llvm::BasicBlock* insert_bb = nullptr);
    void createInitFunction();
    void emitInitMappingCalls();
    void generateTestRunner();

    // ---- Statement generation ----
    void generateBlock(const BlockStmt& stmt);
    void generateStmt(const Stmt& stmt);
    void generateVarDecl(const VarDecl& decl);
    void generateIfStmt(const IfStmt& stmt);
    void generateWhileStmt(const WhileStmt& stmt);
    void generateForStmt(const ForStmt& stmt);
    void generateReturnStmt(const ReturnStmt& stmt);
    void generateBreakStmt(const BreakStmt& stmt);
    void generateContinueStmt(const ContinueStmt& stmt);

    // ---- Expression generation ----
    llvm::Value* generateExpr(const Expr& expr);
    llvm::Value* generateIntegerLiteral(const IntegerLiteralExpr& expr);
    llvm::Value* generateFloatLiteral(const FloatLiteralExpr& expr);
    llvm::Value* generateBoolLiteral(const BoolLiteralExpr& expr);
    llvm::Value* generateStringLiteral(const StringLiteralExpr& expr);
    llvm::Value* generateNullLiteral(const NullLiteralExpr& expr);
    llvm::Value* generateIdentifier(const IdentifierExpr& expr);
    llvm::Value* generateBinaryOp(const BinaryOpExpr& expr);
    llvm::Value* generateUnaryOp(const UnaryOpExpr& expr);
    llvm::Value* generateCall(const CallExpr& expr);
    llvm::Value* generateMemberAccess(const MemberAccessExpr& expr);
    llvm::Value* generateSubscript(const SubscriptExpr& expr);
    llvm::Value* generateNewExpr(const NewExpr& expr);
    llvm::Value* generateThisExpr(const ThisExpr& expr);
    llvm::Value* generateAssignment(const AssignmentExpr& expr);
    llvm::Value* generateTernary(const TernaryExpr& expr);
    llvm::Value* generateRange(const RangeExpr& expr);
    llvm::Value* generateEnumVariant(const EnumVariantExpr& expr);
    llvm::Value* generateLambda(const LambdaExpr& expr);
    void generateFFIDecl(const FFIDecl& decl);

    // ---- Match codegen ----
    void generateMatchStmt(const MatchStmt& stmt);
    void generateTryStmt(const TryStmt& stmt);

    // ---- Helper ----
    TypeInfo builtinTypeToInfo(BuiltinType bt) const;

    // ---- Runtime setup ----
    void declareRuntimeFunctions();

    // ---- Output ----
    bool writeObjectFile(const std::string& output_path, int opt_level = 0);

    DiagnosticEngine& diag_;
    TranslationUnit* current_tu_ = nullptr;

    // ---- Intrinsic name→function map ----
    std::unordered_map<std::string, llvm::Function*> intrinsic_map_;

    // ---- Flags ----
    bool emit_llvm_ = false;
    bool library_mode_ = false;
    bool test_mode_ = false;

public:
    void setEmitLLVM(bool v) { emit_llvm_ = v; }
    void setLibraryMode(bool v) { library_mode_ = v; }
    void setTestMode(bool v) { test_mode_ = v; }
    bool saveIR(const std::string& path) const;

    // ---- Helper: find struct decl ----
    const StructDecl* findStruct(const std::string& name) const;
};

} // namespace mylang

#endif // MYLANG_CODEGEN_H
