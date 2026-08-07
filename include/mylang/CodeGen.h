#ifndef MYLANG_CODEGEN_H
#define MYLANG_CODEGEN_H

#include "AST.h"
#include "DiagnosticEngine.h"
#include "Type.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Function.h>

#include <set>
#include <map>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include <string>
#include <optional>
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
    std::unordered_set<std::string> debug_declared_; // names with DWARF declared

    // ---- Class struct tracking ----
    // Maps class name → LLVM struct type
    std::unordered_map<std::string, llvm::StructType*> class_structs_;
    // Maps class name → property name → index in struct
    std::unordered_map<std::string, std::unordered_map<std::string, unsigned>> property_indices_;
    // Anonymous tuple structs, keyed by element LLVM type signature.
    std::unordered_map<std::string, llvm::StructType*> tuple_structs_;

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

    // ---- Generic type-param mapping (set per class generation) ----
    // Maps a generic type param name (e.g. "T") to its concrete TypeNode for the
    // currently generated class. Empty for non-generic / template classes.
    std::vector<std::pair<std::string, TypeNode>> current_type_params_;

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
    // ARC (§五-1)
    llvm::Function* runtime_alloc_object_ = nullptr;
    llvm::Function* runtime_retain_ = nullptr;
    llvm::Function* runtime_release_ = nullptr;
    llvm::Function* runtime_free_object_ = nullptr;
    llvm::GlobalVariable* release_table_gv_ = nullptr;
    // ARC scope tracking: per-scope list of local reference slots to release
    // at scope exit (parallel to named_values_). kind: 0=class ptr,
    // 1=interface fat ptr, 2=function value fat ptr {closure, call_fn}.
    struct ArcSlot { llvm::Value* alloca; int kind; };
    std::vector<std::vector<ArcSlot>> arc_scope_slots_;
    // Exception unwinding (§五-1 剩余项): when a throw longjmps to a catch, the
    // scope-exit releases inside the abandoned try block are skipped → locals
    // leak. Each try records the ARC slots registered while inside it; the
    // dispatch/propagate path releases them, and outward rethrow sites release
    // the remaining (outer) slots. A slot is collected only into the INNERMOST
    // active try's list so inner-no-match→outer-catch never double-releases.
    struct TryUnwindCtx { std::vector<ArcSlot> inner_slots; };
    std::vector<TryUnwindCtx> try_ctx_stack_;
    // Emit myp_release for every slot collected in the innermost try's unwind
    // list (called at the top of the dispatch/propagate exception paths).
    void emitReleaseTryInnerSlots();
    // Drop a slot from the innermost try's unwind list after its scope exits
    // NORMALLY (popScope) — otherwise the dispatch path would release it a
    // second time (double free → release of a freed pointer).
    void removeTryUnwindSlot(llvm::Value* alloca);
    // Release the current function's still-live slots before an outward
    // longjmp. `rethrow_site` true when propagating an existing exception:
    // release whenever there is no ENCLOSING same-function try (size<=1);
    // for a fresh throw release only when there is no same-function try at all.
    void emitUnwindRelease(bool rethrow_site);
    // Source-level return type of the current function (for retain-at-return).
    TypeInfo current_ret_ti_;
    void registerArcSlot(llvm::Value* alloca, int kind);
    void releaseArcSlot(llvm::Value* alloca, int kind);
    // ARC coroutine-frame registry (§五-1 收尾): every local ARC slot inside a
    // @coro body mirrors the object it currently holds into the coroutine's
    // runtime frame list (set at each store, clear at every release — all
    // normal releases funnel through releaseArcSlot). On Coro.destroy or an
    // uncaught exception the runtime releases the still-live objects, because
    // those paths longjmp/skip the normal scope-exit epilogue (frame objects
    // would leak). We mirror OBJECT pointers (not stack addresses) so release
    // stays safe after the exception has unwound/reused the coroutine stack.
    llvm::Function* runtime_coro_frame_set_ = nullptr;
    llvm::Function* runtime_coro_frame_clear_ = nullptr;
    void emitCoroFrameSet(llvm::Value* alloca, llvm::Value* obj);
    void emitCoroFrameClear(llvm::Value* alloca);
    llvm::Value* emitRetain(llvm::Value* data);
    // ARC store into a strong reference slot (local alloca or property GEP):
    // retain(new) unless fresh, release(old), caller then stores new.
    void arcStoreRef(llvm::Value* slot, llvm::Value* new_val,
                     bool is_interface, bool is_fresh);
    // True if `alloca` is a currently-scoped local class reference slot.
    bool isArcClassLocal(llvm::Value* alloca);
    // True if `alloca` is a currently-scoped local function-value (closure) slot.
    bool isArcFunctionLocal(llvm::Value* alloca);
    // True for NewExpr / CallExpr / LambdaExpr results: transfer (no retain)
    // at a strong slot.
    static bool isFreshArcExpr(const Expr& e);
    // §五-5 形态3: is the awaited operand a call to an @async-annotated
    // function/static method (an await-able async IO operation)?
    bool isAsyncCallTarget(const Expr* callee) const;
    // ---- Statement-end temporary release (§五-1 M-ARC-2) ----
    // A `new` expression's fresh object is owned by the current statement; if a
    // store site takes it (transfer) it calls arcConsumeTemp; otherwise
    // arcFlushTemps (end of every statement) releases the leftover temporaries.
    std::vector<llvm::Value*> arc_pending_temps_;
    bool arc_skip_retain_return_ = false;
    void arcPushTemp(llvm::Value* v);
    void arcConsumeTemp(llvm::Value* v);
    void arcFlushTemps();
    // Release every live scope's local reference slots (function epilogue —
    // called before a return so locals that would otherwise be skipped by the
    // dead-path popScope are freed; retain-at-return already +1'd the result).
    void arcReleaseAllScopes();
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
    llvm::Function* runtime_io_current_handle_ = nullptr;
    llvm::Function* runtime_io_select_ = nullptr;
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
    llvm::Function* runtime_ord_ = nullptr;

    // ---- RTTI (§五-4) ----
    llvm::Function* runtime_type_id_ = nullptr;
    llvm::Function* runtime_type_name_ = nullptr;

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
    // §五-1 收尾 bug fix: instance globals created on-the-fly for a plain local
    // `X v = new X()` are transient (mapping convenience only) and must NOT
    // retain. A later var in the same TU with the same name used to see the
    // global as "pre-existing" and retain into it → the object leaked (rc 2).
    std::unordered_set<std::string> class_inst_globals_transient_;
    // ---- Static property globals: "ClassName_propName" -> GlobalVariable ----
    std::unordered_map<std::string, llvm::GlobalVariable*> static_property_globals_;

    // ---- Variable name → class name map (for method resolution) ----
    std::unordered_map<std::string, std::string> var_class_map_;
    /// Track slice element types for local slice variables ("name" -> TypeInfo).
    std::unordered_map<std::string, TypeInfo> var_slice_types_;
    /// Track function-typed variables ("name" -> Function TypeInfo) for call dispatch.
    std::unordered_map<std::string, TypeInfo> func_val_types_;
    /// Track element types for local array variables (for subscript codegen).
    std::unordered_map<std::string, llvm::Type*> array_elem_types_;
    /// ARC: whether a local array variable's elements are class references.
    std::unordered_map<std::string, bool> array_elem_is_class_;
    std::unordered_map<std::string, llvm::Type*> var_value_types_;

    // ---- Global event ID map: "ClassName::eventName" -> int ----
    std::unordered_map<std::string, int> event_id_map_;

    // ---- Static action tracking: "ClassName_action" -> true/false ----
    std::unordered_map<std::string, bool> is_static_action_;

    // ---- Coroutine methods: "ClassName_method" (only @coro annotated) ----
    std::unordered_set<std::string> coro_methods_;
    // @coro(stack=N) → stack size in KB (0 = default 128KB), keyed by "Class_method"
    std::unordered_map<std::string, int> coro_stack_map_;
    // True while generating the body of an @coro method (return stores result)
    bool current_is_coro_ = false;

    // ---- Global event name→id table (runtime-constructed timers) ----
    llvm::Value* event_table_names_ = nullptr;
    llvm::Value* event_table_ids_ = nullptr;
    int event_table_count_ = 0;
    void buildEventNameTable();

    // ---- GPU / CUDA offload ----
    std::string ptx_code_;  // Generated PTX for @gpu for kernels
    bool cuda_enabled_ = false;
    // Track array byte sizes for GPU data transfer
    std::unordered_map<std::string, llvm::Value*> array_byte_sizes_;
    /// Fixed-array local variable name → byte size（用于 return 时堆拷贝，避免悬垂指针）
    std::unordered_map<std::string, uint64_t> stack_array_sizes_;

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
    /// Mangle a TypeNode to the same string sema uses for generic-instance
    /// names (typeName(typeNodeToTypeInfo(...))), resolving generic type-param
    /// placeholders (e.g. `R`) against current_type_params_ first.
    std::string mangleConcreteTypeNode(const TypeNode& node);
    llvm::Type* typeNodeToLLVMType(const TypeNode& tn);
    const TypeAliasDecl* findAlias(const std::string& name) const;
    void declareFuncSignature(const FuncDecl& decl);
    // Function value = fat pointer { closure ptr, call_fn ptr }
    llvm::StructType* getFunctionValueType();
    void generateLambdaTramp(const ClassDecl& cls);

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
    // ARC (§五-1): per-class destroy stubs + __myp_release_table.
    void generateArcSupport(TranslationUnit& tu);
    // True if a TypeNode is a class instance / interface reference slot (ARC-counted).
    bool isArcRefType(const TypeNode& tn);
    // Emit myp_release(load of local alloca) if it holds a class ref.
    void maybeReleaseLocal(const std::string& name, llvm::Value* alloca);
    // True if name is the Error interface declared in this TU.
    bool isErrorInterface(const std::string& name);
    void createClassActionDecl(const ClassDecl& cls, const ActionDecl& action);
    void createStaticActionDecl(const ClassDecl& cls, const ActionDecl& action);
    void createClassFunctionDecl(const ClassDecl& cls, const FuncDecl& fn);
    // trait 默认实现（§三-5）：接口方法带默认体 → 按实现类特化的默认函数
    // `__ifdef_<Iface>_<method>_<Class>`（this 绑定具体类，this.method() 静态解析）
    void createClassDefaultDecl(const ClassDecl& cls, const InterfaceDecl& iface, const ActionDecl& action);
    void generateClassDefaultAction(const ClassDecl& cls, const InterfaceDecl& iface, const ActionDecl& action);
    // trait 默认函数名：__ifdef_<Iface>_<method>_<Class>
    std::string ifaceDefaultName(const std::string& iface, const std::string& method,
                                 const std::string& cls);
    // 查找类未覆盖的接口默认函数（无 → nullptr）
    llvm::Function* findInterfaceDefault(const std::string& cls_name, const std::string& method);
    // 关联类型 X::Item → 绑定类型（X 为具体类或当前类型参数；无绑定 → nullopt）
    std::optional<TypeNode> resolveAssocType(const std::string& owner, const std::string& member);
    void generateClass(const ClassDecl& decl);
    void generateClassAction(const ClassDecl& cls, const ActionDecl& action);
    void generateStaticAction(const ClassDecl& cls, const ActionDecl& action);
    void generateCoroBuiltin(const ClassDecl& cls, const ActionDecl& action);
    void generateCoroEntry(const ClassDecl& cls, const ActionDecl& action);
    void generateCoroFuncEntry(const FuncDecl& decl);  // top-level @coro function
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
    void generateDestructureStmt(const DestructureStmt& stmt);
    void generateIfStmt(const IfStmt& stmt);
    void generateWhileStmt(const WhileStmt& stmt);
    void generateForStmt(const ForStmt& stmt);
    void generateForInStmt(const ForInStmt& stmt);
    void generateParallelFor(const ForStmt& stmt);
    void generateGpuFor(const ForStmt& stmt);
    bool generateGpuKernel(const ForStmt& stmt);
    void generateReturnStmt(const ReturnStmt& stmt);
    /// 若返回表达式是固定数组栈变量，则堆拷贝后返回新指针（修复返回悬垂/共享存储）
    llvm::Value* heapCopyArrayReturn(llvm::Value* v, const Expr* value_expr);
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
    /// 逻辑 && / || 短路求值（a 为 false/true 时不再求值 b）
    llvm::Value* generateShortCircuitLogic(const BinaryOpExpr& expr);
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
    llvm::Value* generateTupleExpr(const TupleExpr& expr);
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
    bool debug_mode_ = false;
    std::string myp_passes_; // --passes=<name>: run custom MYP pass pipeline

    // ---- DWARF debug info (DIBuilder) ----
    std::unique_ptr<llvm::DIBuilder> dbg_builder_;
    llvm::DICompileUnit* dbg_cu_ = nullptr;
    llvm::DIFile* dbg_file_ = nullptr;
    llvm::DISubprogram* debug_scope_ = nullptr;

    // ---- Debug helpers ----
    void initDebugInfo(const std::string& filename);
    llvm::DIType* getDebugType(llvm::Type* ty, unsigned line = 0);
    void setDebugLoc(const SourceRange& r);
    void beginFunctionDebug(llvm::Function* func, const std::string& name,
                            const SourceRange& r);
    void endFunctionDebug();
    void emitParamDebug(llvm::Value* alloca, const std::string& name,
                        llvm::Type* ty, unsigned line, unsigned arg_idx);
    void emitScopeLocalsDebug();
    void finalizeDebugInfo();

public:
    void setEmitLLVM(bool v) { emit_llvm_ = v; }
    void setLibraryMode(bool v) { library_mode_ = v; }
    void setTestMode(bool v) { test_mode_ = v; }
    void setDebugMode(bool v) { debug_mode_ = v; }
    void setMypPasses(const std::string& v) { myp_passes_ = v; }
    bool saveIR(const std::string& path) const;

    // ---- Helper: find struct decl ----
    const StructDecl* findStruct(const std::string& name) const;
};

} // namespace mylang

#endif // MYLANG_CODEGEN_H
