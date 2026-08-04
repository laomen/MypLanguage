#ifndef MYLANG_CODEGEN_H
#define MYLANG_CODEGEN_H

#include "AST.h"
#include "DiagnosticEngine.h"
#include "Type.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>

#include <set>
#include <map>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
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
    std::unordered_map<std::string, llvm::Type*> named_value_types_;
    std::set<llvm::Function*> scope_functions_; // functions with @scope mappings

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

    // ---- try/finally control flow (return/break/continue pass through finally) ----
    // finally_flag per try is now a "mode": 0=normal, 1=propagate(rethrow),
    // 2=return, 3=break, 4=continue. The stack lets an inner finally forward
    // the mode to the enclosing finally (nested try/finally), so the actual
    // return/break/continue happens only after the outermost finally runs.
    struct FinallyCtx {
        llvm::Value* flag_slot;          // i8 mode slot for this try
        llvm::BasicBlock* finally_bb;    // this try's finally block
        llvm::BasicBlock* merge_bb;      // this try's normal-exit merge
        llvm::BasicBlock* outer_finally_bb;  // enclosing finally block, or null
        llvm::Value* outer_flag_slot;    // enclosing flag slot (for mode forwarding)
        llvm::BasicBlock* break_bb;      // loop break target (outermost, no outer finally)
        llvm::BasicBlock* continue_bb;   // loop continue target
        bool in_finally = false;         // while this try's finally body is being generated
    };
    std::vector<FinallyCtx> finally_ctx_stack_;
    llvm::Value* finally_ret_slot_ = nullptr;  // function-level return-value slot (shared)

    // ---- Current class context (for bare method calls) ----
    std::string current_class_name_;

    // ---- @region memory arena state (function currently being generated) ----
    bool in_region_function_ = false;         // generating a @region fn (non-escaping)
    llvm::Value* current_region_mark_ = nullptr;  // mark alloca for the region fn

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
    llvm::Function* runtime_event_push_scope_ = nullptr;
    llvm::Function* runtime_event_pop_scope_ = nullptr;
    // Thread system
    llvm::Function* runtime_thread_create_ = nullptr;
    llvm::Function* runtime_thread_run_loop_ = nullptr;
    llvm::Function* runtime_thread_stop_ = nullptr;
    llvm::Function* runtime_thread_destroy_ = nullptr;
    llvm::Function* runtime_thread_assoc_instance_ = nullptr;

    // Thread pool functions
    llvm::Function* runtime_pool_ensure_ = nullptr;
    llvm::Function* runtime_parallel_for_ = nullptr;

    // ---- Vtable tracking for interface dispatch ----
    std::map<std::string, llvm::GlobalVariable*> vtables_; // key: "iface_class"

    // ---- Math runtime functions ----
    llvm::Function* runtime_math_sqrt_ = nullptr;
    llvm::Function* runtime_math_abs_ = nullptr;
    llvm::Function* runtime_math_floor_ = nullptr;
    llvm::Function* runtime_math_ceil_ = nullptr;
    llvm::Function* runtime_math_sin_ = nullptr;
    llvm::Function* runtime_math_cos_ = nullptr;
    llvm::Function* runtime_math_tan_ = nullptr;
    llvm::Function* runtime_math_asin_ = nullptr;
    llvm::Function* runtime_math_acos_ = nullptr;
    llvm::Function* runtime_math_atan_ = nullptr;
    llvm::Function* runtime_math_atan2_ = nullptr;
    llvm::Function* runtime_math_sinh_ = nullptr;
    llvm::Function* runtime_math_cosh_ = nullptr;
    llvm::Function* runtime_math_tanh_ = nullptr;
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

    // ---- Terminal size (TUI) ----
    llvm::Function* runtime_term_width_ = nullptr;
    llvm::Function* runtime_term_height_ = nullptr;

    // ---- String utilities ----
    llvm::Function* runtime_strlen_ = nullptr;
    llvm::Function* runtime_chr_ = nullptr;

    // ---- String to double ----
    llvm::Function* runtime_atof_ = nullptr;

    // ---- Timer runtime functions ----
    llvm::Function* runtime_timer_create_ = nullptr;

    // ---- Memory runtime functions ----
    llvm::Function* runtime_free_all_ = nullptr;

    // ---- GPU / CUDA runtime ----
    llvm::Function* runtime_gpu_init_ = nullptr;
    llvm::Function* runtime_gpu_alloc_ = nullptr;
    llvm::Function* runtime_gpu_free_ = nullptr;
    llvm::Function* runtime_gpu_to_device_ = nullptr;
    llvm::Function* runtime_gpu_to_host_ = nullptr;
    llvm::Function* runtime_gpu_load_kernel_ = nullptr;
    llvm::Function* runtime_gpu_launch_ = nullptr;
    llvm::Function* runtime_gpu_destroy_kernel_ = nullptr;
    // CUDA device info
    llvm::Function* runtime_cuda_count_ = nullptr;
    llvm::Function* runtime_cuda_name_ = nullptr;
    llvm::Function* runtime_cuda_memory_ = nullptr;
    llvm::Function* runtime_cuda_capability_ = nullptr;
    llvm::Function* runtime_cuda_multiprocessors_ = nullptr;
    llvm::Function* runtime_cuda_max_threads_ = nullptr;
    llvm::Function* runtime_cuda_warp_ = nullptr;

    // ---- Init function ----
    llvm::Function* init_func_ = nullptr;

    // ---- Error handling runtime functions ----
    llvm::Function* runtime_setjmp_ = nullptr;
    llvm::Function* runtime_longjmp_ = nullptr;
    llvm::Function* runtime_throw_ = nullptr;
    llvm::Function* runtime_get_error_ = nullptr;
    llvm::Function* runtime_exception_push_ = nullptr;
    llvm::Function* runtime_exception_pop_ = nullptr;
    llvm::Function* runtime_exception_get_jmpbuf_ = nullptr;
    llvm::Function* runtime_throw_object_ = nullptr;
    llvm::Function* runtime_exception_get_type_ = nullptr;
    llvm::Function* runtime_exception_get_object_ = nullptr;
    llvm::StructType* jmp_buf_type_ = nullptr;
    llvm::GlobalVariable* global_jmp_buf_ = nullptr;
    // class name → compile-time exception type ID (>0; 0 = string message)
    std::unordered_map<std::string, int> class_type_ids_;
    // Global array __myp_error_vtables[type_id] → Error vtable ptr (null if the
    // class does not implement the Error interface). Used by catch (Error e).
    llvm::GlobalVariable* error_vtables_gv_ = nullptr;

    // ---- Test framework runtime functions ----
    llvm::Function* runtime_assert_ = nullptr;
    llvm::Function* runtime_assert_eq_ = nullptr;
    llvm::Function* runtime_assert_str_eq_ = nullptr;
    llvm::Function* runtime_assert_neq_ = nullptr;
    llvm::Function* runtime_assert_long_eq_ = nullptr;
    llvm::Function* runtime_assert_str_neq_ = nullptr;
    llvm::Function* runtime_test_report_ = nullptr;

    // ---- Global class instance refs (for mapping handler lookup) ----
    std::unordered_map<std::string, llvm::GlobalVariable*> class_instance_globals_;
    // ---- Static property globals: "ClassName_propName" -> GlobalVariable ----
    std::unordered_map<std::string, llvm::GlobalVariable*> static_property_globals_;

    // ---- Variable name → class name map (for method resolution) ----
    std::unordered_map<std::string, std::string> var_class_map_;
    /// Track slice element types for local slice variables ("name" -> TypeInfo).
    std::unordered_map<std::string, TypeInfo> var_slice_types_;
    /// Track element types for local array variables (for subscript codegen).
    std::unordered_map<std::string, llvm::Type*> array_elem_types_;
    std::unordered_map<std::string, llvm::Type*> var_value_types_;

    // ---- Global event ID map: "ClassName::eventName" -> int ----
    std::unordered_map<std::string, int> event_id_map_;

    // ---- Static action tracking: "ClassName_action" -> true/false ----
    std::unordered_map<std::string, bool> is_static_action_;

    // ---- Coroutine methods: "ClassName_method" (only @coro annotated) ----
    std::unordered_set<std::string> coro_methods_;
    // True while generating the body of an @coro method (return stores result)
    bool current_is_coro_ = false;

    // ---- GPU / CUDA offload ----
    std::string ptx_code_;  // Generated PTX for @gpu for kernels
    bool cuda_enabled_ = false;
    // Track array byte sizes for GPU data transfer
    std::unordered_map<std::string, llvm::Value*> array_byte_sizes_;

    // ---- Stage 4: GPU kernel body compilation ----
    struct KernelArgInfo {
        std::string name;
        llvm::Type* type;
        bool is_array;
        int array_arg_idx;      // index in kernel args for array data
        int size_arg_idx;       // index in kernel args for array byte-size (-1 if none)
    };
    std::vector<KernelArgInfo> kernel_args_;
    const ForStmt* gpu_for_stmt_ = nullptr;
    // Set when the GPU kernel body uses a math function that requires CUDA
    // libdevice (sin/cos/tan/exp/log/pow). The runtime does not link libdevice,
    // so such kernels must fall back to CPU.
    bool gpu_math_unsupported_ = false;
    // Set when the GPU kernel body emits a call to a __nv_* libdevice function.
    // When true, CUDA libdevice.10.bc is linked into the kernel module at
    // compile time so the generated PTX is self-contained.
    bool gpu_math_used_ = false;

    // AST walk helpers for GPU kernel body compilation
    void collectExprIdentifiers(const Expr& expr, std::set<std::string>& out,
                                std::set<std::string>& loop_decls) const;
    void collectStmtIdentifiers(const Stmt& stmt, std::set<std::string>& out,
                                std::set<std::string>& loop_decls) const;
    void analyzeGpuCapturedVars(const ForStmt& stmt, const std::string& loop_var);

    // Kernel body codegen (uses kb on PTX module)
    llvm::Value* emitKernelExpr(const Expr& expr, llvm::IRBuilder<>& kb,
        std::map<std::string, llvm::Value*>& kernel_vars,
        const std::vector<llvm::Value*>& kernel_arg_values,
        const std::string& loop_var_name, llvm::Value* tid_val);
    void emitKernelStmt(const Stmt& stmt, llvm::IRBuilder<>& kb,
        std::map<std::string, llvm::Value*>& kernel_vars,
        const std::vector<llvm::Value*>& kernel_arg_values,
        const std::string& loop_var_name, llvm::Value* tid_val,
        llvm::BasicBlock* break_target = nullptr);
    std::vector<llvm::BasicBlock*> kernel_break_stack_; // for break/continue in kernel

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
    void setNamedTypedValue(const std::string& name, llvm::Value* ptr, llvm::Type* ty);
    llvm::Value* getNamedValue(const std::string& name);
    llvm::Type* getNamedValueType(const std::string& name);
    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func,
                                              llvm::Type* type,
                                              const std::string& name);

    // ---- Top-level generation ----
    void generateTranslationUnit(TranslationUnit& tu);
    // Build per-class Error vtables + the __myp_error_vtables global array.
    void generateErrorVTables();
    // True if name is the Error interface declared in this TU.
    bool isErrorInterface(const std::string& name);
    void createClassActionDecl(const ClassDecl& cls, const ActionDecl& action);
    void createStaticActionDecl(const ClassDecl& cls, const ActionDecl& action);
    void generateClass(const ClassDecl& decl);
    void generateClassAction(const ClassDecl& cls, const ActionDecl& action);
    void generateStaticAction(const ClassDecl& cls, const ActionDecl& action);
    void generateCoroBuiltin(const ClassDecl& cls, const ActionDecl& action);
    void generateCoroEntry(const ClassDecl& cls, const ActionDecl& action);
    llvm::Value* generateCoroSpawn(llvm::Function* target, const CallExpr& e,
                                   llvm::Value* mthis, bool is_method);
    llvm::Value* generateAwaitExpr(const AwaitExpr& e);
    llvm::Value* castToI64(llvm::Value* v);  // normalize any value to i64 slot
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
    void generateParallelFor(const ForStmt& stmt);
    void generateGpuFor(const ForStmt& stmt);
    bool generateGpuKernel(const ForStmt& stmt);
    void generateReturnStmt(const ReturnStmt& stmt);
    // @region memory arena: enter/exit and reference-type predicate
    void emitRegionEnter();
    void emitRegionExit();
    static bool typeIsReference(const TypeInfo& t);
    // Emit the actual function return (in_main cleanup + region exit + ret).
    void emitFunctionReturn(llvm::Value* ret_val);
    void generateBreakStmt(const BreakStmt& stmt);
    void generateContinueStmt(const ContinueStmt& stmt);
    void generateAwaitStmt(const AwaitStmt& stmt);

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
    llvm::Value* generateNewArrayExpr(const NewArrayExpr& expr);
    llvm::Value* generateThisExpr(const ThisExpr& expr);
    llvm::Value* generateAssignment(const AssignmentExpr& expr);
    llvm::Value* generateTernary(const TernaryExpr& expr);
    llvm::Value* generateTryExpr(const TryExpr& expr);
    llvm::Value* generateRange(const RangeExpr& expr);
    llvm::Value* generateEnumVariant(const EnumVariantExpr& expr);
    llvm::Value* generateLambda(const LambdaExpr& expr);
    llvm::Value* generatePipe(const PipeExpr& expr);
    void generateFFIDecl(const FFIDecl& decl);

    // ---- Match codegen ----
    void generateMatchStmt(const MatchStmt& stmt);
    void generateTryStmt(const TryStmt& stmt);
    void generateThrowStmt(const ThrowStmt& stmt);
    // Pop this handler + longjmp to the next (outer) handler + unreachable.
    void emitExceptionRethrow();

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
