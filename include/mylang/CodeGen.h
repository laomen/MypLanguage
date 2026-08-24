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

// Small shared integer-conversion helpers used across the split codegen TUs.
// Definitions live in src/codegen/codegen.cpp (not file-static, so the
// per-concern translation units can call them).
llvm::Value* convertIntegerValue(llvm::IRBuilder<>& b, llvm::Value* v,
                                 llvm::Type* expected, const Expr* src);
llvm::Value* zextIndexValue(llvm::IRBuilder<>& b, llvm::Value* v);

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
    std::unordered_map<std::string, std::unordered_map<std::string, llvm::Type*>> property_types_;
    struct InterfaceMethodInfo {
        unsigned index;
        const ActionDecl* action;
    };
    std::unordered_map<std::string,
        std::unordered_map<std::string, InterfaceMethodInfo>> interface_methods_;
    std::unordered_map<std::string, InterfaceMethodInfo> interface_method_fallback_;
    // Anonymous tuple structs, keyed by element LLVM type signature.
    std::unordered_map<std::string, llvm::StructType*> tuple_structs_;

    // Enum structs: { i32 disc, [N x i8] payload } keyed by enum name.
    std::unordered_map<std::string, llvm::StructType*> enum_structs_;
    // Set of all enum struct types (for disc-extraction in equality).
    std::unordered_set<llvm::StructType*> enum_struct_set_;

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
    bool regionBodyMayEscape(const Stmt& body) const;

    // ---- Struct type tracking ----
    // Maps struct name (e.g. "Vec2" or "Sensor::Config") → LLVM struct type
    std::unordered_map<std::string, llvm::StructType*> struct_types_;
    // Maps struct name → field name → index
    std::unordered_map<std::string, std::unordered_map<std::string, unsigned>> struct_field_indices_;
    // Local variable/param name → struct type name (for struct method dispatch:
    // `h.method()` needs to know `h` is a `Holder` struct, not a class).
    std::unordered_map<std::string, std::string> var_struct_map_;
    // Struct field name → its TypeNode (bare field names inside struct methods
    // are registered as named GEPs; this lets assignments retain/consume ARC
    // refs stored into `this.field` instead of plain-storing a fresh temp).
    std::unordered_map<std::string, TypeNode> struct_field_types_;

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
    // Mark `slot` as escaped memory via the myp_try_escape no-op: prevents
    // mem2reg/SROA from promoting it to SSA and DSE from deleting stores to it.
    // Used for every local live across a setjmp/longjmp try (see
    // generateTryStmt), so the finally/catch paths read the true physical value
    // after a longjmp (LLVM's CFG does not model the longjmp edge).
    void escapeSlot(llvm::Value* slot);
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
    // For @coro bodies: params and `this` are borrowed from the caller, but the
    // coroutine outlives the caller's scope. Retain each ARC param and register
    // it as a scope slot (released at normal completion) + mirror it into the
    // coroutine frame registry (released on Coro.destroy / uncaught exception).
    // Without the retain, an object held only by a coroutine param (e.g. a
    // Channel) is freed when the caller rebinds its variable, and the parked
    // coroutine later reads a reused object (BUG-002).
    void registerCoroParam(const TypeNode& tn, const TypeInfo& ti,
                           llvm::Value* alloca, llvm::Value* val);
    llvm::Value* emitRetain(llvm::Value* data);
    // M8: retain the counted backing of a slice value ({data, len} fat pointer).
    llvm::Value* emitRetainSlice(llvm::Value* slice_val);
    void arcStoreSlice(llvm::Value* slot, llvm::Value* new_val, bool is_fresh);
    bool isArcSliceLocal(llvm::Value* alloca);
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
    bool isFreshArcExpr(const Expr& e);
    // M7: is `obj.weakProp` a read of a @weak property (fresh strong upgrade)?
    bool isWeakMemberAccess(const MemberAccessExpr& e);
    // M7: load a class property field — weak → myp_weak_load (fresh strong
    // temp), strong → plain load (borrowed).
    llvm::Value* loadPropertyField(llvm::Value* gep, const ClassDecl& cls,
                                   const std::string& member_name);
    // M7: store into a class property field. Returns true if the field is weak
    // (handled via myp_weak_store; caller must skip the strong arcStoreRef).
    bool storePropertyField(llvm::Value* gep, llvm::Value* v,
                            const ClassDecl& cls, const std::string& member_name);
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
    // M7: release temps created during a condition evaluation (the branch uses
    // only the derived i1) — prevents a conditional-block flush from leaking
    // them on the other path.
    void arcReleaseConditionTemps(size_t before);
    void arcFlushTemps();
    // Release branch-created temporaries inside the branch block; transfer the
    // branch result if it is a fresh class-ref temp (returns it for the merge
    // phi to take ownership). See arcEndBranch in codegen.cpp.
    llvm::Value* arcEndBranch(size_t before, llvm::Value* branch_result);
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
    // BUG-005: mapping handler 跨线程路由（目标实例线程检查 + 路由投递）
    llvm::Function* runtime_thread_is_current_ = nullptr;
    llvm::Function* runtime_event_route_inst_ = nullptr;
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
    llvm::Function* runtime_charcode_ = nullptr;
    // In-place string append (`s = s + x` fast path, M4): runtime myp_str_append
    llvm::Function* runtime_str_append_ = nullptr;
    // M7 weak references: runtime myp_weak_store / myp_weak_load / myp_weak_clear
    llvm::Function* runtime_weak_store_ = nullptr;
    llvm::Function* runtime_weak_load_ = nullptr;
    llvm::Function* runtime_weak_clear_ = nullptr;

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
    llvm::Function* runtime_gpu_force_cpu_ = nullptr;
    llvm::Function* runtime_gpu_alloc_ = nullptr;
    llvm::Function* runtime_gpu_free_ = nullptr;
    llvm::Function* runtime_gpu_to_device_ = nullptr;
    llvm::Function* runtime_gpu_to_host_ = nullptr;
    llvm::Function* runtime_gpu_to_host_async_ = nullptr;
    llvm::Function* runtime_gpu_load_kernel_ = nullptr;
    llvm::Function* runtime_gpu_launch_ = nullptr;
    llvm::Function* runtime_gpu_destroy_kernel_ = nullptr;
    // §8.4 unique 模式索引校验失败（越界/重复）→ 打印并退出（noreturn）
    llvm::Function* runtime_gpu_scatter_check_fail_ = nullptr;
    // CUDA device info
    llvm::Function* runtime_cuda_count_ = nullptr;
    llvm::Function* runtime_cuda_name_ = nullptr;
    llvm::Function* runtime_cuda_memory_ = nullptr;
    llvm::Function* runtime_cuda_capability_ = nullptr;
    llvm::Function* runtime_cuda_multiprocessors_ = nullptr;
    llvm::Function* runtime_cuda_max_threads_ = nullptr;
    llvm::Function* runtime_cuda_warp_ = nullptr;
    // §7.4 厂商探测 + 能力查询（vendor-neutral __myp_gpu_* 前缀）
    llvm::Function* runtime_gpu_vendor_ = nullptr;
    llvm::Function* runtime_gpu_gfx_arch_ = nullptr;
    llvm::Function* runtime_gpu_shared_per_block_ = nullptr;
    llvm::Function* runtime_gpu_regs_per_block_ = nullptr;
    llvm::Function* runtime_gpu_max_grid_dim_ = nullptr;
    llvm::Function* runtime_gpu_max_block_dim_ = nullptr;
    llvm::Function* runtime_gpu_clock_mhz_ = nullptr;
    llvm::Function* runtime_gpu_concurrent_kernels_ = nullptr;
    llvm::Function* runtime_gpu_mem_alignment_ = nullptr;
    llvm::Function* runtime_gpu_double_precision_ = nullptr;
    llvm::Function* runtime_gpu_atomics64_ = nullptr;
    // §P5 ② kernel printk/assert 调试
    llvm::Function* runtime_printf_ = nullptr;            // myp_printf（宿主，CPU 回退）
    llvm::Function* runtime_assert_abort_ = nullptr;      // myp_assert_abort（CPU 回退 assert 硬失败）
    llvm::Function* runtime_gpu_flush_printf_ = nullptr;  // myp_gpu_flush_printf（staging 回读）
    llvm::Function* runtime_gpu_printf_buf_ = nullptr;    // 设备缓冲/计数器指针访问器
    llvm::Function* runtime_gpu_printf_cnt_ = nullptr;
    llvm::Function* runtime_gpu_printf_fail_ = nullptr;
    // per-kernel printk 状态（generateGpuKernel 内重置）
    std::vector<std::string> gpu_kernel_fmts_;           // fmt_id → 格式串
    std::map<std::string, int> gpu_kernel_fmt_id_;       // 格式串 → fmt_id
    bool gpu_kernel_printf_used_ = false;

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
    // IR-level escape barrier for try-inner ARC slots (§五-3): keeps them as
    // escaped memory so the -O pipeline can't fold dispatch loads to undef.
    llvm::Function* runtime_try_escape_ = nullptr;
    // Reads a local ARC slot's physical memory in the runtime and releases the
    // object (§五-3 × -O): LLVM cannot fold this opaque read on the longjmp path.
    llvm::Function* runtime_release_slot_ = nullptr;
    llvm::StructType* jmp_buf_type_ = nullptr;
    llvm::GlobalVariable* global_jmp_buf_ = nullptr;
    // class name → compile-time exception type ID (>0; 0 = string message)
    std::unordered_map<std::string, int> class_type_ids_;
    // Global array __myp_error_vtables[type_id] → Error vtable ptr (null if the
    // class does not implement the Error interface). Used by catch (Error e).
    llvm::GlobalVariable* error_vtables_gv_ = nullptr;

    // ---- Test framework runtime functions ----
    llvm::Function* runtime_assert_ = nullptr;
    llvm::Function* runtime_assert_msg_ = nullptr;
    llvm::Function* runtime_test_set_msg_ = nullptr;
    llvm::Function* runtime_assert_eq_ = nullptr;
    llvm::Function* runtime_assert_str_eq_ = nullptr;
    llvm::Function* runtime_assert_neq_ = nullptr;
    llvm::Function* runtime_assert_long_eq_ = nullptr;
    llvm::Function* runtime_assert_str_neq_ = nullptr;
    llvm::Function* runtime_test_report_ = nullptr;
    llvm::Function* runtime_test_fail_msg_ = nullptr;
    llvm::Function* runtime_test_summary_ = nullptr;
    llvm::Function* runtime_assert_long_neq_ = nullptr;
    llvm::Function* runtime_assert_float_neq_ = nullptr;
    llvm::Function* runtime_assert_null_ = nullptr;
    llvm::Function* runtime_assert_not_null_ = nullptr;
    // @test 输出捕获（阶段 1）
    llvm::Function* runtime_test_capture_start_ = nullptr;
    llvm::Function* runtime_test_capture_stop_ = nullptr;
    llvm::Function* runtime_test_capture_get_ = nullptr;
    llvm::Function* runtime_test_capture_contains_ = nullptr;
    llvm::Function* runtime_test_capture_eq_ = nullptr;

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
    /// Interface variables that have been reassigned (devirt safety: a
    /// reassigned interface variable's concrete class is no longer statically
    /// known → keep vtable dispatch instead of devirtualizing).
    std::set<std::string> iface_reassigned_;
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
    // 局部动态数组的字节大小缓存：键=变量名，值=(所属函数, 字节数)。
    // 按函数作用域：不同函数同名的局部数组互不污染（M4 GpuOps 大量用 a/b/o
    // 等短名局部，若无函数作用域会让后续函数的同名参数读到别人的字节数 →
    // @gpu for 少传/多传数据）。
    std::unordered_map<std::string, std::pair<llvm::Function*, llvm::Value*>> array_byte_sizes_;
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
    // 是否正在编译 GPU kernel body（@gpu for / @gpu tile）。emitKernelExpr 用
    // 它判断是否走 libdevice 数学/内核语义；@gpu tile 的 body 无 ForStmt。
    bool gpu_kernel_mode_ = false;
    // Set when the GPU kernel body uses a math function that requires CUDA
    // libdevice (sin/cos/tan/exp/log/pow). The runtime does not link libdevice,
    // so such kernels must fall back to CPU.
    bool gpu_math_unsupported_ = false;
    // Set when the GPU kernel body emits a call to a __nv_* libdevice function.
    // When true, CUDA libdevice.10.bc is linked into the kernel module at
    // compile time so the generated PTX is self-contained.
    bool gpu_math_used_ = false;
    // §3.1 kernel 执行上下文（@gpu for 内核内当前块/线程值，generateGpuKernel
    // 填充，emitKernelExpr 读取）：tid_x=threadIdx.x、ntid=blockDim.x、
    // ctaid=blockIdx.x、tid=全局线程 id（=循环变量 p）、n_arg=循环上界。
    llvm::Value* gpu_ctx_tid_x_ = nullptr;
    llvm::Value* gpu_ctx_ntid_ = nullptr;
    llvm::Value* gpu_ctx_ctaid_ = nullptr;
    llvm::Value* gpu_ctx_tid_ = nullptr;
    llvm::Value* gpu_ctx_n_arg_ = nullptr;
    // CPU 回退（运行时无 GPU / MYP_GPU 未设）时模拟 kernel 上下文：
    // 置标志后普通 generateForStmt 编译 body，kernel.gid/tx/bx/bd/gx 按
    // 顺序循环变量 p 计算（gid=p、tx=p%256、bx=p/256、bd=256、gx=ceil(n/256)），
    // kernel.sync() 为空操作。
    // §3.1 CPU 回退模拟：bd = 块大小（@gpu block(n)，默认 256）
    int gpu_cpu_block_ = 256;
    bool gpu_cpu_fallback_ = false;
    std::string gpu_cpu_loop_var_;
    llvm::Value* gpu_cpu_bound_ = nullptr;
    // §3.2 当前 @gpu tile 的共享数组：名字 → 元素 LLVM 类型（emitKernelExpr
    // Subscript 用它确定 GEP/load 的元素类型；名字不进入捕获参数）。
    std::map<std::string, llvm::Type*> gpu_shared_arrays_;

    // AST walk helpers for GPU kernel body compilation
    void collectExprIdentifiers(const Expr& expr, std::set<std::string>& out,
                                std::set<std::string>& loop_decls) const;
    void collectStmtIdentifiers(const Stmt& stmt, std::set<std::string>& out,
                                std::set<std::string>& loop_decls) const;
    void analyzeGpuCapturedVars(const ForStmt& stmt, const std::string& loop_var);
    void analyzeGpuTileCapturedVars(const GpuTileStmt& stmt);
    // §3.2 @gpu tile：共享内存协作 kernel（PTX + launch + CPU 回退）
    void generateGpuTile(const GpuTileStmt& stmt);
    // §8.2 @gpu reduce：声明式归约（grid 分块 + host 合并；CPU 回退顺序 fold）
    void generateGpuReduce(const GpuReduceStmt& stmt);
    // §8.2 @gpu reduce 表达式形式：合成临时标量承接结果，复用 generateGpuReduce
    llvm::Value* generateGpuReduceExpr(const GpuReduceExpr& expr);
    // §8.3 @gpu scan：声明式前缀和（两遍：K1 块和 + host 块前缀 + K2 块内 scan）
    void generateGpuScan(const GpuScanStmt& stmt);
    // §8.4 @gpu scatter：声明式散点（grid-stride 写 kernel；unique 预扫校验；
    // atomic_add 原子；CPU 回退顺序写/累加）
    void generateGpuScatter(const GpuScatterStmt& stmt);
    // §8.2/8.3 K1 块和 kernel PTX：每块 tx==0 串行归约块内区间 → partials[bid]
    std::string emitBlockSumPtx(const Expr& op_expr, const Expr& init_expr,
                                llvm::Type* elem_ty, int block_size,
                                const std::string& kernel_name);    // §8.6 块内并行：ping-pong 共享内存 halving 树（BS 线程协作，块大小须 2 的幂）
    std::string emitBlockSumTreePtx(const Expr& op_expr, const Expr& init_expr,
                                    llvm::Type* elem_ty, int block_size,
                                    const std::string& kernel_name);    // §8.3 K2 块内 scan kernel PTX：acc = init⊕offsets[bid]，扫块内写 b[i]=acc
    //（exclusive=true：b[i]=acc 取更新前 → 不含自身，b[块首]=offsets[bid]）
    std::string emitScanK2Ptx(const Expr& op_expr, const Expr& init_expr,
                              llvm::Type* elem_ty, int block_size, bool exclusive);
    // §8.3 K2 块内 scan kernel PTX（Hillis-Steele 并行版，inclusive；块大小 2 的幂）
    std::string emitScanK2HsPtx(const Expr& op_expr, const Expr& init_expr,
                                llvm::Type* elem_ty, int block_size);
    // §8.2 host 顺序归约：acc=init（或 src[0]）；for i in [start,cnt): x=src[i]; acc=op(acc,x)；out=acc
    void emitSeqFold(llvm::Value* src, llvm::Value* cnt, llvm::Type* elem_ty,
                     const GpuReduceStmt& stmt, llvm::Value* out_slot, bool use_init);
    // §8.6 规范归约顺序：L1 每块顺序归约（同 GPU K1 分块：partials[j] = fold(init,
    // a[j*bs .. min((j+1)*bs, cnt)))）→ L2/L3 顺序合并 partials（同 GPU host 合并）。
    // GPU 与 CPU 按同一组合顺序 → 浮点位级一致（可双实现 diff 测试）。
    void emitSeqBlockReduce(llvm::Value* a_src, llvm::Value* cnt,
                            llvm::Value* blocks, llvm::Value* bs,
                            llvm::Type* elem_ty, const GpuReduceStmt& stmt,
                            llvm::Value* out_slot);
    // §8.6 块内并行 CPU 镜像：同 emitBlockSumTreePtx 的 halving 树（bs 须 2 的幂）
    void emitSeqBlockTreeReduce(llvm::Value* a_src, llvm::Value* cnt,
                                llvm::Value* blocks, int bs,
                                llvm::Type* elem_ty, const GpuReduceStmt& stmt,
                                llvm::Value* out_slot);
    // §8.3 host 顺序前缀扫描：acc=init；for i in [0,cnt): x=src[i]; acc=op(acc,x)；dst[i]=acc
    void emitSeqScan(llvm::Value* src, llvm::Value* dst, llvm::Value* cnt,
                     llvm::Type* elem_ty, const GpuScanStmt& stmt);
    // §8.3 块内并行 CPU 镜像（inclusive）：块和 → offsets → 每块 Hillis-Steele →
    // b = offsets[j]∘local（同 GPU K1+HS 顺序 → 位级一致；bs 须 2 的幂）
    void emitSeqScanBlocked(llvm::Value* a_src, llvm::Value* b_src,
                            llvm::Value* cnt, llvm::Value* blocks, int bs,
                            llvm::Type* elem_ty, const GpuScanStmt& stmt);
    // §8.4 scatter 写 kernel PTX（grid-stride）：b[idx[p]] = a[p]
    //（unique/any）；atomic=true 用 atomicrmw（atomic_add）。
    std::string emitScatterPtx(llvm::Type* elem_ty, int block_size,
                               bool atomic_add, const std::string& kernel_name);
    // §8.4 host 顺序散点（CPU 回退）：b[idx[lo_i+p]] = a[lo_a+p]（unique/any）；
    // atomic_add 顺序累加（规范顺序）。
    void emitSeqScatter(llvm::Value* a_src, llvm::Value* idx_src,
                        llvm::Value* b_src, llvm::Value* cnt,
                        llvm::Type* elem_ty, bool atomic_add);
    // §8.4 unique 模式 host 预扫：越界/重复 → runtime_gpu_scatter_check_fail_。
    // idx 元素 i32；b 目标数据指针（长度从 backing 头 -24 读）。
    void emitScatterIdxCheck(llvm::Value* idx_src, llvm::Value* cnt,
                             llvm::Value* b_src);
    // §3.2 @gpu tile CPU 回退（降级）：单线程执行 body，共享数组 = host 栈数组
    void generateGpuTileCpuFallback(const GpuTileStmt& stmt);

    // Kernel body codegen (uses kb on PTX module)
    llvm::Value* emitKernelExpr(const Expr& expr, llvm::IRBuilder<>& kb,
        std::map<std::string, llvm::Value*>& kernel_vars,
        const std::vector<llvm::Value*>& kernel_arg_values,
        const std::string& loop_var_name, llvm::Value* tid_val);
    // §P5 ② kernel.printk / kernel.assert：kernel 内写 staging 记录（设备全局
    // myp_pbuf + 原子槽位 myp_pcnt；assert 失败置 myp_pfail）。
    llvm::Value* emitKernelPrintk(const CallExpr& e, llvm::IRBuilder<>& kb,
        std::map<std::string, llvm::Value*>& kernel_vars,
        const std::vector<llvm::Value*>& kernel_arg_values,
        const std::string& loop_var_name, llvm::Value* tid_val, bool is_assert);
    // §P5 ② kernel.printk / kernel.assert CPU 回退：宿主 myp_printf / 硬失败。
    llvm::Value* emitCpuPrintk(const CallExpr& e, bool is_assert);
    void emitKernelStmt(const Stmt& stmt, llvm::IRBuilder<>& kb,
        std::map<std::string, llvm::Value*>& kernel_vars,
        const std::vector<llvm::Value*>& kernel_arg_values,
        const std::string& loop_var_name, llvm::Value* tid_val,
        llvm::BasicBlock* break_target = nullptr);
    std::vector<llvm::BasicBlock*> kernel_break_stack_; // for break/continue in kernel
    // Kernel-path element ADDRESS for `arr[idx]` where arr is a slice variable
    // (unpacks {data,len}, bounds-checks) or a plain array variable (direct GEP).
    // Backs `v[i].field` read/write inside a @parallel for body.
    llvm::Value* emitKernelElementAddr(const Expr* arr_expr, llvm::Value* idx,
        llvm::IRBuilder<>& kb, std::map<std::string, llvm::Value*>& kernel_vars,
        const std::vector<llvm::Value*>& kernel_arg_values,
        const std::string& loop_var_name, llvm::Value* tid_val);

    // ---- Class-related methods ----
    void buildClassStructTypes(TranslationUnit& tu);
    llvm::StructType* getClassStruct(const std::string& name);
    bool getPropertyIndex(const std::string& class_name, const std::string& prop_name, unsigned& idx);
    llvm::Type* getPropertyType(const ClassDecl& cls, const std::string& prop_name);
    const ClassDecl* findClass(const std::string& name);
    const InterfaceDecl* findInterface(const std::string& name);
    bool isInterfaceName(const std::string& name) const;

    // ---- Struct-related methods ----
    void buildStructTypes(TranslationUnit& tu);
    // Enum struct type: { i32 disc, [N x i8] payload }, built & cached per enum.
    llvm::StructType* getEnumStructType(const std::string& name);
    const EnumDecl* findEnum(const std::string& name) const;
    // Build an enum struct value: disc = variant_index, payload = packed args.
    llvm::Value* buildEnumVariant(const std::string& enum_name, size_t variant_index,
                                  const std::vector<llvm::Value*>& args);
    // Byte offset of field field_idx in a variant's packed payload.
    uint64_t enumPayloadOffset(const EnumVariant& v, size_t field_idx);
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

    // ---- M-FN-2 nonlocal（按引用捕获）----
    // 当前函数/action 中被 lambda `nonlocal` 捕获的变量名（codegen 序言把参数/
    // 局部提升为堆 cell；generateVarDecl/形参分配处读取）。
    std::set<std::string> current_fn_nonlocal_vars_;
    // nonlocal 变量名 → 其 cell 类（__cell_N），sema 注解于 decl.nonlocal_cell_class。
    std::map<std::string, std::string> current_fn_nonlocal_cell_class_;
    // 外层函数里 nonlocal 变量名 → cell 对象（供 generateLambda 捕获 cell 对象）。
    std::map<std::string, llvm::Value*> cell_owners_;
    // 把变量提升为堆 cell（共享可变）：分配 __cell_N、存初值、把属性 GEP 注册为
    // 命名值（T*，读写与栈 alloca 一致）、登记 cell 所有者 + ARC 作用域退出释放。
    llvm::Value* promoteNonlocalToCell(const std::string& name, llvm::Type* vt,
                                       llvm::Value* init);
    // lambda __call 开头注入 nonlocal 别名：把 this.cap_i（cell 对象）的属性 GEP
    // 注册为外层变量名，使 body 读写直达共享 cell。
    void setupNonlocalAliases(const ClassDecl& cls);
    // M3: guard a signed length/dimension against negative values — emit a
    // branch to myp_bounds_error (deterministic abort, no OOB) and return a
    // zero-extended i64 copy safe to multiply into a byte size. Accepts any
    // integer width (i32/i64/etc.).
    llvm::Value* guardNonNegativeLen(llvm::Value* v);
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
    // M8: dynamic T[] (array_size==0, any element) — the backing is a counted
    // ref now (myp_alloc_slice_backing / myp_alloc_class_array), so slots
    // holding it retain/release like a class ref. Fixed [N x T] are stack
    // values (not counted).
    bool isCountedArrayType(const TypeNode& tn);
    // M8 strings: the builtin string type (class_name empty, basic String).
    // Strings are ref-counted (counted header from myp_alloc), so string
    // locals/fields are strong ARC slots exactly like class refs.
    bool isStringType(const TypeNode& tn);
    // M8: any type whose value is a counted/ARC reference that flows through
    // retain-at-return: class, interface, slice, dynamic T[], and string.
    bool isArcReturnType(const TypeNode& tn);
    // M8 structs: a struct FIELD that holds an ARC reference (or is a nested
    // struct that transitively does) — needs retain/release on struct copies.
    bool isArcFieldType(const TypeNode& tn);
    // M8 structs: emit retain (retain=true) or release on one loaded field value.
    void emitArcFieldOp(llvm::IRBuilderBase& b, llvm::Value* field_val, const TypeNode& tn, bool retain);
    // M8 structs: operate on every ARC field of a struct VALUE (loaded value).
    void emitStructFieldsValue(llvm::IRBuilderBase& b, llvm::Value* struct_val, const StructDecl& sd, bool retain);
    // M8 structs: operate on every ARC field of a struct at an alloca/pointer.
    void emitStructFieldsPtr(llvm::IRBuilderBase& b, llvm::Value* struct_ptr, const StructDecl& sd, bool retain);
    // Owned struct locals (kind-5 ARC slots): alloca -> struct name. A struct
    // PARAM is borrowed and NOT here (its field stores stay plain copies).
    std::unordered_map<llvm::Value*, std::string> arc_struct_slot_types_;
    bool isOwnedStructLocal(llvm::Value* alloca);
    // True if tn is a class (not interface) reference — resolves generic type
    // params through current_type_params_. Used to decide ref-counted arrays.
    bool isArcClassType(const TypeNode& tn);
    // Fixed (stack) class-array slots: alloca → element count (kind-3 slots).
    std::unordered_map<llvm::Value*, uint64_t> arc_fixed_array_counts_;
    // Array variable → element class name (mangled for generics, e.g. Box_int_inst).
    // Used to resolve `arr[i].method()` method dispatch on class-element arrays.
    std::unordered_map<std::string, std::string> array_elem_class_map_;
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
    // 获取（或创建）(接口, 实现类) 的 vtable 全局：__myp_vtable_<Iface>_<Class>
    llvm::GlobalVariable* getOrCreateVtable(const std::string& iface_name, const std::string& cls_name);
    // 是否为接口胖指针类型 {ptr data, ptr vtable}
    static bool isInterfaceFatType(llvm::Type* ty);
    // 把具体类实例(ptr) 构造成接口胖指针 {data, vtable}（接口参数 upcast 用）
    llvm::Value* buildInterfaceFat(llvm::Value* inst, const std::string& iface_name,
                                   const std::string& cls_name);
    // BUG-034: 接口方法调用（vtable 动态分派）——接口形参的实参若为具体类实例
    // (ptr)，构造接口 fat {data, vtable} 替换 call_args[1+ai]。此前直接传裸 ptr，
    // 被调方法按 fat {ptr,ptr} 解释 → 参数错位/段错误。call_args[0] 为 this。
    void upcastIfaceCallArgs(std::vector<llvm::Value*>& call_args,
                             const CallExpr& e,
                             const InterfaceMethodInfo* method);
    const InterfaceMethodInfo* findInterfaceMethod(const std::string& iface_name,
                                                   const std::string& method) const;
    // BUG-017: 接口动态分派的返回类型。关联类型方法（`Item getVal()`）的接口声明返回
    // 类型是关联类型占位符 → typeNodeToCodegenType 回落 i32；动态分派须用具体类的返回
    // 类型（vtable 指向具体类方法）。已知具体类（var_class_map_ 等）时解析其同名方法，
    // 否则回落接口声明类型。
    llvm::Type* ifaceDispatchReturnType(const MemberAccessExpr& ma,
                                        const InterfaceMethodInfo* method);
    // 解析实参表达式的具体类名：new X / 局部变量 / 本类属性（无 → ""）
    std::string resolveArgClassName(const Expr& arg);
    // 解析被调函数第 rel 个参数声明的接口名（形参为接口类型时；无 → ""）
    std::string paramIfaceName(llvm::Function* cf, size_t rel);
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
    // Wrap one @test invocation with an exception handler so an uncaught
    // exception in a single test marks it FAIL and continues the runner,
    // instead of terminating the whole suite.
    void emitTestRunnerProtectedCall(llvm::Function* printf_fn,
                                     const std::string& label,
                                     const std::function<void()>& call_body);
    // @test runtime 函数声明 + __myp_* 测试 intrinsic 注册（定义于
    // codegen_test.cpp，由 declareRuntimeFunctions 调用）。
    void declareTestRuntimeFunctions();
    void registerTestIntrinsics();
    // Non-library builds: mark every function definition except `main` as
    // `internal` so LLVM's IPO (IPSCCP + inliner + vectorizer) can
    // constant-specialize and inline hot top-level kernels (e.g. benchmarks
    // called with constant args), which unlocks unrolling/vectorization.
    // Skipped in library mode (--shared/--static) where symbols are an API.
    void markNonMainFunctionsInternal();

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
    // §4.1 @gpu stream(s)：求值 GpuStream 实例的 handle()（long 句柄）。
    // 返回 i64；无 stream 时返回 0（默认流同步）。
    llvm::Value* emitGpuStreamHandle(const Expr* stream_expr);
    void generateReturnStmt(const ReturnStmt& stmt);
    /// 若返回表达式是固定数组栈变量，则堆拷贝后返回新指针（修复返回悬垂/共享存储）
    llvm::Value* heapCopyArrayReturn(llvm::Value* v, const Expr* value_expr);
    // @region memory arena: enter/exit and reference-type predicate
    void emitRegionEnter();
    void emitRegionExit();
    static bool typeIsReference(const TypeInfo& t);
    // Emit the actual function return (in_main cleanup + region exit + ret).
    void emitFunctionReturn(llvm::Value* ret_val, const Expr* src = nullptr);
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
    llvm::Value* generateConvert(const ConvertExpr& expr);
    llvm::Value* generateCall(const CallExpr& expr);
    llvm::Value* generateBitcast(const CallExpr& expr);
    llvm::Value* generateBytesStr(const CallExpr& expr, const std::string& name);
    llvm::Value* generateBytesOf(const CallExpr& expr);
    llvm::Value* generateParse(const CallExpr& expr, const std::string& name);
    // §6.2 P4 parseIntOpt：(value:int, ok:bool) 元组——myp_str_parse_int_opt(s,&ok)。
    llvm::Value* generateParseOpt(const CallExpr& expr);
    llvm::Value* generateBitOps(const CallExpr& expr, const std::string& name);
    // §9.5 多态数学 intrinsic（__myp_math_* 一元实数/abs/trunc）：按实参类型
    // 发 LLVM 标量 intrinsic（f32→llvm.sqrt.f32、f64→llvm.sqrt.f64；整型 abs
    // →llvm.abs；其余一元实数 → llvm.sin/exp/…）。返回 nullptr 表示不拦截
    //（pow/atan2/abs_int 走 runtime 调用）。
    llvm::Value* generatePolyMathIntrinsic(const CallExpr& expr, const std::string& name);
    // §3.6 向量打包访问：load4(float[] a, long i) : float4 / store4(..., float4 v)。
    // GEP + <4 x float> 打包 load/store（CPU 回退与 host 侧；GPU 走 emitKernelExpr）。
    llvm::Value* emitVec4Access(const CallExpr& expr, const std::string& name);
    // §4.2 P3 checked 溢出变体：checkedAdd/checkedMul → @llvm.sadd/smul.with.overflow
    // （按实参整型位宽选 iN，返回 {iN, i1} 结构体 = 元组 (value, overflow)）。
    llvm::Value* generateCheckedOp(const CallExpr& expr, const std::string& name);
    // True if a call returns an ARC-owned class / class-array reference (the
    // caller owns the returned +1 and must store it or release it).
    bool callReturnsArcRef(const CallExpr& e);
    // M8 structs: does this call return a struct whose fields hold ARC refs?
    // Used ONLY for `return call()` skip-retain (NOT generateCall temp-push —
    // structs aren't released as single refs).
    bool callReturnsArcStruct(const CallExpr& e);
    // M8: does this call return a slice or dynamic T[] (counted backing)?
    // Used ONLY for `return call()` skip-retain — a `return sliceCall()`/array
    // otherwise retain-at-return (+1) the backing the caller then drops to 1,
    // leaking one reference per call. Not used by generateCall temp-push.
    bool callReturnsArcSliceOrArray(const CallExpr& e);
    // M8: resolve the return TypeNode of a call (class method / free fn / FFI),
    // or nullptr if unresolvable. Used to detect string-producing exprs.
    const TypeNode* callReturnTypeNode(const CallExpr& e);
    // M8: does this expression yield a string value?
    bool exprIsString(const Expr& e);
    // M8: string-ness for ==/!=/</>/<: sema-resolved String OR generic
    // type-param placeholder fallback (see exprIsString).
    bool exprResolvedString(const Expr& e);
    // M8: is this `a + b` a string concatenation (fresh counted string)?
    bool isStringConcatExpr(const Expr& e);
    // M4: convert a scalar operand to a string for concatenation (bool/byte/
    // short/int/long/double → myp_to_string_*). Pointer operands pass through
    // (borrowed). Result is a fresh counted string only when converted.
    llvm::Value* stringifyForConcat(llvm::Value* v, const Expr* src = nullptr);
    // Resolve the class (mangled for generics) of a member-access object
    // expression: identifier (var/static), this, array element, `new X<...>()`,
    // or a call (via its return type).
    std::string memberObjectClassName(const Expr& obj);
    // Return-type class name (mangled for generics) of a call, "" if not a class.
    std::string callReturnClassName(const CallExpr& e);
    // generateCall's body (renamed): the public generateCall wraps it to push
    // ARC-owned call results as statement temps.
    llvm::Value* generateCallImpl(const CallExpr& expr);
    llvm::Value* generateMemberAccess(const MemberAccessExpr& expr);
    // Address (pointer) of a struct member-access chain like v.a.b — used for
    // chained struct-field assignment (v.a.b = expr). Returns nullptr if the
    // chain cannot be resolved to a struct field.
    llvm::Value* generateStructMemberAddress(const MemberAccessExpr& expr);
    // Address (pointer) of struct-array element `arr[i]` (base + i·sizeof(Elem)).
    // Returns nullptr if the array isn't a resolvable struct-element array.
    // Backs `arr[i].field` read/write and chained `arr[i].field.sub` access.
    llvm::Value* generateArrayElementAddress(const SubscriptExpr& ss);
    // If `expr` evaluates to a slice value, return its TypeInfo (recursing
    // through subscripts so nested slices like rows[i] resolve to slice<int>).
    const TypeInfo* sliceTypeOfExpr(const Expr* expr);
    // Bounds-checked element address for a slice-valued expression (incl. nested
    // slice subscripts like rows[i][j]); nullptr if expr isn't a slice.
    llvm::Value* generateSliceElementAddress(const Expr* arr, llvm::Value* idx);
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

    // ---- O(1) 声明索引（generate() 入口一次建全，避免热路径线性扫描类/接口/enum）----
    // 指向 current_tu_ 内对象；codegen 期间 TU 不再增长（sema 已合并完成）→ 指针稳定。
    std::unordered_map<std::string, const ClassDecl*> class_decls_;
    std::unordered_map<std::string, const InterfaceDecl*> interface_decls_;
    std::unordered_map<std::string, const EnumDecl*> enum_decls_;
    std::unordered_map<std::string, const StructDecl*> struct_decls_;   // key: name 或 Parent::name
    // 成员名 → 第一个定义它的类（按声明序；actions/static_actions/functions 合并）。
    // 供方法调用 name-only 回退路径 O(1) 查——P6 规模 O(N²) 根因。
    std::unordered_map<std::string, std::string> first_member_class_;

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
