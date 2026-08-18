// GPU / parallel codegen — part of the CodeGen implementation, split from codegen.cpp.
// Pure refactor: member definitions moved out of the 10k-line monolith;
// no behavior change. See codegen.cpp for the class declaration.

#include "mylang/CodeGen.h"
#include "mylang/MypPasses.h"

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/ADT/SmallString.h>

#ifdef MYP_ENABLE_GPU
#include <llvm/IR/IntrinsicsNVPTX.h>
#include <llvm/IR/IntrinsicsAMDGPU.h>
#endif
#include <llvm/IRReader/IRReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Instrumentation/ThreadSanitizer.h>

// NVPTX target initialization (must be at global scope) — 仅 GPU 构建需要
#ifdef MYP_ENABLE_GPU
extern "C" void LLVMInitializeNVPTXTargetInfo(void);
extern "C" void LLVMInitializeNVPTXTarget(void);
extern "C" void LLVMInitializeNVPTXTargetMC(void);
extern "C" void LLVMInitializeNVPTXAsmPrinter(void);
// §9.5 P4 跨厂商（AMD）：AMDGPU 后端初始化（交叉编译 GCN code object）
extern "C" void LLVMInitializeAMDGPUTargetInfo(void);
extern "C" void LLVMInitializeAMDGPUTarget(void);
extern "C" void LLVMInitializeAMDGPUTargetMC(void);
extern "C" void LLVMInitializeAMDGPUAsmPrinter(void);
#endif

// §3.2 前向声明：CUDA libdevice 链接（generateGpuTile 在定义前调用）
namespace mylang { class DiagnosticEngine; }
namespace mylang { static bool linkGpuLibdevice(llvm::Module* ptx_mod, DiagnosticEngine* diag); }

#ifdef MYP_ENABLE_GPU
// ============================================================================
// §7.7 / §6.4 编译期 GpuTarget：跨厂商参数化（单源 → NVPTX / AMDGPU）。
// 激活目标由 MYP_GPU_TARGET 选择（默认 NVPTX；"amdgcn"/"amdgpu"/"gcn" → AMD）。
// MYP_GPU_ARCH 指定架构（AMD 默认 gfx1030 / NV 固定 sm_75）。无 AMD 硬件时
// AMD 路径仅用于交叉编译验证产出（§9.5 ⑤），运行期仍回退 CPU。
// ============================================================================
static bool gpuTargetAmd() {
    const char* e = getenv("MYP_GPU_TARGET");
    return e && (strcmp(e, "amdgcn") == 0 || strcmp(e, "amdgpu") == 0 ||
                 strcmp(e, "gcn") == 0);
}
static std::string gpuTargetTriple() {
    return gpuTargetAmd() ? "amdgcn-amd-amdhsa" : "nvptx64-nvidia-cuda";
}
static std::string gpuTargetArch() {
    if (gpuTargetAmd()) {
        const char* a = getenv("MYP_GPU_ARCH");
        return (a && *a) ? std::string(a) : std::string("gfx1030");
    }
    return "sm_75";
}
// 两个后端都初始化（静态一次；libLLVM 统一符号，安全）。
static void ensureGpuTargetsInited() {
    static bool inited = false;
    if (inited) return;
    LLVMInitializeNVPTXTargetInfo();
    LLVMInitializeNVPTXTarget();
    LLVMInitializeNVPTXTargetMC();
    LLVMInitializeNVPTXAsmPrinter();
    LLVMInitializeAMDGPUTargetInfo();
    LLVMInitializeAMDGPUTarget();
    LLVMInitializeAMDGPUTargetMC();
    LLVMInitializeAMDGPUAsmPrinter();
    inited = true;
}
// 线程/网格索引 intrinsic（跨厂商）：NVVM sreg ↔ AMDGCN workitem/workgroup。
// LLVM 21 把 AMDGCN intrinsic 放在独立 `enum AMDGCNIntrinsics : unsigned`
// （值即主 Intrinsic::ID 编号空间），故参数用 unsigned、内部转 Intrinsic::ID。
static llvm::Value* emitGpuIndex(llvm::IRBuilder<>& kb, unsigned nvvm_id,
                                 unsigned amdgcn_id) {
    unsigned id = gpuTargetAmd() ? amdgcn_id : nvvm_id;
    return kb.CreateIntrinsic(static_cast<llvm::Intrinsic::ID>(id),
        llvm::ArrayRef<llvm::Type*>(), llvm::ArrayRef<llvm::Value*>());
}
static llvm::Value* emitGpuThreadIdx(llvm::IRBuilder<>& kb) {
    return emitGpuIndex(kb, llvm::Intrinsic::nvvm_read_ptx_sreg_tid_x,
                        llvm::Intrinsic::amdgcn_workitem_id_x);
}
static llvm::Value* emitGpuBlockIdx(llvm::IRBuilder<>& kb) {
    return emitGpuIndex(kb, llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_x,
                        llvm::Intrinsic::amdgcn_workgroup_id_x);
}
// blockDim（workgroup size）：NV → nvvm ntid.x；AMD → 无 intrinsic，workgroup 大小
// 即 launch 块大小（编译期已知常量，语义正确）。
static llvm::Value* emitGpuBlockDim(llvm::IRBuilder<>& kb, int block_size) {
    if (gpuTargetAmd())
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(kb.getContext()),
                                      block_size);
    return emitGpuIndex(kb, llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_x, 0);
}
// gridDim（网格数）：NV → nvvm nctaid.x；AMD → 无 intrinsic（AMDGCN 无直接查询），
// 置标记 → 调用方（grid-stride / scatter）走 CPU 回退（§6.4 留待 implicitarg）。
static bool gpu_amd_grid_used_ = false;
static llvm::Value* emitGpuGridDim(llvm::IRBuilder<>& kb) {
    if (gpuTargetAmd()) {
        gpu_amd_grid_used_ = true;
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(kb.getContext()), 0);
    }
    return emitGpuIndex(kb, llvm::Intrinsic::nvvm_read_ptx_sreg_nctaid_x, 0);
}
// §3.1 kernel.sync()：块内屏障（NVPTX bar.sync 0 / AMD s_barrier）。
static llvm::Value* emitGpuBarrier(llvm::IRBuilder<>& kb) {
    auto id = static_cast<llvm::Intrinsic::ID>(
        gpuTargetAmd() ? static_cast<unsigned>(llvm::Intrinsic::amdgcn_s_barrier)
                       : static_cast<unsigned>(llvm::Intrinsic::nvvm_barrier_cta_sync_aligned_all));
    if (gpuTargetAmd())
        return kb.CreateIntrinsic(id, llvm::ArrayRef<llvm::Type*>(),
                                  llvm::ArrayRef<llvm::Value*>());
    // NVVM barrier 需 i32 屏障号参数（bar.sync 0）
    llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(kb.getContext()), 0);
    return kb.CreateIntrinsic(id, llvm::ArrayRef<llvm::Type*>(),
                              llvm::ArrayRef<llvm::Value*>(zero));
}
// §7.7 跨厂商 kernel 调用约定：NVPTX → PTX_Kernel；AMD → AMDGPU_KERNEL（后端要求）。
static void setGpuKernelCC(llvm::Function* f) {
    f->setCallingConv(gpuTargetAmd() ? llvm::CallingConv::AMDGPU_KERNEL
                                     : llvm::CallingConv::PTX_Kernel);
}
// §6.4 AMDGPU：kernel 内 alloca 必须 addrspace(5)（private/栈）；NVPTX 用
// addrspace(0)（generic）。所有 kernel 局部变量/临时 alloca 都走此帮助函数。
static llvm::AllocaInst* createKernelAlloca(llvm::IRBuilder<>& kb, llvm::Type* ty,
                                            llvm::Value* array_size,
                                            const llvm::Twine& name) {
    unsigned as = gpuTargetAmd() ? 5 : 0;
    return kb.CreateAlloca(ty, as, array_size, name);
}
// §7.7 GpuCompiler 发射：NVPTX → PTX 文本（AssemblyFile）；AMD → GCN ELF code
// object（ObjectFile，供 hipModuleLoadData 加载）。返回空 = 失败。
static std::string emitGpuModule(llvm::Module* mod, llvm::TargetMachine* tm,
                                 bool amd, bool object) {
    llvm::legacy::PassManager pm;
    llvm::SmallString<32768> buf;
    llvm::raw_svector_ostream os(buf);
    auto ft = (amd && object) ? llvm::CodeGenFileType::ObjectFile
                              : llvm::CodeGenFileType::AssemblyFile;
    if (tm->addPassesToEmitFile(pm, os, nullptr, ft)) {
        delete tm;
        return "";
    }
    pm.run(*mod);
    delete tm;
    return std::string(buf.data(), buf.size());
}
// §3.5 内核优化管线（默认 O2，MYP_GPU_KERNEL_OPT 覆盖）：SROA/InstCombine/GVN/
// LICM/循环展开/向量化。AMD 声明式 kernel（block_sum/scan_k2）需它把某些构造
// （如 dynamic_stackalloc）消解成 AMDGPU 可选中形式（@gpu for 已验证）。
static void runGpuOptPipeline(llvm::Module* mod, llvm::TargetMachine* tm) {
    unsigned kopt = 2;
    if (const char* env = getenv("MYP_GPU_KERNEL_OPT")) {
        std::string want(env);
        if (want == "O0") kopt = 0;
        else if (want == "O1") kopt = 1;
        else if (want == "O2") kopt = 2;
        else if (want == "O3") kopt = 3;
    }
    tm->setOptLevel(kopt >= 3 ? llvm::CodeGenOptLevel::Aggressive :
                   kopt == 2 ? llvm::CodeGenOptLevel::Default :
                   kopt == 1 ? llvm::CodeGenOptLevel::Less :
                               llvm::CodeGenOptLevel::None);
    if (kopt <= 0) return;
    llvm::OptimizationLevel OL =
        kopt >= 3 ? llvm::OptimizationLevel::O3 :
        kopt == 2 ? llvm::OptimizationLevel::O2 :
                    llvm::OptimizationLevel::O1;
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    llvm::PassBuilder PB(tm);
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
    llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OL);
    MPM.addPass(llvm::VerifierPass());
    MPM.run(*mod, MAM);
}
// §9.5 ⑤ 交叉编译验证：AMD 目标时把 GCN ELF 写到文件（默认 /tmp/myp_kernel.gcn，
// MYP_GPU_EMIT_FILE 覆盖），返回空 → 运行期回退 CPU（无 AMD runtime/HIP）。
static std::string emitGpuTargetBytes(llvm::Module* mod, llvm::TargetMachine* tm,
                                      const std::string& label) {
    bool amd = gpuTargetAmd();
    std::string bytes = emitGpuModule(mod, tm, amd, amd);
    if (amd && !bytes.empty()) {
        const char* p = getenv("MYP_GPU_EMIT_FILE");
        std::string path = (p && *p) ? p : "/tmp/myp_kernel.gcn";
        FILE* f = fopen(path.c_str(), "wb");
        if (f) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); }
        fprintf(stderr, "[myp GPU] AMD cross-compile (%s): wrote GCN code object "
                "(%zu bytes) to %s\n", label.c_str(), bytes.size(), path.c_str());
    }
    return amd ? std::string("") : bytes;   // AMD → 空 → 调用方走 CPU 回退
}
#endif


#include <cstdlib>
#include <iostream>

namespace mylang {

void CodeGen::analyzeGpuCapturedVars(const ForStmt& stmt, const std::string& loop_var) {
    kernel_args_.clear();

    // Collect all identifier references in the body, condition, step
    std::set<std::string> all_refs;
    std::set<std::string> loop_decls;
    loop_decls.insert(loop_var);  // loop variable is NOT captured

    if (stmt.body)
        collectStmtIdentifiers(*stmt.body, all_refs, loop_decls);
    if (stmt.condition)
        collectExprIdentifiers(*stmt.condition, all_refs, loop_decls);
    if (stmt.step)
        collectExprIdentifiers(*stmt.step, all_refs, loop_decls);

    // Also add identifiers from init (they're not captured but may be referenced)
    if (stmt.init)
        collectStmtIdentifiers(*stmt.init, all_refs, loop_decls);

    // Remove loop-declared variables
    for (auto& ld : loop_decls)
        all_refs.erase(ld);

    // Resolve types for each captured variable and add to kernel_args_
    auto* i64_ty = llvm::Type::getInt64Ty(ctx_);
    auto* double_ty = llvm::Type::getDoubleTy(ctx_);
    auto* i32_ty = llvm::Type::getInt32Ty(ctx_);
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);

    for (auto& name : all_refs) {
        // Skip runtime function names
        if (name == "Math" || name == "Atomic" || name == "Console")
            continue;
        if (name.rfind("__myp_", 0) == 0)
            continue;

        KernelArgInfo kai;
        kai.name = name;
        kai.is_array = false;
        kai.array_arg_idx = -1;
        kai.size_arg_idx = -1;

        // Try to resolve type from named values
        auto* nv = getNamedValue(name);
        if (nv) {
            // Prefer the alloca's actual allocated type (source of truth for the
            // variable in THIS function) over getNamedValueType, which is keyed by
            // name and can hold a stale type from a previously compiled function.
            llvm::Type* val_type = nullptr;
            if (llvm::isa<llvm::AllocaInst>(nv))
                val_type = llvm::cast<llvm::AllocaInst>(nv)->getAllocatedType();
            if (!val_type)
                val_type = getNamedValueType(name);

            // Check if it's an array (pointer to element type).
            // NOTE: array_elem_types_ is keyed by name and can hold stale entries
            // from a previously compiled function (e.g. a scalar param `a` while
            // another method declared `double[] a`). A real array variable is always
            // stored as a pointer-typed alloca, so require that to avoid
            // misclassifying scalars as arrays.
            bool is_array_here = false;
            if (array_elem_types_.find(name) != array_elem_types_.end()) {
                if (llvm::isa<llvm::AllocaInst>(nv)) {
                    llvm::Type* aty = llvm::cast<llvm::AllocaInst>(nv)->getAllocatedType();
                    if (aty->isPointerTy())
                        is_array_here = true;
                } else {
                    is_array_here = true;  // non-alloca (e.g. global) — trust map
                }
            }
            if (is_array_here) {
                kai.is_array = true;
                kai.type = ptr_ty;  // kernel arg is a pointer
            } else if (val_type) {
                if (val_type->isArrayTy()) {
                    kai.is_array = true;
                    kai.type = ptr_ty;
                } else if (val_type->isIntegerTy(64) || val_type->isIntegerTy(32) ||
                           val_type->isDoubleTy() || val_type->isFloatTy() ||
                           val_type->isPointerTy()) {
                    kai.type = val_type;
                } else {
                    kai.type = i64_ty;  // default
                }
            } else {
                kai.type = i64_ty;
            }
        } else {
            // Not a local variable — could be a @static class access like "CrossSectionDB"
            // Check if it's a static class name
            auto sit = static_property_globals_.find(name);
            if (sit != static_property_globals_.end()) {
                // Pass the static class struct pointer
                kai.is_array = false;
                kai.type = ptr_ty;
            } else {
                // Unknown — skip
                continue;
            }
        }

        kernel_args_.push_back(kai);
    }
}

// §3.2 @gpu tile 捕获变量分析：收集 body 引用的外部变量（排除共享数组名），
// 类型解析逻辑与 analyzeGpuCapturedVars 相同（无 init/condition/step）。
void CodeGen::analyzeGpuTileCapturedVars(const GpuTileStmt& stmt) {
    kernel_args_.clear();

    std::set<std::string> all_refs;
    std::set<std::string> loop_decls;
    loop_decls.insert(stmt.name);  // 共享数组不捕获（kernel 内声明）

    if (stmt.body)
        collectStmtIdentifiers(*stmt.body, all_refs, loop_decls);
    for (auto& ld : loop_decls)
        all_refs.erase(ld);

    auto* i64_ty = llvm::Type::getInt64Ty(ctx_);
    auto* double_ty = llvm::Type::getDoubleTy(ctx_);
    auto* i32_ty = llvm::Type::getInt32Ty(ctx_);
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);

    for (auto& name : all_refs) {
        if (name == "Math" || name == "Atomic" || name == "Console")
            continue;
        if (name.rfind("__myp_", 0) == 0)
            continue;

        KernelArgInfo kai;
        kai.name = name;
        kai.is_array = false;
        kai.array_arg_idx = -1;
        kai.size_arg_idx = -1;

        auto* nv = getNamedValue(name);
        if (nv) {
            llvm::Type* val_type = nullptr;
            if (llvm::isa<llvm::AllocaInst>(nv))
                val_type = llvm::cast<llvm::AllocaInst>(nv)->getAllocatedType();
            if (!val_type)
                val_type = getNamedValueType(name);

            bool is_array_here = false;
            if (array_elem_types_.find(name) != array_elem_types_.end()) {
                if (llvm::isa<llvm::AllocaInst>(nv)) {
                    llvm::Type* aty = llvm::cast<llvm::AllocaInst>(nv)->getAllocatedType();
                    if (aty->isPointerTy())
                        is_array_here = true;
                } else {
                    is_array_here = true;
                }
            }
            if (is_array_here) {
                kai.is_array = true;
                kai.type = ptr_ty;
            } else if (val_type) {
                if (val_type->isArrayTy()) {
                    kai.is_array = true;
                    kai.type = ptr_ty;
                } else if (val_type->isIntegerTy(64) || val_type->isIntegerTy(32) ||
                           val_type->isDoubleTy() || val_type->isFloatTy() ||
                           val_type->isPointerTy()) {
                    kai.type = val_type;
                } else {
                    kai.type = i64_ty;
                }
            } else {
                kai.type = i64_ty;
            }
        } else {
            auto sit = static_property_globals_.find(name);
            if (sit != static_property_globals_.end()) {
                kai.is_array = false;
                kai.type = ptr_ty;
            } else {
                continue;
            }
        }
        kernel_args_.push_back(kai);
    }
}

llvm::Value* CodeGen::emitKernelElementAddr(
    const Expr* arr_expr, llvm::Value* idx, llvm::IRBuilder<>& kb,
    std::map<std::string, llvm::Value*>& kernel_vars,
    const std::vector<llvm::Value*>& kernel_arg_values,
    const std::string& loop_var_name, llvm::Value* tid_val) {
    if (!arr_expr || arr_expr->kind != ExprKind::Identifier || !idx) return nullptr;
    auto& id = static_cast<const IdentifierExpr&>(*arr_expr);
    auto* i64_ty = llvm::Type::getInt64Ty(ctx_);

    // slice<T> variable: arr evaluates to the captured {data,len} slice value.
    auto sit = var_slice_types_.find(id.name);
    if (sit != var_slice_types_.end() && sit->second.element_type) {
        auto* arr = emitKernelExpr(*arr_expr, kb, kernel_vars, kernel_arg_values,
                                   loop_var_name, tid_val);
        if (!arr || !arr->getType()->isStructTy()) return nullptr;
        auto* data = kb.CreateExtractValue(arr, 0);
        auto* len = kb.CreateExtractValue(arr, 1);
        auto* i64idx = idx;
        if (i64idx->getType()->isIntegerTy(32) || i64idx->getType()->isIntegerTy(8)
            || i64idx->getType()->isIntegerTy(16))
            i64idx = kb.CreateZExt(i64idx, i64_ty);
        auto* nonneg = kb.CreateICmpSGE(i64idx, llvm::ConstantInt::get(i64_ty, 0));
        auto* inb = kb.CreateICmpULT(i64idx, len);
        auto* ok = kb.CreateAnd(nonneg, inb);
        auto* be_fn = module_->getFunction("myp_bounds_error");
        if (!be_fn) {
            auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                {i64_ty, i64_ty}, false);
            be_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                "myp_bounds_error", module_.get());
        }
        auto* cur_fn = kb.GetInsertBlock()->getParent();
        auto* err_bb = llvm::BasicBlock::Create(ctx_, "slice_oob", cur_fn);
        auto* cont_bb = llvm::BasicBlock::Create(ctx_, "slice_ok", cur_fn);
        kb.CreateCondBr(ok, cont_bb, err_bb);
        kb.SetInsertPoint(err_bb);
        kb.CreateCall(be_fn, {i64idx, len});
        kb.CreateBr(cont_bb);
        kb.SetInsertPoint(cont_bb);
        auto* et = getLLVMType(*sit->second.element_type);
        return kb.CreateGEP(et, data, i64idx);
    }

    // Plain array variable: arr is a pointer; GEP directly.
    auto eit = array_elem_types_.find(id.name);
    if (eit != array_elem_types_.end()) {
        auto* arr = emitKernelExpr(*arr_expr, kb, kernel_vars, kernel_arg_values,
                                   loop_var_name, tid_val);
        if (!arr) return nullptr;
        return kb.CreateGEP(eit->second, arr, zextIndexValue(kb, idx));
    }
    return nullptr;
}

llvm::Value* CodeGen::emitKernelExpr(const Expr& expr, llvm::IRBuilder<>& kb,
    std::map<std::string, llvm::Value*>& kernel_vars,
    const std::vector<llvm::Value*>& kernel_arg_values,
    const std::string& loop_var_name, llvm::Value* tid_val) {

    auto* i32_ty = llvm::Type::getInt32Ty(ctx_);
    auto* i64_ty = llvm::Type::getInt64Ty(ctx_);
    auto* double_ty = llvm::Type::getDoubleTy(ctx_);
    auto* float_ty = llvm::Type::getFloatTy(ctx_);
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);

    switch (expr.kind) {
        case ExprKind::IntegerLiteral: {
            auto& e = static_cast<const IntegerLiteralExpr&>(expr);
            if (e.is_unsigned) {
                if (e.value >= 0 && e.value <= 0xFF) return llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), (uint64_t)e.value, false);
                if (e.value <= 0xFFFF) return llvm::ConstantInt::get(llvm::Type::getInt16Ty(ctx_), (uint64_t)e.value, false);
                if (e.value <= 0xFFFFFFFFLL) return llvm::ConstantInt::get(i32_ty, (uint64_t)e.value, false);
                return llvm::ConstantInt::get(i64_ty, (uint64_t)e.value, false);
            }
            return llvm::ConstantInt::get(e.is_long ? i64_ty : i32_ty, e.value, true);
        }
        case ExprKind::FloatLiteral: {
            auto& e = static_cast<const FloatLiteralExpr&>(expr);
            return e.is_f32 ? llvm::ConstantFP::get(float_ty, e.value)
                            : llvm::ConstantFP::get(double_ty, e.value);
        }
        case ExprKind::BoolLiteral: {
            auto& e = static_cast<const BoolLiteralExpr&>(expr);
            return llvm::ConstantInt::get(i32_ty, e.value ? 1 : 0);
        }
        case ExprKind::Identifier: {
            auto& e = static_cast<const IdentifierExpr&>(expr);
            if (e.name == loop_var_name)
                return tid_val;
            auto vit = kernel_vars.find(e.name);
            if (vit != kernel_vars.end()) {
                // If it's an alloca (local mutable var), load from it
                if (llvm::isa<llvm::AllocaInst>(vit->second))
                    return kb.CreateLoad(
                        llvm::cast<llvm::AllocaInst>(vit->second)->getAllocatedType(),
                        vit->second, e.name);
                return vit->second;
            }
            if (e.name == "sqrt" || e.name == "cos" || e.name == "sin" ||
                e.name == "tan" || e.name == "exp" || e.name == "log" ||
                e.name == "pow" || e.name == "abs" || e.name == "floor" ||
                e.name == "ceil" || e.name == "trunc") {
                return nullptr;
            }
            return llvm::ConstantInt::get(i64_ty, 0);
        }
        case ExprKind::BinaryOp: {
            auto& e = static_cast<const BinaryOpExpr&>(expr);
            // String operations (concat, comparison) are not supported inside
            // @parallel for / @gpu for bodies — the kernel path has no string
            // runtime. Reject cleanly instead of emitting garbage pointer
            // arithmetic (previously: LLVM verify "Call parameter type does
            // not match function signature!" for `"iter " + i` → `add i64`).
            if (e.lhs->resolved_kind == TypeKind::String ||
                e.rhs->resolved_kind == TypeKind::String) {
                diag_.error(expr.range,
                    "string operations are not supported inside '@parallel for' / '@gpu for' bodies");
                return llvm::ConstantInt::get(i64_ty, 0);
            }
            auto* l = emitKernelExpr(*e.lhs, kb, kernel_vars, kernel_arg_values,
                                      loop_var_name, tid_val);
            auto* r = emitKernelExpr(*e.rhs, kb, kernel_vars, kernel_arg_values,
                                      loop_var_name, tid_val);
            if (!l || !r) return llvm::ConstantInt::get(i64_ty, 0);

            // Type promotion
            if (l->getType() != r->getType()) {
                if (l->getType()->isDoubleTy() || r->getType()->isDoubleTy()) {
                    // 混型提升为 double：int→SIToFP，float→FPExt（此前漏了
                    // float→double → fsub(float,double) LLVM verify 失败）。
                    if (!l->getType()->isDoubleTy()) {
                        if (l->getType()->isIntegerTy()) l = kb.CreateSIToFP(l, double_ty);
                        else if (l->getType()->isFloatTy()) l = kb.CreateFPExt(l, double_ty);
                    }
                    if (!r->getType()->isDoubleTy()) {
                        if (r->getType()->isIntegerTy()) r = kb.CreateSIToFP(r, double_ty);
                        else if (r->getType()->isFloatTy()) r = kb.CreateFPExt(r, double_ty);
                    }
                } else if (l->getType()->isFloatTy() || r->getType()->isFloatTy()) {
                    if (!l->getType()->isFloatTy() && l->getType()->isIntegerTy())
                        l = kb.CreateSIToFP(l, float_ty);
                    if (!r->getType()->isFloatTy() && r->getType()->isIntegerTy())
                        r = kb.CreateSIToFP(r, float_ty);
                } else if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy()) {
                    auto lw = l->getType()->getIntegerBitWidth();
                    auto rw = r->getType()->getIntegerBitWidth();
                    if (lw < rw) l = kb.CreateSExt(l, r->getType());
                    else if (rw < lw) r = kb.CreateSExt(r, l->getType());
                }
            }
            bool fp = l->getType()->isFloatingPointTy();

            // Ensure both operands have the same type before comparison
            if (l->getType() != r->getType()) {
                if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy()) {
                    auto lw = l->getType()->getIntegerBitWidth();
                    auto rw = r->getType()->getIntegerBitWidth();
                    if (lw < rw) l = kb.CreateSExt(l, r->getType());
                    else if (rw < lw) r = kb.CreateSExt(r, l->getType());
                    else r = kb.CreateIntCast(r, l->getType(), true);
                } else if (l->getType()->isDoubleTy() && r->getType()->isIntegerTy()) {
                    r = kb.CreateSIToFP(r, l->getType());
                } else if (l->getType()->isIntegerTy() && r->getType()->isDoubleTy()) {
                    l = kb.CreateSIToFP(l, r->getType());
                } else if (l->getType()->isPointerTy() && r->getType()->isIntegerTy()) {
                    r = kb.CreateIntToPtr(r, l->getType());
                } else if (l->getType()->isIntegerTy() && r->getType()->isPointerTy()) {
                    l = kb.CreateIntToPtr(l, r->getType());
                } else if (l->getType()->isPointerTy() && r->getType()->isPointerTy()) {
                    auto* i8_ptr = llvm::PointerType::get(ctx_, 0);
                    if (l->getType() != i8_ptr) l = kb.CreateBitCast(l, i8_ptr);
                    if (r->getType() != i8_ptr) r = kb.CreateBitCast(r, i8_ptr);
                } else {
                    // Fallback: convert both to i64 for comparison
                    auto* i64 = llvm::Type::getInt64Ty(ctx_);
                    auto valToI64 = [&](llvm::Value* v) -> llvm::Value* {
                        auto* ty = v->getType();
                        if (ty->isIntegerTy()) {
                            return ty == i64 ? v : kb.CreateSExt(v, i64);
                        } else if (ty->isPointerTy()) {
                            return kb.CreatePtrToInt(v, i64);
                        } else if (ty->isFloatingPointTy()) {
                            return kb.CreateFPToSI(v, i64);
                        } else {
                            // Struct, array, or other aggregate: store to alloca and get address
                            auto* alloca = createKernelAlloca(kb, ty, nullptr, "");
                            kb.CreateStore(v, alloca);
                            return kb.CreatePtrToInt(
                                kb.CreateBitCast(alloca, llvm::PointerType::get(ctx_, 0)), i64);
                        }
                    };
                    l = valToI64(l);
                    r = valToI64(r);
                }
            }
            fp = l->getType()->isFloatingPointTy();

            switch (e.op) {
                case BinaryOpKind::Add: return fp ? kb.CreateFAdd(l, r) : kb.CreateAdd(l, r);
                case BinaryOpKind::Sub: return fp ? kb.CreateFSub(l, r) : kb.CreateSub(l, r);
                case BinaryOpKind::Mul: return fp ? kb.CreateFMul(l, r) : kb.CreateMul(l, r);
                case BinaryOpKind::Div: return fp ? kb.CreateFDiv(l, r) : kb.CreateSDiv(l, r);
                case BinaryOpKind::Mod: return fp ? kb.CreateFRem(l, r) : kb.CreateSRem(l, r);
                case BinaryOpKind::Eq:  return fp ? kb.CreateFCmpOEQ(l, r) : kb.CreateICmpEQ(l, r);
                case BinaryOpKind::Ne:  return fp ? kb.CreateFCmpONE(l, r) : kb.CreateICmpNE(l, r);
                case BinaryOpKind::Lt:  return fp ? kb.CreateFCmpOLT(l, r) : kb.CreateICmpSLT(l, r);
                case BinaryOpKind::Gt:  return fp ? kb.CreateFCmpOGT(l, r) : kb.CreateICmpSGT(l, r);
                case BinaryOpKind::Le:  return fp ? kb.CreateFCmpOLE(l, r) : kb.CreateICmpSLE(l, r);
                case BinaryOpKind::Ge:  return fp ? kb.CreateFCmpOGE(l, r) : kb.CreateICmpSGE(l, r);
                case BinaryOpKind::And:
                case BinaryOpKind::BitAnd: return kb.CreateAnd(l, r);
                case BinaryOpKind::Or:
                case BinaryOpKind::BitOr:  return kb.CreateOr(l, r);
                case BinaryOpKind::BitXor: return kb.CreateXor(l, r);
                case BinaryOpKind::Shl:    return kb.CreateShl(l, r);
                case BinaryOpKind::Shr:    return kb.CreateAShr(l, r);
            }
            return nullptr;
        }
        case ExprKind::UnaryOp: {
            auto& e = static_cast<const UnaryOpExpr&>(expr);
            auto* op = emitKernelExpr(*e.operand, kb, kernel_vars, kernel_arg_values,
                                       loop_var_name, tid_val);
            if (!op) return llvm::ConstantInt::get(i64_ty, 0);
            if (e.op == UnaryOpKind::Negate) {
                return op->getType()->isFloatingPointTy()
                    ? kb.CreateFNeg(op) : kb.CreateNeg(op);
            }
            if (e.op == UnaryOpKind::Not)
                return kb.CreateNot(op);
            return op;
        }
        case ExprKind::Convert: {
            auto& e = static_cast<const ConvertExpr&>(expr);
            auto* op = emitKernelExpr(*e.operand, kb, kernel_vars, kernel_arg_values,
                                       loop_var_name, tid_val);
            if (!op) return llvm::ConstantInt::get(i64_ty, 0);
            llvm::Type* target = getLLVMType(TypeInfo(e.to_kind));
            // §7.1 单一转换权威：与 CPU codegen 共用 convertIntegerValue
            // （bool↔int/fp、char 无符号语义、int↔int / fp↔int / float↔double /
            // 指针 bitcast 全覆盖），消除 GPU 内重复转换逻辑。GPU kernel 不支持
            // bit/bitvector/bitfield 目标类型（此类代码本就不应出现在 kernel 内）。
            return convertIntegerValue(kb, op, target, e.operand.get());
        }
        case ExprKind::Call: {
            auto& e = static_cast<const CallExpr&>(expr);
            // §3.1 kernel.sync()：块内屏障（NVPTX bar.sync 0 / llvm.nvvm.barrier0）。
            if (e.callee->kind == ExprKind::MemberAccess) {
                auto& kma = static_cast<const MemberAccessExpr&>(*e.callee);
                if (kma.object->kind == ExprKind::Identifier &&
                    static_cast<const IdentifierExpr&>(*kma.object).name == "kernel") {
                    if (kma.member_name == "sync") {
                        // kernel.sync() → 块内屏障（§7.7 跨厂商：NVPTX bar.sync 0 /
                        // AMD s_barrier）。
                        return emitGpuBarrier(kb);
                    }
                    // §P5 ② kernel.printk(fmt, a, b, c) / kernel.assert(cond, fmt, ...)：
                    // 写设备 staging 记录（myp_pbuf/myp_pcnt/myp_pfail），launch 后
                    // 宿主 myp_gpu_flush_printf 回读格式化打印；assert 失败 exit(1)。
                    if (kma.member_name == "printk" || kma.member_name == "assert") {
                        return emitKernelPrintk(e, kb, kernel_vars, kernel_arg_values,
                            loop_var_name, tid_val, kma.member_name == "assert");
                    }
                    // §3.4 kernel.shfl_down(v, delta)：warp 内 lane 方向移位 delta，
                    // 越界返回自身。NVPTX shfl.sync.down（LLVM 21 只有 i32/f32 →
                    // double 拆 2×i32 重组）。
                    if (kma.member_name == "shfl_down") {
                        auto* v = emitKernelExpr(*e.args[0], kb, kernel_vars, kernel_arg_values, loop_var_name, tid_val);
                        auto* delta = emitKernelExpr(*e.args[1], kb, kernel_vars, kernel_arg_values, loop_var_name, tid_val);
                        auto* mask = llvm::ConstantInt::get(i32_ty, -1);   // 全掩码
                        auto* clamp = llvm::ConstantInt::get(i32_ty, -1);  // 越界返回自身
                        if (delta->getType() != i32_ty)
                            delta = kb.CreateIntCast(delta, i32_ty, false);
                        // driver 595.84 对 clamp<0 的 shfl.sync.down JIT 有 bug
                        // （整个 shfl 不交换，lane 0 也返回自身）→ 用 clamp=31
                        // 保证交换；越界 lane（lane+delta>=32）手动用自身 v 替换。
                        auto* clamp31 = llvm::ConstantInt::get(i32_ty, 31);
                        // lane = tid % 32（gpu_ctx_tid_x_ 是 i64 tid_x）
                        auto* tx32 = kb.CreateTrunc(gpu_ctx_tid_x_, i32_ty, "tx32");
                        auto* lane = kb.CreateAnd(tx32,
                            llvm::ConstantInt::get(i32_ty, 31), "lane");
                        auto* in_range = kb.CreateICmpULT(lane,
                            kb.CreateSub(llvm::ConstantInt::get(i32_ty, 32), delta,
                                         "shfl_bound"), "shfl_ok");
                        llvm::Module* cur_mod = kb.GetInsertBlock()->getParent()->getParent();
                        if (v->getType()->isDoubleTy()) {
                            auto* bits = kb.CreateBitCast(v, i64_ty, "shfl_bits");
                            auto* lo = kb.CreateTrunc(bits, i32_ty, "lo");
                            auto* hi = kb.CreateLShr(bits, llvm::ConstantInt::get(i64_ty, 32));
                            hi = kb.CreateTrunc(hi, i32_ty, "hi");
                            auto* f_i32 = llvm::Intrinsic::getDeclaration(cur_mod,
                                llvm::Intrinsic::nvvm_shfl_sync_down_i32);
                            auto* lo2 = kb.CreateCall(f_i32, {mask, lo, delta, clamp31});
                            auto* hi2 = kb.CreateCall(f_i32, {mask, hi, delta, clamp31});
                            auto* lo_z = kb.CreateZExt(lo2, i64_ty);
                            auto* hi_z = kb.CreateZExt(hi2, i64_ty);
                            hi_z = kb.CreateShl(hi_z, llvm::ConstantInt::get(i64_ty, 32));
                            auto* nb = kb.CreateOr(lo_z, hi_z);
                            auto* res = kb.CreateBitCast(nb, double_ty, "shfl_d");
                            return kb.CreateSelect(in_range, res, v, "shfl_sel");
                        }
                        if (v->getType()->isFloatTy()) {
                            auto* f_f32 = llvm::Intrinsic::getDeclaration(cur_mod,
                                llvm::Intrinsic::nvvm_shfl_sync_down_f32);
                            auto* res = kb.CreateCall(f_f32, {mask, v, delta, clamp31});
                            return kb.CreateSelect(in_range, res, v, "shfl_sel");
                        }
                        if (v->getType()->isIntegerTy(32)) {
                            auto* f_i32 = llvm::Intrinsic::getDeclaration(cur_mod,
                                llvm::Intrinsic::nvvm_shfl_sync_down_i32);
                            auto* res = kb.CreateCall(f_i32, {mask, v, delta, clamp31});
                            return kb.CreateSelect(in_range, res, v, "shfl_sel");
                        }
                        return v;  // 其他类型降级
                    }
                    // §3.4 kernel.block_reduce_sum/max(v)：块内归约。
                    // warp 内 shuffle 树 → lane0 写 shared[warp] → sync → warp0
                    // 归约 shared → sync → 读 shared[0]。block 固定 256 → 8 warps，
                    // shared[8] 在 kernel entry 块（addrspace 3 静态 __shared__）。
                    if (kma.member_name == "block_reduce_sum" ||
                        kma.member_name == "block_reduce_max") {
                        bool is_sum = (kma.member_name == "block_reduce_sum");
                        auto* v = emitKernelExpr(*e.args[0], kb, kernel_vars,
                            kernel_arg_values, loop_var_name, tid_val);
                        llvm::Type* fnty = v->getType();
                        if (!fnty->isDoubleTy() && !fnty->isFloatTy() &&
                            !fnty->isIntegerTy(32))
                            return v;
                        llvm::Module* cur_mod = kb.GetInsertBlock()->getParent()->getParent();
                        // 类型专用 shfl_down（clamp=31；warp0 归约只需 lane0，非
                        // lane0 结果丢弃，越界不影响 lane0）
                        auto shfl_d = [&](llvm::Value* val, llvm::Value* off) -> llvm::Value* {
                            auto* mask = llvm::ConstantInt::get(i32_ty, -1);
                            auto* c31 = llvm::ConstantInt::get(i32_ty, 31);
                            if (fnty->isDoubleTy()) {
                                auto* bits = kb.CreateBitCast(val, i64_ty);
                                auto* lo = kb.CreateTrunc(bits, i32_ty);
                                auto* hi = kb.CreateLShr(bits, llvm::ConstantInt::get(i64_ty, 32));
                                hi = kb.CreateTrunc(hi, i32_ty);
                                auto* fi = llvm::Intrinsic::getDeclaration(cur_mod,
                                    llvm::Intrinsic::nvvm_shfl_sync_down_i32);
                                auto* lo2 = kb.CreateCall(fi, {mask, lo, off, c31});
                                auto* hi2 = kb.CreateCall(fi, {mask, hi, off, c31});
                                auto* lo_z = kb.CreateZExt(lo2, i64_ty);
                                auto* hi_z = kb.CreateZExt(hi2, i64_ty);
                                hi_z = kb.CreateShl(hi_z, llvm::ConstantInt::get(i64_ty, 32));
                                return kb.CreateBitCast(kb.CreateOr(lo_z, hi_z), double_ty);
                            }
                            if (fnty->isFloatTy()) {
                                auto* ff = llvm::Intrinsic::getDeclaration(cur_mod,
                                    llvm::Intrinsic::nvvm_shfl_sync_down_f32);
                                return kb.CreateCall(ff, {mask, val, off, c31});
                            }
                            auto* fi = llvm::Intrinsic::getDeclaration(cur_mod,
                                llvm::Intrinsic::nvvm_shfl_sync_down_i32);
                            return kb.CreateCall(fi, {mask, val, off, c31});
                        };
                        auto combine = [&](llvm::Value* a, llvm::Value* b) -> llvm::Value* {
                            if (is_sum) {
                                if (fnty->isDoubleTy() || fnty->isFloatTy())
                                    return kb.CreateFAdd(a, b);
                                return kb.CreateAdd(a, b);
                            }
                            if (fnty->isDoubleTy() || fnty->isFloatTy())
                                return kb.CreateSelect(kb.CreateFCmpOGT(a, b), a, b);
                            return kb.CreateSelect(kb.CreateICmpSGT(a, b), a, b);
                        };
                        auto* tx32 = kb.CreateTrunc(gpu_ctx_tid_x_, i32_ty, "tx32");
                        auto* lane = kb.CreateAnd(tx32, llvm::ConstantInt::get(i32_ty, 31), "lane");
                        auto* warp = kb.CreateLShr(tx32, llvm::ConstantInt::get(i32_ty, 5), "warp");
                        // shared[8] = kernel 模块 addrspace(3) 全局（真 __shared__）。
                        // NVPTX 对 alloca(addrspace 3) 会降到 .local → cvta.shared
                        // 非法访问（error 700）；GlobalVariable(addrspace 3) 才是
                        // 真 .shared（同 @gpu tile 的 smem）。
                        auto* smem_arr = llvm::ArrayType::get(fnty, 8);
                        auto* smem_mod = kb.GetInsertBlock()->getParent()->getParent();
                        static int blkred_id = 0;
                        auto* smem = new llvm::GlobalVariable(*smem_mod, smem_arr,
                            false, llvm::GlobalValue::InternalLinkage,
                            llvm::ConstantAggregateZero::get(smem_arr),
                            "myp_blkred_smem_" + std::to_string(blkred_id++),
                            nullptr, llvm::GlobalValue::NotThreadLocal, 3);
                        auto smem_gep = [&](llvm::Value* idx) -> llvm::Value* {
                            return kb.CreateGEP(smem_arr, smem,
                                {llvm::ConstantInt::get(i32_ty, 0), idx});
                        };
                        auto* bar = llvm::Intrinsic::getDeclaration(cur_mod,
                            llvm::Intrinsic::nvvm_barrier_cta_sync_aligned_all);
                        auto* zero = llvm::ConstantInt::get(i32_ty, 0);
                        auto* zval = llvm::Constant::getNullValue(fnty);
                        // 1) warp 内树归约（所有线程）
                        auto* acc = v;
                        for (int off : {16, 8, 4, 2, 1}) {
                            auto* o = llvm::ConstantInt::get(i32_ty, off);
                            acc = combine(acc, shfl_d(acc, o));
                        }
                        // 2) lane0 写 smem[warp]（条件 store，避免同 warp 写竞争）
                        auto* is_l0 = kb.CreateICmpEQ(lane,
                            llvm::ConstantInt::get(i32_ty, 0));
                        auto* st_bb = llvm::BasicBlock::Create(ctx_, "blkred_st",
                            kb.GetInsertBlock()->getParent());
                        auto* st_join = llvm::BasicBlock::Create(ctx_, "blkred_st_join",
                            kb.GetInsertBlock()->getParent());
                        kb.CreateCondBr(is_l0, st_bb, st_join);
                        kb.SetInsertPoint(st_bb);
                        kb.CreateStore(acc, smem_gep(warp));
                        kb.CreateBr(st_join);
                        kb.SetInsertPoint(st_join);
                        // 3) sync（uniform，所有线程）
                        kb.CreateCall(bar, {zero});
                        // 4) warp0 归约 shared（lane<8 有效；shfl 只 warp0 内 uniform）
                        auto* cur_bb = kb.GetInsertBlock();
                        auto* then_bb = llvm::BasicBlock::Create(ctx_, "blkred_then",
                            cur_bb->getParent());
                        auto* join_bb = llvm::BasicBlock::Create(ctx_, "blkred_join",
                            cur_bb->getParent());
                        kb.CreateCondBr(kb.CreateICmpEQ(warp, llvm::ConstantInt::get(i32_ty, 0)),
                                        then_bb, join_bb);
                        kb.SetInsertPoint(then_bb);
                        // lane 0..7 有效（8 warps）；lane>=8 归零（clamp 读避免越界）
                        auto* lane_c = kb.CreateAnd(lane, llvm::ConstantInt::get(i32_ty, 7));
                        llvm::Value* acc2 = kb.CreateLoad(fnty, smem_gep(lane_c));
                        acc2 = kb.CreateSelect(
                            kb.CreateICmpULT(lane, llvm::ConstantInt::get(i32_ty, 8)),
                            acc2, zval);
                        for (int off : {4, 2, 1}) {
                            auto* o = llvm::ConstantInt::get(i32_ty, off);
                            acc2 = combine(acc2, shfl_d(acc2, o));
                        }
                        auto* is_l0b = kb.CreateICmpEQ(lane,
                            llvm::ConstantInt::get(i32_ty, 0));
                        auto* st2_bb = llvm::BasicBlock::Create(ctx_, "blkred_st2",
                            kb.GetInsertBlock()->getParent());
                        auto* st2_join = llvm::BasicBlock::Create(ctx_, "blkred_st2_join",
                            kb.GetInsertBlock()->getParent());
                        kb.CreateCondBr(is_l0b, st2_bb, st2_join);
                        kb.SetInsertPoint(st2_bb);
                        kb.CreateStore(acc2, smem_gep(llvm::ConstantInt::get(i32_ty, 0)));
                        kb.CreateBr(st2_join);
                        kb.SetInsertPoint(st2_join);
                        kb.CreateBr(join_bb);
                        kb.SetInsertPoint(join_bb);
                        // 5) sync（uniform）
                        kb.CreateCall(bar, {zero});
                        // 6) 读 smem[0]（所有线程 broadcast）
                        return kb.CreateLoad(fnty, smem_gep(llvm::ConstantInt::get(i32_ty, 0)),
                            "blkred_result");
                    }
                }
            }
            // Handle math functions
            std::string callee_name;
            if (e.callee->kind == ExprKind::Identifier)
                callee_name = static_cast<const IdentifierExpr&>(*e.callee).name;

            if (!callee_name.empty()) {
                // §3.6 向量打包访问：load4(a, i) / store4(a, i, v) —— kernel 内
                // <4 x float> 打包 load/store（与 CPU 回退同语义，未对齐 16B）。
                if (callee_name == "load4" || callee_name == "store4") {
                    auto* f4 = llvm::FixedVectorType::get(float_ty, 4);
                    auto* ap = emitKernelExpr(*e.args[0], kb, kernel_vars,
                        kernel_arg_values, loop_var_name, tid_val);
                    auto* idx = emitKernelExpr(*e.args[1], kb, kernel_vars,
                        kernel_arg_values, loop_var_name, tid_val);
                    if (!ap) return llvm::ConstantInt::get(i64_ty, 0);
                    if (idx->getType() != i64_ty)
                        idx = kb.CreateIntCast(idx, i64_ty, false);
                    auto* elem_off = kb.CreateMul(idx,
                        llvm::ConstantInt::get(i64_ty, 4), "v4off");
                    auto* p = kb.CreateGEP(float_ty, ap, elem_off, "v4p");
                    if (callee_name == "store4") {
                        auto* v = emitKernelExpr(*e.args[2], kb, kernel_vars,
                            kernel_arg_values, loop_var_name, tid_val);
                        kb.CreateAlignedStore(v, p, llvm::Align(4));
                        return nullptr;
                    }
                    return kb.CreateAlignedLoad(f4, p, llvm::Align(4), "v4");
                }
                // GPU math: emit calls to CUDA libdevice device functions (__nv_*).
                // generateGpuKernel links NVIDIA's libdevice.10.bc bitcode into the
                // kernel module at compile time, so __nv_sin/__nv_cos/__nv_exp/...
                // are resolved and the emitted PTX is fully self-contained.
                // §6.3 统一数学：LLVM intrinsic 化——把 __nv_* libdevice 调用改为
                // llvm.* intrinsic（NVPTX/AMDGPU 均原生下降，摆脱厂商数学库）。
                auto nvMathIntrinsic = [](const std::string& nv) -> llvm::Intrinsic::ID {
                    if (nv == "__nv_sqrt")  return llvm::Intrinsic::sqrt;
                    if (nv == "__nv_sin")   return llvm::Intrinsic::sin;
                    if (nv == "__nv_cos")   return llvm::Intrinsic::cos;
                    if (nv == "__nv_tan")   return llvm::Intrinsic::tan;
                    if (nv == "__nv_exp")   return llvm::Intrinsic::exp;
                    if (nv == "__nv_log")   return llvm::Intrinsic::log;
                    if (nv == "__nv_fabs")  return llvm::Intrinsic::fabs;
                    if (nv == "__nv_floor") return llvm::Intrinsic::floor;
                    if (nv == "__nv_ceil")  return llvm::Intrinsic::ceil;
                    if (nv == "__nv_trunc") return llvm::Intrinsic::trunc;
                    if (nv == "__nv_asin")  return llvm::Intrinsic::asin;
                    if (nv == "__nv_acos")  return llvm::Intrinsic::acos;
                    if (nv == "__nv_atan")  return llvm::Intrinsic::atan;
                    if (nv == "__nv_sinh")  return llvm::Intrinsic::sinh;
                    if (nv == "__nv_cosh")  return llvm::Intrinsic::cosh;
                    if (nv == "__nv_tanh")  return llvm::Intrinsic::tanh;
                    if (nv == "__nv_pow")   return llvm::Intrinsic::pow;
                    if (nv == "__nv_atan2") return llvm::Intrinsic::atan2;
                    return llvm::Intrinsic::not_intrinsic;
                };
                auto emit_math_gpu = [&](const char* nv) -> llvm::Value* {
                    if (e.args.size() < 1) return nullptr;
                    auto* a = emitKernelExpr(*e.args[0], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    if (!a) return nullptr;
                    std::string nvname = nv;
                    // §9.5 整型 abs 内联 select（返回同宽整型）；其余整型提升 f64
                    if (a->getType()->isIntegerTy()) {
                        if (nvname == "__nv_fabs") {
                            auto* zero = llvm::ConstantInt::get(a->getType(), 0);
                            auto* neg = kb.CreateSub(zero, a);
                            auto* isneg = kb.CreateICmpSLT(a, zero);
                            return kb.CreateSelect(isneg, neg, a);
                        }
                        a = kb.CreateSIToFP(a, double_ty);
                    }
                    // §6.3：NVPTX 有原生指令的数学（sqrt/fabs/floor/ceil/trunc）
                    // 用 LLVM intrinsic——直接降为指令，无需 libdevice；其余超越
                    // （sin/cos/tan/exp/log/asin/...）NVPTX 无指令，llvm.* 会降为
                    // flog/fexp 等 libcall（libdevice 只提供 __nv_*，无此名），
                    // 故仍走 __nv_* + libdevice。跨厂商时超越侧换 AMDGPU ocml 即可。
                    if (a->getType()->isFloatingPointTy()) {
                        bool native = (nvname == "__nv_sqrt" || nvname == "__nv_fabs" ||
                                       nvname == "__nv_floor" || nvname == "__nv_ceil" ||
                                       nvname == "__nv_trunc");
                        llvm::Intrinsic::ID id = nvMathIntrinsic(nvname);
                        if (native && id != llvm::Intrinsic::not_intrinsic)
                            return kb.CreateIntrinsic(id, {a->getType()}, {a});
                    }
                    // 超越 → __nv_* + libdevice
                    gpu_math_used_ = true;
                    llvm::Module* cur_mod = kb.GetInsertBlock()->getParent()->getParent();
                    std::string fname = nvname;
                    llvm::Type* arg_ty = double_ty;
                    if (a->getType()->isFloatTy()) {
                        arg_ty = float_ty;
                        if (fname.back() != 'f') fname += 'f';
                    } else if (a->getType()->isIntegerTy()) {
                        a = kb.CreateSIToFP(a, double_ty);
                    }
                    auto* fn = cur_mod->getFunction(fname);
                    if (!fn)
                        fn = llvm::Function::Create(
                            llvm::FunctionType::get(arg_ty, {arg_ty}, false),
                            llvm::Function::ExternalLinkage, fname, cur_mod);
                    return kb.CreateCall(fn, {a});
                };
                auto emit_math_pow_gpu = [&]() -> llvm::Value* {
                    if (e.args.size() < 2) return nullptr;
                    auto* a = emitKernelExpr(*e.args[0], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    auto* b = emitKernelExpr(*e.args[1], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    if (!a || !b) return nullptr;
                    // 超越（pow）→ __nv_powf/__nv_pow + libdevice
                    gpu_math_used_ = true;
                    llvm::Module* cur_mod = kb.GetInsertBlock()->getParent()->getParent();
                    if (a->getType()->isFloatTy() && b->getType()->isFloatTy()) {
                        auto* fn = cur_mod->getFunction("__nv_powf");
                        if (!fn)
                            fn = llvm::Function::Create(
                                llvm::FunctionType::get(float_ty, {float_ty, float_ty}, false),
                                llvm::Function::ExternalLinkage, "__nv_powf", cur_mod);
                        return kb.CreateCall(fn, {a, b});
                    }
                    if (!a->getType()->isDoubleTy() && a->getType()->isIntegerTy()) a = kb.CreateSIToFP(a, double_ty);
                    if (!b->getType()->isDoubleTy() && b->getType()->isIntegerTy()) b = kb.CreateSIToFP(b, double_ty);
                    if (a->getType()->isFloatTy()) a = kb.CreateFPExt(a, double_ty);
                    if (b->getType()->isFloatTy()) b = kb.CreateFPExt(b, double_ty);
                    auto* fn = cur_mod->getFunction("__nv_pow");
                    if (!fn)
                        fn = llvm::Function::Create(
                            llvm::FunctionType::get(double_ty, {double_ty, double_ty}, false),
                            llvm::Function::ExternalLinkage, "__nv_pow", cur_mod);
                    return kb.CreateCall(fn, {a, b});
                };
                // §6.3 统一数学：2 参数（atan2）→ llvm.atan2（f32/f64 按实参自动选）
                auto emit_math_gpu_2 = [&](const char* nv) -> llvm::Value* {
                    if (e.args.size() < 2) return nullptr;
                    auto* a = emitKernelExpr(*e.args[0], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    auto* b = emitKernelExpr(*e.args[1], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    if (!a || !b) return nullptr;
                    (void)nv;
                    // 超越（atan2）→ __nv_atan2f/__nv_atan2 + libdevice
                    gpu_math_used_ = true;
                    llvm::Module* cur_mod = kb.GetInsertBlock()->getParent()->getParent();
                    if (a->getType()->isFloatTy() && b->getType()->isFloatTy()) {
                        auto* fn = cur_mod->getFunction("__nv_atan2f");
                        if (!fn)
                            fn = llvm::Function::Create(
                                llvm::FunctionType::get(float_ty, {float_ty, float_ty}, false),
                                llvm::Function::ExternalLinkage, "__nv_atan2f", cur_mod);
                        return kb.CreateCall(fn, {a, b});
                    }
                    if (!a->getType()->isDoubleTy() && a->getType()->isIntegerTy()) a = kb.CreateSIToFP(a, double_ty);
                    if (!b->getType()->isDoubleTy() && b->getType()->isIntegerTy()) b = kb.CreateSIToFP(b, double_ty);
                    if (a->getType()->isFloatTy()) a = kb.CreateFPExt(a, double_ty);
                    if (b->getType()->isFloatTy()) b = kb.CreateFPExt(b, double_ty);
                    auto* fn = cur_mod->getFunction("__nv_atan2");
                    if (!fn)
                        fn = llvm::Function::Create(
                            llvm::FunctionType::get(double_ty, {double_ty, double_ty}, false),
                            llvm::Function::ExternalLinkage, "__nv_atan2", cur_mod);
                    return kb.CreateCall(fn, {a, b});
                };
                // Math functions
                auto emit_math_1 = [&](const char* n) -> llvm::Value* {
                    if (e.args.size() < 1) return nullptr;
                    auto* a = emitKernelExpr(*e.args[0], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    if (!a) return nullptr;
                    // GPU mode: call CUDA libdevice device functions
                    if (gpu_kernel_mode_) {
                        if (std::string(n) == "myp_math_sqrt")  return emit_math_gpu("__nv_sqrt");
                        if (std::string(n) == "myp_math_sin")   return emit_math_gpu("__nv_sin");
                        if (std::string(n) == "myp_math_cos")   return emit_math_gpu("__nv_cos");
                        if (std::string(n) == "myp_math_tan")   return emit_math_gpu("__nv_tan");
                        if (std::string(n) == "myp_math_exp")   return emit_math_gpu("__nv_exp");
                        if (std::string(n) == "myp_math_log")   return emit_math_gpu("__nv_log");
                        if (std::string(n) == "myp_math_abs")   return emit_math_gpu("__nv_fabs");
                        if (std::string(n) == "myp_math_floor") return emit_math_gpu("__nv_floor");
                        if (std::string(n) == "myp_math_ceil")  return emit_math_gpu("__nv_ceil");
                        if (std::string(n) == "myp_math_asin")  return emit_math_gpu("__nv_asin");
                        if (std::string(n) == "myp_math_acos")  return emit_math_gpu("__nv_acos");
                        if (std::string(n) == "myp_math_atan")  return emit_math_gpu("__nv_atan");
                        if (std::string(n) == "myp_math_sinh")  return emit_math_gpu("__nv_sinh");
                        if (std::string(n) == "myp_math_cosh")  return emit_math_gpu("__nv_cosh");
                        if (std::string(n) == "myp_math_tanh")  return emit_math_gpu("__nv_tanh");
                        if (std::string(n) == "trunc")          return emit_math_gpu("__nv_trunc");
                    }
                    return kb.CreateCall(
                        llvm::Function::Create(
                            llvm::FunctionType::get(double_ty, {double_ty}, false),
                            llvm::Function::ExternalLinkage, n, kb.GetInsertBlock()->getParent()->getParent()),
                        {a});
                };
                if (callee_name == "sqrt") return emit_math_1("myp_math_sqrt");
                if (callee_name == "cos")  return emit_math_1("myp_math_cos");
                if (callee_name == "sin")  return emit_math_1("myp_math_sin");
                if (callee_name == "tan")  return emit_math_1("myp_math_tan");
                if (callee_name == "exp")  return emit_math_1("myp_math_exp");
                if (callee_name == "log")  return emit_math_1("myp_math_log");
                if (callee_name == "abs")  return emit_math_1("myp_math_abs");
                if (callee_name == "floor") return emit_math_1("myp_math_floor");
                if (callee_name == "ceil")  return emit_math_1("myp_math_ceil");
                if (callee_name == "trunc") return emit_math_1("trunc");
                if (callee_name == "asin")  return emit_math_1("myp_math_asin");
                if (callee_name == "acos")  return emit_math_1("myp_math_acos");
                if (callee_name == "atan")  return emit_math_1("myp_math_atan");
                if (callee_name == "sinh")  return emit_math_1("myp_math_sinh");
                if (callee_name == "cosh")  return emit_math_1("myp_math_cosh");
                if (callee_name == "tanh")  return emit_math_1("myp_math_tanh");
                if (callee_name == "atan2" && e.args.size() >= 2) {
                    auto* a = emitKernelExpr(*e.args[0], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    auto* b = emitKernelExpr(*e.args[1], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    if (!a || !b) return nullptr;
                    if (gpu_kernel_mode_) return emit_math_gpu_2("__nv_atan2");
                    return kb.CreateCall(
                        llvm::Function::Create(
                            llvm::FunctionType::get(double_ty, {double_ty, double_ty}, false),
                            llvm::Function::ExternalLinkage, "myp_math_atan2",
                            kb.GetInsertBlock()->getParent()->getParent()),
                        {a, b});
                }
                // Handle __myp_math_* intrinsic calls (e.g., from Math.log inlining)
                if (callee_name.find("__myp_math_") == 0 && e.args.size() >= 1) {
                    std::string runtime_name = callee_name.substr(2); // __myp_math_log -> myp_math_log
                    // GPU mode: call CUDA libdevice device functions
                    if (gpu_kernel_mode_) {
                        // 2-arg functions first (atan2)
                        if (runtime_name == "myp_math_atan2" && e.args.size() >= 2)
                            return emit_math_gpu_2("__nv_atan2");
                        auto* a = emitKernelExpr(*e.args[0], kb, kernel_vars,
                                                  kernel_arg_values, loop_var_name, tid_val);
                        if (!a) return nullptr;
                        if (runtime_name == "myp_math_sqrt")  return emit_math_gpu("__nv_sqrt");
                        if (runtime_name == "myp_math_sin")   return emit_math_gpu("__nv_sin");
                        if (runtime_name == "myp_math_cos")   return emit_math_gpu("__nv_cos");
                        if (runtime_name == "myp_math_tan")   return emit_math_gpu("__nv_tan");
                        if (runtime_name == "myp_math_exp")   return emit_math_gpu("__nv_exp");
                        if (runtime_name == "myp_math_log")   return emit_math_gpu("__nv_log");
                        if (runtime_name == "myp_math_abs")   return emit_math_gpu("__nv_fabs");
                        if (runtime_name == "myp_math_floor") return emit_math_gpu("__nv_floor");
                        if (runtime_name == "myp_math_ceil")  return emit_math_gpu("__nv_ceil");
                        if (runtime_name == "myp_math_asin")  return emit_math_gpu("__nv_asin");
                        if (runtime_name == "myp_math_acos")  return emit_math_gpu("__nv_acos");
                        if (runtime_name == "myp_math_atan")  return emit_math_gpu("__nv_atan");
                        if (runtime_name == "myp_math_sinh")  return emit_math_gpu("__nv_sinh");
                        if (runtime_name == "myp_math_cosh")  return emit_math_gpu("__nv_cosh");
                        if (runtime_name == "myp_math_tanh")  return emit_math_gpu("__nv_tanh");
                        if (runtime_name == "myp_math_trunc") return emit_math_gpu("__nv_trunc");
                        if (runtime_name == "myp_math_pow")   return emit_math_pow_gpu();
                        return emit_math_gpu("__nv_sqrt"); // fallback
                    }
                    // CPU mode: resolve runtime function in the CURRENT module.
                    llvm::Module* cur_mod = kb.GetInsertBlock()->getParent()->getParent();
                    auto* runtime_fn = cur_mod->getFunction(runtime_name);
                    if (!runtime_fn) {
                        // Handle 2-arg functions (atan2)
                        if (runtime_name == "myp_math_atan2" && e.args.size() >= 2) {
                            auto* a = emitKernelExpr(*e.args[0], kb, kernel_vars,
                                                      kernel_arg_values, loop_var_name, tid_val);
                            auto* b = emitKernelExpr(*e.args[1], kb, kernel_vars,
                                                      kernel_arg_values, loop_var_name, tid_val);
                            if (!a || !b) return nullptr;
                            auto* ft = llvm::FunctionType::get(double_ty, {double_ty, double_ty}, false);
                            runtime_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                runtime_name, cur_mod);
                            return kb.CreateCall(runtime_fn, {a, b});
                        }
                        // Handle pow with 2 args
                        if (runtime_name == "myp_math_pow" && e.args.size() >= 2) {
                            auto* b = emitKernelExpr(*e.args[1], kb, kernel_vars,
                                                      kernel_arg_values, loop_var_name, tid_val);
                            if (!b) return nullptr;
                            auto* ft = llvm::FunctionType::get(double_ty, {double_ty, double_ty}, false);
                            runtime_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                runtime_name, cur_mod);
                        } else {
                            runtime_fn = llvm::Function::Create(
                                llvm::FunctionType::get(double_ty, {double_ty}, false),
                                llvm::Function::ExternalLinkage, runtime_name,
                                cur_mod);
                        }
                    }
                    auto* a = emitKernelExpr(*e.args[0], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    if (!a) return nullptr;
                    return kb.CreateCall(runtime_fn, {a});
                }
                if (callee_name == "pow" && e.args.size() >= 2) {
                    auto* a = emitKernelExpr(*e.args[0], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    auto* b = emitKernelExpr(*e.args[1], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    if (!a || !b) return nullptr;
                    if (gpu_kernel_mode_) return emit_math_pow_gpu();
                    return kb.CreateCall(
                        llvm::Function::Create(
                            llvm::FunctionType::get(double_ty, {double_ty, double_ty}, false),
                            llvm::Function::ExternalLinkage, "myp_math_pow",
                            kb.GetInsertBlock()->getParent()->getParent()),
                        {a, b});
                }
                // Pool worker id: 当前 @parallel for worker 索引（0..N-1），非池线程为 -1。
                // 供并行 body 检测是否真的由多个 worker 线程执行（多线程启动检测）。
                if (callee_name == "myp_pool_worker_id" ||
                    callee_name == "__myp_pool_worker_id") {
                    llvm::Module* cur_mod = kb.GetInsertBlock()->getParent()->getParent();
                    auto* wid_fn = cur_mod->getFunction("myp_pool_worker_id");
                    if (!wid_fn) {
                        wid_fn = llvm::Function::Create(
                            llvm::FunctionType::get(i32_ty, {}, false),
                            llvm::Function::ExternalLinkage, "myp_pool_worker_id", cur_mod);
                        // Worker index is a thread-local value, constant for the
                        // duration of a @parallel for chunk loop. Mark
                        // readnone/nounwind/willreturn so LICM hoists the call
                        // out of the loop body (perf: workerId() was ~8.9% of
                        // the parallel-reduce hot loop when called per-iteration).
                        wid_fn->setDoesNotAccessMemory();
                        wid_fn->setDoesNotThrow();
                        wid_fn->setWillReturn();
                    }
                    return kb.CreateCall(wid_fn, {});
                }
            }

            // Handle Atomic.addDouble/Atomic.addInt — use atomicrmw in PTX
            // Check BEFORE function inlining to avoid inlining __myp_atomic_* calls
            if (e.callee->kind == ExprKind::MemberAccess) {
                auto& ma = static_cast<const MemberAccessExpr&>(*e.callee);
                if (ma.object->kind == ExprKind::Identifier &&
                    static_cast<const IdentifierExpr&>(*ma.object).name == "Atomic") {
                    bool is_double = (ma.member_name == "addDouble" || ma.member_name == "addFloat");
                    bool is_int = (ma.member_name == "addInt" || ma.member_name == "addLong");

                    if ((is_double || is_int) && e.args.size() >= 3) {
                        auto* arr_ptr = emitKernelExpr(*e.args[0], kb, kernel_vars,
                                                        kernel_arg_values, loop_var_name, tid_val);
                        auto* idx = emitKernelExpr(*e.args[1], kb, kernel_vars,
                                                    kernel_arg_values, loop_var_name, tid_val);
                        auto* val = emitKernelExpr(*e.args[2], kb, kernel_vars,
                                                    kernel_arg_values, loop_var_name, tid_val);
                        if (arr_ptr && idx && val) {
                            llvm::Type* elem_ty = is_double ? double_ty : i32_ty;
                            auto* elem_ptr = kb.CreateGEP(elem_ty, arr_ptr, zextIndexValue(kb, idx), "atomic_ptr");
                            if (is_double) {
                                if (val->getType()->isIntegerTy())
                                    val = kb.CreateSIToFP(val, double_ty);
                                else if (val->getType()->isFloatTy())
                                    val = kb.CreateFPExt(val, double_ty);  // float→double
                                return kb.CreateAtomicRMW(llvm::AtomicRMWInst::FAdd, elem_ptr, val,
                                    llvm::MaybeAlign(), llvm::AtomicOrdering::SequentiallyConsistent);
                            } else {
                                if (val->getType()->isDoubleTy())
                                    val = kb.CreateFPToSI(val, i32_ty);
                                else if (val->getType()->isIntegerTy() && !val->getType()->isIntegerTy(32))
                                    val = kb.CreateIntCast(val, i32_ty, true);
                                return kb.CreateAtomicRMW(llvm::AtomicRMWInst::Add, elem_ptr, val,
                                    llvm::MaybeAlign(), llvm::AtomicOrdering::SequentiallyConsistent);
                            }
                        }
                    }
                }
            }

            // Handle function call inlining for GPU kernels / parallel for
            // Look up the function definition and inline its body
            if (e.callee->kind == ExprKind::MemberAccess) {
                auto& ma = static_cast<const MemberAccessExpr&>(*e.callee);
                if (ma.object->kind == ExprKind::Identifier) {
                    auto& cls_name = static_cast<const IdentifierExpr&>(*ma.object).name;
                    std::string func_name = ma.member_name;
                    std::string fn_name = cls_name + "_" + func_name;
                    // Try to generate a direct function call instead of inlining.
                    // NOTE: only safe for CPU @parallel for (same module).
                    // In GPU kernel mode (gpu_for_stmt_), the kernel is a SEPARATE
                    // LLVM module — calling main-module functions would produce a
                    // cross-module reference that fails LLVM verification.
                    if (!gpu_kernel_mode_) {
                        auto* callee_fn = module_->getFunction(fn_name);
                        if (callee_fn) {
                            std::vector<llvm::Value*> call_args;
                            for (auto& arg : e.args) {
                                auto* v = emitKernelExpr(*arg, kb, kernel_vars,
                                                          kernel_arg_values, loop_var_name, tid_val);
                                if (!v) break;
                                call_args.push_back(v);
                            }
                            if (call_args.size() == e.args.size()) {
                                return kb.CreateCall(callee_fn, call_args);
                            }
                        }
                    }
                }
            }
            if (e.callee->kind == ExprKind::MemberAccess) {
                auto& ma = static_cast<const MemberAccessExpr&>(*e.callee);
                if (ma.object->kind == ExprKind::Identifier) {
                    auto& cls_name = static_cast<const IdentifierExpr&>(*ma.object).name;
                    std::string func_name = ma.member_name;

                    // Try to find the function in the translation unit
                    if (current_tu_) {
                        // Look in class static actions
                        for (auto& cls : current_tu_->classes) {
                            if (cls.name != cls_name) continue;
                            for (auto& sa : cls.static_actions) {
                                if (sa.name != func_name || !sa.body) continue;
                                
                                // Create parameter mapping
                                std::map<std::string, llvm::Value*> param_vars;
                                for (size_t ai = 0; ai < sa.params.size() && ai < e.args.size(); ai++) {
                                    auto* arg_val = emitKernelExpr(*e.args[ai], kb, kernel_vars,
                                                                    kernel_arg_values, loop_var_name, tid_val);
                                    if (arg_val) param_vars[sa.params[ai].name] = arg_val;
                                }

                                // Inline the body
                                // For return expr statements: just emit the expression
                                if (sa.body->kind == StmtKind::ExprStmt) {
                                    auto* es = static_cast<const ExprStmt*>(sa.body.get());
                                    if (es->expression) {
                                        return emitKernelExpr(*es->expression, kb, kernel_vars,
                                                               kernel_arg_values, loop_var_name, tid_val);
                                    }
                                }
                                // For block bodies: merge param vars into kernel_vars then emit
                                if (sa.body->kind == StmtKind::Block) {
                                    auto& blk = static_cast<const BlockStmt&>(*sa.body);
                                    // Merge param vars
                                    auto merged_vars = kernel_vars;
                                    for (auto& pv : param_vars)
                                        merged_vars[pv.first] = pv.second;
                                    // Generate the block
                                    for (auto& s : blk.statements) {
                                        // If it's the last statement, handle return value
                                        if (&s == &blk.statements.back()) {
                                            if (s->kind == StmtKind::ExprStmt) {
                                                auto* es = static_cast<const ExprStmt*>(s.get());
                                                if (es->expression)
                                                    return emitKernelExpr(*es->expression, kb, merged_vars,
                                                                           kernel_arg_values, loop_var_name, tid_val);
                                            } else if (s->kind == StmtKind::ReturnStmt) {
                                                auto& rs = static_cast<const ReturnStmt&>(*s);
                                                if (rs.value)
                                                    return emitKernelExpr(*rs.value, kb, merged_vars,
                                                                           kernel_arg_values, loop_var_name, tid_val);
                                                return llvm::ConstantInt::get(i64_ty, 0);
                                            }
                                        }
                                        emitKernelStmt(*s, kb, merged_vars, kernel_arg_values,
                                                       loop_var_name, tid_val);
                                    }
                                    return llvm::ConstantInt::get(i64_ty, 0);
                                }
                                break;
                            }
                        }
                        // Look in standalone functions
                        for (auto& fn : current_tu_->functions) {
                            if (fn.name != func_name || !fn.body) continue;
                            std::map<std::string, llvm::Value*> param_vars;
                            for (size_t ai = 0; ai < fn.params.size() && ai < e.args.size(); ai++) {
                                auto* arg_val = emitKernelExpr(*e.args[ai], kb, kernel_vars,
                                                                kernel_arg_values, loop_var_name, tid_val);
                                if (arg_val) param_vars[fn.params[ai].name] = arg_val;
                            }
                            // fn.body is BlockStmt — iterate statements
                            auto merged_vars2 = kernel_vars;
                            for (auto& pv : param_vars) merged_vars2[pv.first] = pv.second;
                            for (auto& s : fn.body->statements) {
                                if (&s == &fn.body->statements.back()) {
                                    if (s->kind == StmtKind::ExprStmt) {
                                        auto* es = static_cast<const ExprStmt*>(s.get());
                                        if (es->expression)
                                            return emitKernelExpr(*es->expression, kb, merged_vars2,
                                                                   kernel_arg_values, loop_var_name, tid_val);
                                    } else if (s->kind == StmtKind::ReturnStmt) {
                                        auto& rs = static_cast<const ReturnStmt&>(*s);
                                        if (rs.value)
                                            return emitKernelExpr(*rs.value, kb, merged_vars2,
                                                                   kernel_arg_values, loop_var_name, tid_val);
                                    }
                                }
                                emitKernelStmt(*s, kb, merged_vars2, kernel_arg_values,
                                               loop_var_name, tid_val);
                            }
                            break;
                        }
                    }
                }
            }

            // Fallback: just return 0
            return llvm::ConstantInt::get(i64_ty, 0);
        }
        case ExprKind::MemberAccess: {
            auto& e = static_cast<const MemberAccessExpr&>(expr);
            // §3.1 kernel 执行上下文字段（线程不变量）：gid/bx/tx/bd/gx。
            if (e.object->kind == ExprKind::Identifier) {
                auto& ko = static_cast<const IdentifierExpr&>(*e.object);
                if (ko.name == "kernel") {
                    if (e.member_name == "gid") return gpu_ctx_tid_;
                    if (e.member_name == "bx")  return gpu_ctx_ctaid_;
                    if (e.member_name == "tx")  return gpu_ctx_tid_x_;
                    if (e.member_name == "bd")  return gpu_ctx_ntid_;
                    if (e.member_name == "gx") {
                        // ceil(n / bd) = (n + bd - 1) / bd
                        auto* one = llvm::ConstantInt::get(i64_ty, 1);
                        auto* num = kb.CreateAdd(gpu_ctx_n_arg_,
                            kb.CreateSub(gpu_ctx_ntid_, one), "gx_num");
                        return kb.CreateUDiv(num, gpu_ctx_ntid_, "gx");
                    }
                    return llvm::ConstantInt::get(i64_ty, 0);
                }
            }
            // §3.6 向量成员读：float4.x/y/z/w → extractelement（resolved_kind 标记）。
            if (e.object->resolved_kind == TypeKind::Float4 ||
                e.object->resolved_kind == TypeKind::Double2 ||
                e.object->resolved_kind == TypeKind::Int4) {
                auto* objv = emitKernelExpr(*e.object, kb, kernel_vars,
                    kernel_arg_values, loop_var_name, tid_val);
                if (objv && objv->getType()->isVectorTy()) {
                    const std::string names = (e.object->resolved_kind == TypeKind::Double2)
                        ? "xy" : "xyzw";
                    if (e.member_name.size() == 1 &&
                        names.find(e.member_name[0]) != std::string::npos) {
                        unsigned idx = (e.member_name[0] == 'x') ? 0 :
                                       (e.member_name[0] == 'y') ? 1 :
                                       (e.member_name[0] == 'z') ? 2 : 3;
                        return kb.CreateExtractElement(objv,
                            llvm::ConstantInt::get(i32_ty, idx), "vec_comp");
                    }
                }
                return llvm::ConstantInt::get(i64_ty, 0);
            }
            // Three cases:
            // 1. ClassName.property (static class) — look up in static_property_globals_
            // 2. obj.property (struct field) — struct GEP access
            // 3. this.property — this pointer struct access

            if (e.object->kind == ExprKind::Identifier) {
                auto& oi = static_cast<const IdentifierExpr&>(*e.object);
                auto sit = static_property_globals_.find(oi.name);
                if (sit != static_property_globals_.end()) {
                    // BUG-023: 静态类属性数组在 @parallel for / @gpu for 体内访问。
                    // 此前要求 X 在 kernel_vars（只捕获外层局部变量）→ 静态类名不在
                    // → 落到 i64 0 占位 → 下标 GEP 基址为整数 0（LLVM verify 失败：
                    // "GEP base pointer is not a vector..."）。修复：直接以模块全局
                    // __myp_static_<Class> 为基址 GEP 进属性槽（CPU @parallel 同模块
                    // 可直取）；@gpu 核函数（独立 PTX 模块）仍走捕获的 kernel arg。
                    llvm::Value* st_ptr = sit->second;
                    auto cvit = kernel_vars.find(oi.name);
                    if (cvit != kernel_vars.end())
                        st_ptr = cvit->second;
                    // Find the class and property index
                    for (auto& cls : current_tu_->classes) {
                        if (cls.name == oi.name) {
                            unsigned pi = 0;
                            if (getPropertyIndex(cls.name, e.member_name, pi)) {
                                auto* st = getClassStruct(cls.name);
                                if (st) {
                                    auto* gep = kb.CreateStructGEP(st, st_ptr, pi);
                                    auto* pt = getPropertyType(cls, e.member_name);
                                    if (pt->isArrayTy()) return gep;
                                    return kb.CreateLoad(pt, gep);
                                }
                            }
                        }
                    }
                }
                // Struct field access
                auto* oa = getNamedValue(oi.name);
                if (oa) {
                    llvm::StructType* st2 = nullptr;
                    llvm::Value* bp = nullptr;
                    if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(oa)) {
                        auto* at = ai->getAllocatedType();
                        if (at->isStructTy()) { st2 = llvm::cast<llvm::StructType>(at); bp = oa; }
                    } else {
                        auto* nty = getNamedValueType(oi.name);
                        if (nty && nty->isStructTy()) { st2 = llvm::cast<llvm::StructType>(nty); bp = oa; }
                    }
                    // For kernel: we pass struct by pointer
                    auto cvit = kernel_vars.find(oi.name);
                    if (cvit != kernel_vars.end() && st2) {
                        unsigned fi = 0;
                        if (getStructFieldIndex(st2->getName().str(), e.member_name, fi)) {
                            auto* gep = kb.CreateStructGEP(st2, cvit->second, fi);
                            auto* ft = st2->getElementType(fi);
                            if (ft->isArrayTy()) return gep;
                            return kb.CreateLoad(ft, gep);
                        }
                    }
                }
            }
            // v[i].field — struct-array / slice-of-struct element field read in
            // a @parallel for body.
            if (e.object->kind == ExprKind::Subscript) {
                auto& ss = static_cast<const SubscriptExpr&>(*e.object);
                auto* idx = emitKernelExpr(*ss.index, kb, kernel_vars, kernel_arg_values,
                                           loop_var_name, tid_val);
                if (idx) {
                    auto* elem_ptr = emitKernelElementAddr(ss.array.get(), idx, kb,
                        kernel_vars, kernel_arg_values, loop_var_name, tid_val);
                    if (elem_ptr) {
                        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(elem_ptr)) {
                            auto* rt = gep->getResultElementType();
                            if (rt && rt->isStructTy()) {
                                auto* st = llvm::cast<llvm::StructType>(rt);
                                unsigned fi = 0;
                                if (getStructFieldIndex(st->getName().str(), e.member_name, fi)) {
                                    auto* fgep = kb.CreateStructGEP(st, elem_ptr, fi);
                                    auto* ft = st->getElementType(fi);
                                    if (ft->isArrayTy()) return fgep;
                                    return kb.CreateLoad(ft, fgep);
                                }
                            }
                        }
                    }
                }
            }
            // Class-instance field access (obj.field where obj is a class object)
            // is not supported by the kernel path — it previously fell through to
            // constant 0 (reads) / was dropped (writes), silently corrupting
            // results (tested: `new Node(); n.val = 7;` → all reads wrong).
            if (e.object->kind == ExprKind::Identifier) {
                auto& oid = static_cast<const IdentifierExpr&>(*e.object);
                if (var_class_map_.count(oid.name)) {
                    diag_.error(expr.range,
                        "class-instance field access is not supported inside '@parallel for' / '@gpu for' bodies");
                    return llvm::ConstantInt::get(i64_ty, 0);
                }
            }
            return llvm::ConstantInt::get(i64_ty, 0);
        }
        case ExprKind::Subscript: {
            auto& e = static_cast<const SubscriptExpr&>(expr);
            auto* arr = emitKernelExpr(*e.array, kb, kernel_vars, kernel_arg_values,
                                        loop_var_name, tid_val);
            auto* idx = emitKernelExpr(*e.index, kb, kernel_vars, kernel_arg_values,
                                        loop_var_name, tid_val);
            if (!arr || !idx) return llvm::ConstantInt::get(i64_ty, 0);

            // slice<T>[i] inside a @parallel for body: arr is the captured
            // {data,len} slice value (data pointer is thread-shared). Unpack,
            // bounds-check, GEP.
            if (e.array->kind == ExprKind::Identifier) {
                auto& id = static_cast<const IdentifierExpr&>(*e.array);
                auto sit = var_slice_types_.find(id.name);
                if (sit != var_slice_types_.end() && sit->second.element_type &&
                    arr->getType()->isStructTy()) {
                    auto* data = kb.CreateExtractValue(arr, 0);
                    auto* len = kb.CreateExtractValue(arr, 1);
                    auto* i64idx = idx;
                    if (i64idx->getType()->isIntegerTy(32) || i64idx->getType()->isIntegerTy(8)
                        || i64idx->getType()->isIntegerTy(16))
                        i64idx = kb.CreateZExt(i64idx, i64_ty);
                    auto* nonneg = kb.CreateICmpSGE(i64idx, llvm::ConstantInt::get(i64_ty, 0));
                    auto* inb = kb.CreateICmpULT(i64idx, len);
                    auto* ok = kb.CreateAnd(nonneg, inb);
                    auto* be_fn = module_->getFunction("myp_bounds_error");
                    if (!be_fn) {
                        auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                            {i64_ty, i64_ty}, false);
                        be_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                            "myp_bounds_error", module_.get());
                    }
                    auto* cur_fn = kb.GetInsertBlock()->getParent();
                    auto* err_bb = llvm::BasicBlock::Create(ctx_, "slice_oob", cur_fn);
                    auto* cont_bb = llvm::BasicBlock::Create(ctx_, "slice_ok", cur_fn);
                    kb.CreateCondBr(ok, cont_bb, err_bb);
                    kb.SetInsertPoint(err_bb);
                    kb.CreateCall(be_fn, {i64idx, len});
                    kb.CreateBr(cont_bb);
                    kb.SetInsertPoint(cont_bb);
                    auto* et = getLLVMType(*sit->second.element_type);
                    auto* gep = kb.CreateGEP(et, data, i64idx);
                    return kb.CreateLoad(et, gep);
                }
            }

            // Determine element type from the subscript context
            llvm::Type* elem_ty = double_ty;  // default
            // Try to infer element type
            if (e.array->kind == ExprKind::Identifier) {
                auto& id = static_cast<const IdentifierExpr&>(*e.array);
                // §3.2 @gpu tile 共享数组：元素类型来自 gpu_shared_arrays_
                auto sit2 = gpu_shared_arrays_.find(id.name);
                if (sit2 != gpu_shared_arrays_.end()) {
                    elem_ty = sit2->second;
                } else {
                    auto eit = array_elem_types_.find(id.name);
                    if (eit != array_elem_types_.end())
                        elem_ty = eit->second;
                }
            } else if (e.array->kind == ExprKind::MemberAccess) {
                auto& ma = static_cast<const MemberAccessExpr&>(*e.array);
                if (ma.object->kind == ExprKind::Identifier) {
                    auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
                    auto sit = static_property_globals_.find(oi.name);
                    if (sit != static_property_globals_.end() && current_tu_) {
                        for (auto& cls : current_tu_->classes) {
                            if (cls.name == oi.name) {
                                for (auto& p : cls.properties) {
                                    if (p.name == ma.member_name && p.type.isArray()) {
                                        elem_ty = typeNodeToLLVMType(*p.type.element_type);
                                        goto found_elem;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            found_elem:
            // For int[] arrays, the pointer is i32*, not double*
            if (elem_ty->isIntegerTy(32)) {
                // The arr is already a pointer, GEP on it
                auto* gep = kb.CreateGEP(elem_ty, arr, zextIndexValue(kb, idx));
                return kb.CreateLoad(elem_ty, gep);
            }
            auto* gep = kb.CreateGEP(elem_ty, arr, zextIndexValue(kb, idx));
            return kb.CreateLoad(elem_ty, gep);
        }
        case ExprKind::Assignment: {
            auto& e = static_cast<const AssignmentExpr&>(expr);
            // §3.6 向量成员写：v.x = value → load + insertelement + store。
            if (e.target->kind == ExprKind::MemberAccess) {
                auto& tma = static_cast<const MemberAccessExpr&>(*e.target);
                if (tma.object->resolved_kind == TypeKind::Float4 ||
                    tma.object->resolved_kind == TypeKind::Double2 ||
                    tma.object->resolved_kind == TypeKind::Int4) {
                    // v.x = value → 读 v 的 alloca，insertelement，写回 alloca。
                    llvm::Value* slot = nullptr;
                    if (tma.object->kind == ExprKind::Identifier) {
                        auto& oid = static_cast<const IdentifierExpr&>(*tma.object);
                        auto vit = kernel_vars.find(oid.name);
                        if (vit != kernel_vars.end() && llvm::isa<llvm::AllocaInst>(vit->second))
                            slot = vit->second;
                    }
                    auto* val = emitKernelExpr(*e.value, kb, kernel_vars,
                        kernel_arg_values, loop_var_name, tid_val);
                    if (slot && val) {
                        auto* objv = kb.CreateLoad(
                            llvm::cast<llvm::AllocaInst>(slot)->getAllocatedType(),
                            slot, "vcv");
                        const std::string names =
                            (tma.object->resolved_kind == TypeKind::Double2) ? "xy" : "xyzw";
                        if (tma.member_name.size() == 1 &&
                            names.find(tma.member_name[0]) != std::string::npos) {
                            unsigned idx = (tma.member_name[0] == 'x') ? 0 :
                                           (tma.member_name[0] == 'y') ? 1 :
                                           (tma.member_name[0] == 'z') ? 2 : 3;
                            auto* scalar_ty = objv->getType()->getScalarType();
                            if (val->getType() != scalar_ty) {
                                if (val->getType()->isIntegerTy() && scalar_ty->isFloatingPointTy())
                                    val = kb.CreateSIToFP(val, scalar_ty);
                                else if (val->getType()->isFloatingPointTy() && scalar_ty->isIntegerTy())
                                    val = kb.CreateFPToSI(val, scalar_ty);
                                else if (val->getType()->isIntegerTy() && scalar_ty->isIntegerTy())
                                    val = kb.CreateIntCast(val, scalar_ty, false);
                                else if (val->getType()->isFloatingPointTy() && scalar_ty->isFloatingPointTy())
                                    val = kb.CreateFPCast(val, scalar_ty);
                            }
                            auto* nv = kb.CreateInsertElement(objv, val,
                                llvm::ConstantInt::get(i32_ty, idx), "vec_set");
                            kb.CreateStore(nv, slot);
                            return val;
                        }
                    }
                }
            }
            // v[i].field = value — struct-array / slice-of-struct element field
            // write inside a @parallel for body.
            if (e.target->kind == ExprKind::MemberAccess) {
                auto& tma = static_cast<const MemberAccessExpr&>(*e.target);
                if (tma.object->kind == ExprKind::Subscript) {
                    auto& ss = static_cast<const SubscriptExpr&>(*tma.object);
                    auto* idx = emitKernelExpr(*ss.index, kb, kernel_vars, kernel_arg_values,
                                               loop_var_name, tid_val);
                    auto* val = emitKernelExpr(*e.value, kb, kernel_vars, kernel_arg_values,
                                               loop_var_name, tid_val);
                    if (idx && val) {
                        auto* elem_ptr = emitKernelElementAddr(ss.array.get(), idx, kb,
                            kernel_vars, kernel_arg_values, loop_var_name, tid_val);
                        if (elem_ptr) {
                            if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(elem_ptr)) {
                                auto* rt = gep->getResultElementType();
                                if (rt && rt->isStructTy()) {
                                    auto* st = llvm::cast<llvm::StructType>(rt);
                                    unsigned fi = 0;
                                    if (getStructFieldIndex(st->getName().str(), tma.member_name, fi)) {
                                        auto* fgep = kb.CreateStructGEP(st, elem_ptr, fi);
                                        auto* ft = st->getElementType(fi);
                                        if (val->getType() != ft) {
                                            if (ft->isDoubleTy() && val->getType()->isIntegerTy())
                                                val = kb.CreateSIToFP(val, ft);
                                            else if (ft->isIntegerTy() && val->getType()->isDoubleTy())
                                                val = kb.CreateFPToSI(val, ft);
                                            else if (ft->isIntegerTy() && val->getType()->isIntegerTy())
                                                val = kb.CreateIntCast(val, ft, true);
                                            else if (ft->isDoubleTy() && val->getType()->isFloatTy())
                                                val = kb.CreateFPExt(val, ft);
                                            else if (ft->isFloatTy() && val->getType()->isDoubleTy())
                                                val = kb.CreateFPTrunc(val, ft);
                                        }
                                        kb.CreateStore(val, fgep);
                                        return val;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (e.target->kind == ExprKind::Subscript) {
                auto& ss = static_cast<const SubscriptExpr&>(*e.target);
                auto* arr = emitKernelExpr(*ss.array, kb, kernel_vars, kernel_arg_values,
                                            loop_var_name, tid_val);
                auto* idx = emitKernelExpr(*ss.index, kb, kernel_vars, kernel_arg_values,
                                            loop_var_name, tid_val);
                auto* val = emitKernelExpr(*e.value, kb, kernel_vars, kernel_arg_values,
                                            loop_var_name, tid_val);
                if (!arr || !idx || !val) return llvm::ConstantInt::get(i64_ty, 0);

                // slice<T>[i] = v inside a @parallel for body: unpack the captured
                // {data,len} slice value, bounds-check, GEP, store.
                if (ss.array->kind == ExprKind::Identifier) {
                    auto& id = static_cast<const IdentifierExpr&>(*ss.array);
                    auto sit = var_slice_types_.find(id.name);
                    if (sit != var_slice_types_.end() && sit->second.element_type &&
                        arr->getType()->isStructTy()) {
                        auto* data = kb.CreateExtractValue(arr, 0);
                        auto* len = kb.CreateExtractValue(arr, 1);
                        auto* i64idx = idx;
                        if (i64idx->getType()->isIntegerTy(32) || i64idx->getType()->isIntegerTy(8)
                            || i64idx->getType()->isIntegerTy(16))
                            i64idx = kb.CreateZExt(i64idx, i64_ty);
                        auto* nonneg = kb.CreateICmpSGE(i64idx, llvm::ConstantInt::get(i64_ty, 0));
                        auto* inb = kb.CreateICmpULT(i64idx, len);
                        auto* ok = kb.CreateAnd(nonneg, inb);
                        auto* be_fn = module_->getFunction("myp_bounds_error");
                        if (!be_fn) {
                            auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                                {i64_ty, i64_ty}, false);
                            be_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                "myp_bounds_error", module_.get());
                        }
                        auto* cur_fn = kb.GetInsertBlock()->getParent();
                        auto* err_bb = llvm::BasicBlock::Create(ctx_, "slice_oob", cur_fn);
                        auto* cont_bb = llvm::BasicBlock::Create(ctx_, "slice_ok", cur_fn);
                        kb.CreateCondBr(ok, cont_bb, err_bb);
                        kb.SetInsertPoint(err_bb);
                        kb.CreateCall(be_fn, {i64idx, len});
                        kb.CreateBr(cont_bb);
                        kb.SetInsertPoint(cont_bb);
                        auto* et = getLLVMType(*sit->second.element_type);
                        auto* gep = kb.CreateGEP(et, data, i64idx);
                        if (val->getType() != et) {
                            if (et->isDoubleTy() && val->getType()->isIntegerTy())
                                val = kb.CreateSIToFP(val, et);
                            else if (et->isIntegerTy() && val->getType()->isDoubleTy())
                                val = kb.CreateFPToSI(val, et);
                            else if (et->isIntegerTy() && val->getType()->isIntegerTy())
                                val = kb.CreateIntCast(val, et, true);
                            else if (et->isDoubleTy() && val->getType()->isFloatTy())
                                val = kb.CreateFPExt(val, et);
                            else if (et->isFloatTy() && val->getType()->isDoubleTy())
                                val = kb.CreateFPTrunc(val, et);
                        }
                        kb.CreateStore(val, gep);
                        return val;
                    }
                }

                // Determine element type
                llvm::Type* elem_ty = double_ty;
                if (ss.array->kind == ExprKind::Identifier) {
                    auto& id = static_cast<const IdentifierExpr&>(*ss.array);
                    // §3.2 @gpu tile 共享数组：元素类型来自 gpu_shared_arrays_
                    auto st2 = gpu_shared_arrays_.find(id.name);
                    if (st2 != gpu_shared_arrays_.end()) {
                        elem_ty = st2->second;
                    } else {
                        auto eit = array_elem_types_.find(id.name);
                        if (eit != array_elem_types_.end())
                            elem_ty = eit->second;
                    }
                } else if (ss.array->kind == ExprKind::MemberAccess) {
                    auto& ma = static_cast<const MemberAccessExpr&>(*ss.array);
                    if (ma.object->kind == ExprKind::Identifier) {
                        auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
                        if (current_tu_) {
                            for (auto& cls : current_tu_->classes) {
                                if (cls.name == oi.name) {
                                    for (auto& p : cls.properties) {
                                        if (p.name == ma.member_name && p.type.isArray()) {
                                            elem_ty = typeNodeToLLVMType(*p.type.element_type);
                                            goto assign_found1;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                assign_found1:
                // Type cast value to element type
                if (val->getType() != elem_ty) {
                    if (elem_ty->isDoubleTy() && val->getType()->isIntegerTy())
                        val = kb.CreateSIToFP(val, elem_ty);
                    else if (elem_ty->isIntegerTy() && val->getType()->isDoubleTy())
                        val = kb.CreateFPToSI(val, elem_ty);
                    else if (elem_ty->isIntegerTy() && val->getType()->isIntegerTy())
                        val = kb.CreateIntCast(val, elem_ty, true);
                    else if (elem_ty->isDoubleTy() && val->getType()->isFloatTy())
                        val = kb.CreateFPExt(val, elem_ty);
                    else if (elem_ty->isFloatTy() && val->getType()->isDoubleTy())
                        val = kb.CreateFPTrunc(val, elem_ty);
                }
                auto* gep = kb.CreateGEP(elem_ty, arr, zextIndexValue(kb, idx));
                kb.CreateStore(val, gep);
                return val;
            }
            // Simple var = value (not subscript)
            if (e.target->kind == ExprKind::Identifier) {
                auto& id = static_cast<const IdentifierExpr&>(*e.target);
                auto* val = emitKernelExpr(*e.value, kb, kernel_vars, kernel_arg_values,
                                            loop_var_name, tid_val);
                if (val) {
                    auto vit2 = kernel_vars.find(id.name);
                    if (vit2 != kernel_vars.end() && llvm::isa<llvm::AllocaInst>(vit2->second)) {
                        auto* alloca_p = llvm::cast<llvm::AllocaInst>(vit2->second);
                        if (val->getType() != alloca_p->getAllocatedType()) {
                            if (alloca_p->getAllocatedType()->isIntegerTy() && val->getType()->isIntegerTy())
                                val = kb.CreateIntCast(val, alloca_p->getAllocatedType(), true);
                            else if (alloca_p->getAllocatedType()->isDoubleTy() && val->getType()->isIntegerTy())
                                val = kb.CreateSIToFP(val, alloca_p->getAllocatedType());
                            else if (alloca_p->getAllocatedType()->isIntegerTy() && val->getType()->isDoubleTy())
                                val = kb.CreateFPToSI(val, alloca_p->getAllocatedType());
                            // 浮点宽窄转换：float→double FPExt，double→float FPTrunc。
                            // 此前缺失 → float 原样 store 进 double 槽 = 位重解释垃圾值。
                            else if (alloca_p->getAllocatedType()->isDoubleTy() && val->getType()->isFloatTy())
                                val = kb.CreateFPExt(val, alloca_p->getAllocatedType());
                            else if (alloca_p->getAllocatedType()->isFloatTy() && val->getType()->isDoubleTy())
                                val = kb.CreateFPTrunc(val, alloca_p->getAllocatedType());
                        }
                        kb.CreateStore(val, alloca_p);
                    }
                    // Don't update kernel_vars — value doesn't dominate all uses
                }
                return val;
            }
            return llvm::ConstantInt::get(i64_ty, 0);
        }
        case ExprKind::Ternary: {
            auto& e = static_cast<const TernaryExpr&>(expr);
            auto* cond = emitKernelExpr(*e.condition, kb, kernel_vars, kernel_arg_values,
                                         loop_var_name, tid_val);
            if (!cond) return llvm::ConstantInt::get(i64_ty, 0);
            if (cond->getType()->isIntegerTy() && !cond->getType()->isIntegerTy(1))
                cond = kb.CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0));

            auto* func = kb.GetInsertBlock()->getParent();
            auto* true_bb = llvm::BasicBlock::Create(ctx_, "kt_true", func);
            auto* false_bb = llvm::BasicBlock::Create(ctx_, "kt_false", func);
            auto* merge_bb = llvm::BasicBlock::Create(ctx_, "kt_merge", func);

            kb.CreateCondBr(cond, true_bb, false_bb);

            kb.SetInsertPoint(true_bb);
            auto* true_val = emitKernelExpr(*e.true_expr, kb, kernel_vars,
                                             kernel_arg_values, loop_var_name, tid_val);
            if (!true_val) true_val = llvm::ConstantInt::get(i64_ty, 0);
            if (!kb.GetInsertBlock()->getTerminator())
                kb.CreateBr(merge_bb);

            kb.SetInsertPoint(false_bb);
            auto* false_val = emitKernelExpr(*e.false_expr, kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
            if (!false_val) false_val = llvm::ConstantInt::get(i64_ty, 0);
            if (!kb.GetInsertBlock()->getTerminator())
                kb.CreateBr(merge_bb);

            kb.SetInsertPoint(merge_bb);
            auto* phi = kb.CreatePHI(true_val->getType(), 2);
            phi->addIncoming(true_val, true_bb);
            phi->addIncoming(false_val, false_bb);
            return phi;
        }
        case ExprKind::ThisExpr:
        case ExprKind::NullLiteral:
        case ExprKind::Range:
        case ExprKind::Lambda:
        case ExprKind::EnumVariant:
            return llvm::ConstantInt::get(i64_ty, 0);
        // Class/array allocation inside a @parallel for / @gpu for body is not
        // supported by the kernel path — it previously silently returned 0
        // (null), so `new Node()` + field writes produced garbage results with
        // no error (tested: 999/1000 wrong). Reject cleanly; allocate outside
        // the parallel loop and write into it.
        case ExprKind::NewExpr:
        case ExprKind::NewArrayExpr:
            diag_.error(expr.range,
                "'new' inside '@parallel for' / '@gpu for' bodies is not supported — allocate before the loop and write into the captured variable");
            return llvm::ConstantInt::get(i64_ty, 0);
        case ExprKind::StringLiteral:
            diag_.error(expr.range,
                "string literals are not supported inside '@parallel for' / '@gpu for' bodies");
            return llvm::ConstantInt::get(i64_ty, 0);
    }
    return llvm::ConstantInt::get(i64_ty, 0);
}

void CodeGen::emitKernelStmt(const Stmt& stmt, llvm::IRBuilder<>& kb,
    std::map<std::string, llvm::Value*>& kernel_vars,
    const std::vector<llvm::Value*>& kernel_arg_values,
    const std::string& loop_var_name, llvm::Value* tid_val,
    llvm::BasicBlock* break_target) {

    auto* i64_ty = llvm::Type::getInt64Ty(ctx_);

    switch (stmt.kind) {
        case StmtKind::Block: {
            auto& b = static_cast<const BlockStmt&>(stmt);
            for (auto& s : b.statements)
                emitKernelStmt(*s, kb, kernel_vars, kernel_arg_values,
                               loop_var_name, tid_val, break_target);
            break;
        }
        case StmtKind::ExprStmt: {
            auto& es = static_cast<const ExprStmt&>(stmt);
            if (es.expression)
                emitKernelExpr(*es.expression, kb, kernel_vars, kernel_arg_values,
                               loop_var_name, tid_val);
            break;
        }
        case StmtKind::VarDeclStmt: {
            auto& vds = static_cast<const VarDeclStmt&>(stmt);
            auto* kernel_func = kb.GetInsertBlock()->getParent();
            for (auto& d : vds.decls) {
                // 用「声明类型」确定变量类型（不能用初始化表达式类型——
                // `double x = <float expr>` 必须分配 double 并 fpext）。
                llvm::Value* init_val = nullptr;
                llvm::Type* var_ty = i64_ty;
                llvm::Type* declared_ty = getLLVMType(typeNodeToCodegenType(d.type));
                if (declared_ty) var_ty = declared_ty;
                if (d.init_expr) {
                    init_val = emitKernelExpr(*d.init_expr, kb, kernel_vars,
                                               kernel_arg_values, loop_var_name, tid_val);
                    if (init_val && !declared_ty) var_ty = init_val->getType();
                }
                // Create alloca for mutable local variable（§6.4 AMD → addrspace(5)）
                llvm::BasicBlock& entry_b = kernel_func->getEntryBlock();
                llvm::IRBuilder<> entry_kb(&entry_b, entry_b.getFirstInsertionPt());
                auto* alloca_p = createKernelAlloca(entry_kb, var_ty, nullptr, d.name);
                kernel_vars[d.name] = alloca_p;

                if (init_val) {
                    llvm::Type* src = init_val->getType();
                    llvm::Type* dst = var_ty;
                    if (src != dst) {
                        if (src->isDoubleTy() && dst->isFloatTy())
                            init_val = kb.CreateFPTrunc(init_val, dst);
                        else if (src->isFloatTy() && dst->isDoubleTy())
                            init_val = kb.CreateFPExt(init_val, dst);
                        else if (src->isFloatingPointTy() && dst->isIntegerTy())
                            init_val = kb.CreateFPToSI(init_val, dst);
                        else if (src->isIntegerTy() && dst->isFloatingPointTy())
                            init_val = kb.CreateSIToFP(init_val, dst);
                        else if (src->isIntegerTy() && dst->isIntegerTy())
                            init_val = kb.CreateIntCast(init_val, dst, true);
                        else if (src->isFloatingPointTy() && dst->isFloatingPointTy())
                            init_val = kb.CreateFPExt(init_val, dst);
                    }
                    kb.CreateStore(init_val, alloca_p);
                }
            }
            break;
        }
        case StmtKind::BreakStmt: {
            if (break_target) {
                kb.CreateBr(break_target);
                // After branch, create a new dead block to continue insertion
                auto* dead_bb = llvm::BasicBlock::Create(ctx_, "dead",
                    kb.GetInsertBlock()->getParent());
                kb.SetInsertPoint(dead_bb);
            }
            break;
        }
        case StmtKind::IfStmt: {
            auto& is = static_cast<const IfStmt&>(stmt);
            auto* cond = emitKernelExpr(*is.condition, kb, kernel_vars, kernel_arg_values,
                                         loop_var_name, tid_val);
            if (!cond) return;
            if (cond->getType()->isIntegerTy() && !cond->getType()->isIntegerTy(1))
                cond = kb.CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0));

            auto* func = kb.GetInsertBlock()->getParent();
            auto* then_bb = llvm::BasicBlock::Create(ctx_, "kif_then", func);
            auto* else_bb = llvm::BasicBlock::Create(ctx_, "kif_else", func);
            auto* merge_bb = llvm::BasicBlock::Create(ctx_, "kif_end", func);

            kb.CreateCondBr(cond, then_bb, else_bb);

            kb.SetInsertPoint(then_bb);
            emitKernelStmt(*is.then_block, kb, kernel_vars, kernel_arg_values,
                           loop_var_name, tid_val, break_target);
            if (!kb.GetInsertBlock()->getTerminator())
                kb.CreateBr(merge_bb);

            kb.SetInsertPoint(else_bb);
            if (is.else_block)
                emitKernelStmt(*is.else_block, kb, kernel_vars, kernel_arg_values,
                               loop_var_name, tid_val, break_target);
            if (!kb.GetInsertBlock()->getTerminator())
                kb.CreateBr(merge_bb);

            kb.SetInsertPoint(merge_bb);
            break;
        }
        case StmtKind::WhileStmt: {
            auto& ws = static_cast<const WhileStmt&>(stmt);
            auto* func = kb.GetInsertBlock()->getParent();
            auto* while_cond = llvm::BasicBlock::Create(ctx_, "kw_cond", func);
            auto* while_body = llvm::BasicBlock::Create(ctx_, "kw_body", func);
            auto* while_end = llvm::BasicBlock::Create(ctx_, "kw_end", func);

            kb.CreateBr(while_cond);
            kb.SetInsertPoint(while_cond);
            auto* wc = emitKernelExpr(*ws.condition, kb, kernel_vars, kernel_arg_values,
                                       loop_var_name, tid_val);
            if (!wc) { kb.CreateBr(while_end); break; }
            if (wc->getType()->isIntegerTy() && !wc->getType()->isIntegerTy(1))
                wc = kb.CreateICmpNE(wc, llvm::ConstantInt::get(wc->getType(), 0));
            kb.CreateCondBr(wc, while_body, while_end);

            kb.SetInsertPoint(while_body);
            kernel_break_stack_.push_back(while_end);
            emitKernelStmt(*ws.body, kb, kernel_vars, kernel_arg_values,
                           loop_var_name, tid_val, while_end);
            kernel_break_stack_.pop_back();
            if (!kb.GetInsertBlock()->getTerminator())
                kb.CreateBr(while_cond);

            kb.SetInsertPoint(while_end);
            break;
        }
        case StmtKind::ForStmt: {
            auto& fs = static_cast<const ForStmt&>(stmt);
            if (fs.init) emitKernelStmt(*fs.init, kb, kernel_vars, kernel_arg_values,
                                         loop_var_name, tid_val, break_target);
            auto* func = kb.GetInsertBlock()->getParent();
            auto* fcond = llvm::BasicBlock::Create(ctx_, "kf_cond", func);
            auto* fbody = llvm::BasicBlock::Create(ctx_, "kf_body", func);
            auto* fend = llvm::BasicBlock::Create(ctx_, "kf_end", func);
            kb.CreateBr(fcond);
            kb.SetInsertPoint(fcond);
            if (fs.condition) {
                auto* fc = emitKernelExpr(*fs.condition, kb, kernel_vars, kernel_arg_values,
                                           loop_var_name, tid_val);
                if (fc) {
                    if (fc->getType()->isIntegerTy() && !fc->getType()->isIntegerTy(1))
                        fc = kb.CreateICmpNE(fc, llvm::ConstantInt::get(fc->getType(), 0));
                    kb.CreateCondBr(fc, fbody, fend);
                }
            } else kb.CreateBr(fbody);
            kb.SetInsertPoint(fbody);
            kernel_break_stack_.push_back(fend);
            if (fs.body) emitKernelStmt(*fs.body, kb, kernel_vars, kernel_arg_values,
                                         loop_var_name, tid_val, fend);
            kernel_break_stack_.pop_back();
            if (!kb.GetInsertBlock()->getTerminator()) {
                if (fs.step) emitKernelExpr(*fs.step, kb, kernel_vars, kernel_arg_values,
                                             loop_var_name, tid_val);
                kb.CreateBr(fcond);
            }
            kb.SetInsertPoint(fend);
            break;
        }
        default:
            break;
    }
}

void CodeGen::generateParallelFor(const ForStmt& s) {
    auto* i32_ty = llvm::Type::getInt32Ty(ctx_);
    auto* i64_ty = llvm::Type::getInt64Ty(ctx_);
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    
    // Extract loop variable name and generate start/end/step as runtime values
    std::string loop_var_name;
    llvm::Value* start_val = llvm::ConstantInt::get(i32_ty, 0, true);
    llvm::Value* end_val = llvm::ConstantInt::get(i32_ty, 0, true);
    llvm::Value* step_val = llvm::ConstantInt::get(i32_ty, 1, true);
    
    if (s.init && s.init->kind == StmtKind::VarDeclStmt) {
        auto& vds = static_cast<const VarDeclStmt&>(*s.init);
        if (!vds.decls.empty()) {
            loop_var_name = vds.decls[0].name;
            if (vds.decls[0].init_expr) {
                auto* v = generateExpr(*vds.decls[0].init_expr);
                if (v) {
                    if (v->getType()->isIntegerTy(64))
                        v = builder_.CreateTrunc(v, i32_ty);
                    else if (v->getType() != i32_ty)
                        v = builder_.CreateIntCast(v, i32_ty, true);
                    start_val = v;
                }
            }
        }
    }
    // Generate condition value (end bound) from original for loop's condition
    if (s.condition && s.condition->kind == ExprKind::BinaryOp) {
        auto& cond = static_cast<const BinaryOpExpr&>(*s.condition);
        const Expr* bound_expr = nullptr;
        if (cond.op == BinaryOpKind::Lt || cond.op == BinaryOpKind::Le)
            bound_expr = cond.rhs.get();
        else if (cond.op == BinaryOpKind::Gt || cond.op == BinaryOpKind::Ge)
            bound_expr = cond.lhs.get();
        if (bound_expr) {
            auto* v = generateExpr(*bound_expr);
            if (v) {
                if (v->getType()->isIntegerTy(64))
                    v = builder_.CreateTrunc(v, i32_ty);
                else if (v->getType() != i32_ty)
                    v = builder_.CreateIntCast(v, i32_ty, true);
                end_val = v;
            }
        }
    }
    // Generate step value
    if (s.step && s.step->kind == ExprKind::BinaryOp) {
        auto& step_bin = static_cast<const BinaryOpExpr&>(*s.step);
        if (step_bin.rhs->kind == ExprKind::IntegerLiteral ||
            step_bin.rhs->kind == ExprKind::FloatLiteral) {
            // Literal step - use constant
            if (step_bin.rhs->kind == ExprKind::IntegerLiteral) {
                auto& lit = static_cast<const IntegerLiteralExpr&>(*step_bin.rhs);
                step_val = llvm::ConstantInt::get(i32_ty, (int)lit.value, true);
            }
        } else {
            // Variable step - generate expression
            auto* v = generateExpr(*step_bin.rhs);
            if (v) {
                if (v->getType()->isIntegerTy(64))
                    v = builder_.CreateTrunc(v, i32_ty);
                else if (v->getType() != i32_ty)
                    v = builder_.CreateIntCast(v, i32_ty, true);
                step_val = v;
            }
        }
    }
    if (loop_var_name.empty()) {
        const_cast<ForStmt&>(s).parallel = false;
        generateForStmt(s);
        const_cast<ForStmt&>(s).parallel = true;
        return;
    }

    // ===== Build capture struct from outer-scope named values =====
    // Collect all named values from the scope stack
    std::map<std::string, llvm::Value*> capture_values;
    std::vector<std::string> capture_names;
    std::vector<llvm::Type*> capture_types;
    
    for (auto& scope : named_values_) {
        for (auto& [name, val] : scope) {
            if (name == loop_var_name) continue;
            if (capture_values.count(name)) continue;
            
            llvm::Value* capture_val = val;
            llvm::Type* store_type = nullptr;
            
            if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(val)) {
                // Stack variable: load its current value
                store_type = ai->getAllocatedType();
                capture_val = builder_.CreateLoad(store_type, ai, name + ".cap");
            } else {
                store_type = val->getType();
            }
            
            if (!store_type || store_type->isVoidTy()) continue;
            if (store_type->isLabelTy()) continue;
            if (store_type->isMetadataTy()) continue;
            
            capture_values[name] = capture_val;
            capture_names.push_back(name);
            capture_types.push_back(store_type);
        }
    }
    
    // Create capture struct type
    llvm::StructType* cap_ty = nullptr;
    if (!capture_types.empty()) {
        cap_ty = llvm::StructType::create(ctx_, capture_types, "parallel_capture");
    }
    
    // Allocate and fill capture struct in caller
    llvm::AllocaInst* cap_alloca = nullptr;
    if (cap_ty) {
        cap_alloca = builder_.CreateAlloca(cap_ty, nullptr, "parallel_cap");
        for (size_t ci = 0; ci < capture_names.size(); ci++) {
            auto* gep = builder_.CreateStructGEP(cap_ty, cap_alloca, ci, capture_names[ci] + ".gep");
            auto* loaded = capture_values[capture_names[ci]];
            builder_.CreateStore(loaded, gep);
        }
    }

    // ===== Create parallel body function (chunk loop) =====
    // The body is now a LOOP over its assigned chunk:
    //   void parallel_body_j(int start, int end, int step, void* arg)
    //     for (i = start; i < end; i += step) { user body }
    // vs the old per-iteration form void(i, arg) — one call/ret + stack frame
    // per iteration from the pool worker, and the user body was a single
    // statement LLVM could neither hoist nor vectorize. With the loop inside
    // the body, LLVM hoists loop-invariant work (e.g. Parallel.workerId() TLS
    // read) and can unroll/vectorize the user reduction.
    auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
        {i32_ty, i32_ty, i32_ty, ptr_ty}, false);
    auto* body_fn = llvm::Function::Create(ft, llvm::Function::InternalLinkage,
        "parallel_body_" + loop_var_name, module_.get());
    auto* entry_bb = llvm::BasicBlock::Create(ctx_, "entry", body_fn);
    llvm::IRBuilder<> pb(entry_bb);
    
    auto* start_arg = body_fn->getArg(0); start_arg->setName("start");
    auto* end_arg = body_fn->getArg(1);   end_arg->setName("end");
    auto* step_arg = body_fn->getArg(2);  step_arg->setName("step");
    auto* void_arg = body_fn->getArg(3);  void_arg->setName("arg");
    
    // Build kernel_vars map for the body function
    std::map<std::string, llvm::Value*> kernel_vars;
    std::vector<llvm::Value*> empty_args;
    
    // Unpack capture struct in body function
    if (cap_ty && cap_alloca) {
        auto* cap_ptr = pb.CreateBitCast(void_arg, llvm::PointerType::get(ctx_, 0), "cap_ptr");
        for (size_t ci = 0; ci < capture_names.size(); ci++) {
            auto* gep = pb.CreateStructGEP(cap_ty, cap_ptr, ci, capture_names[ci] + ".capgep");
            auto* loaded = pb.CreateLoad(capture_types[ci], gep, capture_names[ci] + ".capval");
            // Create alloca in body function and store
            auto* body_alloca = pb.CreateAlloca(capture_types[ci], nullptr, capture_names[ci]);
            pb.CreateStore(loaded, body_alloca);
            kernel_vars[capture_names[ci]] = body_alloca;
        }
    }
    
    // Create local alloca for loop variable, init to start
    auto* i_alloca = pb.CreateAlloca(i32_ty, nullptr, loop_var_name);
    pb.CreateStore(start_arg, i_alloca);
    kernel_vars[loop_var_name] = i_alloca;
    
    // Chunk loop: for (i = start; i < end; i += step)
    auto* loop_cond = llvm::BasicBlock::Create(ctx_, "ploop.cond", body_fn);
    auto* loop_body = llvm::BasicBlock::Create(ctx_, "ploop.body", body_fn);
    auto* loop_end  = llvm::BasicBlock::Create(ctx_, "ploop.end", body_fn);
    pb.CreateBr(loop_cond);
    
    pb.SetInsertPoint(loop_cond);
    auto* i_cur = pb.CreateLoad(i32_ty, i_alloca, loop_var_name);
    auto* loop_ok = pb.CreateICmpSLT(i_cur, end_arg, "ploop.cond");
    pb.CreateCondBr(loop_ok, loop_body, loop_end);
    
    // Generate the user body inside the chunk loop
    pb.SetInsertPoint(loop_body);
    pushScope();
    if (s.body) {
        emitKernelStmt(*s.body, pb, kernel_vars, empty_args, loop_var_name, i_cur);
        if (!pb.GetInsertBlock()->getTerminator()) {
            auto* i_next = pb.CreateAdd(i_cur, step_arg, "i.next");
            pb.CreateStore(i_next, i_alloca);
            pb.CreateBr(loop_cond);
        }
    } else {
        auto* i_next = pb.CreateAdd(i_cur, step_arg, "i.next");
        pb.CreateStore(i_next, i_alloca);
        pb.CreateBr(loop_cond);
    }
    popScope();
    
    pb.SetInsertPoint(loop_end);
    pb.CreateRetVoid();
    
    // ===== Call myp_pool_parallel_for in the caller =====
    if (!runtime_pool_ensure_ || !runtime_parallel_for_) {
        const_cast<ForStmt&>(s).parallel = false;
        generateForStmt(s);
        const_cast<ForStmt&>(s).parallel = true;
        return;
    }
    auto* pool = builder_.CreateCall(runtime_pool_ensure_, {}, "pool");
    
    llvm::Value* cap_arg = llvm::ConstantPointerNull::get(ptr_ty);
    if (cap_alloca) cap_arg = cap_alloca;
    
    builder_.CreateCall(runtime_parallel_for_, {pool, start_val, end_val, step_val,
        builder_.CreateBitCast(body_fn, ptr_ty), cap_arg});
}

void CodeGen::generateGpuFor(const ForStmt& s) {
    // Try to generate NVPTX kernel
    bool ptx_ok = generateGpuKernel(s);

    if (!ptx_ok) {
        // Fallback: CPU sequential execution
        diag_.warn(s.range, "'@gpu for' GPU kernel generation failed, running on CPU");
        const_cast<ForStmt&>(s).gpu = false;
        generateForStmt(s);
        const_cast<ForStmt&>(s).gpu = true;
    }
}

// §8.2/8.3 块和 kernel（**并行 halving 树版**，§8.6 块内并行）：void <name>
// (i64 n, T* a, T* partials)。每块 BS 线程协作归约块内区间 → partials[bid]。
//   · 每线程 1 元素：acc = (start+tid < end) ? a[start+tid] : init（末块尾以
//     init 单位元填充——§8.2 init 须为 op 单位元，op(x,init)=x 不改变结果）；
//   · ping-pong 共享内存 halving 树：half ∈ {BS/2,…,1}，每步
//     dst[t] = (t < half) ? op(src[t], src[t+half]) : src[t]，步间 barrier；
//   · 最终 dst[0] → partials[bid]。
// 纯块和（init 只作单位元填充，不加入真数据）；GPU 与 CPU 镜像同树 → 位级一致。
// 要求 block_size 为 2 的幂（否则用串行 emitBlockSumPtx）。
std::string CodeGen::emitBlockSumTreePtx(const Expr& op_expr, const Expr& init_expr,
                                         llvm::Type* elem_ty, int block_size,
                                         const std::string& kernel_name) {
#ifdef MYP_ENABLE_GPU
    auto ptx_mod = std::make_unique<llvm::Module>("myp_gpu_block_sum_tree", ctx_);
    ptx_mod->setTargetTriple(llvm::Triple(gpuTargetTriple()));
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    auto* ptr = llvm::PointerType::get(ctx_, 0);
    auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {i64, ptr, ptr}, false);
    auto* kf = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, kernel_name, ptx_mod.get());
    setGpuKernelCC(kf);
    auto* n_arg = kf->getArg(0); n_arg->setName("n");
    auto* a_arg = kf->getArg(1); a_arg->setName("a");
    auto* p_arg = kf->getArg(2); p_arg->setName("partials");
    auto* entry = llvm::BasicBlock::Create(ctx_, "entry", kf);
    llvm::IRBuilder<> kb(entry);
    auto* tid_x = kb.CreateIntCast(emitGpuThreadIdx(kb), i64, false, "tx");
    auto* ctaid = kb.CreateIntCast(emitGpuBlockIdx(kb), i64, false, "bid");
    std::map<std::string, llvm::Value*> kv;
    kv["n"] = n_arg; kv["a"] = a_arg; kv["partials"] = p_arg;
    auto* start_i = kb.CreateMul(ctaid, llvm::ConstantInt::get(i64, block_size), "istart");
    auto* iend_raw = kb.CreateAdd(start_i, llvm::ConstantInt::get(i64, block_size));
    auto* iend = kb.CreateSelect(kb.CreateICmpSLT(iend_raw, n_arg), iend_raw, n_arg, "iend");
    // acc = (start+tid < end) ? a[start+tid] : init（每线程 1 元素；末块尾以 init 单位元填充）
    auto* in_blk = kb.CreateICmpSLT(kb.CreateAdd(start_i, tid_x), iend, "inblk");
    auto* acc_a = createKernelAlloca(kb, elem_ty, nullptr, "acc");
    llvm::Value* acc_val = kb.CreateLoad(elem_ty, kb.CreateGEP(elem_ty, a_arg,
        kb.CreateAdd(start_i, tid_x)), "myacc");
    auto* init_v = emitKernelExpr(init_expr, kb, kv, {}, "", nullptr);
    if (init_v && init_v->getType() != elem_ty) {
        if (elem_ty->isFloatingPointTy() && init_v->getType()->isFloatingPointTy())
            init_v = kb.CreateFPCast(init_v, elem_ty);
        else if (elem_ty->isFloatingPointTy() && init_v->getType()->isIntegerTy())
            init_v = kb.CreateSIToFP(init_v, elem_ty);
    }
    acc_val = kb.CreateSelect(in_blk, acc_val, init_v, "acc_sel");
    kb.CreateStore(acc_val, acc_a);
    kv["acc"] = acc_a;
    auto* x_a = createKernelAlloca(kb, elem_ty, nullptr, "x");
    kv["x"] = x_a;
    // ping-pong 共享数组（模块 addrspace(3) 静态 __shared__，同 block_reduce）
    auto* arr_ty = llvm::ArrayType::get(elem_ty, block_size);
    auto* mod = kb.GetInsertBlock()->getParent()->getParent();
    static int treesum_id = 0;
    std::string tag = std::to_string(treesum_id++);
    auto* smemA = new llvm::GlobalVariable(*mod, arr_ty, false,
        llvm::GlobalValue::InternalLinkage, llvm::ConstantAggregateZero::get(arr_ty),
        "myp_treesum_a_" + tag, nullptr, llvm::GlobalValue::NotThreadLocal, 3);
    auto* smemB = new llvm::GlobalVariable(*mod, arr_ty, false,
        llvm::GlobalValue::InternalLinkage, llvm::ConstantAggregateZero::get(arr_ty),
        "myp_treesum_b_" + tag, nullptr, llvm::GlobalValue::NotThreadLocal, 3);
    auto sel_smem = [&](llvm::Value* is_a, llvm::Value* idx) -> llvm::Value* {
        auto* pa = kb.CreateGEP(arr_ty, smemA, {llvm::ConstantInt::get(i32, 0), idx}, "sa");
        auto* pb = kb.CreateGEP(arr_ty, smemB, {llvm::ConstantInt::get(i32, 0), idx}, "sb");
        return kb.CreateSelect(is_a, pa, pb, "smem");
    };
    auto* true_v = llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx_), 1);
    auto* false_v = llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx_), 0);
    // 初始写 smemA[tid] = acc
    kb.CreateStore(acc_val, sel_smem(true_v, kb.CreateIntCast(tid_x, i32, false)));
    emitGpuBarrier(kb);
    // halving 树：half ∈ {BS/2,…,1}，ping-pong（src/dst 交替）
    int steps = 0; for (int h = block_size / 2; h >= 1; h >>= 1) steps++;
    llvm::Value* src_is_a = true_v;   // 第 0 步 src = smemA，dst = smemB
    for (int si = 0; si < steps; si++) {
        int half = block_size >> (si + 1);
        llvm::Value* halfv = llvm::ConstantInt::get(i32, half);
        // dst = !src
        llvm::Value* dst_is_a = kb.CreateXor(src_is_a, true_v, "dst");
        auto* tid32 = kb.CreateIntCast(tid_x, i32, false);
        // v = src[tid]
        auto* v = kb.CreateLoad(elem_ty, sel_smem(src_is_a, tid32), "v");
        // if tid < half: w = src[tid+half]; v2 = op(v,w); else v2 = v
        // 越界防护：tid≥half 的线程也执行 load，但索引钳到 tid（读 src[tid]，
        // 结果不用）；否则 tid+half 可能 ≥ block_size → 共享内存越界非法访问。
        auto* t_lt = kb.CreateICmpULT(tid32, halfv, "lt");
        auto* t_plus = kb.CreateAdd(tid32, halfv, "tph");
        auto* safe_idx = kb.CreateSelect(t_lt, t_plus, tid32, "safe");
        auto* w = kb.CreateLoad(elem_ty, sel_smem(src_is_a, safe_idx), "w");
        kb.CreateStore(w, x_a);
        kb.CreateStore(v, acc_a);
        auto* opv = emitKernelExpr(op_expr, kb, kv, {}, "", nullptr);
        if (opv && opv->getType() != elem_ty) {
            if (elem_ty->isFloatingPointTy() && opv->getType()->isFloatingPointTy())
                opv = kb.CreateFPCast(opv, elem_ty);
            else if (elem_ty->isIntegerTy() && opv->getType()->isIntegerTy())
                opv = kb.CreateIntCast(opv, elem_ty, false);
        }
        auto* v2 = kb.CreateSelect(t_lt, opv, v, "v2");
        kb.CreateStore(v2, sel_smem(dst_is_a, tid32));
        emitGpuBarrier(kb);
        src_is_a = dst_is_a;
    }
    // 最终结果在最后一步的 dst（= src_is_a 最后一次交换后的值）
    auto* res_v = kb.CreateLoad(elem_ty, sel_smem(src_is_a, llvm::ConstantInt::get(i32, 0)), "tres");
    auto* is0 = kb.CreateICmpEQ(tid_x, llvm::ConstantInt::get(i64, 0));
    auto* st_bb = llvm::BasicBlock::Create(ctx_, "tree_st", kf);
    auto* join_bb = llvm::BasicBlock::Create(ctx_, "tree_join", kf);
    kb.CreateCondBr(is0, st_bb, join_bb);
    kb.SetInsertPoint(st_bb);
    kb.CreateStore(res_v, kb.CreateGEP(elem_ty, p_arg, ctaid, "pp"));
    kb.CreateBr(join_bb);
    kb.SetInsertPoint(join_bb);
    kb.CreateRetVoid();

    // ---- 目标机器 + verify + emit（§7.7 跨厂商：NVPTX→PTX / AMD→GCN ELF）----
    std::string err;
    ensureGpuTargetsInited();
    auto* tgt = llvm::TargetRegistry::lookupTarget(gpuTargetTriple(), err);
    if (!tgt) { diag_.warn(SourceRange{}, "GPU target not available: " + err); return ""; }
    auto* tm = tgt->createTargetMachine(llvm::Triple(gpuTargetTriple()), gpuTargetArch(), "",
        llvm::TargetOptions{}, llvm::Reloc::PIC_);
    if (!tm) { diag_.warn(SourceRange{}, "GPU target machine creation failed"); return ""; }
    std::string verify_err;
    llvm::raw_string_ostream vos(verify_err);
    if (llvm::verifyModule(*ptx_mod, &vos)) {
        diag_.warn(SourceRange{}, "GPU reduce tree kernel verification failed: " + verify_err);
        delete tm; return "";
    }
    if (gpuTargetAmd()) runGpuOptPipeline(ptx_mod.get(), tm);
    std::string out = emitGpuTargetBytes(ptx_mod.get(), tm, "block_sum_tree");
    if (getenv("MYP_DUMP_PTX") && !out.empty())
        fprintf(stderr, "=== MYP PTX (reduce tree) ===\n%s\n=== END ===\n", out.c_str());
    if (out.empty()) { diag_.warn(SourceRange{}, "GPU emitted empty code"); return ""; }
    return out;
#else
    (void)op_expr; (void)init_expr; (void)elem_ty; (void)block_size; (void)kernel_name;
    return "";
#endif
}

// §8.2/8.3 块和 kernel：void <name>(i64 n, T* a, T* partials)。每块 tx==0 串行
// 归约块内区间 → partials[blockIdx]（K1；reduce/scan 共用，非 2 幂块大小回退）。
// **纯块和**：不从 init 开始（init 只在最终合并/块前缀时应用一次）。块必有 ≥1
// 元素。op_expr 用 emitKernelExpr 生成（acc/x 绑定 kernel 局部 alloca）。
std::string CodeGen::emitBlockSumPtx(const Expr& op_expr, const Expr& init_expr,
                                     llvm::Type* elem_ty, int block_size,
                                     const std::string& kernel_name) {
#ifdef MYP_ENABLE_GPU
    auto ptx_mod = std::make_unique<llvm::Module>("myp_gpu_block_sum", ctx_);
    ptx_mod->setTargetTriple(llvm::Triple(gpuTargetTriple()));
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* ptr = llvm::PointerType::get(ctx_, 0);
    auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {i64, ptr, ptr}, false);
    auto* kf = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, kernel_name, ptx_mod.get());
    setGpuKernelCC(kf);
    auto* n_arg = kf->getArg(0); n_arg->setName("n");
    auto* a_arg = kf->getArg(1); a_arg->setName("a");
    auto* p_arg = kf->getArg(2); p_arg->setName("partials");
    auto* entry = llvm::BasicBlock::Create(ctx_, "entry", kf);
    llvm::IRBuilder<> kb(entry);
    auto* tid_x = kb.CreateIntCast(emitGpuThreadIdx(kb), i64, false, "tx");
    auto* ctaid = kb.CreateIntCast(emitGpuBlockIdx(kb), i64, false, "bid");
    auto* is0 = kb.CreateICmpEQ(tid_x, llvm::ConstantInt::get(i64, 0));
    auto* body_bb = llvm::BasicBlock::Create(ctx_, "body", kf);
    auto* end_bb = llvm::BasicBlock::Create(ctx_, "end", kf);
    kb.CreateCondBr(is0, body_bb, end_bb);
    kb.SetInsertPoint(body_bb);
    std::map<std::string, llvm::Value*> kv;
    kv["n"] = n_arg; kv["a"] = a_arg; kv["partials"] = p_arg;
    auto* start_i = kb.CreateMul(ctaid, llvm::ConstantInt::get(i64, block_size), "istart");
    auto* iend_raw = kb.CreateAdd(start_i, llvm::ConstantInt::get(i64, block_size));
    auto* iend = kb.CreateSelect(kb.CreateICmpSLT(iend_raw, n_arg), iend_raw, n_arg, "iend");
    // acc = a[start]（纯块和，首元素为初值；块必有 ≥1 元素）
    auto* acc_a = createKernelAlloca(kb, elem_ty, nullptr, "acc");
    auto* first_v = kb.CreateLoad(elem_ty, kb.CreateGEP(elem_ty, a_arg, start_i), "first");
    kb.CreateStore(first_v, acc_a);
    kv["acc"] = acc_a;
    auto* x_a = createKernelAlloca(kb, elem_ty, nullptr, "x");
    kv["x"] = x_a;
    auto* i_a = createKernelAlloca(kb, i64, nullptr, "i");
    auto* istart1 = kb.CreateAdd(start_i, llvm::ConstantInt::get(i64, 1));
    kb.CreateStore(istart1, i_a);
    auto* loop_bb = llvm::BasicBlock::Create(ctx_, "loop", kf);
    auto* lbody = llvm::BasicBlock::Create(ctx_, "lbody", kf);
    auto* lend = llvm::BasicBlock::Create(ctx_, "lend", kf);
    kb.CreateBr(loop_bb);
    kb.SetInsertPoint(loop_bb);
    auto* iv = kb.CreateLoad(i64, i_a);
    kb.CreateCondBr(kb.CreateICmpSLT(iv, iend), lbody, lend);
    kb.SetInsertPoint(lbody);
    auto* iv2 = kb.CreateLoad(i64, i_a);
    auto* xv = kb.CreateLoad(elem_ty, kb.CreateGEP(elem_ty, a_arg, iv2), "x");
    kb.CreateStore(xv, x_a);
    auto* opv = emitKernelExpr(op_expr, kb, kv, {}, "", nullptr);
    if (opv && opv->getType() != elem_ty) {
        if (elem_ty->isFloatingPointTy() && opv->getType()->isFloatingPointTy())
            opv = kb.CreateFPCast(opv, elem_ty);
        else if (elem_ty->isIntegerTy() && opv->getType()->isIntegerTy())
            opv = kb.CreateIntCast(opv, elem_ty, false);
    }
    kb.CreateStore(opv, acc_a);
    auto* iv3 = kb.CreateLoad(i64, i_a);
    kb.CreateStore(kb.CreateAdd(iv3, llvm::ConstantInt::get(i64, 1)), i_a);
    kb.CreateBr(loop_bb);
    kb.SetInsertPoint(lend);
    auto* accv = kb.CreateLoad(elem_ty, acc_a);
    kb.CreateStore(accv, kb.CreateGEP(elem_ty, p_arg, ctaid, "pp"));
    kb.CreateBr(end_bb);
    kb.SetInsertPoint(end_bb);
    kb.CreateRetVoid();
    (void)init_expr;

    // ---- 目标机器 + verify + emit（§7.7 跨厂商：NVPTX→PTX / AMD→GCN ELF）----
    std::string err;
    ensureGpuTargetsInited();
    auto* tgt = llvm::TargetRegistry::lookupTarget(gpuTargetTriple(), err);
    if (!tgt) { diag_.warn(SourceRange{}, "GPU target not available: " + err); return ""; }
    auto* tm = tgt->createTargetMachine(llvm::Triple(gpuTargetTriple()), gpuTargetArch(), "",
        llvm::TargetOptions{}, llvm::Reloc::PIC_);
    if (!tm) { diag_.warn(SourceRange{}, "GPU target machine creation failed"); return ""; }
    std::string verify_err;
    llvm::raw_string_ostream vos(verify_err);
    if (llvm::verifyModule(*ptx_mod, &vos)) {
        diag_.warn(SourceRange{}, "GPU reduce kernel verification failed: " + verify_err);
        delete tm; return "";
    }
    // AMD：先跑 O2 管线（@gpu for 同款），消解 AMDGPU ISel 无法选中的构造
    if (gpuTargetAmd()) runGpuOptPipeline(ptx_mod.get(), tm);
    std::string out = emitGpuTargetBytes(ptx_mod.get(), tm, "block_sum");
    if (getenv("MYP_DUMP_PTX") && !out.empty())
        fprintf(stderr, "=== MYP PTX (reduce) ===\n%s\n=== END ===\n", out.c_str());
    if (out.empty()) { diag_.warn(SourceRange{}, "GPU emitted empty code"); return ""; }
    return out;
#else
    (void)s; (void)elem_ty; (void)block_size;
    return "";
#endif
}

// §8.2 host 顺序归约：acc = init（use_init=true）或 src[0]（false，合并 partials）；
// for i in [start, cnt): x = src[i]; acc = op(acc, x)；最后 out = acc。
// op_expr 用 generateExpr（acc/x 绑定到 host 局部 alloca，构建在 entry block）。
void CodeGen::emitSeqFold(llvm::Value* src, llvm::Value* cnt, llvm::Type* elem_ty,
                          const GpuReduceStmt& s, llvm::Value* out_slot, bool use_init) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    // acc
    auto* acc_a = createEntryBlockAlloca(current_function_, elem_ty, "__rd_acc");
    if (use_init) {
        auto* init_v = generateExpr(*s.init_expr);
        if (init_v && init_v->getType() != elem_ty) {
            if (elem_ty->isFloatingPointTy() && init_v->getType()->isFloatingPointTy())
                init_v = builder_.CreateFPCast(init_v, elem_ty);
            else if (elem_ty->isFloatingPointTy() && init_v->getType()->isIntegerTy())
                init_v = builder_.CreateSIToFP(init_v, elem_ty);
            else if (elem_ty->isIntegerTy() && init_v->getType()->isIntegerTy())
                init_v = builder_.CreateIntCast(init_v, elem_ty, false);
        }
        builder_.CreateStore(init_v, acc_a);
    } else {
        auto* v0 = builder_.CreateLoad(elem_ty,
            builder_.CreateGEP(elem_ty, src, llvm::ConstantInt::get(i64, 0)), "rd_p0");
        builder_.CreateStore(v0, acc_a);
    }
    auto* x_a = createEntryBlockAlloca(current_function_, elem_ty, "__rd_x");
    auto* i_a = createEntryBlockAlloca(current_function_, i64, "__rd_i");
    builder_.CreateStore(llvm::ConstantInt::get(i64, use_init ? 0 : 1), i_a);
    auto old_acc = getNamedValue(s.op_acc);
    auto old_x = getNamedValue(s.op_x);
    setNamedValue(s.op_acc, acc_a);
    setNamedValue(s.op_x, x_a);
    auto* loop_bb = llvm::BasicBlock::Create(ctx_, "rdf_loop", current_function_);
    auto* lbody = llvm::BasicBlock::Create(ctx_, "rdf_body", current_function_);
    auto* lend = llvm::BasicBlock::Create(ctx_, "rdf_end", current_function_);
    builder_.CreateBr(loop_bb);
    builder_.SetInsertPoint(loop_bb);
    auto* iv = builder_.CreateLoad(i64, i_a);
    builder_.CreateCondBr(builder_.CreateICmpSLT(iv, cnt), lbody, lend);
    builder_.SetInsertPoint(lbody);
    auto* iv2 = builder_.CreateLoad(i64, i_a);
    auto* xv = builder_.CreateLoad(elem_ty, builder_.CreateGEP(elem_ty, src, iv2), "rdx");
    builder_.CreateStore(xv, x_a);
    auto* opv = generateExpr(*s.op_expr);
    if (opv && opv->getType() != elem_ty) {
        if (elem_ty->isFloatingPointTy() && opv->getType()->isFloatingPointTy())
            opv = builder_.CreateFPCast(opv, elem_ty);
        else if (elem_ty->isFloatingPointTy() && opv->getType()->isIntegerTy())
            opv = builder_.CreateSIToFP(opv, elem_ty);
        else if (elem_ty->isIntegerTy() && opv->getType()->isIntegerTy())
            opv = builder_.CreateIntCast(opv, elem_ty, false);
    }
    builder_.CreateStore(opv, acc_a);
    auto* iv3 = builder_.CreateLoad(i64, i_a);
    builder_.CreateStore(builder_.CreateAdd(iv3, llvm::ConstantInt::get(i64, 1)), i_a);
    builder_.CreateBr(loop_bb);
    builder_.SetInsertPoint(lend);
    auto* accv = builder_.CreateLoad(elem_ty, acc_a);
    builder_.CreateStore(accv, out_slot);
    // 恢复 acc/x 绑定
    named_values_.back().erase(s.op_acc);
    named_values_.back().erase(s.op_x);
    if (old_acc) setNamedValue(s.op_acc, old_acc);
    if (old_x) setNamedValue(s.op_x, old_x);
}

// §8.3 块内并行 CPU 镜像：与 GPU 的 K1(块和) + offsets + K2-HS(每块 Hillis-Steele)
// 同树同序 → 位级一致。
//   1) partials[j] = 纯块和（bs 为 2 幂用 halving 树，否则串行）——同 GPU K1；
//   2) offsets：off[0]=init；off[k]=op(off[k-1], partials[k-1])——同 GPU host 前缀；
//   3) 每块 HS：local[t] = (start+t<end) ? a[start+t] : init；for d in {1..BS/2}：
//      local[t] = (t≥d) ? op(local[t], local[t-d]) : local[t]（t 降序 in-place 安全：
//      源 local[t-d]（t-d<t）本步尚未更新）；
//   4) b[start+t] = op(offsets[j], local[t])（活跃线程）。
// 仅 inclusive 扫描（exclusive 由 generateGpuScan 在写盘阶段用 b[i-1] 变换，见上）。
void CodeGen::emitSeqScanBlocked(llvm::Value* a_src, llvm::Value* b_src,
                                 llvm::Value* cnt, llvm::Value* blocks, int bs,
                                 llvm::Type* elem_ty, const GpuScanStmt& s) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    bool bs_pow2 = (bs & (bs - 1)) == 0;
    auto* partials = builder_.CreateAlloca(elem_ty, blocks, "__sch_part");
    auto* offsets = builder_.CreateAlloca(elem_ty, blocks, "__sch_off");
    // op 求值绑定（共用一对 entry alloca）
    auto* acc_a = createEntryBlockAlloca(current_function_, elem_ty, "__sch_acc");
    auto* x_a = createEntryBlockAlloca(current_function_, elem_ty, "__sch_x");
    auto old_acc = getNamedValue(s.op_acc);
    auto old_x = getNamedValue(s.op_x);
    setNamedValue(s.op_acc, acc_a);
    setNamedValue(s.op_x, x_a);
    auto* init_v = generateExpr(*s.init_expr);
    if (init_v && init_v->getType() != elem_ty) {
        if (elem_ty->isFloatingPointTy() && init_v->getType()->isFloatingPointTy())
            init_v = builder_.CreateFPCast(init_v, elem_ty);
        else if (elem_ty->isFloatingPointTy() && init_v->getType()->isIntegerTy())
            init_v = builder_.CreateSIToFP(init_v, elem_ty);
        else if (elem_ty->isIntegerTy() && init_v->getType()->isIntegerTy())
            init_v = builder_.CreateIntCast(init_v, elem_ty, false);
    }
    // L 数组（树用；bs 编译期常量，栈静态）
    auto* arr_ty = llvm::ArrayType::get(elem_ty, bs);
    auto* L = builder_.CreateAlloca(arr_ty, nullptr, "__sch_L");
    auto L_gep = [&](llvm::Value* idx) -> llvm::Value* {
        return builder_.CreateGEP(arr_ty, L, {llvm::ConstantInt::get(i32, 0),
            builder_.CreateIntCast(idx, i32, false)}, "sch_L");
    };
    // 通用 in-block op：a = op(a, b)（L 数组原地）
    auto op2 = [&](llvm::Value* lv, llvm::Value* lv2) -> llvm::Value* {
        builder_.CreateStore(lv2, x_a);
        builder_.CreateStore(lv, acc_a);
        auto* opv = generateExpr(*s.op_expr);
        if (opv && opv->getType() != elem_ty) {
            if (elem_ty->isFloatingPointTy() && opv->getType()->isFloatingPointTy())
                opv = builder_.CreateFPCast(opv, elem_ty);
            else if (elem_ty->isIntegerTy() && opv->getType()->isIntegerTy())
                opv = builder_.CreateIntCast(opv, elem_ty, false);
        }
        return opv;
    };
    // ===== for j in [0, blocks) =====
    auto* j_a = createEntryBlockAlloca(current_function_, i64, "__sch_j");
    builder_.CreateStore(llvm::ConstantInt::get(i64, 0), j_a);
    auto* jbb = llvm::BasicBlock::Create(ctx_, "sch_j", current_function_);
    auto* jb = llvm::BasicBlock::Create(ctx_, "sch_jb", current_function_);
    auto* je = llvm::BasicBlock::Create(ctx_, "sch_je", current_function_);
    builder_.CreateBr(jbb);
    builder_.SetInsertPoint(jbb);
    auto* jv = builder_.CreateLoad(i64, j_a);
    builder_.CreateCondBr(builder_.CreateICmpSLT(jv, blocks), jb, je);
    builder_.SetInsertPoint(jb);
    auto* jv2 = builder_.CreateLoad(i64, j_a);
    auto* start = builder_.CreateMul(jv2, llvm::ConstantInt::get(i64, bs), "sch_start");
    auto* end_raw = builder_.CreateAdd(start, llvm::ConstantInt::get(i64, bs));
    auto* end = builder_.CreateSelect(builder_.CreateICmpSLT(end_raw, cnt), end_raw, cnt, "sch_end");
    // 1) 填 L：L[t] = (start+t < end) ? a[start+t] : init（t in [0,bs)）
    {
        auto* t_a = createEntryBlockAlloca(current_function_, i64, "__sch_t");
        builder_.CreateStore(llvm::ConstantInt::get(i64, 0), t_a);
        auto* fbb = llvm::BasicBlock::Create(ctx_, "sch_f", current_function_);
        auto* fb = llvm::BasicBlock::Create(ctx_, "sch_fb", current_function_);
        auto* fe = llvm::BasicBlock::Create(ctx_, "sch_fe", current_function_);
        builder_.CreateBr(fbb);
        builder_.SetInsertPoint(fbb);
        auto* tv = builder_.CreateLoad(i64, t_a);
        builder_.CreateCondBr(builder_.CreateICmpSLT(tv, llvm::ConstantInt::get(i64, bs)), fb, fe);
        builder_.SetInsertPoint(fb);
        auto* tv2 = builder_.CreateLoad(i64, t_a);
        auto* idx = builder_.CreateAdd(start, tv2, "sch_idx");
        auto* in_blk = builder_.CreateICmpSLT(idx, end, "sch_in");
        auto* av = builder_.CreateLoad(elem_ty, builder_.CreateGEP(elem_ty, a_src, idx), "sch_av");
        auto* val = builder_.CreateSelect(in_blk, av, init_v, "sch_lv");
        builder_.CreateStore(val, L_gep(tv2));
        auto* tv3 = builder_.CreateLoad(i64, t_a);
        builder_.CreateStore(builder_.CreateAdd(tv3, llvm::ConstantInt::get(i64, 1)), t_a);
        builder_.CreateBr(fbb);
        builder_.SetInsertPoint(fe);
    }
    // 2) 块和 partials[j]：halving 树（bs 2 幂）或串行（emitSeqFold use_init=false）
    if (bs_pow2) {
        auto* half_a = createEntryBlockAlloca(current_function_, i32, "__sch_half");
        builder_.CreateStore(llvm::ConstantInt::get(i32, bs / 2), half_a);
        auto* hbb = llvm::BasicBlock::Create(ctx_, "sch_h", current_function_);
        auto* hb = llvm::BasicBlock::Create(ctx_, "sch_hb", current_function_);
        auto* he = llvm::BasicBlock::Create(ctx_, "sch_he", current_function_);
        builder_.CreateBr(hbb);
        builder_.SetInsertPoint(hbb);
        auto* halfv = builder_.CreateLoad(i32, half_a);
        builder_.CreateCondBr(builder_.CreateICmpSGE(halfv, llvm::ConstantInt::get(i32, 1)), hb, he);
        builder_.SetInsertPoint(hb);
        auto* t_a = createEntryBlockAlloca(current_function_, i32, "__sch_t2");
        builder_.CreateStore(llvm::ConstantInt::get(i32, 0), t_a);
        auto* tbb = llvm::BasicBlock::Create(ctx_, "sch_t", current_function_);
        auto* tb = llvm::BasicBlock::Create(ctx_, "sch_tb", current_function_);
        auto* te = llvm::BasicBlock::Create(ctx_, "sch_te", current_function_);
        builder_.CreateBr(tbb);
        builder_.SetInsertPoint(tbb);
        auto* tv = builder_.CreateLoad(i32, t_a);
        builder_.CreateCondBr(builder_.CreateICmpSLT(tv, builder_.CreateLoad(i32, half_a)), tb, te);
        builder_.SetInsertPoint(tb);
        auto* tv2 = builder_.CreateLoad(i32, t_a);
        auto* tv2_64 = builder_.CreateIntCast(tv2, i64, false);
        auto* hv_64 = builder_.CreateIntCast(builder_.CreateLoad(i32, half_a), i64, false);
        auto* other = builder_.CreateAdd(tv2_64, hv_64, "sch_other");
        auto* lo = builder_.CreateLoad(elem_ty, L_gep(tv2_64), "sch_lo");
        auto* hi = builder_.CreateLoad(elem_ty, L_gep(other), "sch_hi");
        builder_.CreateStore(op2(lo, hi), L_gep(tv2_64));
        auto* tv3 = builder_.CreateLoad(i32, t_a);
        builder_.CreateStore(builder_.CreateAdd(tv3, llvm::ConstantInt::get(i32, 1)), t_a);
        builder_.CreateBr(tbb);
        builder_.SetInsertPoint(te);
        auto* halfn = builder_.CreateLoad(i32, half_a);
        builder_.CreateStore(builder_.CreateLShr(halfn, llvm::ConstantInt::get(i32, 1)), half_a);
        builder_.CreateBr(hbb);
        builder_.SetInsertPoint(he);
        builder_.CreateStore(builder_.CreateLoad(elem_ty, L_gep(llvm::ConstantInt::get(i64, 0)), "sch_l0"),
            builder_.CreateGEP(elem_ty, partials, jv2, "sch_pp"));
    } else {
        // 串行：partials[j] = fold(a[start], a[start+1..end))（纯块和）
        auto* acc_s = createEntryBlockAlloca(current_function_, elem_ty, "__sch_sacc");
        builder_.CreateStore(builder_.CreateLoad(elem_ty, builder_.CreateGEP(elem_ty, a_src, start), "sch_sa0"), acc_s);
        auto* i_a = createEntryBlockAlloca(current_function_, i64, "__sch_si");
        builder_.CreateStore(builder_.CreateAdd(start, llvm::ConstantInt::get(i64, 1)), i_a);
        auto* sbb = llvm::BasicBlock::Create(ctx_, "sch_s", current_function_);
        auto* sb = llvm::BasicBlock::Create(ctx_, "sch_sb", current_function_);
        auto* se = llvm::BasicBlock::Create(ctx_, "sch_se", current_function_);
        builder_.CreateBr(sbb);
        builder_.SetInsertPoint(sbb);
        auto* iv = builder_.CreateLoad(i64, i_a);
        builder_.CreateCondBr(builder_.CreateICmpSLT(iv, end), sb, se);
        builder_.SetInsertPoint(sb);
        auto* iv2 = builder_.CreateLoad(i64, i_a);
        auto* av = builder_.CreateLoad(elem_ty, builder_.CreateGEP(elem_ty, a_src, iv2), "sch_sav");
        auto* curv = builder_.CreateLoad(elem_ty, acc_s);
        builder_.CreateStore(op2(curv, av), acc_s);
        auto* iv3 = builder_.CreateLoad(i64, i_a);
        builder_.CreateStore(builder_.CreateAdd(iv3, llvm::ConstantInt::get(i64, 1)), i_a);
        builder_.CreateBr(sbb);
        builder_.SetInsertPoint(se);
        builder_.CreateStore(builder_.CreateLoad(elem_ty, acc_s, "sch_sacc"),
            builder_.CreateGEP(elem_ty, partials, jv2, "sch_ps"));
    }
    // j++ 并回边（此前缺失 → j-loop 只跑 j=0，partials[1..] 从未写入）
    auto* jv3 = builder_.CreateLoad(i64, j_a);
    builder_.CreateStore(builder_.CreateAdd(jv3, llvm::ConstantInt::get(i64, 1)), j_a);
    builder_.CreateBr(jbb);
    builder_.SetInsertPoint(je);
    // 恢复 acc/x（offsets 与第二趟用独立求值）
    named_values_.back().erase(s.op_acc);
    named_values_.back().erase(s.op_x);
    if (old_acc) setNamedValue(s.op_acc, old_acc);
    if (old_x) setNamedValue(s.op_x, old_x);
    // 5) host 块前缀 offsets：off[0]=init；off[k]=op(off[k-1], partials[k-1])
    //    （步骤 4 结束时已恢复 acc/x 绑定，这里重新绑定供 generateExpr 求 op）
    {
        auto oa5 = getNamedValue(s.op_acc);
        auto ox5 = getNamedValue(s.op_x);
        setNamedValue(s.op_acc, acc_a);
        setNamedValue(s.op_x, x_a);
        auto* oacc_a = createEntryBlockAlloca(current_function_, elem_ty, "__sch_oacc");
        builder_.CreateStore(init_v, oacc_a);
        auto* i_a = createEntryBlockAlloca(current_function_, i64, "__sch_i");
        builder_.CreateStore(llvm::ConstantInt::get(i64, 0), i_a);
        auto* obb = llvm::BasicBlock::Create(ctx_, "sch_off", current_function_);
        auto* ob = llvm::BasicBlock::Create(ctx_, "sch_offb", current_function_);
        auto* oe = llvm::BasicBlock::Create(ctx_, "sch_offe", current_function_);
        builder_.CreateBr(obb);
        builder_.SetInsertPoint(obb);
        auto* iv = builder_.CreateLoad(i64, i_a);
        builder_.CreateCondBr(builder_.CreateICmpSLT(iv, blocks), ob, oe);
        builder_.SetInsertPoint(ob);
        auto* iv2 = builder_.CreateLoad(i64, i_a);
        auto* acc_cur = builder_.CreateLoad(elem_ty, oacc_a);
        builder_.CreateStore(acc_cur, builder_.CreateGEP(elem_ty, offsets, iv2), "sch_offv");
        auto* pv = builder_.CreateLoad(elem_ty, builder_.CreateGEP(elem_ty, partials, iv2), "sch_opv");
        builder_.CreateStore(pv, x_a);
        builder_.CreateStore(acc_cur, acc_a);
        auto* opv = generateExpr(*s.op_expr);
        if (opv && opv->getType() != elem_ty) {
            if (elem_ty->isFloatingPointTy() && opv->getType()->isFloatingPointTy())
                opv = builder_.CreateFPCast(opv, elem_ty);
            else if (elem_ty->isIntegerTy() && opv->getType()->isIntegerTy())
                opv = builder_.CreateIntCast(opv, elem_ty, false);
        }
        builder_.CreateStore(opv, oacc_a);
        auto* iv3 = builder_.CreateLoad(i64, i_a);
        builder_.CreateStore(builder_.CreateAdd(iv3, llvm::ConstantInt::get(i64, 1)), i_a);
        builder_.CreateBr(obb);
        builder_.SetInsertPoint(oe);
        // 恢复 acc/x（步骤 6 会再绑定）
        named_values_.back().erase(s.op_acc);
        named_values_.back().erase(s.op_x);
        if (oa5) setNamedValue(s.op_acc, oa5);
        if (ox5) setNamedValue(s.op_x, ox5);
    }
    // 6) 写盘：b[start+t] = op(offsets[j], L[t])（活跃线程）——重走 j、t 循环
    {
        auto* j2_a = createEntryBlockAlloca(current_function_, i64, "__sch_j2");
        builder_.CreateStore(llvm::ConstantInt::get(i64, 0), j2_a);
        auto* jbb2 = llvm::BasicBlock::Create(ctx_, "sch_j2", current_function_);
        auto* jb2 = llvm::BasicBlock::Create(ctx_, "sch_j2b", current_function_);
        auto* je2 = llvm::BasicBlock::Create(ctx_, "sch_j2e", current_function_);
        builder_.CreateBr(jbb2);
        builder_.SetInsertPoint(jbb2);
        auto* jv = builder_.CreateLoad(i64, j2_a);
        builder_.CreateCondBr(builder_.CreateICmpSLT(jv, blocks), jb2, je2);
        builder_.SetInsertPoint(jb2);
        auto* jv2 = builder_.CreateLoad(i64, j2_a);
        auto* start = builder_.CreateMul(jv2, llvm::ConstantInt::get(i64, bs), "sch_s2");
        auto* end_raw = builder_.CreateAdd(start, llvm::ConstantInt::get(i64, bs));
        auto* end = builder_.CreateSelect(builder_.CreateICmpSLT(end_raw, cnt), end_raw, cnt, "sch_e2");
        auto* offv = builder_.CreateLoad(elem_ty, builder_.CreateGEP(elem_ty, offsets, jv2), "sch_off2");
        // 重绑 acc/x（HS 的 op 求值需要）
        auto old_acc2 = getNamedValue(s.op_acc);
        auto old_x2 = getNamedValue(s.op_x);
        setNamedValue(s.op_acc, acc_a);
        setNamedValue(s.op_x, x_a);
        // 6a) 填 L：L[t] = (start+t < end) ? a[start+t] : init
        {
            auto* tf_a = createEntryBlockAlloca(current_function_, i64, "__sch_tf");
            builder_.CreateStore(llvm::ConstantInt::get(i64, 0), tf_a);
            auto* fbb = llvm::BasicBlock::Create(ctx_, "sch_f2", current_function_);
            auto* fb = llvm::BasicBlock::Create(ctx_, "sch_f2b", current_function_);
            auto* fe = llvm::BasicBlock::Create(ctx_, "sch_f2e", current_function_);
            builder_.CreateBr(fbb);
            builder_.SetInsertPoint(fbb);
            auto* tv = builder_.CreateLoad(i64, tf_a);
            builder_.CreateCondBr(builder_.CreateICmpSLT(tv, llvm::ConstantInt::get(i64, bs)), fb, fe);
            builder_.SetInsertPoint(fb);
            auto* tv2 = builder_.CreateLoad(i64, tf_a);
            auto* idx = builder_.CreateAdd(start, tv2, "sch_f2i");
            auto* inb = builder_.CreateICmpSLT(idx, end, "sch_f2in");
            auto* av = builder_.CreateLoad(elem_ty, builder_.CreateGEP(elem_ty, a_src, idx), "sch_f2a");
            auto* val = builder_.CreateSelect(inb, av, init_v, "sch_f2v");
            builder_.CreateStore(val, L_gep(tv2));
            auto* tv3 = builder_.CreateLoad(i64, tf_a);
            builder_.CreateStore(builder_.CreateAdd(tv3, llvm::ConstantInt::get(i64, 1)), tf_a);
            builder_.CreateBr(fbb);
            builder_.SetInsertPoint(fe);
        }
        // 6b) 块内 Hillis-Steele（in-place，t 降序保证源未更新）
        {
            auto* d_a = createEntryBlockAlloca(current_function_, i32, "__sch_d2");
            builder_.CreateStore(llvm::ConstantInt::get(i32, 1), d_a);
            auto* dbb = llvm::BasicBlock::Create(ctx_, "sch_d2", current_function_);
            auto* db = llvm::BasicBlock::Create(ctx_, "sch_d2b", current_function_);
            auto* de = llvm::BasicBlock::Create(ctx_, "sch_d2e", current_function_);
            builder_.CreateBr(dbb);
            builder_.SetInsertPoint(dbb);
            auto* dv = builder_.CreateLoad(i32, d_a);
            builder_.CreateCondBr(builder_.CreateICmpSLT(dv, llvm::ConstantInt::get(i32, bs)), db, de);
            builder_.SetInsertPoint(db);
            auto* t_a3 = createEntryBlockAlloca(current_function_, i32, "__sch_t3");
            builder_.CreateStore(llvm::ConstantInt::get(i32, bs - 1), t_a3);
            auto* tbb = llvm::BasicBlock::Create(ctx_, "sch_t3", current_function_);
            auto* tb = llvm::BasicBlock::Create(ctx_, "sch_t3b", current_function_);
            auto* te = llvm::BasicBlock::Create(ctx_, "sch_t3e", current_function_);
            builder_.CreateBr(tbb);
            builder_.SetInsertPoint(tbb);
            auto* tv = builder_.CreateLoad(i32, t_a3);
            builder_.CreateCondBr(builder_.CreateICmpSGE(tv, llvm::ConstantInt::get(i32, 0)), tb, te);
            builder_.SetInsertPoint(tb);
            auto* tv2 = builder_.CreateLoad(i32, t_a3);
            auto* dcur = builder_.CreateLoad(i32, d_a);
            auto* ge = builder_.CreateICmpSGE(tv2, dcur, "sch_ge");
            auto* tv2_64 = builder_.CreateIntCast(tv2, i64, false);
            auto* dc_64 = builder_.CreateIntCast(dcur, i64, false);
            auto* prev = builder_.CreateSub(tv2_64, dc_64, "sch_prev");
            auto* safe = builder_.CreateSelect(ge, prev, llvm::ConstantInt::get(i64, 0), "sch_safe");
            auto* lv = builder_.CreateLoad(elem_ty, L_gep(tv2_64), "sch_l");
            auto* pv = builder_.CreateLoad(elem_ty, L_gep(safe), "sch_p");
            auto* newv = op2(lv, pv);
            auto* v2 = builder_.CreateSelect(ge, newv, lv, "sch_nv");
            builder_.CreateStore(v2, L_gep(tv2_64));
            auto* tv3 = builder_.CreateLoad(i32, t_a3);
            builder_.CreateStore(builder_.CreateSub(tv3, llvm::ConstantInt::get(i32, 1)), t_a3);
            builder_.CreateBr(tbb);
            builder_.SetInsertPoint(te);
            auto* dn = builder_.CreateLoad(i32, d_a);
            builder_.CreateStore(builder_.CreateShl(dn, llvm::ConstantInt::get(i32, 1)), d_a);
            builder_.CreateBr(dbb);
            builder_.SetInsertPoint(de);
        }
        // 6c) 写盘：b[start+t] = op(offv, L[t])（活跃线程）
        auto* t_a = createEntryBlockAlloca(current_function_, i64, "__sch_t4");
        builder_.CreateStore(llvm::ConstantInt::get(i64, 0), t_a);
        auto* tbb = llvm::BasicBlock::Create(ctx_, "sch_t4", current_function_);
        auto* tb = llvm::BasicBlock::Create(ctx_, "sch_t4b", current_function_);
        auto* te = llvm::BasicBlock::Create(ctx_, "sch_t4e", current_function_);
        builder_.CreateBr(tbb);
        builder_.SetInsertPoint(tbb);
        auto* tv = builder_.CreateLoad(i64, t_a);
        builder_.CreateCondBr(builder_.CreateICmpSLT(tv, llvm::ConstantInt::get(i64, bs)), tb, te);
        builder_.SetInsertPoint(tb);
        auto* tv2 = builder_.CreateLoad(i64, t_a);
        auto* idx = builder_.CreateAdd(start, tv2, "sch_i2");
        auto* in_blk = builder_.CreateICmpSLT(idx, end, "sch_in2");
        auto* lv = builder_.CreateLoad(elem_ty, L_gep(tv2), "sch_l2");
        builder_.CreateStore(offv, acc_a);
        builder_.CreateStore(lv, x_a);
        auto* opv = generateExpr(*s.op_expr);
        if (opv && opv->getType() != elem_ty) {
            if (elem_ty->isFloatingPointTy() && opv->getType()->isFloatingPointTy())
                opv = builder_.CreateFPCast(opv, elem_ty);
            else if (elem_ty->isIntegerTy() && opv->getType()->isIntegerTy())
                opv = builder_.CreateIntCast(opv, elem_ty, false);
        }
        auto* st2 = llvm::BasicBlock::Create(ctx_, "sch_st2", current_function_);
        auto* jn2 = llvm::BasicBlock::Create(ctx_, "sch_jn2", current_function_);
        builder_.CreateCondBr(in_blk, st2, jn2);
        builder_.SetInsertPoint(st2);
        builder_.CreateStore(opv, builder_.CreateGEP(elem_ty, b_src, idx), "sch_b");
        builder_.CreateBr(jn2);
        builder_.SetInsertPoint(jn2);
        auto* tv3 = builder_.CreateLoad(i64, t_a);
        builder_.CreateStore(builder_.CreateAdd(tv3, llvm::ConstantInt::get(i64, 1)), t_a);
        builder_.CreateBr(tbb);
        builder_.SetInsertPoint(te);
        // 恢复 acc/x（下一块）
        named_values_.back().erase(s.op_acc);
        named_values_.back().erase(s.op_x);
        if (old_acc2) setNamedValue(s.op_acc, old_acc2);
        if (old_x2) setNamedValue(s.op_x, old_x2);
        auto* jv3 = builder_.CreateLoad(i64, j2_a);
        builder_.CreateStore(builder_.CreateAdd(jv3, llvm::ConstantInt::get(i64, 1)), j2_a);
        builder_.CreateBr(jbb2);
        builder_.SetInsertPoint(je2);
    }
    named_values_.back().erase(s.op_acc);
    named_values_.back().erase(s.op_x);
    if (old_acc) setNamedValue(s.op_acc, old_acc);
    if (old_x) setNamedValue(s.op_x, old_x);
}

// §8.3 host 顺序前缀扫描：acc=init；for i in [0,cnt): x=src[i];
//   inclusive：acc=op(acc,x); dst[i]=acc；
//   exclusive：dst[i]=acc（更新前）; acc=op(acc,x)。
// op_expr 用 generateExpr（acc/x 绑定 host alloca）。
void CodeGen::emitSeqScan(llvm::Value* src, llvm::Value* dst, llvm::Value* cnt,
                          llvm::Type* elem_ty, const GpuScanStmt& s) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* acc_a = createEntryBlockAlloca(current_function_, elem_ty, "__sc_acc");
    auto* init_v = generateExpr(*s.init_expr);
    if (init_v && init_v->getType() != elem_ty) {
        if (elem_ty->isFloatingPointTy() && init_v->getType()->isFloatingPointTy())
            init_v = builder_.CreateFPCast(init_v, elem_ty);
        else if (elem_ty->isFloatingPointTy() && init_v->getType()->isIntegerTy())
            init_v = builder_.CreateSIToFP(init_v, elem_ty);
        else if (elem_ty->isIntegerTy() && init_v->getType()->isIntegerTy())
            init_v = builder_.CreateIntCast(init_v, elem_ty, false);
    }
    builder_.CreateStore(init_v, acc_a);
    auto* x_a = createEntryBlockAlloca(current_function_, elem_ty, "__sc_x");
    auto* i_a = createEntryBlockAlloca(current_function_, i64, "__sc_i");
    builder_.CreateStore(llvm::ConstantInt::get(i64, 0), i_a);
    auto old_acc = getNamedValue(s.op_acc);
    auto old_x = getNamedValue(s.op_x);
    setNamedValue(s.op_acc, acc_a);
    setNamedValue(s.op_x, x_a);
    auto* loop_bb = llvm::BasicBlock::Create(ctx_, "scs_loop", current_function_);
    auto* lbody = llvm::BasicBlock::Create(ctx_, "scs_body", current_function_);
    auto* lend = llvm::BasicBlock::Create(ctx_, "scs_end", current_function_);
    builder_.CreateBr(loop_bb);
    builder_.SetInsertPoint(loop_bb);
    auto* iv = builder_.CreateLoad(i64, i_a);
    builder_.CreateCondBr(builder_.CreateICmpSLT(iv, cnt), lbody, lend);
    builder_.SetInsertPoint(lbody);
    auto* iv2 = builder_.CreateLoad(i64, i_a);
    // exclusive：先写 dst[i] = acc（更新前）
    if (s.exclusive) {
        auto* acc_pre = builder_.CreateLoad(elem_ty, acc_a, "sc_acc_pre");
        builder_.CreateStore(acc_pre, builder_.CreateGEP(elem_ty, dst, iv2), "scb_ex");
    }
    auto* xv = builder_.CreateLoad(elem_ty, builder_.CreateGEP(elem_ty, src, iv2), "scx");
    builder_.CreateStore(xv, x_a);
    auto* opv = generateExpr(*s.op_expr);
    if (opv && opv->getType() != elem_ty) {
        if (elem_ty->isFloatingPointTy() && opv->getType()->isFloatingPointTy())
            opv = builder_.CreateFPCast(opv, elem_ty);
        else if (elem_ty->isIntegerTy() && opv->getType()->isIntegerTy())
            opv = builder_.CreateIntCast(opv, elem_ty, false);
    }
    builder_.CreateStore(opv, acc_a);
    // inclusive：dst[i] = acc（更新后）
    if (!s.exclusive)
        builder_.CreateStore(opv, builder_.CreateGEP(elem_ty, dst, iv2), "scb");
    auto* iv3 = builder_.CreateLoad(i64, i_a);
    builder_.CreateStore(builder_.CreateAdd(iv3, llvm::ConstantInt::get(i64, 1)), i_a);
    builder_.CreateBr(loop_bb);
    builder_.SetInsertPoint(lend);
    named_values_.back().erase(s.op_acc);
    named_values_.back().erase(s.op_x);
    if (old_acc) setNamedValue(s.op_acc, old_acc);
    if (old_x) setNamedValue(s.op_x, old_x);
}

// §8.6 块内并行 CPU 镜像：与 GPU 的 emitBlockSumTreePtx 同树归约。
// 每块 BS 个"lane"：L[t] = (start+t < end) ? a[start+t] : init（末块尾以 init 单位元
// 填充）；halving 树（half ∈ {BS/2,…,1}：L[t] = op(L[t], L[t+half]) for t < half——
// in-place 安全：源 L[t+half] 在 [half,BS) 本步不更新）→ partials[j] = L[0]。
// 然后 out = init∘partials[0]∘…（init 应用一次）。与 GPU 同树同序 → 位级一致。
// 要求 bs 为 2 的幂（编译期常量）。
void CodeGen::emitSeqBlockTreeReduce(llvm::Value* a_src, llvm::Value* cnt,
                                     llvm::Value* blocks, int bs,
                                     llvm::Type* elem_ty, const GpuReduceStmt& s,
                                     llvm::Value* out_slot) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    // L = 栈数组 [bs x elem]
    auto* arr_ty = llvm::ArrayType::get(elem_ty, bs);
    auto* L = builder_.CreateAlloca(arr_ty, nullptr, "__rdt_L");
    auto L_gep = [&](llvm::Value* idx) -> llvm::Value* {
        return builder_.CreateGEP(arr_ty, L, {llvm::ConstantInt::get(i32, 0),
            builder_.CreateIntCast(idx, i32, false)}, "rdt_L");
    };
    auto* partials = builder_.CreateAlloca(elem_ty, blocks, "__rdt_part");
    // acc/x 绑定（op 求值复用；所有 combine 共用一对 entry alloca）
    auto* acc_a = createEntryBlockAlloca(current_function_, elem_ty, "__rdt_acc");
    auto* x_a = createEntryBlockAlloca(current_function_, elem_ty, "__rdt_x");
    auto old_acc = getNamedValue(s.op_acc);
    auto old_x = getNamedValue(s.op_x);
    setNamedValue(s.op_acc, acc_a);
    setNamedValue(s.op_x, x_a);
    // init 值（单位元填充）
    auto* init_v = generateExpr(*s.init_expr);
    if (init_v && init_v->getType() != elem_ty) {
        if (elem_ty->isFloatingPointTy() && init_v->getType()->isFloatingPointTy())
            init_v = builder_.CreateFPCast(init_v, elem_ty);
        else if (elem_ty->isFloatingPointTy() && init_v->getType()->isIntegerTy())
            init_v = builder_.CreateSIToFP(init_v, elem_ty);
        else if (elem_ty->isIntegerTy() && init_v->getType()->isIntegerTy())
            init_v = builder_.CreateIntCast(init_v, elem_ty, false);
    }
    // for j in [0, blocks):
    auto* j_a = createEntryBlockAlloca(current_function_, i64, "__rdt_j");
    builder_.CreateStore(llvm::ConstantInt::get(i64, 0), j_a);
    auto* loop_bb = llvm::BasicBlock::Create(ctx_, "rdt_loop", current_function_);
    auto* lbody = llvm::BasicBlock::Create(ctx_, "rdt_body", current_function_);
    auto* lend = llvm::BasicBlock::Create(ctx_, "rdt_end", current_function_);
    builder_.CreateBr(loop_bb);
    builder_.SetInsertPoint(loop_bb);
    auto* jv = builder_.CreateLoad(i64, j_a);
    builder_.CreateCondBr(builder_.CreateICmpSLT(jv, blocks), lbody, lend);
    builder_.SetInsertPoint(lbody);
    auto* jv2 = builder_.CreateLoad(i64, j_a);
    auto* start = builder_.CreateMul(jv2, llvm::ConstantInt::get(i64, bs), "rdt_start");
    auto* end_raw = builder_.CreateAdd(start, llvm::ConstantInt::get(i64, bs));
    auto* end = builder_.CreateSelect(builder_.CreateICmpSLT(end_raw, cnt), end_raw, cnt, "rdt_end");
    // 填 L：for t in [0, bs): L[t] = (start+t < end) ? a[start+t] : init
    {
        auto* t_a = createEntryBlockAlloca(current_function_, i64, "__rdt_t");
        builder_.CreateStore(llvm::ConstantInt::get(i64, 0), t_a);
        auto* fbb = llvm::BasicBlock::Create(ctx_, "rdt_f", current_function_);
        auto* fb = llvm::BasicBlock::Create(ctx_, "rdt_fb", current_function_);
        auto* fe = llvm::BasicBlock::Create(ctx_, "rdt_fe", current_function_);
        builder_.CreateBr(fbb);
        builder_.SetInsertPoint(fbb);
        auto* tv = builder_.CreateLoad(i64, t_a);
        builder_.CreateCondBr(builder_.CreateICmpSLT(tv, llvm::ConstantInt::get(i64, bs)), fb, fe);
        builder_.SetInsertPoint(fb);
        auto* tv2 = builder_.CreateLoad(i64, t_a);
        auto* idx = builder_.CreateAdd(start, tv2, "rdt_idx");
        auto* in_blk = builder_.CreateICmpSLT(idx, end, "rdt_in");
        auto* av = builder_.CreateLoad(elem_ty, builder_.CreateGEP(elem_ty, a_src, idx), "rdt_av");
        auto* val = builder_.CreateSelect(in_blk, av, init_v, "rdt_lv");
        builder_.CreateStore(val, L_gep(tv2));
        auto* tv3 = builder_.CreateLoad(i64, t_a);
        builder_.CreateStore(builder_.CreateAdd(tv3, llvm::ConstantInt::get(i64, 1)), t_a);
        builder_.CreateBr(fbb);
        builder_.SetInsertPoint(fe);
    }
    // halving 树：for half in {bs/2,…,1}: for t in [0, half): L[t] = op(L[t], L[t+half])
    {
        auto* half_a = createEntryBlockAlloca(current_function_, i32, "__rdt_half");
        builder_.CreateStore(llvm::ConstantInt::get(i32, bs / 2), half_a);
        auto* hbb = llvm::BasicBlock::Create(ctx_, "rdt_h", current_function_);
        auto* hb = llvm::BasicBlock::Create(ctx_, "rdt_hb", current_function_);
        auto* he = llvm::BasicBlock::Create(ctx_, "rdt_he", current_function_);
        builder_.CreateBr(hbb);
        builder_.SetInsertPoint(hbb);
        auto* halfv = builder_.CreateLoad(i32, half_a);
        builder_.CreateCondBr(builder_.CreateICmpSGE(halfv, llvm::ConstantInt::get(i32, 1)), hb, he);
        builder_.SetInsertPoint(hb);
        // t loop [0, half)
        auto* t_a = createEntryBlockAlloca(current_function_, i32, "__rdt_t2");
        builder_.CreateStore(llvm::ConstantInt::get(i32, 0), t_a);
        auto* tbb = llvm::BasicBlock::Create(ctx_, "rdt_t", current_function_);
        auto* tb = llvm::BasicBlock::Create(ctx_, "rdt_tb", current_function_);
        auto* te = llvm::BasicBlock::Create(ctx_, "rdt_te", current_function_);
        builder_.CreateBr(tbb);
        builder_.SetInsertPoint(tbb);
        auto* tv = builder_.CreateLoad(i32, t_a);
        auto* halfc = builder_.CreateLoad(i32, half_a);
        builder_.CreateCondBr(builder_.CreateICmpSLT(tv, halfc), tb, te);
        builder_.SetInsertPoint(tb);
        auto* tv2 = builder_.CreateLoad(i32, t_a);
        auto* tv2_64 = builder_.CreateIntCast(tv2, i64, false);
        auto* hv_64 = builder_.CreateIntCast(builder_.CreateLoad(i32, half_a), i64, false);
        auto* other = builder_.CreateAdd(tv2_64, hv_64, "rdt_other");
        auto* lo = builder_.CreateLoad(elem_ty, L_gep(tv2_64), "rdt_lo");
        auto* hi = builder_.CreateLoad(elem_ty, L_gep(other), "rdt_hi");
        builder_.CreateStore(hi, x_a);
        builder_.CreateStore(lo, acc_a);
        auto* opv = generateExpr(*s.op_expr);
        if (opv && opv->getType() != elem_ty) {
            if (elem_ty->isFloatingPointTy() && opv->getType()->isFloatingPointTy())
                opv = builder_.CreateFPCast(opv, elem_ty);
            else if (elem_ty->isIntegerTy() && opv->getType()->isIntegerTy())
                opv = builder_.CreateIntCast(opv, elem_ty, false);
        }
        builder_.CreateStore(opv, L_gep(tv2_64));
        auto* tv3 = builder_.CreateLoad(i32, t_a);
        builder_.CreateStore(builder_.CreateAdd(tv3, llvm::ConstantInt::get(i32, 1)), t_a);
        builder_.CreateBr(tbb);
        builder_.SetInsertPoint(te);
        // half >>= 1
        auto* halfn = builder_.CreateLoad(i32, half_a);
        builder_.CreateStore(builder_.CreateLShr(halfn, llvm::ConstantInt::get(i32, 1)), half_a);
        builder_.CreateBr(hbb);
        builder_.SetInsertPoint(he);
    }
    // partials[j] = L[0]
    auto* l0 = builder_.CreateLoad(elem_ty, L_gep(llvm::ConstantInt::get(i64, 0)), "rdt_l0");
    builder_.CreateStore(l0, builder_.CreateGEP(elem_ty, partials, jv2, "rdt_pp"));
    auto* jv3 = builder_.CreateLoad(i64, j_a);
    builder_.CreateStore(builder_.CreateAdd(jv3, llvm::ConstantInt::get(i64, 1)), j_a);
    builder_.CreateBr(loop_bb);
    builder_.SetInsertPoint(lend);
    // 恢复 acc/x 绑定
    named_values_.back().erase(s.op_acc);
    named_values_.back().erase(s.op_x);
    if (old_acc) setNamedValue(s.op_acc, old_acc);
    if (old_x) setNamedValue(s.op_x, old_x);
    // out = init∘partials[0]∘partials[1]…（init 应用一次）
    emitSeqFold(partials, blocks, elem_ty, s, out_slot, true);
}

// §8.6 规范归约顺序（浮点位一致）：CPU 回退按与 GPU 相同的组合顺序归约。
// L1：for j in [0, blocks): partials[j] = fold(a[j*bs .. min((j+1)*bs, cnt)))
//      **纯块和**（不含 init——同 GPU K1 纯块和；emitSeqFold use_init=false 复用）；
// L2/L3：out = init∘partials[0]∘partials[1]∘…（init 只在最终合并应用一次，
//      emitSeqFold use_init=true 复用——同 GPU host 合并）。
// GPU 与 CPU 用同一桶划分 + 同一合并序 → 浮点结果位级一致（可双实现 diff 测试）。
void CodeGen::emitSeqBlockReduce(llvm::Value* a_src, llvm::Value* cnt,
                                 llvm::Value* blocks, llvm::Value* bs,
                                 llvm::Type* elem_ty, const GpuReduceStmt& s,
                                 llvm::Value* out_slot) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* partials = builder_.CreateAlloca(elem_ty, blocks, "__rd_part");
    auto* j_a = createEntryBlockAlloca(current_function_, i64, "__rd_j");
    builder_.CreateStore(llvm::ConstantInt::get(i64, 0), j_a);
    auto* loop_bb = llvm::BasicBlock::Create(ctx_, "rdb_loop", current_function_);
    auto* lbody = llvm::BasicBlock::Create(ctx_, "rdb_body", current_function_);
    auto* lend = llvm::BasicBlock::Create(ctx_, "rdb_end", current_function_);
    builder_.CreateBr(loop_bb);
    builder_.SetInsertPoint(loop_bb);
    auto* jv = builder_.CreateLoad(i64, j_a);
    builder_.CreateCondBr(builder_.CreateICmpSLT(jv, blocks), lbody, lend);
    builder_.SetInsertPoint(lbody);
    auto* jv2 = builder_.CreateLoad(i64, j_a);
    auto* start = builder_.CreateMul(jv2, bs, "rdb_start");
    auto* end_raw = builder_.CreateAdd(start, bs);
    auto* end = builder_.CreateSelect(builder_.CreateICmpSLT(end_raw, cnt), end_raw, cnt,
                                      "rdb_end");
    auto* blk_len = builder_.CreateSub(end, start);
    auto* a_blk = builder_.CreateGEP(elem_ty, a_src, start, "rdb_a");
    auto* part_slot = builder_.CreateGEP(elem_ty, partials, jv2, "rdb_p");
    // 纯块和（use_init=false：acc=a_blk[0]）；块必有 ≥1 元素（blocks=ceil(cnt/bs)）
    emitSeqFold(a_blk, blk_len, elem_ty, s, part_slot, false);
    auto* jv3 = builder_.CreateLoad(i64, j_a);
    builder_.CreateStore(builder_.CreateAdd(jv3, llvm::ConstantInt::get(i64, 1)), j_a);
    builder_.CreateBr(loop_bb);
    builder_.SetInsertPoint(lend);
    // L2/L3：out = init∘partials[0]∘partials[1]…（init 应用一次）
    emitSeqFold(partials, blocks, elem_ty, s, out_slot, true);
}

// §8.2 @gpu reduce：声明式归约 out = fold(init, a[lo..hi))。
// GPU：H2D a 范围 → 单 kernel（每块 tx==0 归约 → partials[bid]）→ D2H partials →
// host 顺序合并 → out。CPU 回退：§8.6 规范归约顺序（分块部分和 + 合并）。
void CodeGen::generateGpuReduce(const GpuReduceStmt& s) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    auto* ptr = llvm::PointerType::get(ctx_, 0);
    llvm::Type* elem_ty = nullptr;
    auto eit = array_elem_types_.find(s.array_name);
    if (eit != array_elem_types_.end()) elem_ty = eit->second;
    if (!elem_ty) { diag_.error(s.range, "'@gpu reduce' cannot determine element type"); return; }
    uint64_t esz = module_->getDataLayout().getTypeAllocSize(elem_ty);
    int block_size = s.block_val > 0 ? (int)s.block_val : 256;

    auto* lo = generateExpr(*s.begin_expr);
    if (lo->getType() != i64) lo = builder_.CreateSExtOrTrunc(lo, i64);
    auto* hi = generateExpr(*s.end_expr);
    if (hi->getType() != i64) hi = builder_.CreateSExtOrTrunc(hi, i64);
    auto* n = builder_.CreateSub(hi, lo, "rdn");

    auto* func = builder_.GetInsertBlock()->getParent();
    auto* done_bb = llvm::BasicBlock::Create(ctx_, "rd_done", func);
    // §8.6 块内并行：块大小为 2 的幂时用 halving 树 K1（BS 线程协作）+ 树镜像
    // （更快）；否则回退串行 K1（同旧行为）。GPU/CPU 同选 → 位级一致。
    bool bs_pow2 = (block_size & (block_size - 1)) == 0;
    // §8.8 空输入 n<=0：reduce → out = init（单位元），跳过分块归约（blocks=0
    // 会使 kernel grid=0 / partials[0] 越界）。
    auto* nz_bb = llvm::BasicBlock::Create(ctx_, "rd_nz", func);
    auto* empty_bb = llvm::BasicBlock::Create(ctx_, "rd_empty", func);
    builder_.CreateCondBr(builder_.CreateICmpSLE(n, llvm::ConstantInt::get(i64, 0)),
                          empty_bb, nz_bb);
    builder_.SetInsertPoint(empty_bb);
    {
        auto* init_v = generateExpr(*s.init_expr);
        if (init_v && init_v->getType() != elem_ty) {
            if (elem_ty->isFloatingPointTy() && init_v->getType()->isFloatingPointTy())
                init_v = builder_.CreateFPCast(init_v, elem_ty);
            else if (elem_ty->isFloatingPointTy() && init_v->getType()->isIntegerTy())
                init_v = builder_.CreateSIToFP(init_v, elem_ty);
            else if (elem_ty->isIntegerTy() && init_v->getType()->isIntegerTy())
                init_v = builder_.CreateIntCast(init_v, elem_ty, false);
        }
        builder_.CreateStore(init_v, getNamedValue(s.out_name));
    }
    builder_.CreateBr(done_bb);
    builder_.SetInsertPoint(nz_bb);
    auto* blocks = builder_.CreateUDiv(
        builder_.CreateAdd(n, llvm::ConstantInt::get(i64, block_size - 1)),
        llvm::ConstantInt::get(i64, block_size), "rdblk");

    auto* gpu_bb = llvm::BasicBlock::Create(ctx_, "rd_gpu", func);
    auto* cpu_bb = llvm::BasicBlock::Create(ctx_, "rd_cpu", func);
    auto* gpu_ok = builder_.CreateCall(runtime_gpu_init_, {}, "gpu_ok");
    auto* gpu_ok_i1 = builder_.CreateICmpNE(gpu_ok, llvm::ConstantInt::get(i32, 0));
    builder_.CreateCondBr(gpu_ok_i1, gpu_bb, cpu_bb);

    // ============ GPU path ============
    builder_.SetInsertPoint(gpu_bb);
    std::string ptx_str = bs_pow2
        ? emitBlockSumTreePtx(*s.op_expr, *s.init_expr, elem_ty, block_size, "myp_reduce")
        : emitBlockSumPtx(*s.op_expr, *s.init_expr, elem_ty, block_size, "myp_reduce");
    if (ptx_str.empty()) {
        diag_.warn(s.range, "'@gpu reduce' GPU kernel generation failed, running on CPU");
        builder_.CreateBr(cpu_bb);
        builder_.SetInsertPoint(cpu_bb);
        auto* arr_h0 = builder_.CreateLoad(ptr, getNamedValue(s.array_name), s.array_name);
        auto* arr_h0_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), arr_h0,
            builder_.CreateMul(lo, llvm::ConstantInt::get(i64, esz)), "rdh0");
        // §8.6 规范归约顺序：与 GPU 同分块 + 同合并序 → 位级一致
        if (bs_pow2)
            emitSeqBlockTreeReduce(arr_h0_off, n, blocks, block_size, elem_ty, s,
                                   getNamedValue(s.out_name));
        else
            emitSeqBlockReduce(arr_h0_off, n, blocks,
                               llvm::ConstantInt::get(i64, block_size),
                               elem_ty, s, getNamedValue(s.out_name));
        builder_.CreateBr(done_bb);
        builder_.SetInsertPoint(done_bb);
        return;
    }
    auto* ptx_global = builder_.CreateGlobalString(ptx_str, "__myp_ptx_reduce");
    auto* kctx = builder_.CreateCall(runtime_gpu_load_kernel_,
        {ptx_global, builder_.CreateGlobalString("myp_reduce", "rn")}, "rd_k");
    auto* k_ok = builder_.CreateICmpNE(kctx,
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)), "k_ok");
    auto* launch_bb = llvm::BasicBlock::Create(ctx_, "rd_launch", func);
    auto* gpu_done_bb = llvm::BasicBlock::Create(ctx_, "rd_gpu_done", func);
    builder_.CreateCondBr(k_ok, launch_bb, cpu_bb);

    builder_.SetInsertPoint(launch_bb);
    // H2D a 范围 [lo, hi)：arr_h + lo*esz，n*esz 字节
    auto* arr_va = getNamedValue(s.array_name);
    auto* arr_h = builder_.CreateLoad(ptr, arr_va, s.array_name);
    auto* arr_h_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), arr_h,
        builder_.CreateMul(lo, llvm::ConstantInt::get(i64, esz)), "rdh");
    auto* nbytes = builder_.CreateMul(n, llvm::ConstantInt::get(i64, esz));
    auto* a_dev = builder_.CreateCall(runtime_gpu_alloc_, {nbytes}, "rd_ad");
    builder_.CreateCall(runtime_gpu_to_device_, {a_dev, arr_h_off, nbytes});
    // partials 设备
    auto* pbytes = builder_.CreateMul(blocks, llvm::ConstantInt::get(i64, esz));
    auto* p_dev = builder_.CreateCall(runtime_gpu_alloc_, {pbytes}, "rd_pd");
    // args: n, a_dev, p_dev —— args 数组每元素 = 指向参数值的指针（void** 约定）
    const unsigned nargs = 3;
    auto* args_a = builder_.CreateAlloca(ptr, llvm::ConstantInt::get(i32, nargs), "rd_args");
    auto* t0 = builder_.CreateAlloca(i64);
    builder_.CreateStore(n, t0);
    builder_.CreateStore(builder_.CreateBitCast(t0, ptr),
        builder_.CreateGEP(ptr, args_a, llvm::ConstantInt::get(i32, 0)));
    auto* a_tmp = builder_.CreateAlloca(ptr);
    builder_.CreateStore(a_dev, a_tmp);
    builder_.CreateStore(builder_.CreateBitCast(a_tmp, ptr),
        builder_.CreateGEP(ptr, args_a, llvm::ConstantInt::get(i32, 1)));
    auto* p_tmp = builder_.CreateAlloca(ptr);
    builder_.CreateStore(p_dev, p_tmp);
    builder_.CreateStore(builder_.CreateBitCast(p_tmp, ptr),
        builder_.CreateGEP(ptr, args_a, llvm::ConstantInt::get(i32, 2)));
    builder_.CreateCall(runtime_gpu_launch_,
        {kctx, builder_.CreateIntCast(blocks, i32, false),
         llvm::ConstantInt::get(i32, block_size),
         builder_.CreateBitCast(args_a, ptr),
         llvm::ConstantInt::get(i32, nargs),
         llvm::ConstantInt::get(i64, 0)});
    // D2H partials → host 栈数组
    auto* ph = builder_.CreateAlloca(elem_ty, blocks, "rd_ph");
    builder_.CreateCall(runtime_gpu_to_host_, {ph, p_dev, pbytes});
    // host 合并：out = init∘ph[0]∘ph[1]∘…（K1 为纯块和，init 应用一次）
    emitSeqFold(ph, blocks, elem_ty, s, getNamedValue(s.out_name), true);
    // 清理
    builder_.CreateCall(runtime_gpu_free_, {a_dev});
    builder_.CreateCall(runtime_gpu_free_, {p_dev});
    builder_.CreateCall(runtime_gpu_destroy_kernel_, {kctx});
    builder_.CreateBr(gpu_done_bb);
    builder_.SetInsertPoint(gpu_done_bb);
    builder_.CreateBr(done_bb);

    // ============ CPU fallback ============
    builder_.SetInsertPoint(cpu_bb);
    diag_.warn(s.range, "'@gpu reduce' GPU fallback — running on CPU");
    auto* arr_hc = builder_.CreateLoad(ptr, getNamedValue(s.array_name), s.array_name);
    auto* arr_hc_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), arr_hc,
        builder_.CreateMul(lo, llvm::ConstantInt::get(i64, esz)), "rdhc");
    // §8.6 规范归约顺序：与 GPU 同分块 + 同合并序 → 位级一致
    if (bs_pow2)
        emitSeqBlockTreeReduce(arr_hc_off, n, blocks, block_size, elem_ty, s,
                               getNamedValue(s.out_name));
    else
        emitSeqBlockReduce(arr_hc_off, n, blocks, llvm::ConstantInt::get(i64, block_size),
                           elem_ty, s, getNamedValue(s.out_name));
    builder_.CreateBr(done_bb);

    builder_.SetInsertPoint(done_bb);
}

// §8.2 @gpu reduce 表达式形式（GpuReduceExpr）：无 `-> out`，结果作为表达式值。
// 合成元素类型临时标量（stmt.out_name）注册进当前作用域 → 复用 generateGpuReduce
// 写入 → 读出作为表达式值 → 注销临时。
llvm::Value* CodeGen::generateGpuReduceExpr(const GpuReduceExpr& e) {
    auto& stmt = *e.stmt;
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    llvm::Type* elem_ty = nullptr;
    auto eit = array_elem_types_.find(stmt.array_name);
    if (eit != array_elem_types_.end()) elem_ty = eit->second;
    if (!elem_ty) {
        diag_.error(e.range, "'@gpu reduce' cannot determine element type");
        return llvm::ConstantInt::get(i64, 0);
    }
    auto* tmp_a = createEntryBlockAlloca(current_function_, elem_ty, stmt.out_name);
    setNamedValue(stmt.out_name, tmp_a);
    generateGpuReduce(stmt);
    auto* val = builder_.CreateLoad(elem_ty, tmp_a, "rd_expr");
    named_values_.back().erase(stmt.out_name);
    return val;
}

// §8.3 K2 块内 scan kernel（**Hillis-Steele 并行版**）：void myp_scan_k2_hs
// (i64 n, T* offsets, T* a, T* b)。每块 BS 线程协作做块内前缀：
//   · local[t] = (start+t < end) ? a[start+t] : init（末块尾以 init 单位元填充）；
//   · Hillis-Steele（ping-pong 双缓冲）：for d in {1,2,4,…,BS/2}：
//       dst[t] = (t ≥ d) ? op(src[t], src[t-d]) : src[t]；步间 barrier；
//   · 对活跃线程：b[start+t] = op(offsets[bid], result[t])（含前面块前缀）。
// 与 CPU 镜像（emitSeqScanBlocked）同 HS 顺序 → 位级一致。块大小须 2 的幂。
std::string CodeGen::emitScanK2HsPtx(const Expr& op_expr, const Expr& init_expr,
                                     llvm::Type* elem_ty, int block_size) {
#ifdef MYP_ENABLE_GPU
    auto ptx_mod = std::make_unique<llvm::Module>("myp_gpu_scan_k2_hs", ctx_);
    ptx_mod->setTargetTriple(llvm::Triple(gpuTargetTriple()));
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    auto* ptr = llvm::PointerType::get(ctx_, 0);
    auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {i64, ptr, ptr, ptr}, false);
    auto* kf = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_scan_k2_hs", ptx_mod.get());
    setGpuKernelCC(kf);
    auto* n_arg = kf->getArg(0); n_arg->setName("n");
    auto* off_arg = kf->getArg(1); off_arg->setName("offsets");
    auto* a_arg = kf->getArg(2); a_arg->setName("a");
    auto* b_arg = kf->getArg(3); b_arg->setName("b");
    auto* entry = llvm::BasicBlock::Create(ctx_, "entry", kf);
    llvm::IRBuilder<> kb(entry);
    auto* tid_x = kb.CreateIntCast(emitGpuThreadIdx(kb), i64, false, "tx");
    auto* ctaid = kb.CreateIntCast(emitGpuBlockIdx(kb), i64, false, "bid");
    std::map<std::string, llvm::Value*> kv;
    kv["n"] = n_arg; kv["a"] = a_arg; kv["b"] = b_arg; kv["offsets"] = off_arg;
    auto* start_i = kb.CreateMul(ctaid, llvm::ConstantInt::get(i64, block_size), "istart");
    auto* iend_raw = kb.CreateAdd(start_i, llvm::ConstantInt::get(i64, block_size));
    auto* iend = kb.CreateSelect(kb.CreateICmpSLT(iend_raw, n_arg), iend_raw, n_arg, "iend");
    auto* in_blk = kb.CreateICmpSLT(kb.CreateAdd(start_i, tid_x), iend, "inblk");
    // acc/x 绑定（op 求值）
    auto* acc_a = createKernelAlloca(kb, elem_ty, nullptr, "acc");
    kv["acc"] = acc_a;
    auto* x_a = createKernelAlloca(kb, elem_ty, nullptr, "x");
    kv["x"] = x_a;
    auto* init_v = emitKernelExpr(init_expr, kb, kv, {}, "", nullptr);
    if (init_v && init_v->getType() != elem_ty) {
        if (elem_ty->isFloatingPointTy() && init_v->getType()->isFloatingPointTy())
            init_v = kb.CreateFPCast(init_v, elem_ty);
        else if (elem_ty->isFloatingPointTy() && init_v->getType()->isIntegerTy())
            init_v = kb.CreateSIToFP(init_v, elem_ty);
    }
    // ping-pong 共享数组
    auto* arr_ty = llvm::ArrayType::get(elem_ty, block_size);
    auto* mod = kb.GetInsertBlock()->getParent()->getParent();
    static int hs_id = 0;
    std::string tag = std::to_string(hs_id++);
    auto* smemA = new llvm::GlobalVariable(*mod, arr_ty, false,
        llvm::GlobalValue::InternalLinkage, llvm::ConstantAggregateZero::get(arr_ty),
        "myp_hs_a_" + tag, nullptr, llvm::GlobalValue::NotThreadLocal, 3);
    auto* smemB = new llvm::GlobalVariable(*mod, arr_ty, false,
        llvm::GlobalValue::InternalLinkage, llvm::ConstantAggregateZero::get(arr_ty),
        "myp_hs_b_" + tag, nullptr, llvm::GlobalValue::NotThreadLocal, 3);
    auto sel_smem = [&](llvm::Value* is_a, llvm::Value* idx) -> llvm::Value* {
        auto* pa = kb.CreateGEP(arr_ty, smemA, {llvm::ConstantInt::get(i32, 0), idx}, "sa");
        auto* pb = kb.CreateGEP(arr_ty, smemB, {llvm::ConstantInt::get(i32, 0), idx}, "sb");
        return kb.CreateSelect(is_a, pa, pb, "smem");
    };
    auto* true_v = llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx_), 1);
    // local[t] 填 smemA[t]
    llvm::Value* lv = kb.CreateLoad(elem_ty, kb.CreateGEP(elem_ty, a_arg,
        kb.CreateAdd(start_i, tid_x)), "hslv");
    lv = kb.CreateSelect(in_blk, lv, init_v, "hs_sel");
    kb.CreateStore(lv, sel_smem(true_v, kb.CreateIntCast(tid_x, i32, false)));
    emitGpuBarrier(kb);
    // Hillis-Steele：for d in {1,2,4,…,BS/2}
    int steps = 0; for (int d = 1; d < block_size; d <<= 1) steps++;
    llvm::Value* src_is_a = true_v;   // 第 0 步 src=smemA, dst=smemB
    for (int si = 0; si < steps; si++) {
        int d = 1 << si;
        llvm::Value* dv = llvm::ConstantInt::get(i32, d);
        llvm::Value* dst_is_a = kb.CreateXor(src_is_a, true_v, "dst");
        auto* tid32 = kb.CreateIntCast(tid_x, i32, false);
        auto* v = kb.CreateLoad(elem_ty, sel_smem(src_is_a, tid32), "hv");
        auto* ge = kb.CreateICmpUGE(tid32, dv, "ge");
        auto* safe_idx = kb.CreateSelect(ge, kb.CreateSub(tid32, dv, "hd"), llvm::ConstantInt::get(i32, 0), "hidx");
        auto* w = kb.CreateLoad(elem_ty, sel_smem(src_is_a, safe_idx), "hw");
        kb.CreateStore(w, x_a);
        kb.CreateStore(v, acc_a);
        auto* opv = emitKernelExpr(op_expr, kb, kv, {}, "", nullptr);
        if (opv && opv->getType() != elem_ty) {
            if (elem_ty->isFloatingPointTy() && opv->getType()->isFloatingPointTy())
                opv = kb.CreateFPCast(opv, elem_ty);
            else if (elem_ty->isIntegerTy() && opv->getType()->isIntegerTy())
                opv = kb.CreateIntCast(opv, elem_ty, false);
        }
        auto* v2 = kb.CreateSelect(ge, opv, v, "hv2");
        kb.CreateStore(v2, sel_smem(dst_is_a, tid32));
        emitGpuBarrier(kb);
        src_is_a = dst_is_a;
    }
    // 结果在最后 dst；b[start+t] = op(offsets[bid], res[t])（活跃线程）
    auto* res_v = kb.CreateLoad(elem_ty, sel_smem(src_is_a, kb.CreateIntCast(tid_x, i32, false)), "hsres");
    auto* off_v = kb.CreateLoad(elem_ty, kb.CreateGEP(elem_ty, off_arg, ctaid), "hoff");
    kb.CreateStore(off_v, acc_a);
    kb.CreateStore(res_v, x_a);
    auto* fopv = emitKernelExpr(op_expr, kb, kv, {}, "", nullptr);
    if (fopv && fopv->getType() != elem_ty) {
        if (elem_ty->isFloatingPointTy() && fopv->getType()->isFloatingPointTy())
            fopv = kb.CreateFPCast(fopv, elem_ty);
        else if (elem_ty->isIntegerTy() && fopv->getType()->isIntegerTy())
            fopv = kb.CreateIntCast(fopv, elem_ty, false);
    }
    auto* st_bb = llvm::BasicBlock::Create(ctx_, "hs_st", kf);
    auto* join_bb = llvm::BasicBlock::Create(ctx_, "hs_join", kf);
    kb.CreateCondBr(in_blk, st_bb, join_bb);
    kb.SetInsertPoint(st_bb);
    kb.CreateStore(fopv, kb.CreateGEP(elem_ty, b_arg, kb.CreateAdd(start_i, tid_x)), "hsb");
    kb.CreateBr(join_bb);
    kb.SetInsertPoint(join_bb);
    kb.CreateRetVoid();
    (void)init_expr;

    // ---- 目标机器 + verify + emit ----
    std::string err;
    ensureGpuTargetsInited();
    auto* tgt = llvm::TargetRegistry::lookupTarget(gpuTargetTriple(), err);
    if (!tgt) { diag_.warn(SourceRange{}, "GPU target not available: " + err); return ""; }
    auto* tm = tgt->createTargetMachine(llvm::Triple(gpuTargetTriple()), gpuTargetArch(), "",
        llvm::TargetOptions{}, llvm::Reloc::PIC_);
    if (!tm) { diag_.warn(SourceRange{}, "GPU target machine creation failed"); return ""; }
    std::string verify_err;
    llvm::raw_string_ostream vos(verify_err);
    if (llvm::verifyModule(*ptx_mod, &vos)) {
        diag_.warn(SourceRange{}, "GPU scan HS kernel verification failed: " + verify_err);
        delete tm; return "";
    }
    if (gpuTargetAmd()) runGpuOptPipeline(ptx_mod.get(), tm);
    std::string out = emitGpuTargetBytes(ptx_mod.get(), tm, "scan_k2_hs");
    if (getenv("MYP_DUMP_PTX") && !out.empty())
        fprintf(stderr, "=== MYP PTX (scan_k2_hs) ===\n%s\n=== END ===\n", out.c_str());
    if (out.empty()) { diag_.warn(SourceRange{}, "GPU emitted empty code"); return ""; }
    return out;
#else
    (void)op_expr; (void)init_expr; (void)elem_ty; (void)block_size;
    return "";
#endif
}

// §8.3 K2 块内 scan kernel：void myp_scan_k2(i64 n, T* offsets, T* a, T* b)。
// 每块 tx==0：acc = offsets[bid]（前面块前缀，含 init）；扫块内：
//   inclusive：b[i] = acc; acc = op(acc, a[i])（b[i] = init∘…∘a[i]）
//   exclusive：b[i] = acc; acc = op(acc, a[i]) 中 b[i] 取「更新前」的 acc
//     （b[i] = init∘…∘a[i-1]，b[块首]=offsets[bid]）
std::string CodeGen::emitScanK2Ptx(const Expr& op_expr, const Expr& init_expr,
                                   llvm::Type* elem_ty, int block_size,
                                   bool exclusive) {
#ifdef MYP_ENABLE_GPU
    auto ptx_mod = std::make_unique<llvm::Module>("myp_gpu_scan_k2", ctx_);
    ptx_mod->setTargetTriple(llvm::Triple(gpuTargetTriple()));
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* ptr = llvm::PointerType::get(ctx_, 0);
    auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {i64, ptr, ptr, ptr}, false);
    auto* kf = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_scan_k2", ptx_mod.get());
    setGpuKernelCC(kf);
    auto* n_arg = kf->getArg(0); n_arg->setName("n");
    auto* off_arg = kf->getArg(1); off_arg->setName("offsets");
    auto* a_arg = kf->getArg(2); a_arg->setName("a");
    auto* b_arg = kf->getArg(3); b_arg->setName("b");
    auto* entry = llvm::BasicBlock::Create(ctx_, "entry", kf);
    llvm::IRBuilder<> kb(entry);
    auto* tid_x = kb.CreateIntCast(emitGpuThreadIdx(kb), i64, false, "tx");
    auto* ctaid = kb.CreateIntCast(emitGpuBlockIdx(kb), i64, false, "bid");
    auto* is0 = kb.CreateICmpEQ(tid_x, llvm::ConstantInt::get(i64, 0));
    auto* body_bb = llvm::BasicBlock::Create(ctx_, "body", kf);
    auto* end_bb = llvm::BasicBlock::Create(ctx_, "end", kf);
    kb.CreateCondBr(is0, body_bb, end_bb);
    kb.SetInsertPoint(body_bb);
    std::map<std::string, llvm::Value*> kv;
    kv["n"] = n_arg; kv["a"] = a_arg; kv["b"] = b_arg; kv["offsets"] = off_arg;
    // acc = offsets[bid]（已含 init + 前面块前缀）
    auto* acc_a = createKernelAlloca(kb, elem_ty, nullptr, "acc");
    auto* offv = kb.CreateLoad(elem_ty, kb.CreateGEP(elem_ty, off_arg, ctaid), "off");
    kb.CreateStore(offv, acc_a);
    kv["acc"] = acc_a;
    auto* x_a = createKernelAlloca(kb, elem_ty, nullptr, "x");
    kv["x"] = x_a;
    auto* i_a = createKernelAlloca(kb, i64, nullptr, "i");
    auto* start_i = kb.CreateMul(ctaid, llvm::ConstantInt::get(i64, block_size), "istart");
    kb.CreateStore(start_i, i_a);
    auto* iend_raw = kb.CreateAdd(start_i, llvm::ConstantInt::get(i64, block_size));
    auto* iend = kb.CreateSelect(kb.CreateICmpSLT(iend_raw, n_arg), iend_raw, n_arg, "iend");
    auto* loop_bb = llvm::BasicBlock::Create(ctx_, "loop", kf);
    auto* lbody = llvm::BasicBlock::Create(ctx_, "lbody", kf);
    auto* lend = llvm::BasicBlock::Create(ctx_, "lend", kf);
    kb.CreateBr(loop_bb);
    kb.SetInsertPoint(loop_bb);
    auto* iv = kb.CreateLoad(i64, i_a);
    kb.CreateCondBr(kb.CreateICmpSLT(iv, iend), lbody, lend);
    kb.SetInsertPoint(lbody);
    auto* iv2 = kb.CreateLoad(i64, i_a);
    // exclusive：先写 b[i] = acc（更新前），再 acc = op(acc, a[i])
    if (exclusive) {
        auto* acc_before = kb.CreateLoad(elem_ty, acc_a, "acc_pre");
        kb.CreateStore(acc_before, kb.CreateGEP(elem_ty, b_arg, iv2), "scb_ex");
    }
    auto* xv = kb.CreateLoad(elem_ty, kb.CreateGEP(elem_ty, a_arg, iv2), "x");
    kb.CreateStore(xv, x_a);
    auto* opv = emitKernelExpr(op_expr, kb, kv, {}, "", nullptr);
    if (opv && opv->getType() != elem_ty) {
        if (elem_ty->isFloatingPointTy() && opv->getType()->isFloatingPointTy())
            opv = kb.CreateFPCast(opv, elem_ty);
        else if (elem_ty->isIntegerTy() && opv->getType()->isIntegerTy())
            opv = kb.CreateIntCast(opv, elem_ty, false);
    }
    kb.CreateStore(opv, acc_a);
    // inclusive：b[i] = acc（更新后）
    if (!exclusive)
        kb.CreateStore(opv, kb.CreateGEP(elem_ty, b_arg, iv2), "scb");
    auto* iv3 = kb.CreateLoad(i64, i_a);
    kb.CreateStore(kb.CreateAdd(iv3, llvm::ConstantInt::get(i64, 1)), i_a);
    kb.CreateBr(loop_bb);
    kb.SetInsertPoint(lend);
    kb.CreateBr(end_bb);
    kb.SetInsertPoint(end_bb);
    kb.CreateRetVoid();
    (void)init_expr;

    // ---- 目标机器 + verify + emit（§7.7 跨厂商）----
    std::string err;
    ensureGpuTargetsInited();
    auto* tgt = llvm::TargetRegistry::lookupTarget(gpuTargetTriple(), err);
    if (!tgt) { diag_.warn(SourceRange{}, "GPU target not available: " + err); return ""; }
    auto* tm = tgt->createTargetMachine(llvm::Triple(gpuTargetTriple()), gpuTargetArch(), "",
        llvm::TargetOptions{}, llvm::Reloc::PIC_);
    if (!tm) { diag_.warn(SourceRange{}, "GPU target machine creation failed"); return ""; }
    std::string verify_err;
    llvm::raw_string_ostream vos(verify_err);
    if (llvm::verifyModule(*ptx_mod, &vos)) {
        diag_.warn(SourceRange{}, "GPU scan kernel verification failed: " + verify_err);
        delete tm; return "";
    }
    // AMD：先跑 O2 管线，消解 AMDGPU ISel 无法选中的构造
    if (gpuTargetAmd()) runGpuOptPipeline(ptx_mod.get(), tm);
    std::string out = emitGpuTargetBytes(ptx_mod.get(), tm, "scan_k2");
    if (getenv("MYP_DUMP_PTX") && !out.empty())
        fprintf(stderr, "=== MYP PTX (scan_k2) ===\n%s\n=== END ===\n", out.c_str());
    if (out.empty()) { diag_.warn(SourceRange{}, "GPU emitted empty code"); return ""; }
    return out;
#else
    (void)op_expr; (void)init_expr; (void)elem_ty; (void)block_size; (void)exclusive;
    return "";
#endif
}

// §8.3 @gpu scan：前缀和 b[lo+i] = init∘a[lo]∘…∘a[lo+i]。
// GPU 两遍：K1 块和 → D2H partials → host 块前缀 offsets → H2D → K2 块内 scan → D2H b。
// CPU 回退：顺序前缀扫描。
void CodeGen::generateGpuScan(const GpuScanStmt& s) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    auto* ptr = llvm::PointerType::get(ctx_, 0);
    llvm::Type* elem_ty = nullptr;
    auto eit = array_elem_types_.find(s.in_name);
    if (eit != array_elem_types_.end()) elem_ty = eit->second;
    if (!elem_ty) { diag_.error(s.range, "'@gpu scan' cannot determine element type"); return; }
    uint64_t esz = module_->getDataLayout().getTypeAllocSize(elem_ty);
    int block_size = s.block_val > 0 ? (int)s.block_val : 256;

    auto* lo = generateExpr(*s.begin_expr);
    if (lo->getType() != i64) lo = builder_.CreateSExtOrTrunc(lo, i64);
    auto* hi = generateExpr(*s.end_expr);
    if (hi->getType() != i64) hi = builder_.CreateSExtOrTrunc(hi, i64);
    auto* n = builder_.CreateSub(hi, lo, "scn");

    auto* func = builder_.GetInsertBlock()->getParent();
    auto* done_bb = llvm::BasicBlock::Create(ctx_, "sc_done", func);
    // §8.6 块内并行：块大小为 2 的幂且非 exclusive 时，K1 用 halving 树、K2 用
    // Hillis-Steele（BS 线程协作）+ CPU 块镜像；否则回退串行（同旧行为，
    // exclusive 走串行保证实现简单可靠）。GPU/CPU 同选 → 位级一致。
    bool bs_pow2 = (block_size & (block_size - 1)) == 0;
    bool use_hs = bs_pow2 && !s.exclusive;
    // §8.8 空输入 n<=0：scan 输出不变（无前缀元素），跳过分块扫描。
    auto* nz_bb = llvm::BasicBlock::Create(ctx_, "sc_nz", func);
    auto* empty_bb = llvm::BasicBlock::Create(ctx_, "sc_empty", func);
    builder_.CreateCondBr(builder_.CreateICmpSLE(n, llvm::ConstantInt::get(i64, 0)),
                          empty_bb, nz_bb);
    builder_.SetInsertPoint(empty_bb);
    builder_.CreateBr(done_bb);
    builder_.SetInsertPoint(nz_bb);
    auto* blocks = builder_.CreateUDiv(
        builder_.CreateAdd(n, llvm::ConstantInt::get(i64, block_size - 1)),
        llvm::ConstantInt::get(i64, block_size), "scblk");

    auto* gpu_bb = llvm::BasicBlock::Create(ctx_, "sc_gpu", func);
    auto* cpu_bb = llvm::BasicBlock::Create(ctx_, "sc_cpu", func);
    auto* gpu_ok = builder_.CreateCall(runtime_gpu_init_, {}, "gpu_ok");
    auto* gpu_ok_i1 = builder_.CreateICmpNE(gpu_ok, llvm::ConstantInt::get(i32, 0));
    builder_.CreateCondBr(gpu_ok_i1, gpu_bb, cpu_bb);

    // ============ GPU path ============
    builder_.SetInsertPoint(gpu_bb);
    std::string k1_ptx = bs_pow2
        ? emitBlockSumTreePtx(*s.op_expr, *s.init_expr, elem_ty, block_size, "myp_scan_k1")
        : emitBlockSumPtx(*s.op_expr, *s.init_expr, elem_ty, block_size, "myp_scan_k1");
    std::string k2_ptx = use_hs
        ? emitScanK2HsPtx(*s.op_expr, *s.init_expr, elem_ty, block_size)
        : emitScanK2Ptx(*s.op_expr, *s.init_expr, elem_ty, block_size, s.exclusive);
    if (k1_ptx.empty() || k2_ptx.empty()) {
        diag_.warn(s.range, "'@gpu scan' GPU kernel generation failed, running on CPU");
        builder_.CreateBr(cpu_bb);
        builder_.SetInsertPoint(cpu_bb);
        // CPU 顺序前缀（快速回退；HS 位一致镜像 emitSeqScanBlocked 保留作参考，
        // 但 O(n log bs) 在串行 CPU 上慢 10×，回退路径不采用）
        auto* in_h0 = builder_.CreateLoad(ptr, getNamedValue(s.in_name), s.in_name);
        auto* in_h0_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), in_h0,
            builder_.CreateMul(lo, llvm::ConstantInt::get(i64, esz)), "sch0");
        auto* out_h0 = builder_.CreateLoad(ptr, getNamedValue(s.out_name), s.out_name);
        auto* out_h0_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), out_h0,
            builder_.CreateMul(lo, llvm::ConstantInt::get(i64, esz)), "sco0");
        emitSeqScan(in_h0_off, out_h0_off, n, elem_ty, s);
        builder_.CreateBr(done_bb);
        builder_.SetInsertPoint(done_bb);
        return;
    }
    // 设备分配
    auto* in_va = getNamedValue(s.in_name);
    auto* in_h = builder_.CreateLoad(ptr, in_va, s.in_name);
    auto* in_h_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), in_h,
        builder_.CreateMul(lo, llvm::ConstantInt::get(i64, esz)), "sch");
    auto* out_va = getNamedValue(s.out_name);
    auto* out_h = builder_.CreateLoad(ptr, out_va, s.out_name);
    auto* out_h_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), out_h,
        builder_.CreateMul(lo, llvm::ConstantInt::get(i64, esz)), "sco");
    auto* nbytes = builder_.CreateMul(n, llvm::ConstantInt::get(i64, esz));
    auto* a_dev = builder_.CreateCall(runtime_gpu_alloc_, {nbytes}, "sc_ad");
    builder_.CreateCall(runtime_gpu_to_device_, {a_dev, in_h_off, nbytes});
    auto* b_dev = builder_.CreateCall(runtime_gpu_alloc_, {nbytes}, "sc_bd");
    auto* pbytes = builder_.CreateMul(blocks, llvm::ConstantInt::get(i64, esz));
    auto* p_dev = builder_.CreateCall(runtime_gpu_alloc_, {pbytes}, "sc_pd");

    // 通用 launch helper（本地 lambda）：load + args + launch
    auto launch_kernel = [&](llvm::Value*& kctx_out, const std::string& ptx_str,
                             const std::string& gname, const std::string& kname,
                             std::vector<std::pair<llvm::Value*, llvm::Type*>> arg_vals,
                             llvm::BasicBlock* ok_bb) {
        auto* ptx_global = builder_.CreateGlobalString(ptx_str, gname);
        auto* kctx = builder_.CreateCall(runtime_gpu_load_kernel_,
            {ptx_global, builder_.CreateGlobalString(kname, "k")}, "sc_k");
        auto* k_ok = builder_.CreateICmpNE(kctx,
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)), "k_ok");
        auto* lb = llvm::BasicBlock::Create(ctx_, "sc_lb", func);
        auto* fall_bb = llvm::BasicBlock::Create(ctx_, "sc_fall", func);
        builder_.CreateCondBr(k_ok, lb, fall_bb);
        builder_.SetInsertPoint(lb);
        unsigned nargs = (unsigned)arg_vals.size();
        auto* args_a = builder_.CreateAlloca(ptr, llvm::ConstantInt::get(i32, nargs), "sc_args");
        for (unsigned i = 0; i < nargs; i++) {
            auto* tmp = builder_.CreateAlloca(arg_vals[i].second);
            auto* v = arg_vals[i].first;
            if (v->getType() != arg_vals[i].second)
                v = builder_.CreateBitCast(v, arg_vals[i].second);
            builder_.CreateStore(v, tmp);
            builder_.CreateStore(builder_.CreateBitCast(tmp, ptr),
                builder_.CreateGEP(ptr, args_a, llvm::ConstantInt::get(i32, i)));
        }
        builder_.CreateCall(runtime_gpu_launch_,
            {kctx, builder_.CreateIntCast(blocks, i32, false),
             llvm::ConstantInt::get(i32, block_size),
             builder_.CreateBitCast(args_a, ptr),
             llvm::ConstantInt::get(i32, nargs),
             llvm::ConstantInt::get(i64, 0)});
        builder_.CreateBr(ok_bb);
        builder_.SetInsertPoint(fall_bb);
        kctx_out = nullptr;
    };

    // K1：块和 → partials
    llvm::Value* k1_ctx = nullptr;
    auto* k1_ok = llvm::BasicBlock::Create(ctx_, "sc_k1_ok", func);
    auto* k1_fail = llvm::BasicBlock::Create(ctx_, "sc_k1_fail", func);
    launch_kernel(k1_ctx, k1_ptx, "__myp_ptx_scan_k1", "myp_scan_k1",
        {{n, i64}, {a_dev, ptr}, {p_dev, ptr}}, k1_ok);
    builder_.CreateBr(k1_fail);
    builder_.SetInsertPoint(k1_ok);
    // D2H partials
    auto* ph = builder_.CreateAlloca(elem_ty, blocks, "sc_ph");
    builder_.CreateCall(runtime_gpu_to_host_, {ph, p_dev, pbytes});
    // host 块前缀 offsets：off[0]=init; off[k]=op(off[k-1], ph[k-1])
    auto* off_h = builder_.CreateAlloca(elem_ty, blocks, "sc_off");
    {
        auto* acc_a = createEntryBlockAlloca(current_function_, elem_ty, "__sc_acc");
        auto* init_v = generateExpr(*s.init_expr);
        if (init_v && init_v->getType() != elem_ty) {
            if (elem_ty->isFloatingPointTy() && init_v->getType()->isFloatingPointTy())
                init_v = builder_.CreateFPCast(init_v, elem_ty);
            else if (elem_ty->isFloatingPointTy() && init_v->getType()->isIntegerTy())
                init_v = builder_.CreateSIToFP(init_v, elem_ty);
        }
        builder_.CreateStore(init_v, acc_a);
        auto* x_a = createEntryBlockAlloca(current_function_, elem_ty, "__sc_x");
        auto* i_a = createEntryBlockAlloca(current_function_, i64, "__sc_i");
        builder_.CreateStore(llvm::ConstantInt::get(i64, 0), i_a);
        auto old_acc = getNamedValue(s.op_acc);
        auto old_x = getNamedValue(s.op_x);
        setNamedValue(s.op_acc, acc_a);
        setNamedValue(s.op_x, x_a);
        auto* loop_bb = llvm::BasicBlock::Create(ctx_, "scf_loop", current_function_);
        auto* lbody = llvm::BasicBlock::Create(ctx_, "scf_body", current_function_);
        auto* lend = llvm::BasicBlock::Create(ctx_, "scf_end", current_function_);
        builder_.CreateBr(loop_bb);
        builder_.SetInsertPoint(loop_bb);
        auto* iv = builder_.CreateLoad(i64, i_a);
        builder_.CreateCondBr(builder_.CreateICmpSLT(iv, blocks), lbody, lend);
        builder_.SetInsertPoint(lbody);
        // off[k] = acc; acc = op(acc, ph[k])
        auto* iv2 = builder_.CreateLoad(i64, i_a);
        auto* acc_cur = builder_.CreateLoad(elem_ty, acc_a);
        builder_.CreateStore(acc_cur, builder_.CreateGEP(elem_ty, off_h, iv2), "scoff");
        auto* pv = builder_.CreateLoad(elem_ty, builder_.CreateGEP(elem_ty, ph, iv2), "scpv");
        builder_.CreateStore(pv, x_a);
        auto* opv = generateExpr(*s.op_expr);
        if (opv && opv->getType() != elem_ty) {
            if (elem_ty->isFloatingPointTy() && opv->getType()->isFloatingPointTy())
                opv = builder_.CreateFPCast(opv, elem_ty);
            else if (elem_ty->isIntegerTy() && opv->getType()->isIntegerTy())
                opv = builder_.CreateIntCast(opv, elem_ty, false);
        }
        builder_.CreateStore(opv, acc_a);
        auto* iv3 = builder_.CreateLoad(i64, i_a);
        builder_.CreateStore(builder_.CreateAdd(iv3, llvm::ConstantInt::get(i64, 1)), i_a);
        builder_.CreateBr(loop_bb);
        builder_.SetInsertPoint(lend);
        named_values_.back().erase(s.op_acc);
        named_values_.back().erase(s.op_x);
        if (old_acc) setNamedValue(s.op_acc, old_acc);
        if (old_x) setNamedValue(s.op_x, old_x);
    }
    // H2D offsets
    auto* off_dev = builder_.CreateCall(runtime_gpu_alloc_, {pbytes}, "sc_od");
    builder_.CreateCall(runtime_gpu_to_device_, {off_dev, off_h, pbytes});
    // K2：块内 scan
    llvm::Value* k2_ctx = nullptr;
    auto* k2_ok = llvm::BasicBlock::Create(ctx_, "sc_k2_ok", func);
    auto* k2_fail = llvm::BasicBlock::Create(ctx_, "sc_k2_fail", func);
    // HS 路径 kernel 名是 myp_scan_k2_hs，串行路径是 myp_scan_k2（此前硬编码
    // myp_scan_k2 → HS 路径 load 失败静默回退 CPU）。
    launch_kernel(k2_ctx, k2_ptx, "__myp_ptx_scan_k2",
        use_hs ? "myp_scan_k2_hs" : "myp_scan_k2",
        {{n, i64}, {off_dev, ptr}, {a_dev, ptr}, {b_dev, ptr}}, k2_ok);
    builder_.CreateBr(k2_fail);
    builder_.SetInsertPoint(k2_ok);
    // D2H b
    builder_.CreateCall(runtime_gpu_to_host_, {out_h_off, b_dev, nbytes});
    builder_.CreateCall(runtime_gpu_free_, {a_dev});
    builder_.CreateCall(runtime_gpu_free_, {b_dev});
    builder_.CreateCall(runtime_gpu_free_, {p_dev});
    builder_.CreateCall(runtime_gpu_free_, {off_dev});
    builder_.CreateBr(done_bb);

    // GPU kernel load 失败 → CPU fallback
    builder_.SetInsertPoint(k1_fail);
    builder_.CreateBr(cpu_bb);
    builder_.SetInsertPoint(k2_fail);
    builder_.CreateBr(cpu_bb);

    // ============ CPU fallback ============
    builder_.SetInsertPoint(cpu_bb);
    diag_.warn(s.range, "'@gpu scan' GPU fallback — running on CPU");
    auto* in_hc = builder_.CreateLoad(ptr, getNamedValue(s.in_name), s.in_name);
    auto* in_hc_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), in_hc,
        builder_.CreateMul(lo, llvm::ConstantInt::get(i64, esz)), "schc");
    auto* out_hc = builder_.CreateLoad(ptr, getNamedValue(s.out_name), s.out_name);
    auto* out_hc_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), out_hc,
        builder_.CreateMul(lo, llvm::ConstantInt::get(i64, esz)), "scoc");
    // CPU 回退统一用串行 emitSeqScan（快速，恢复基线）；GPU HS 位一致镜像
    // emitSeqScanBlocked 保留作参考/测试，但 O(n log bs) 在串行 CPU 上慢 10×，
    // 回退路径不采用（GPU/CPU 浮点可能差几位，容差内；reduce 仍严格位一致）。
    emitSeqScan(in_hc_off, out_hc_off, n, elem_ty, s);
    builder_.CreateBr(done_bb);

    builder_.SetInsertPoint(done_bb);
}

// §8.4 scatter 写 kernel PTX（grid-stride）：void <name>(i64 n, T* a, i32* idx, T* b)。
// 每线程 gid = bid*blockDim + tid，处理 p = gid, gid+stride, ...（stride = blockDim*gridDim）。
// unique/any：b[idx[p]] = a[p]；atomic_add：atomicrmw（Add / FAdd）。
std::string CodeGen::emitScatterPtx(llvm::Type* elem_ty, int block_size,
                                    bool atomic_add, const std::string& kernel_name) {
#ifdef MYP_ENABLE_GPU
    gpu_amd_grid_used_ = false;
    auto ptx_mod = std::make_unique<llvm::Module>("myp_gpu_scatter", ctx_);
    ptx_mod->setTargetTriple(llvm::Triple(gpuTargetTriple()));
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    auto* ptr = llvm::PointerType::get(ctx_, 0);
    auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
        {i64, ptr, ptr, ptr}, false);
    auto* kf = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
        kernel_name, ptx_mod.get());
    setGpuKernelCC(kf);
    auto* n_arg = kf->getArg(0); n_arg->setName("n");
    auto* a_arg = kf->getArg(1); a_arg->setName("a");
    auto* idx_arg = kf->getArg(2); idx_arg->setName("idx");
    auto* b_arg = kf->getArg(3); b_arg->setName("b");
    auto* entry = llvm::BasicBlock::Create(ctx_, "entry", kf);
    llvm::IRBuilder<> kb(entry);
    auto* tid = kb.CreateIntCast(emitGpuThreadIdx(kb), i64, false, "tx");
    auto* ntid = kb.CreateIntCast(emitGpuBlockDim(kb, block_size), i64, false, "ntid");
    auto* ctaid = kb.CreateIntCast(emitGpuBlockIdx(kb), i64, false, "bid");
    auto* nctaid = kb.CreateIntCast(emitGpuGridDim(kb), i64, false, "nb");
    auto* gid0 = kb.CreateAdd(kb.CreateMul(ctaid, ntid, "b0"), tid, "gid");
    auto* stride = kb.CreateMul(ntid, nctaid, "stride");
    auto* gid_a = createKernelAlloca(kb, i64, nullptr, "gid");
    kb.CreateStore(gid0, gid_a);
    auto* loop_bb = llvm::BasicBlock::Create(ctx_, "loop", kf);
    auto* lbody = llvm::BasicBlock::Create(ctx_, "lbody", kf);
    auto* lend = llvm::BasicBlock::Create(ctx_, "lend", kf);
    kb.CreateBr(loop_bb);
    kb.SetInsertPoint(loop_bb);
    auto* gv = kb.CreateLoad(i64, gid_a);
    kb.CreateCondBr(kb.CreateICmpSLT(gv, n_arg), lbody, lend);
    kb.SetInsertPoint(lbody);
    auto* gv2 = kb.CreateLoad(i64, gid_a);
    auto* av = kb.CreateLoad(elem_ty, kb.CreateGEP(elem_ty, a_arg, gv2), "av");
    auto* j32 = kb.CreateLoad(i32, kb.CreateGEP(i32, idx_arg, gv2), "j");
    auto* j = kb.CreateSExt(j32, i64, "j64");
    auto* bp = kb.CreateGEP(elem_ty, b_arg, j, "bp");
    if (atomic_add) {
        if (elem_ty->isFloatingPointTy())
            kb.CreateAtomicRMW(llvm::AtomicRMWInst::FAdd, bp, av, llvm::MaybeAlign(),
                llvm::AtomicOrdering::SequentiallyConsistent);
        else
            kb.CreateAtomicRMW(llvm::AtomicRMWInst::Add, bp, av, llvm::MaybeAlign(),
                llvm::AtomicOrdering::SequentiallyConsistent);
    } else {
        kb.CreateStore(av, bp);
    }
    auto* gv3 = kb.CreateLoad(i64, gid_a);
    kb.CreateStore(kb.CreateAdd(gv3, stride), gid_a);
    kb.CreateBr(loop_bb);
    kb.SetInsertPoint(lend);
    kb.CreateRetVoid();
    // §6.4 AMD：grid-stride 需 gridDim（AMDGCN 无直接 intrinsic）→ 回退 CPU
    if (gpu_amd_grid_used_) {
        diag_.warn(SourceRange{},
            "AMD target: grid-stride scatter unsupported (no gridDim intrinsic); running on CPU");
        return "";
    }

    // ---- 目标机器 + verify + emit（§7.7 跨厂商）----
    std::string err;
    ensureGpuTargetsInited();
    auto* tgt = llvm::TargetRegistry::lookupTarget(gpuTargetTriple(), err);
    if (!tgt) { diag_.warn(SourceRange{}, "GPU target not available: " + err); return ""; }
    auto* tm = tgt->createTargetMachine(llvm::Triple(gpuTargetTriple()), gpuTargetArch(), "",
        llvm::TargetOptions{}, llvm::Reloc::PIC_);
    if (!tm) { diag_.warn(SourceRange{}, "GPU target machine creation failed"); return ""; }
    std::string verify_err;
    llvm::raw_string_ostream vos(verify_err);
    if (llvm::verifyModule(*ptx_mod, &vos)) {
        diag_.warn(SourceRange{}, "GPU scatter kernel verification failed: " + verify_err);
        delete tm; return "";
    }
    // AMD：先跑 O2 管线，消解 AMDGPU ISel 无法选中的构造
    if (gpuTargetAmd()) runGpuOptPipeline(ptx_mod.get(), tm);
    std::string out = emitGpuTargetBytes(ptx_mod.get(), tm, "scatter");
    if (getenv("MYP_DUMP_PTX") && !out.empty())
        fprintf(stderr, "=== MYP PTX (scatter) ===\n%s\n=== END ===\n", out.c_str());
    if (out.empty()) { diag_.warn(SourceRange{}, "GPU emitted empty code"); return ""; }
    return out;
#else
    (void)elem_ty; (void)block_size; (void)atomic_add;
    return "";
#endif
}

// §8.4 host 顺序散点（CPU 回退）：for p in [0,cnt): j = sext(idx[p]);
// atomic_add ? b[j] += a[p]（规范顺序）: b[j] = a[p]。idx 元素 i32。
void CodeGen::emitSeqScatter(llvm::Value* a_src, llvm::Value* idx_src,
                             llvm::Value* b_src, llvm::Value* cnt,
                             llvm::Type* elem_ty, bool atomic_add) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    auto* i_a = createEntryBlockAlloca(current_function_, i64, "__st_i");
    builder_.CreateStore(llvm::ConstantInt::get(i64, 0), i_a);
    auto* loop_bb = llvm::BasicBlock::Create(ctx_, "st_loop", current_function_);
    auto* lbody = llvm::BasicBlock::Create(ctx_, "st_body", current_function_);
    auto* lend = llvm::BasicBlock::Create(ctx_, "st_end", current_function_);
    builder_.CreateBr(loop_bb);
    builder_.SetInsertPoint(loop_bb);
    auto* iv = builder_.CreateLoad(i64, i_a);
    builder_.CreateCondBr(builder_.CreateICmpSLT(iv, cnt), lbody, lend);
    builder_.SetInsertPoint(lbody);
    auto* iv2 = builder_.CreateLoad(i64, i_a);
    auto* av = builder_.CreateLoad(elem_ty, builder_.CreateGEP(elem_ty, a_src, iv2), "stv");
    auto* j32 = builder_.CreateLoad(i32, builder_.CreateGEP(i32, idx_src, iv2), "stj");
    auto* j = builder_.CreateSExt(j32, i64, "stj64");
    auto* bp = builder_.CreateGEP(elem_ty, b_src, j, "stbp");
    if (atomic_add) {
        auto* cur = builder_.CreateLoad(elem_ty, bp);
        auto* nv = elem_ty->isFloatingPointTy()
            ? builder_.CreateFAdd(cur, av)
            : builder_.CreateAdd(cur, av);
        builder_.CreateStore(nv, bp);
    } else {
        builder_.CreateStore(av, bp);
    }
    auto* iv3 = builder_.CreateLoad(i64, i_a);
    builder_.CreateStore(builder_.CreateAdd(iv3, llvm::ConstantInt::get(i64, 1)), i_a);
    builder_.CreateBr(loop_bb);
    builder_.SetInsertPoint(lend);
}

// §8.4 unique 模式 host 预扫：len_b = *(i64*)((char*)b_src - 24)（backing count）。
// for p in [0,cnt): j = sext(idx[p])；越界（j<0 || j>=len_b）或重复 → 报错退出。
// idx 元素 i32；mark = 每槽 1 字节，len_b 大小。
void CodeGen::emitScatterIdxCheck(llvm::Value* idx_src, llvm::Value* cnt,
                                  llvm::Value* b_src) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    auto* i8 = llvm::Type::getInt8Ty(ctx_);
    auto* ptr = llvm::PointerType::get(ctx_, 0);
    auto* len_b = builder_.CreateLoad(i64,
        builder_.CreateGEP(i8, b_src, llvm::ConstantInt::get(i64, -24)), "b_len");
    // mark：ref-counted byte backing（myp_alloc_slice_backing），用后 myp_release。
    // 不能 myp_alloc/myp_free（myp_alloc 现为计数字符串专用，直接 free 指针非法）。
    auto* alloc_sb = module_->getFunction("myp_alloc_slice_backing");
    if (!alloc_sb) {
        auto* ft = llvm::FunctionType::get(ptr, {i64, i32, i32}, false);
        alloc_sb = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
            "myp_alloc_slice_backing", module_.get());
    }
    auto* mark = builder_.CreateCall(alloc_sb,
        {len_b, llvm::ConstantInt::get(i32, 1), llvm::ConstantInt::get(i32, 1)},
        "st_mark");
    builder_.CreateMemSet(mark, llvm::ConstantInt::get(i8, 0), len_b, llvm::Align(1));
    auto* i_a = createEntryBlockAlloca(current_function_, i64, "__chk_i");
    builder_.CreateStore(llvm::ConstantInt::get(i64, 0), i_a);
    auto* loop_bb = llvm::BasicBlock::Create(ctx_, "chk_loop", current_function_);
    auto* lbody = llvm::BasicBlock::Create(ctx_, "chk_body", current_function_);
    auto* lend = llvm::BasicBlock::Create(ctx_, "chk_end", current_function_);
    auto* oob_fail_bb = llvm::BasicBlock::Create(ctx_, "chk_oob", current_function_);
    auto* dup_check_bb = llvm::BasicBlock::Create(ctx_, "chk_dupchk", current_function_);
    auto* dup_fail_bb = llvm::BasicBlock::Create(ctx_, "chk_dup", current_function_);
    auto* ok_bb = llvm::BasicBlock::Create(ctx_, "chk_ok", current_function_);
    builder_.CreateBr(loop_bb);
    builder_.SetInsertPoint(loop_bb);
    auto* iv = builder_.CreateLoad(i64, i_a);
    builder_.CreateCondBr(builder_.CreateICmpSLT(iv, cnt), lbody, lend);
    builder_.SetInsertPoint(lbody);
    auto* iv2 = builder_.CreateLoad(i64, i_a);
    auto* j32 = builder_.CreateLoad(i32, builder_.CreateGEP(i32, idx_src, iv2), "chk_j");
    auto* j = builder_.CreateSExt(j32, i64, "chk_j64");
    auto* oob = builder_.CreateOr(
        builder_.CreateICmpSLT(j, llvm::ConstantInt::get(i64, 0)),
        builder_.CreateICmpSGE(j, len_b));
    builder_.CreateCondBr(oob, oob_fail_bb, dup_check_bb);
    // 越界 fail（noreturn）
    builder_.SetInsertPoint(oob_fail_bb);
    builder_.CreateCall(runtime_gpu_scatter_check_fail_,
        {builder_.CreateGlobalString("scatter index out of bounds", "st_msg1")});
    builder_.CreateUnreachable();
    // 重复检测：mark[j] != 0 → dup_fail_bb；否则标记后继续
    builder_.SetInsertPoint(dup_check_bb);
    auto* mv = builder_.CreateLoad(i8, builder_.CreateGEP(i8, mark, j), "chk_m");
    auto* is_dup = builder_.CreateICmpNE(mv, llvm::ConstantInt::get(i8, 0));
    builder_.CreateCondBr(is_dup, dup_fail_bb, ok_bb);
    builder_.SetInsertPoint(ok_bb);
    builder_.CreateStore(llvm::ConstantInt::get(i8, 1),
        builder_.CreateGEP(i8, mark, j));
    auto* iv3 = builder_.CreateLoad(i64, i_a);
    builder_.CreateStore(builder_.CreateAdd(iv3, llvm::ConstantInt::get(i64, 1)), i_a);
    builder_.CreateBr(loop_bb);
    // 重复 fail（noreturn）
    builder_.SetInsertPoint(dup_fail_bb);
    builder_.CreateCall(runtime_gpu_scatter_check_fail_,
        {builder_.CreateGlobalString("scatter index duplicate", "st_msg2")});
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(lend);
    builder_.CreateCall(runtime_release_, {mark});
}

// §8.4 @gpu scatter：b[idx[lo_i+p]] = a[lo_a+p]（p ∈ [0,n)，n = 两区间公共长度，
// 运行时校验 n_a == n_i）。
// GPU：H2D a 范围 + H2D idx 范围 + H2D 整块 b（保留未写槽）→ unique 预扫（host）
// → grid-stride 写/原子 kernel → D2H 整块 b。CPU 回退：unique 预扫 → 顺序写/累加。
void CodeGen::generateGpuScatter(const GpuScatterStmt& s) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    auto* ptr = llvm::PointerType::get(ctx_, 0);
    llvm::Type* elem_ty = nullptr;
    auto eit = array_elem_types_.find(s.a_name);
    if (eit != array_elem_types_.end()) elem_ty = eit->second;
    if (!elem_ty) { diag_.error(s.range, "'@gpu scatter' cannot determine element type"); return; }
    uint64_t esz = module_->getDataLayout().getTypeAllocSize(elem_ty);
    int block_size = s.block_val > 0 ? (int)s.block_val : 256;
    bool atomic_add = (s.mode == 2);
    bool unique = (s.mode == 1);

    auto* lo_a = generateExpr(*s.a_begin);
    if (lo_a->getType() != i64) lo_a = builder_.CreateSExtOrTrunc(lo_a, i64);
    auto* hi_a = generateExpr(*s.a_end);
    if (hi_a->getType() != i64) hi_a = builder_.CreateSExtOrTrunc(hi_a, i64);
    auto* n_a = builder_.CreateSub(hi_a, lo_a, "sta_n");
    auto* lo_i = generateExpr(*s.idx_begin);
    if (lo_i->getType() != i64) lo_i = builder_.CreateSExtOrTrunc(lo_i, i64);
    auto* hi_i = generateExpr(*s.idx_end);
    if (hi_i->getType() != i64) hi_i = builder_.CreateSExtOrTrunc(hi_i, i64);
    auto* n_i = builder_.CreateSub(hi_i, lo_i, "sti_n");

    auto* func = builder_.GetInsertBlock()->getParent();
    // 运行时校验：n_a == n_i（契约，违约 → 报错退出）
    auto* len_ok_bb = llvm::BasicBlock::Create(ctx_, "st_len_ok", func);
    auto* len_fail_bb = llvm::BasicBlock::Create(ctx_, "st_len_fail", func);
    builder_.CreateCondBr(builder_.CreateICmpEQ(n_a, n_i), len_ok_bb, len_fail_bb);
    builder_.SetInsertPoint(len_fail_bb);
    builder_.CreateCall(runtime_gpu_scatter_check_fail_,
        {builder_.CreateGlobalString("a and idx range lengths differ", "st_lmsg")});
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(len_ok_bb);
    auto* n = n_a;
    auto* done_bb = llvm::BasicBlock::Create(ctx_, "st_done", func);
    // §8.8 空输入 n<=0：scatter 无元素，直接结束（b 不变）。
    auto* nz_bb = llvm::BasicBlock::Create(ctx_, "st_nz", func);
    auto* empty_bb = llvm::BasicBlock::Create(ctx_, "st_empty", func);
    builder_.CreateCondBr(builder_.CreateICmpSLE(n, llvm::ConstantInt::get(i64, 0)),
                          empty_bb, nz_bb);
    builder_.SetInsertPoint(empty_bb);
    builder_.CreateBr(done_bb);
    builder_.SetInsertPoint(nz_bb);
    auto* blocks = builder_.CreateUDiv(
        builder_.CreateAdd(n, llvm::ConstantInt::get(i64, block_size - 1)),
        llvm::ConstantInt::get(i64, block_size), "stblk");

    auto* gpu_bb = llvm::BasicBlock::Create(ctx_, "st_gpu", func);
    auto* cpu_bb = llvm::BasicBlock::Create(ctx_, "st_cpu", func);
    auto* gpu_ok = builder_.CreateCall(runtime_gpu_init_, {}, "gpu_ok");
    auto* gpu_ok_i1 = builder_.CreateICmpNE(gpu_ok, llvm::ConstantInt::get(i32, 0));
    builder_.CreateCondBr(gpu_ok_i1, gpu_bb, cpu_bb);

    // ============ GPU path ============
    builder_.SetInsertPoint(gpu_bb);
    std::string kname = atomic_add ? "myp_scatter_add" : "myp_scatter";
    std::string ptx_str = emitScatterPtx(elem_ty, block_size, atomic_add, kname);
    if (ptx_str.empty()) {
        diag_.warn(s.range, "'@gpu scatter' GPU kernel generation failed, running on CPU");
        builder_.CreateBr(cpu_bb);
        builder_.SetInsertPoint(cpu_bb);
        // CPU：unique 预扫 + 顺序写/累加
        auto* a_hc = builder_.CreateLoad(ptr, getNamedValue(s.a_name), s.a_name);
        auto* a_hc_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), a_hc,
            builder_.CreateMul(lo_a, llvm::ConstantInt::get(i64, esz)), "stahc");
        auto* idx_hc = builder_.CreateLoad(ptr, getNamedValue(s.idx_name), s.idx_name);
        auto* idx_hc_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), idx_hc,
            builder_.CreateMul(lo_i, llvm::ConstantInt::get(i64, 4)), "stihc");
        auto* b_hc = builder_.CreateLoad(ptr, getNamedValue(s.b_name), s.b_name);
        if (unique) emitScatterIdxCheck(idx_hc_off, n, b_hc);
        emitSeqScatter(a_hc_off, idx_hc_off, b_hc, n, elem_ty, atomic_add);
        builder_.CreateBr(done_bb);
        builder_.SetInsertPoint(done_bb);
        return;
    }
    auto* ptx_global = builder_.CreateGlobalString(ptx_str, "__myp_ptx_scatter");
    auto* kctx = builder_.CreateCall(runtime_gpu_load_kernel_,
        {ptx_global, builder_.CreateGlobalString(kname, "kn")}, "st_k");
    auto* k_ok = builder_.CreateICmpNE(kctx,
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)), "k_ok");
    auto* launch_bb = llvm::BasicBlock::Create(ctx_, "st_launch", func);
    auto* gpu_done_bb = llvm::BasicBlock::Create(ctx_, "st_gpu_done", func);
    builder_.CreateCondBr(k_ok, launch_bb, cpu_bb);

    builder_.SetInsertPoint(launch_bb);
    // host 指针
    auto* a_va = getNamedValue(s.a_name);
    auto* a_h = builder_.CreateLoad(ptr, a_va, s.a_name);
    auto* a_h_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), a_h,
        builder_.CreateMul(lo_a, llvm::ConstantInt::get(i64, esz)), "stah");
    auto* idx_va = getNamedValue(s.idx_name);
    auto* idx_h = builder_.CreateLoad(ptr, idx_va, s.idx_name);
    auto* idx_h_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), idx_h,
        builder_.CreateMul(lo_i, llvm::ConstantInt::get(i64, 4)), "stih");
    auto* b_va = getNamedValue(s.b_name);
    auto* b_h = builder_.CreateLoad(ptr, b_va, s.b_name);
    // unique 预扫（host，kernel 前）
    if (unique) emitScatterIdxCheck(idx_h_off, n, b_h);
    // len_b = backing count（b_h - 24）
    auto* len_b = builder_.CreateLoad(i64,
        builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), b_h,
            llvm::ConstantInt::get(i64, -24)), "st_blen");
    auto* b_nbytes = builder_.CreateMul(len_b, llvm::ConstantInt::get(i64, esz));
    // H2D
    auto* nbytes = builder_.CreateMul(n, llvm::ConstantInt::get(i64, esz));
    auto* a_dev = builder_.CreateCall(runtime_gpu_alloc_, {nbytes}, "st_ad");
    builder_.CreateCall(runtime_gpu_to_device_, {a_dev, a_h_off, nbytes});
    auto* inbytes = builder_.CreateMul(n, llvm::ConstantInt::get(i64, 4));
    auto* idx_dev = builder_.CreateCall(runtime_gpu_alloc_, {inbytes}, "st_id");
    builder_.CreateCall(runtime_gpu_to_device_, {idx_dev, idx_h_off, inbytes});
    auto* b_dev = builder_.CreateCall(runtime_gpu_alloc_, {b_nbytes}, "st_bd");
    builder_.CreateCall(runtime_gpu_to_device_, {b_dev, b_h, b_nbytes});
    // args: n, a_dev, idx_dev, b_dev（void** 约定）
    const unsigned nargs = 4;
    auto* args_a = builder_.CreateAlloca(ptr, llvm::ConstantInt::get(i32, nargs), "st_args");
    auto* t0 = builder_.CreateAlloca(i64);
    builder_.CreateStore(n, t0);
    builder_.CreateStore(builder_.CreateBitCast(t0, ptr),
        builder_.CreateGEP(ptr, args_a, llvm::ConstantInt::get(i32, 0)));
    auto* a_tmp = builder_.CreateAlloca(ptr);
    builder_.CreateStore(a_dev, a_tmp);
    builder_.CreateStore(builder_.CreateBitCast(a_tmp, ptr),
        builder_.CreateGEP(ptr, args_a, llvm::ConstantInt::get(i32, 1)));
    auto* i_tmp = builder_.CreateAlloca(ptr);
    builder_.CreateStore(idx_dev, i_tmp);
    builder_.CreateStore(builder_.CreateBitCast(i_tmp, ptr),
        builder_.CreateGEP(ptr, args_a, llvm::ConstantInt::get(i32, 2)));
    auto* b_tmp = builder_.CreateAlloca(ptr);
    builder_.CreateStore(b_dev, b_tmp);
    builder_.CreateStore(builder_.CreateBitCast(b_tmp, ptr),
        builder_.CreateGEP(ptr, args_a, llvm::ConstantInt::get(i32, 3)));
    builder_.CreateCall(runtime_gpu_launch_,
        {kctx, builder_.CreateIntCast(blocks, i32, false),
         llvm::ConstantInt::get(i32, block_size),
         builder_.CreateBitCast(args_a, ptr),
         llvm::ConstantInt::get(i32, nargs),
         llvm::ConstantInt::get(i64, 0)});
    // D2H 整块 b
    builder_.CreateCall(runtime_gpu_to_host_, {b_h, b_dev, b_nbytes});
    // 清理
    builder_.CreateCall(runtime_gpu_free_, {a_dev});
    builder_.CreateCall(runtime_gpu_free_, {idx_dev});
    builder_.CreateCall(runtime_gpu_free_, {b_dev});
    builder_.CreateCall(runtime_gpu_destroy_kernel_, {kctx});
    builder_.CreateBr(gpu_done_bb);
    builder_.SetInsertPoint(gpu_done_bb);
    builder_.CreateBr(done_bb);

    // ============ CPU fallback ============
    builder_.SetInsertPoint(cpu_bb);
    diag_.warn(s.range, "'@gpu scatter' GPU fallback — running on CPU");
    auto* a_hc2 = builder_.CreateLoad(ptr, getNamedValue(s.a_name), s.a_name);
    auto* a_hc2_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), a_hc2,
        builder_.CreateMul(lo_a, llvm::ConstantInt::get(i64, esz)), "stahc");
    auto* idx_hc2 = builder_.CreateLoad(ptr, getNamedValue(s.idx_name), s.idx_name);
    auto* idx_hc2_off = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), idx_hc2,
        builder_.CreateMul(lo_i, llvm::ConstantInt::get(i64, 4)), "stihc");
    auto* b_hc2 = builder_.CreateLoad(ptr, getNamedValue(s.b_name), s.b_name);
    if (unique) emitScatterIdxCheck(idx_hc2_off, n, b_hc2);
    emitSeqScatter(a_hc2_off, idx_hc2_off, b_hc2, n, elem_ty, atomic_add);
    builder_.CreateBr(done_bb);

    builder_.SetInsertPoint(done_bb);
}



// §4.1 @gpu stream(s)：求值 GpuStream 实例的 handle()（long 句柄）。
// 构造 `s.handle()` 调用并 generateExpr（launch 点 host 求值，同 grid 运行时
// 表达式）。返回 i64；无 stream 或失败返回 0（默认流 = 同步）。
llvm::Value* CodeGen::emitGpuStreamHandle(const Expr* stream_expr) {
    auto* i64_ty = llvm::Type::getInt64Ty(ctx_);
    if (!stream_expr) return llvm::ConstantInt::get(i64_ty, 0);
    SourceRange r = stream_expr->range;
    std::unique_ptr<Expr> obj;
    if (stream_expr->kind == ExprKind::Identifier) {
        obj = std::make_unique<IdentifierExpr>(
            static_cast<const IdentifierExpr&>(*stream_expr).name, r);
    } else {
        // sema 已保证 GpuStream 类型；非标识符形式（属性/下标）暂降级默认流。
        diag_.warn(r, "'stream(...)' expects a 'GpuStream' variable; using default stream");
        return llvm::ConstantInt::get(i64_ty, 0);
    }
    auto ma = std::make_unique<MemberAccessExpr>(std::move(obj), "handle", r);
    ma->resolved_object_class = "GpuStream";
    std::vector<std::unique_ptr<Expr>> args;
    auto call = std::make_unique<CallExpr>(std::move(ma), std::move(args), r);
    llvm::Value* h = generateExpr(*call);
    if (!h) return llvm::ConstantInt::get(i64_ty, 0);
    if (h->getType() != i64_ty) h = builder_.CreateIntCast(h, i64_ty, false);
    return h;
}

// §3.2 @gpu tile：共享内存协作 kernel（docs/gpu_library_design）。
// 共享数组 = 块内 __shared__（addrspace(3)，展平 [total x T]）；body 每个线程
// 执行一次，kernel.bx/tx/gid 定位协作职责，kernel.sync()（bar.sync 0）控制阶段。
// grid(nb) 指定块数（默认 1），block 固定 256。CPU 回退 = 单线程执行（降级）。
void CodeGen::generateGpuTile(const GpuTileStmt& s) {
#ifdef MYP_ENABLE_GPU
    gpu_amd_grid_used_ = false;
    // §6.4 AMD 留待：tile 的 __shared__ 对象发射（addrspace(3) 无初始值 + body
    // 内 alloca addrspace(5)）需 AMDGPU 专项；无硬件时先走单线程 CPU 降级。
    if (gpuTargetAmd()) {
        diag_.warn(s.range, "AMD target: '@gpu tile' shared-memory emission 留待 "
            "§6.4; single-threaded CPU fallback");
        generateGpuTileCpuFallback(s);
        return;
    }
    analyzeGpuTileCapturedVars(s);

    auto ptx_mod = std::make_unique<llvm::Module>("myp_gpu_tile", ctx_);
    ptx_mod->setTargetTriple(llvm::Triple(gpuTargetTriple()));

    auto* i64_ty = llvm::Type::getInt64Ty(ctx_);
    auto* i32_ty = llvm::Type::getInt32Ty(ctx_);
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    // §3.7 @gpu tile block(n)：块大小可调（默认 256；AMD workgroup 大小 = 该常量）
    int block_size = (s.block_val > 0) ? (int)s.block_val : 256;

    // 共享数组：addrspace(3) 展平数组 [total x T]（AMDGPU 不支持 addrspace(3)
    // 带初始值 → AMD 用 nullptr 初始值，__shared__ 本应使用前写入；NVPTX 可显式零）。
    int64_t total = 1;
    for (auto d : s.dim_vals) total *= d;
    llvm::Type* elem_llvm = getLLVMType(s.elem_type_info);
    llvm::Type* shared_ty = llvm::ArrayType::get(elem_llvm, (uint64_t)total);
    llvm::Constant* shared_init = gpuTargetAmd()
        ? nullptr : llvm::ConstantAggregateZero::get(shared_ty);
    auto* shared_gv = new llvm::GlobalVariable(*ptx_mod, shared_ty, false,
        llvm::GlobalValue::InternalLinkage,
        shared_init, "smem", nullptr,
        llvm::GlobalValue::NotThreadLocal, 3 /* addrspace(3) = __shared__ */);

    // kernel 签名：仅捕获变量（tile 无循环上界 n）
    std::vector<llvm::Type*> kernel_param_types;
    for (auto& ka : kernel_args_) kernel_param_types.push_back(ka.type);
    auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                                        kernel_param_types, false);
    auto* kernel_func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                               "myp_kernel", ptx_mod.get());
    setGpuKernelCC(kernel_func);

    std::map<std::string, llvm::Value*> kernel_vars_map;
    std::vector<llvm::Value*> kernel_arg_values;
    int arg_idx = 0;
    for (auto& ka : kernel_args_) {
        auto* arg = kernel_func->getArg(arg_idx++);
        arg->setName(ka.name);
        kernel_vars_map[ka.name] = arg;
        kernel_arg_values.push_back(arg);
    }

    // kernel body：tid = blockIdx.x*blockDim.x + threadIdx.x
    auto* entry_bb = llvm::BasicBlock::Create(ctx_, "entry", kernel_func);
    llvm::IRBuilder<> kb(entry_bb);
    auto* tid_x = kb.CreateIntCast(emitGpuThreadIdx(kb), i64_ty, false, "tid_x");
    auto* ntid = kb.CreateIntCast(emitGpuBlockDim(kb, block_size), i64_ty, false, "ntid");
    auto* ctaid = kb.CreateIntCast(emitGpuBlockIdx(kb), i64_ty, false, "ctaid");
    auto* tid_val = kb.CreateAdd(kb.CreateMul(ctaid, ntid, "bid_off"), tid_x, "tid");

    // §3.1 kernel 上下文（tile 内）：gid=bx*256+tx、bx=ctaid.x、tx=tid.x、
    // bd=ntid.x、gx=grid 块数。
    gpu_ctx_tid_x_ = tid_x;
    gpu_ctx_ntid_ = ntid;
    gpu_ctx_ctaid_ = ctaid;
    gpu_ctx_tid_ = tid_val;
    // tile 内 kernel.gx = grid 块数；grid 运行时无法传入 kernel → 置 0（conv3d
    // tiled 不用 kernel.gx）。字面量 grid 时仍可为常量。
    gpu_ctx_n_arg_ = (s.grid_val > 0)
        ? llvm::ConstantInt::get(i64_ty, s.grid_val)
        : llvm::ConstantInt::get(i64_ty, 0);

    kernel_vars_map[s.name] = shared_gv;
    gpu_shared_arrays_[s.name] = elem_llvm;

    gpu_kernel_mode_ = true;
    gpu_math_unsupported_ = false;
    gpu_math_used_ = false;
    if (s.body) emitKernelStmt(*s.body, kb, kernel_vars_map, kernel_arg_values,
                               "", tid_val);
    gpu_kernel_mode_ = false;
    gpu_shared_arrays_.clear();
    if (!kb.GetInsertBlock()->getTerminator()) kb.CreateRetVoid();

    // ---- 目标机器 + 校验 + 生成（§7.7 跨厂商：NVPTX→PTX / AMD→GCN ELF）----
    std::string err;
    ensureGpuTargetsInited();
    auto* tgt = llvm::TargetRegistry::lookupTarget(gpuTargetTriple(), err);
    auto* tm = tgt ? tgt->createTargetMachine(
        llvm::Triple(gpuTargetTriple()), gpuTargetArch(), "",
        llvm::TargetOptions{}, llvm::Reloc::PIC_)
                   : nullptr;
    if (!tm) {
        diag_.warn(SourceRange{}, "GPU target machine creation failed");
        // 无 PTX → 单线程 CPU 回退（下方共用）
    }
    if (tm) ptx_mod->setDataLayout(tm->createDataLayout());

    std::string verify_err;
    llvm::raw_string_ostream verify_os(verify_err);
    bool mod_ok = tm && !llvm::verifyModule(*ptx_mod, &verify_os);
    if (tm && !mod_ok) {
        diag_.warn(SourceRange{}, "GPU kernel verification failed: " + verify_err);
        delete tm; tm = nullptr;
    }
    if (mod_ok && gpu_math_used_ && !linkGpuLibdevice(ptx_mod.get(), &diag_)) {
        diag_.warn(s.range,
            "'@gpu tile' uses libdevice math but libdevice.10.bc was not found; "
            "running single-threaded on CPU");
        delete tm; tm = nullptr;
    }

    std::string ptx_str;
    if (tm) {
        unsigned kopt = 2;
        if (const char* env = getenv("MYP_GPU_KERNEL_OPT")) {
            std::string want(env);
            if (want == "O0") kopt = 0;
            else if (want == "O1") kopt = 1;
            else if (want == "O2") kopt = 2;
            else if (want == "O3") kopt = 3;
        }
        tm->setOptLevel(kopt >= 3 ? llvm::CodeGenOptLevel::Aggressive :
                       kopt == 2 ? llvm::CodeGenOptLevel::Default :
                       kopt == 1 ? llvm::CodeGenOptLevel::Less :
                                   llvm::CodeGenOptLevel::None);
        if (kopt > 0) {
            llvm::OptimizationLevel OL =
                kopt >= 3 ? llvm::OptimizationLevel::O3 :
                kopt == 2 ? llvm::OptimizationLevel::O2 :
                            llvm::OptimizationLevel::O1;
            llvm::LoopAnalysisManager LAM;
            llvm::FunctionAnalysisManager FAM;
            llvm::CGSCCAnalysisManager CGAM;
            llvm::ModuleAnalysisManager MAM;
            llvm::PassBuilder PB(tm);
            PB.registerModuleAnalyses(MAM);
            PB.registerCGSCCAnalyses(CGAM);
            PB.registerFunctionAnalyses(FAM);
            PB.registerLoopAnalyses(LAM);
            PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
            llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OL);
            MPM.addPass(llvm::VerifierPass());
            MPM.run(*ptx_mod, MAM);
        }
        // §7.7：AMD 目标 → GCN ELF 写文件，返回空 → 下方 CPU 降级
        ptx_str = emitGpuTargetBytes(ptx_mod.get(), tm, "tile");
    }

    // MYP_DUMP_PTX：临时调试——打印生成的 kernel PTX。
    if (!ptx_str.empty() && getenv("MYP_DUMP_PTX")) {
        fprintf(stderr, "=== MYP PTX (tile) ===\n%s\n=== END ===\n", ptx_str.c_str());
    }

    // 无可用 PTX → 直接单线程 CPU 执行（降级）
    if (ptx_str.empty()) {
        diag_.warn(s.range, "'@gpu tile' running single-threaded on CPU (no PTX)");
        generateGpuTileCpuFallback(s);
        return;
    }

    // Embed PTX as a global string constant in the main module
    auto* ptx_global = builder_.CreateGlobalString(ptx_str, "__myp_ptx_kernel");

    // ---- Generate GPU launch + CPU fallback at the call site ----
    auto* func = builder_.GetInsertBlock()->getParent();
    auto* gpu_bb = llvm::BasicBlock::Create(ctx_, "gpu_launch", func);
    auto* cpu_bb = llvm::BasicBlock::Create(ctx_, "gpu_cpu_fallback", func);

    auto* gpu_ok = builder_.CreateCall(runtime_gpu_init_, {}, "gpu_ok");
    auto* gpu_ok_i1 = builder_.CreateICmpNE(gpu_ok,
        llvm::ConstantInt::get(i32_ty, 0), "gpu_ok_cmp");
    builder_.CreateCondBr(gpu_ok_i1, gpu_bb, cpu_bb);

    // === GPU path ===
    builder_.SetInsertPoint(gpu_bb);
    auto* kernel_ctx = builder_.CreateCall(runtime_gpu_load_kernel_,
        {ptx_global, builder_.CreateGlobalString("myp_kernel", "kn")}, "kernel_ctx");
    auto* kernel_ok = builder_.CreateICmpNE(kernel_ctx,
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr_ty)), "k_ok");
    auto* launch_bb = llvm::BasicBlock::Create(ctx_, "gpu_launch_run", func);
    auto* gpu_done_bb = llvm::BasicBlock::Create(ctx_, "gpu_done", func);
    // §5.2 kernel 加载失败 → 走 CPU 回退（cpu_bb），而不是静默跳过 gpu_done_bb
    // （旧行为：load_kernel 失败时什么都不做，捕获数组保持未初始化 → 结果错）。
    builder_.CreateCondBr(kernel_ok, launch_bb, cpu_bb);

    builder_.SetInsertPoint(launch_bb);
    // grid 维度：字面量 → 常量；运行时表达式 → host 求值（如 conv3d 的 nTiles）
    llvm::Value* grid_v;
    if (s.has_grid && s.grid_val <= 0) {
        grid_v = generateExpr(*s.grid_expr);
    } else {
        grid_v = llvm::ConstantInt::get(i64_ty, s.grid_val);
    }
    if (grid_v->getType() != i64_ty)
        grid_v = builder_.CreateIntCast(grid_v, i64_ty, false);
    auto* grid_i32 = builder_.CreateIntCast(grid_v, i32_ty, false, "grid");
    // §3.7 @gpu tile block(n)：块大小可调（默认 256；已 hoist 到函数顶部）
    auto* block_i32 = llvm::ConstantInt::get(i32_ty, block_size);

    // ---- Data transfer for captured arrays ----
    struct ArrayAlloc {
        llvm::Value* gpu_ptr;
        llvm::Value* host_ptr;
        llvm::Value* byte_size;
        bool needs_copyback;
        std::string name;
    };
    std::vector<ArrayAlloc> array_allocs;

    // M3 设备驻留：resident(arr = devVar) — 被标记数组跳过 H2D/D2H/释放，
    // kernel 直接用 devVar（long）所持设备指针（同 generateGpuKernel）。
    std::map<std::string, std::string> resident_dev;
    for (auto& [arr, dev] : s.resident)
        resident_dev[arr] = dev;

    for (auto& ka : kernel_args_) {
        if (!ka.is_array) continue;
        if (resident_dev.count(ka.name)) continue;  // M3: 设备驻留，跳过传输
        llvm::Value* host_ptr = nullptr;
        llvm::Value* byte_size = nullptr;
        auto* nv = getNamedValue(ka.name);
        if (nv) {
            auto* loaded = builder_.CreateLoad(ptr_ty, nv, ka.name);
            host_ptr = loaded;
            auto bsit = array_byte_sizes_.find(ka.name);
            bool bs_valid = false;
            if (bsit != array_byte_sizes_.end())
                bs_valid = (bsit->second.first == func);
            if (bs_valid) {
                byte_size = bsit->second.second;
            } else {
                auto eit = array_elem_types_.find(ka.name);
                uint64_t elem_sz = 8;
                if (eit != array_elem_types_.end()) {
                    if (eit->second->isDoubleTy()) elem_sz = 8;
                    else if (eit->second->isFloatTy()) elem_sz = 4;
                    else if (eit->second->isIntegerTy(32)) elem_sz = 4;
                }
                auto* i8_ty = llvm::Type::getInt8Ty(ctx_);
                auto* hdr = builder_.CreateGEP(i8_ty, loaded,
                    llvm::ConstantInt::get(i64_ty, -(int64_t)24), ka.name + "_hdr");
                auto* cnt_ptr = builder_.CreateBitCast(hdr, llvm::PointerType::get(i64_ty, 0));
                auto* cnt = builder_.CreateLoad(i64_ty, cnt_ptr, ka.name + "_cnt");
                byte_size = builder_.CreateMul(cnt, llvm::ConstantInt::get(i64_ty, elem_sz),
                                               ka.name + "_sz");
            }
        }
        if (host_ptr && byte_size) {
            auto* gpu_ptr = builder_.CreateCall(runtime_gpu_alloc_, {byte_size},
                                                 ka.name + "_gpu");
            builder_.CreateCall(runtime_gpu_to_device_, {gpu_ptr, host_ptr, byte_size});
            array_allocs.push_back({gpu_ptr, host_ptr, byte_size, true, ka.name});
        }
    }

    // ---- Build kernel launch args ----
    int total_args = (int)kernel_args_.size();
    auto* args_arr = builder_.CreateAlloca(ptr_ty,
        llvm::ConstantInt::get(i32_ty, total_args), "gpu_args");
    std::map<std::string, llvm::Value*> gpu_ptr_map;
    for (auto& aa : array_allocs) gpu_ptr_map[aa.name] = aa.gpu_ptr;

    int arg_pos = 0;
    for (auto& ka : kernel_args_) {
        llvm::Value* store_val = nullptr;
        if (ka.is_array) {
            auto rdit = resident_dev.find(ka.name);
            if (rdit != resident_dev.end()) {
                // M3 设备驻留：设备指针来自 devVar（long，i64）的值
                auto* nv = getNamedValue(rdit->second);
                if (nv) {
                    auto* dev_val = builder_.CreateLoad(builder_.getInt64Ty(), nv,
                                                        ka.name + "_dev");
                    auto* dev_ptr = builder_.CreateIntToPtr(dev_val, ptr_ty, ka.name + "_devptr");
                    auto* tmp = builder_.CreateAlloca(ptr_ty);
                    builder_.CreateStore(dev_ptr, tmp);
                    store_val = tmp;
                }
            } else {
                auto gpit = gpu_ptr_map.find(ka.name);
                if (gpit != gpu_ptr_map.end()) {
                    auto* tmp = builder_.CreateAlloca(ptr_ty);
                    builder_.CreateStore(gpit->second, tmp);
                    store_val = tmp;
                }
            }
        } else {
            auto* nv = getNamedValue(ka.name);
            if (nv) {
                auto* val = builder_.CreateLoad(ka.type, nv, ka.name);
                auto* tmp = builder_.CreateAlloca(ka.type);
                builder_.CreateStore(val, tmp);
                store_val = tmp;
            }
        }
        if (!store_val) {
            auto* tmp = builder_.CreateAlloca(ka.type);
            builder_.CreateStore(llvm::ConstantInt::get(i64_ty, 0), tmp);
            store_val = tmp;
        }
        llvm::Value* idxs[] = { llvm::ConstantInt::get(i32_ty, arg_pos) };
        auto* ap = builder_.CreateGEP(ptr_ty, args_arr, idxs);
        builder_.CreateStore(builder_.CreateBitCast(store_val, ptr_ty), ap);
        arg_pos++;
    }

    // §4.1 @gpu stream(s)：stream 句柄（0 = 默认流同步）。
    llvm::Value* stream_h = s.has_stream
        ? emitGpuStreamHandle(s.stream_expr.get())
        : llvm::ConstantInt::get(i64_ty, 0);
    // Launch kernel
    builder_.CreateCall(runtime_gpu_launch_,
        {kernel_ctx, grid_i32, block_i32, builder_.CreateBitCast(args_arr, ptr_ty),
         llvm::ConstantInt::get(i32_ty, total_args), stream_h});

    // ---- Copy back results ----
    for (auto& aa : array_allocs) {
        if (aa.needs_copyback) {
            if (s.has_stream) {
                // stream 模式：异步 D2H 排队到同流（须 s.sync() 后取回）
                builder_.CreateCall(runtime_gpu_to_host_async_,
                    {aa.host_ptr, aa.gpu_ptr, aa.byte_size, stream_h});
            } else {
                builder_.CreateCall(runtime_gpu_to_host_,
                    {aa.host_ptr, aa.gpu_ptr, aa.byte_size});
            }
        }
        builder_.CreateCall(runtime_gpu_free_, {aa.gpu_ptr});
    }
    builder_.CreateCall(runtime_gpu_destroy_kernel_, {kernel_ctx});
    builder_.CreateBr(gpu_done_bb);

    // === CPU fallback（单线程降级）===
    builder_.SetInsertPoint(cpu_bb);
    generateGpuTileCpuFallback(s);
    builder_.CreateBr(gpu_done_bb);

    builder_.SetInsertPoint(gpu_done_bb);

#ifdef MYP_CUDA_ENABLED
    cuda_enabled_ = true;
#endif

    diag_.warn(s.range, "'@gpu tile' PTX kernel generated (" +
               std::to_string(ptx_str.size()) + " bytes)");
#else
    (void)s;
    diag_.warn(s.range, "'@gpu tile' GPU offload not built (MYP_ENABLE_GPU=OFF); "
               "running single-threaded on CPU");
    generateGpuTileCpuFallback(s);
#endif
}

// §3.2/§8.5 CPU 回退（降级）：忽略 tiling，退化为顺序嵌套循环（§8.5）。
// 共享数组 → host 栈数组；协作载入 + kernel.sync() → 顺序赋值 + 空操作；
// body 遍历展平线程网格 p ∈ [0, grid*block) 每线程执行一次：
// kernel.gid=p、bx=p/bd、tx=p%bd、bd=block_size、gx=grid 块数。
// 每线程只读写自己 smem 槽的 tile（协作载入 → 本线程写 → 读回）→ GPU/CPU 产出
// 相同（"语义不变的降级"）。
void CodeGen::generateGpuTileCpuFallback(const GpuTileStmt& s) {
    diag_.warn(s.range, "'@gpu tile' GPU fallback — single-threaded CPU execution");
    auto* i64_ty = llvm::Type::getInt64Ty(ctx_);
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    int64_t total = 1;
    for (auto d : s.dim_vals) total *= d;
    llvm::Type* elem_llvm = getLLVMType(s.elem_type_info);
    // §8.5 降级 1：smem 声明退化为普通局部数组（host 栈数组）。body 里 smem[i]
    // 走标准数组 GEP/load/store 路径（指针变量 + array_elem_types_ 注解）。
    auto* storage = builder_.CreateAlloca(elem_llvm,
        llvm::ConstantInt::get(i64_ty, total), s.name + "_stor");
    auto* st_ptr = builder_.CreateBitCast(storage,
        llvm::PointerType::get(elem_llvm, 0), s.name + "_p");
    auto* pv = builder_.CreateAlloca(ptr_ty, nullptr, s.name);
    builder_.CreateStore(st_ptr, pv);
    named_values_.emplace_back();
    named_values_.back()[s.name] = pv;
    array_elem_types_[s.name] = elem_llvm;
    // §8.5 降级 2/3：顺序循环遍历展平线程网格（kernel.sync() 为空操作）。
    int block_size = (s.block_val > 0) ? (int)s.block_val : 256;
    llvm::Value* total_th;
    if (s.has_grid && s.grid_val <= 0) {
        // 运行时 grid 表达式（如 conv3d 的 nTiles）：host 求值
        auto* gv = generateExpr(*s.grid_expr);
        if (gv->getType() != i64_ty) gv = builder_.CreateIntCast(gv, i64_ty, false);
        total_th = builder_.CreateMul(gv, llvm::ConstantInt::get(i64_ty, block_size));
    } else {
        int64_t grid = s.grid_val > 0 ? s.grid_val : 1;
        total_th = llvm::ConstantInt::get(i64_ty, grid * block_size);
    }
    auto* p_a = createEntryBlockAlloca(current_function_, i64_ty, "__tile_p");
    builder_.CreateStore(llvm::ConstantInt::get(i64_ty, 0), p_a);
    gpu_cpu_fallback_ = true;
    gpu_cpu_loop_var_ = "__tile_p";
    gpu_cpu_bound_ = total_th;
    gpu_cpu_block_ = block_size;   // §3.7 block(n)
    named_values_.back()["__tile_p"] = p_a;
    auto* loop_bb = llvm::BasicBlock::Create(ctx_, "tile_loop", current_function_);
    auto* lbody = llvm::BasicBlock::Create(ctx_, "tile_body", current_function_);
    auto* lend = llvm::BasicBlock::Create(ctx_, "tile_end", current_function_);
    builder_.CreateBr(loop_bb);
    builder_.SetInsertPoint(loop_bb);
    auto* pv2 = builder_.CreateLoad(i64_ty, p_a);
    builder_.CreateCondBr(builder_.CreateICmpSLT(pv2, total_th), lbody, lend);
    builder_.SetInsertPoint(lbody);
    if (s.body) generateStmt(*s.body);
    auto* pv3 = builder_.CreateLoad(i64_ty, p_a);
    builder_.CreateStore(builder_.CreateAdd(pv3, llvm::ConstantInt::get(i64_ty, 1)), p_a);
    builder_.CreateBr(loop_bb);
    builder_.SetInsertPoint(lend);
    gpu_cpu_fallback_ = false;
    gpu_cpu_bound_ = nullptr;
    gpu_cpu_loop_var_.clear();
    named_values_.back().erase("__tile_p");
    named_values_.pop_back();
}

static std::string findLibDevicePath() {
    const char* env = getenv("MYP_CUDA_LIBDEVICE");
    if (env && *env) {
        if (llvm::sys::fs::exists(env)) return std::string(env);
    }
    static const char* kPaths[] = {
        "/usr/lib/nvidia-cuda-toolkit/libdevice/libdevice.10.bc",
        "/usr/local/cuda/nvvm/libdevice/libdevice.10.bc",
        "/usr/local/cuda/libdevice/libdevice.10.bc",
        "/opt/cuda/nvvm/libdevice/libdevice.10.bc",
        "/usr/lib/cuda/nvvm/libdevice/libdevice.10.bc",
        "/usr/lib/x86_64-linux-gnu/nvidia-cuda-toolkit/libdevice/libdevice.10.bc",
        "/usr/local/cuda-12/nvvm/libdevice/libdevice.10.bc",
        "/usr/local/cuda-11/nvvm/libdevice/libdevice.10.bc",
    };
    for (const char* p : kPaths) {
        if (llvm::sys::fs::exists(p)) return std::string(p);
    }
    return "";
}

static void resolveNvvmReflect(llvm::Module* M) {
    llvm::Function* reflect = M->getFunction("__nvvm_reflect");
    if (!reflect) return;
    std::vector<llvm::CallInst*> calls;
    for (llvm::User* u : reflect->users())
        if (auto* ci = llvm::dyn_cast<llvm::CallInst>(u))
            calls.push_back(ci);
    auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(M->getContext()), 0);
    for (auto* ci : calls) {
        ci->replaceAllUsesWith(zero);
        ci->eraseFromParent();
    }
    if (reflect->use_empty())
        reflect->eraseFromParent();
}

static bool linkGpuLibdevice(llvm::Module* ptx_mod, DiagnosticEngine* diag) {
    std::string ld_path = findLibDevicePath();
    if (ld_path.empty()) {
        if (diag)
            diag->warn(SourceRange{},
                "@gpu for uses libdevice math but libdevice.10.bc was not found "
                "(set MYP_CUDA_LIBDEVICE)");
        return false;
    }
    llvm::SMDiagnostic sm_err;
    auto ld_mod = llvm::parseIRFile(ld_path, sm_err, ptx_mod->getContext());
    if (!ld_mod) {
        if (diag)
            diag->warn(SourceRange{}, "@gpu for: failed to parse libdevice: " + ld_path);
        return false;
    }
    if (llvm::Linker::linkModules(*ptx_mod, std::move(ld_mod))) {
        if (diag)
            diag->warn(SourceRange{}, "@gpu for: failed to link libdevice: " + ld_path);
        return false;
    }
    // libdevice functions are linkonce_odr; make them internal so unused ones
    // are eliminated by GlobalDCE (keeps the generated PTX small).
    for (auto& fn : *ptx_mod) {
        if (!fn.isDeclaration() && fn.getLinkage() == llvm::GlobalValue::LinkOnceODRLinkage)
            fn.setLinkage(llvm::GlobalValue::InternalLinkage);
    }
    for (auto& gv : ptx_mod->globals()) {
        if (gv.getLinkage() == llvm::GlobalValue::LinkOnceODRLinkage)
            gv.setLinkage(llvm::GlobalValue::InternalLinkage);
    }
    resolveNvvmReflect(ptx_mod);

    // Inline always-inline (libdevice) functions and drop dead code.
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    llvm::PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
    llvm::ModulePassManager MPM;
    MPM.addPass(llvm::AlwaysInlinerPass());
    MPM.addPass(llvm::GlobalDCEPass());
    MPM.run(*ptx_mod, MAM);
    return true;
}

// §P5 ② kernel.printk / kernel.assert：kernel 内把记录写入设备 staging 全局。
//
// 记录布局（7×i64=56B，myp_pbuf 平铺 [MAXREC*7 x i64]）：
//   [0] type（0=printk 1=assert） [1] fmt_id [2] gid（线性线程号）
//   [3..5] a0..a2（int=zext i64；double/float=位型 i64） [6] mask（bit_i=1 → a_i 为 double）
// 槽位由 myp_pcnt（原子计数器，atomicrmw add）领取；assert 失败同时置 myp_pfail。
// 宿主 myp_gpu_flush_printf 在 launch 后回读（见 generateGpuKernel launch 尾）。
llvm::Value* CodeGen::emitKernelPrintk(const CallExpr& e, llvm::IRBuilder<>& kb,
    std::map<std::string, llvm::Value*>& kernel_vars,
    const std::vector<llvm::Value*>& kernel_arg_values,
    const std::string& loop_var_name, llvm::Value* tid_val, bool is_assert) {
    auto* i64_ty = llvm::Type::getInt64Ty(ctx_);
    auto* i32_ty = llvm::Type::getInt32Ty(ctx_);
    auto* i1_ty = llvm::Type::getInt1Ty(ctx_);
    auto* double_ty = llvm::Type::getDoubleTy(ctx_);
    constexpr unsigned kMax = 1024;

    // 格式串（StringLiteralExpr）→ fmt_id（本 kernel 内唯一）
    int val_start = is_assert ? 2 : 1;   // 值参起点（assert: cond, fmt, v...；printk: fmt, v...）
    std::string fmt = "(fmt?)";
    if ((size_t)val_start - 1 < e.args.size() &&
        e.args[val_start - 1]->kind == ExprKind::StringLiteral) {
        fmt = static_cast<const StringLiteralExpr&>(*e.args[val_start - 1]).value;
    }
    int fid = 0;
    auto fit = gpu_kernel_fmt_id_.find(fmt);
    if (fit != gpu_kernel_fmt_id_.end()) {
        fid = fit->second;
    } else {
        fid = (int)gpu_kernel_fmts_.size();
        gpu_kernel_fmts_.push_back(fmt);
        gpu_kernel_fmt_id_[fmt] = fid;
    }
    gpu_kernel_printf_used_ = true;

    // §P5 ② 设备指针来自 kernel 末尾 3 个 i64 参数（宿主 launch 时传入；runtime
    // 分配缓冲/计数器）。inttoptr 成 addrspace(1) 指针——避免模块全局被 O2
    // GlobalDCE 删除，也避免 cuModuleGetGlobal 的上下文 TLS 依赖。
    auto getarg = [&](const char* name) -> llvm::Value* {
        auto it = kernel_vars.find(name);
        if (it != kernel_vars.end()) return it->second;   // i64 参数值
        return llvm::ConstantInt::get(i64_ty, 0);
    };
    auto* asp1 = llvm::PointerType::get(ctx_, 1);
    auto* pbuf = kb.CreateIntToPtr(getarg("myp_pbuf"), asp1, "pf_pbuf");
    auto* pcnt = kb.CreateIntToPtr(getarg("myp_pcnt"), asp1, "pf_pcnt");
    auto* pfail = kb.CreateIntToPtr(getarg("myp_pfail"), asp1, "pf_pfail");

    // 写入条件：printk → 恒真；assert → !cond
    llvm::Value* do_write = llvm::ConstantInt::get(i1_ty, 1);
    if (is_assert) {
        auto* cond = emitKernelExpr(*e.args[0], kb, kernel_vars, kernel_arg_values,
                                    loop_var_name, tid_val);
        auto* c0 = kb.CreateICmpEQ(cond, llvm::ConstantInt::get(cond->getType(), 0), "asrt0");
        do_write = c0;   // 断言失败（cond==0）时写记录
    }

    // 仅 do_write 才领槽（atomicrmw）——否则 assert 通过的线程也消耗槽位，
    // 计数器与记录数不符（flush 读到上一 kernel 的残留记录）。
    auto* claim_bb = llvm::BasicBlock::Create(ctx_, "pf_claim",
        kb.GetInsertBlock()->getParent());
    auto* write_bb = llvm::BasicBlock::Create(ctx_, "pf_write",
        kb.GetInsertBlock()->getParent());
    auto* skip_bb = llvm::BasicBlock::Create(ctx_, "pf_skip",
        kb.GetInsertBlock()->getParent());
    kb.CreateCondBr(do_write, claim_bb, skip_bb);

    kb.SetInsertPoint(claim_bb);
    // slot = atomicrmw add pcnt, 1（NVPTX atom.add.u64，sm_60+ 支持）
    auto* slot = kb.CreateAtomicRMW(llvm::AtomicRMWInst::Add, pcnt,
        llvm::ConstantInt::get(i64_ty, 1), llvm::MaybeAlign(),
        llvm::AtomicOrdering::SequentiallyConsistent);
    auto* in_range = kb.CreateICmpULT(slot, llvm::ConstantInt::get(i64_ty, kMax));
    kb.CreateCondBr(in_range, write_bb, skip_bb);

    kb.SetInsertPoint(write_bb);
    // pbuf 平铺 [kMax*7 x i64]；按 slot*7 单索引定位记录基址。
    auto* rec = kb.CreateInBoundsGEP(i64_ty, pbuf,
        kb.CreateMul(slot, llvm::ConstantInt::get(i64_ty, 7), "pf_base"), "pf_rec");
    auto st = [&](int field, llvm::Value* val) {
        kb.CreateStore(val, kb.CreateInBoundsGEP(i64_ty, rec,
            llvm::ConstantInt::get(i32_ty, field)));
    };
    st(0, llvm::ConstantInt::get(i64_ty, is_assert ? 1 : 0));      // type
    st(1, llvm::ConstantInt::get(i64_ty, fid));                    // fmt_id
    st(2, kb.CreateIntCast(tid_val, i64_ty, false, "pf_gid"));     // gid
    long long mask = 0;
    int nval = (int)e.args.size() - val_start;
    if (nval > 3) nval = 3;
    for (int i = 0; i < 3; i++) {
        llvm::Value* v = llvm::ConstantInt::get(i64_ty, 0);
        if (i < nval) {
            auto* av = emitKernelExpr(*e.args[val_start + i], kb, kernel_vars,
                                      kernel_arg_values, loop_var_name, tid_val);
            if (!av) av = llvm::ConstantInt::get(i64_ty, 0);
            if (av->getType()->isDoubleTy()) {
                v = kb.CreateBitCast(av, i64_ty, "pf_d");
                mask |= (1LL << i);
            } else if (av->getType()->isFloatTy()) {
                auto* d = kb.CreateFPExt(av, double_ty, "pf_f2d");
                v = kb.CreateBitCast(d, i64_ty, "pf_d");
                mask |= (1LL << i);
            } else if (av->getType()->isIntegerTy(32)) {
                v = kb.CreateZExt(av, i64_ty, "pf_i");
            } else if (av->getType()->isIntegerTy(64)) {
                v = av;
            } else if (av->getType()->isIntegerTy(1)) {
                v = kb.CreateZExt(av, i64_ty, "pf_b");
            } else {
                v = llvm::ConstantInt::get(i64_ty, 0);
            }
        }
        st(3 + i, v);
    }
    st(6, llvm::ConstantInt::get(i64_ty, mask));
    if (is_assert)
        kb.CreateStore(llvm::ConstantInt::get(i64_ty, 1), pfail);
    kb.CreateBr(skip_bb);
    kb.SetInsertPoint(skip_bb);
    return nullptr;   // void
}

bool CodeGen::generateGpuKernel(const ForStmt& s) {
#ifdef MYP_ENABLE_GPU
    gpu_amd_grid_used_ = false;
    // §P5 ② kernel printk/assert 状态（每次 kernel 重置）
    gpu_kernel_fmts_.clear();
    gpu_kernel_fmt_id_.clear();
    gpu_kernel_printf_used_ = false;
    // Create a new module for PTX generation
    auto ptx_mod = std::make_unique<llvm::Module>("myp_gpu_kernel", ctx_);
    ptx_mod->setTargetTriple(llvm::Triple(gpuTargetTriple()));

    auto* i64_ty = llvm::Type::getInt64Ty(ctx_);
    auto* i32_ty = llvm::Type::getInt32Ty(ctx_);
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    auto* double_ty = llvm::Type::getDoubleTy(ctx_);
    // §3.7 @gpu block(n)：块大小可调（默认 256；AMD workgroup 大小 = 该常量）
    int block_size = (s.block_val > 0) ? (int)s.block_val : 256;

    // Extract loop variable name from init statement
    std::string loop_var;
    if (s.init && s.init->kind == StmtKind::VarDeclStmt) {
        auto& vds = static_cast<const VarDeclStmt&>(*s.init);
        if (!vds.decls.empty())
            loop_var = vds.decls[0].name;
    }
    if (loop_var.empty()) {
        diag_.warn(SourceRange{}, "GPU for: cannot determine loop variable");
        return false;
    }

    // Extract loop bound from condition (expects i < n or i <= n pattern)
    llvm::Value* loop_bound_val = nullptr;
    if (s.condition && s.condition->kind == ExprKind::BinaryOp) {
        auto& cond = static_cast<const BinaryOpExpr&>(*s.condition);
        if (cond.op == BinaryOpKind::Lt || cond.op == BinaryOpKind::Le ||
            cond.op == BinaryOpKind::Gt || cond.op == BinaryOpKind::Ge) {
            if (cond.lhs->kind == ExprKind::Identifier &&
                static_cast<const IdentifierExpr&>(*cond.lhs).name == loop_var) {
                loop_bound_val = generateExpr(*cond.rhs);
            } else if (cond.rhs->kind == ExprKind::Identifier &&
                       static_cast<const IdentifierExpr&>(*cond.rhs).name == loop_var) {
                loop_bound_val = generateExpr(*cond.lhs);
            }
        }
    }

    // Analyze captured variables
    analyzeGpuCapturedVars(s, loop_var);

    // Remove duplicates: if a captured var is the same as the loop bound variable
    // used in the kernel's first parameter, exclude it from args
    // (the bound variable is already passed as kernel param 0)
    std::set<std::string> bound_var_names;
    std::string bound_var_name;   // 循环边界标识符名（body 里引用它时映射到 n 参数）
    if (s.condition && s.condition->kind == ExprKind::BinaryOp) {
        auto& cond = static_cast<const BinaryOpExpr&>(*s.condition);
        if (cond.lhs->kind == ExprKind::Identifier &&
            static_cast<const IdentifierExpr&>(*cond.lhs).name != loop_var) {
            bound_var_names.insert(static_cast<const IdentifierExpr&>(*cond.lhs).name);
            bound_var_name = static_cast<const IdentifierExpr&>(*cond.lhs).name;
        } else if (cond.rhs->kind == ExprKind::Identifier &&
                   static_cast<const IdentifierExpr&>(*cond.rhs).name != loop_var) {
            bound_var_names.insert(static_cast<const IdentifierExpr&>(*cond.rhs).name);
            bound_var_name = static_cast<const IdentifierExpr&>(*cond.rhs).name;
        }
    }
    // Also exclude the loop variable itself
    bound_var_names.insert(loop_var);

    // Filter kernel_args_ to remove bound vars (they are already kernel params)
    std::vector<KernelArgInfo> filtered_args;
    for (auto& ka : kernel_args_) {
        if (bound_var_names.find(ka.name) == bound_var_names.end())
            filtered_args.push_back(ka);
    }
    kernel_args_ = filtered_args;

    // Build kernel function signature: (i64 n, captured_vars...,
    //                                    i64 myp_pbuf, i64 myp_pcnt, i64 myp_pfail)
    // §P5 ② 末尾 3 个 i64 为 printk/assert staging 设备指针（非 printk kernel 传 0）。
    // tid is NOT a parameter — computed from blockIdx/threadIdx NVVM intrinsics
    std::vector<llvm::Type*> kernel_param_types;
    kernel_param_types.push_back(i64_ty);  // n (loop bound)
    (void)ptr_ty; (void)double_ty;

    std::map<std::string, llvm::Value*> kernel_vars_map;
    std::vector<llvm::Value*> kernel_arg_values;

    for (auto& ka : kernel_args_) {
        kernel_param_types.push_back(ka.type);
    }
    // §P5 ② printk/assert staging 设备指针（i64）——非 printk kernel 宿主传 0
    kernel_param_types.push_back(i64_ty);
    kernel_param_types.push_back(i64_ty);
    kernel_param_types.push_back(i64_ty);

    auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), kernel_param_types, false);
    auto* kernel_func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                                "myp_kernel", ptx_mod.get());
    // Mark as kernel entry point（§7.7 跨厂商：NVPTX .entry / AMD amdgpu_kernel）
    setGpuKernelCC(kernel_func);

    int arg_idx = 0;
    auto* n_arg = kernel_func->getArg(arg_idx++);
    n_arg->setName("n");

    for (auto& ka : kernel_args_) {
        auto* arg = kernel_func->getArg(arg_idx++);
        arg->setName(ka.name);
        kernel_vars_map[ka.name] = arg;
        kernel_arg_values.push_back(arg);
    }

    // §P5 ② printk/assert staging 设备指针（i64 参数；非 printk kernel 由宿主传 0）
    for (const char* pn : {"myp_pbuf", "myp_pcnt", "myp_pfail"}) {
        auto* arg = kernel_func->getArg(arg_idx++);
        arg->setName(pn);
        kernel_vars_map[pn] = arg;
        kernel_arg_values.push_back(arg);
    }

    // 循环边界变量被排除在 kernel_args_ 之外（作为 param_0=n 传入）；把它的名字
    // 映射到 n 参数，使 kernel body 里对边界变量的引用（如 if (p < nTh)）正确解析。
    // 否则 body 引用边界标识符时找不到值 → 内核形同空转（此前多数组/原子归约
    // 全挂的根因：边界用 long 局部标识符时整 kernel 不执行）。
    if (!bound_var_name.empty())
        kernel_vars_map[bound_var_name] = n_arg;

    // Build kernel body
    auto* entry_bb = llvm::BasicBlock::Create(ctx_, "entry", kernel_func);
    llvm::IRBuilder<> kb(entry_bb);

    // Compute tid = blockIdx.x * blockDim.x + threadIdx.x（§7.7 跨厂商 intrinsic）
    auto* tid_x = kb.CreateIntCast(emitGpuThreadIdx(kb), i64_ty, false, "tid_x");
    auto* ntid = kb.CreateIntCast(emitGpuBlockDim(kb, block_size), i64_ty, false, "ntid");
    auto* ctaid = kb.CreateIntCast(emitGpuBlockIdx(kb), i64_ty, false, "ctaid");
    auto* bid_offset = kb.CreateMul(ctaid, ntid, "bid_off");
    auto* tid_val = kb.CreateAdd(bid_offset, tid_x, "tid");

    // §3.1 kernel 执行上下文：存当前块/线程值供 emitKernelExpr 的 kernel.xxx 读取
    gpu_ctx_tid_x_ = tid_x;
    gpu_ctx_ntid_ = ntid;
    gpu_ctx_ctaid_ = ctaid;
    gpu_ctx_tid_ = tid_val;
    gpu_ctx_n_arg_ = n_arg;

    // Generate tid < n check（@gpu stride：grid-stride 循环头）
    auto* body_bb = llvm::BasicBlock::Create(ctx_, "body", kernel_func);
    auto* end_bb = llvm::BasicBlock::Create(ctx_, "end", kernel_func);
    llvm::BasicBlock* loop_bb = nullptr;   // stride 模式循环头（body 后回跳）
    llvm::Value* loop_var_kv = tid_val;   // 普通 @gpu for：body 里循环变量 = tid
    if (s.stride) {
        // §3.5 grid-stride：i = tid；while (i < n) { body; i += ntid*nctaid; }
        auto* nctaid = kb.CreateIntCast(emitGpuGridDim(kb), i64_ty, false, "nctaid");
        auto* nthreads = kb.CreateMul(ntid, nctaid, "nthreads");
        loop_bb = llvm::BasicBlock::Create(ctx_, "loop", kernel_func);
        kb.CreateBr(loop_bb);
        kb.SetInsertPoint(loop_bb);
        auto* i_phi = kb.CreatePHI(i64_ty, 2, "i");
        i_phi->addIncoming(tid_val, entry_bb);
        auto* cond_s = kb.CreateICmpSLT(i_phi, n_arg);
        kb.CreateCondBr(cond_s, body_bb, end_bb);
        loop_var_kv = i_phi;
    } else {
        auto* cond_check = kb.CreateICmpSLT(tid_val, n_arg);
        kb.CreateCondBr(cond_check, body_bb, end_bb);
    }

    // Compile loop body into kernel
    kb.SetInsertPoint(body_bb);
    gpu_for_stmt_ = &s;  // Mark GPU kernel mode (affects emitKernelExpr)
    gpu_kernel_mode_ = true;
    gpu_math_unsupported_ = false;
    gpu_math_used_ = false;
    if (s.body) {
        emitKernelStmt(*s.body, kb, kernel_vars_map, kernel_arg_values,
                       loop_var, loop_var_kv);
    }
    gpu_for_stmt_ = nullptr;
    gpu_kernel_mode_ = false;
    if (gpu_math_unsupported_) {
        // Kernel uses transcendental math (sin/cos/tan/exp/log/pow) which needs
        // CUDA libdevice — the runtime doesn't link libdevice, so fall back to CPU.
        diag_.warn(s.range, "'@gpu for' uses libdevice math (sin/cos/tan/exp/log/pow); falling back to CPU");
        return false;
    }
    if (!kb.GetInsertBlock()->getTerminator()) {
        if (s.stride && loop_var_kv) {
            auto* i_phi = llvm::dyn_cast<llvm::PHINode>(loop_var_kv);
            if (i_phi) {
                auto* nctaid = kb.CreateIntCast(emitGpuGridDim(kb), i64_ty, false, "nctaid2");
                auto* nthreads = kb.CreateMul(ntid, nctaid, "nthreads2");
                auto* i_next = kb.CreateAdd(i_phi, nthreads, "i_next");
                i_phi->addIncoming(i_next, kb.GetInsertBlock());
                kb.CreateBr(loop_bb);
            }
        } else {
            kb.CreateBr(end_bb);
        }
    }

    kb.SetInsertPoint(end_bb);
    kb.CreateRetVoid();

    // §6.4 AMD：grid-stride 需 gridDim（AMDGCN 无直接 intrinsic）→ 不发射对象，
    // 继续下方 gpu/cpu 双块（空 PTX → 运行期 cpu_bb，stride step 修复正确）。
    if (gpu_amd_grid_used_) {
        diag_.warn(s.range,
            "AMD target: '@gpu stride' has no gridDim intrinsic; runtime CPU fallback");
    }

    // Store PTX for later use（§7.7 跨厂商：NVPTX→PTX / AMD→GCN ELF）
    std::string err;
    ensureGpuTargetsInited();

    auto* tgt = llvm::TargetRegistry::lookupTarget(gpuTargetTriple(), err);
    if (!tgt) {
        diag_.warn(SourceRange{}, "GPU target not available: " + err);
        return false;
    }

    auto* tm = tgt->createTargetMachine(
        llvm::Triple(gpuTargetTriple()), gpuTargetArch(), "", llvm::TargetOptions{}, llvm::Reloc::PIC_);
    if (!tm) {
        diag_.warn(SourceRange{}, "GPU target machine creation failed");
        return false;
    }
    ptx_mod->setDataLayout(tm->createDataLayout());

    // Verify the kernel module
    std::string verify_err;
    llvm::raw_string_ostream verify_os(verify_err);
    if (llvm::verifyModule(*ptx_mod, &verify_os)) {
        diag_.warn(SourceRange{}, "GPU kernel verification failed: " + verify_err);
        delete tm;
        return false;
    }

    // Link CUDA libdevice so __nv_* math calls (sin/cos/exp/log/pow/...) are
    // self-contained in the emitted PTX（NV 专属；AMD 用 LLVM intrinsic / ocml，留 §6.4）。
    if (gpu_math_used_ && !gpuTargetAmd()) {
        if (!linkGpuLibdevice(ptx_mod.get(), &diag_)) {
            diag_.warn(s.range,
                "'@gpu for' uses libdevice math (sin/cos/exp/log/pow) but "
                "libdevice.10.bc was not found; running on CPU");
            delete tm;
            return false;
        }
    }

    // ---- M3.5 内核优化：默认 O2（与宿主 -O 解耦），MYP_GPU_KERNEL_OPT 可覆盖 ----
    // 之前内核以 -O0 直出 PTX（只内联+删死码），卷积等大循环未展开/向量化。
    // 这里对内核模块跑 LLVM 默认模块管线（SROA/InstCombine/GVN/LICM/循环展开/
    // 向量化等），再用对应 CodeGenOptLevel 生成 PTX。
    unsigned kopt = 2;
    if (const char* env = getenv("MYP_GPU_KERNEL_OPT")) {
        std::string want(env);
        if (want == "O0") kopt = 0;
        else if (want == "O1") kopt = 1;
        else if (want == "O2") kopt = 2;
        else if (want == "O3") kopt = 3;
    }
    tm->setOptLevel(kopt >= 3 ? llvm::CodeGenOptLevel::Aggressive :
                   kopt == 2 ? llvm::CodeGenOptLevel::Default :
                   kopt == 1 ? llvm::CodeGenOptLevel::Less :
                               llvm::CodeGenOptLevel::None);
    if (kopt > 0) {
        llvm::OptimizationLevel OL =
            kopt >= 3 ? llvm::OptimizationLevel::O3 :
            kopt == 2 ? llvm::OptimizationLevel::O2 :
                        llvm::OptimizationLevel::O1;
        llvm::LoopAnalysisManager LAM;
        llvm::FunctionAnalysisManager FAM;
        llvm::CGSCCAnalysisManager CGAM;
        llvm::ModuleAnalysisManager MAM;
        llvm::PassBuilder PB(tm);  // 带 TM → TTI 注册，向量化有成本模型
        PB.registerModuleAnalyses(MAM);
        PB.registerCGSCCAnalyses(CGAM);
        PB.registerFunctionAnalyses(FAM);
        PB.registerLoopAnalyses(LAM);
        PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
        llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OL);
        MPM.addPass(llvm::VerifierPass());
        MPM.run(*ptx_mod, MAM);
    }

    // Emit（§7.7 跨厂商：NV → PTX 文本；AMD → GCN ELF 写文件，嵌入空 PTX）
    std::string ptx_str;
    if (gpuTargetAmd() && !gpu_amd_grid_used_) {
        // §9.5 ⑤ 交叉编译：GCN code object 写文件（emitGpuTargetBytes 副作用），
        // 嵌入空 PTX → 运行期 myp_gpu_load_kernel 失败 → cpu_bb 回退。
        ptx_str = emitGpuTargetBytes(ptx_mod.get(), tm, "for");
        ptx_code_ = "";
    } else if (gpuTargetAmd()) {
        // grid-stride 无 gridDim intrinsic：不发射对象，运行期 CPU 回退
        ptx_code_ = "";
        delete tm;
    } else {
        ptx_str = emitGpuTargetBytes(ptx_mod.get(), tm, "for");
        if (getenv("MYP_DUMP_PTX") && !ptx_str.empty()) {
            fprintf(stderr, "=== MYP PTX (for) ===\n%s\n=== END ===\n", ptx_str.c_str());
        }
        if (ptx_str.empty()) {
            diag_.warn(SourceRange{}, "GPU emitted empty code");
            delete tm;
            return false;
        }
        ptx_code_ = ptx_str;
    }

    // Embed PTX as a global string constant in the main module
    auto* ptx_global = builder_.CreateGlobalString(ptx_str, "__myp_ptx_kernel");

    // ---- Generate GPU launch + CPU fallback at the call site ----
    auto* func = builder_.GetInsertBlock()->getParent();
    auto* gpu_bb = llvm::BasicBlock::Create(ctx_, "gpu_launch", func);
    auto* cpu_bb = llvm::BasicBlock::Create(ctx_, "gpu_cpu_fallback", func);

    // Get n_val from the extracted loop bound, with fallback
    llvm::Value* n_val = loop_bound_val;
    if (!n_val)
        n_val = llvm::ConstantInt::get(i64_ty, 10);  // fallback
    // Cast to i64 if needed
    if (n_val->getType() != i64_ty) {
        if (n_val->getType()->isIntegerTy())
            n_val = builder_.CreateIntCast(n_val, i64_ty, false);
        else
            n_val = builder_.CreateFPToSI(n_val, i64_ty);
    }

    // Try GPU init
    auto* gpu_ok = builder_.CreateCall(runtime_gpu_init_, {}, "gpu_ok");
    auto* gpu_ok_i1 = builder_.CreateICmpNE(gpu_ok,
        llvm::ConstantInt::get(i32_ty, 0), "gpu_ok_cmp");
    builder_.CreateCondBr(gpu_ok_i1, gpu_bb, cpu_bb);

    // === GPU path ===
    builder_.SetInsertPoint(gpu_bb);
    auto* kernel_ctx = builder_.CreateCall(runtime_gpu_load_kernel_,
        {ptx_global, builder_.CreateGlobalString("myp_kernel", "kn")}, "kernel_ctx");

    auto* kernel_ok = builder_.CreateICmpNE(kernel_ctx,
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr_ty)), "k_ok");
    auto* launch_bb = llvm::BasicBlock::Create(ctx_, "gpu_launch_run", func);
    auto* gpu_done_bb = llvm::BasicBlock::Create(ctx_, "gpu_done", func);
    // §5.2 kernel 加载失败 → 走 CPU 回退（cpu_bb），而非静默跳过 gpu_done_bb。
    builder_.CreateCondBr(kernel_ok, launch_bb, cpu_bb);

    // Launch kernel —— §3.7 @gpu block(n)：块大小可调（默认 256），grid=ceil(n/block)。
    builder_.SetInsertPoint(launch_bb);
    auto* block_i32 = llvm::ConstantInt::get(i32_ty, block_size);
    auto* grid_val = builder_.CreateUDiv(
        builder_.CreateAdd(n_val, llvm::ConstantInt::get(i64_ty, block_size - 1)),
        llvm::ConstantInt::get(i64_ty, block_size), "grid");
    auto* grid_i32 = builder_.CreateIntCast(grid_val, i32_ty, false);

    // ---- Data transfer for captured arrays ----
    struct ArrayAlloc {
        llvm::Value* gpu_ptr;
        llvm::Value* host_ptr;
        llvm::Value* byte_size;
        bool needs_copyback;
        std::string name;
    };
    std::vector<ArrayAlloc> array_allocs;

    // M3 设备驻留：resident(arr = devVar) — 被标记数组跳过 H2D/D2H/释放，
    // 内核直接使用 devVar（long）所持设备指针。
    std::map<std::string, std::string> resident_dev;
    for (auto& [arr, dev] : s.resident)
        resident_dev[arr] = dev;


    for (auto& ka : kernel_args_) {
        if (!ka.is_array) continue;
        if (resident_dev.count(ka.name)) continue;  // M3: 设备驻留，跳过传输

        llvm::Value* host_ptr = nullptr;
        llvm::Value* byte_size = nullptr;

        auto* nv = getNamedValue(ka.name);
        if (nv) {
            auto* loaded = builder_.CreateLoad(ptr_ty, nv, ka.name);
            host_ptr = loaded;
            // Use tracked byte size if available, otherwise compute from n_val.
            // NOTE: array_byte_sizes_ is keyed by name and can hold stale entries
            // from a previously compiled function (e.g. a local array in another
            // method with the same name). Only trust it when the value belongs to
            // the current function.
            auto bsit = array_byte_sizes_.find(ka.name);
            bool bs_valid = false;
            if (bsit != array_byte_sizes_.end()) {
                // 仅信任属于当前函数的字节数（函数参数/跨函数同名局部不会被
                // 其它函数的缓存污染）。
                bs_valid = (bsit->second.first == func);
            }
            if (bs_valid) {
                byte_size = bsit->second.second;
            } else {
                auto eit = array_elem_types_.find(ka.name);
                uint64_t elem_sz = 8;
                if (eit != array_elem_types_.end()) {
                    if (eit->second->isDoubleTy()) elem_sz = 8;
                    else if (eit->second->isFloatTy()) elem_sz = 4;
                    else if (eit->second->isIntegerTy(32)) elem_sz = 4;
                }
                // 从 ref-counted 数组头（obj-24: count * elem_size）读取真实字节数。
                // loop_bound × elem_size 对参数/属性数组超/欠传输（kernel 访问可能
                // 超出 loop_bound 或数组小于 loop_bound），改用真实大小最稳妥。
                // 注意：固定栈数组（double[1] 等）无数组头，不能被 @gpu for 捕获；
                //       调用方需用 new 分配（如 double[] acc = new double[1]）。
                auto* i8_ty = llvm::Type::getInt8Ty(ctx_);
                auto* hdr = builder_.CreateGEP(i8_ty, loaded,
                    llvm::ConstantInt::get(i64_ty, -(int64_t)24), ka.name + "_hdr");
                auto* cnt_ptr = builder_.CreateBitCast(hdr, llvm::PointerType::get(i64_ty, 0));
                auto* cnt = builder_.CreateLoad(i64_ty, cnt_ptr, ka.name + "_cnt");
                byte_size = builder_.CreateMul(cnt, llvm::ConstantInt::get(i64_ty, elem_sz),
                                               ka.name + "_sz");
            }
        }

        if (host_ptr && byte_size) {
            auto* gpu_ptr = builder_.CreateCall(runtime_gpu_alloc_, {byte_size},
                                                 ka.name + "_gpu");
            builder_.CreateCall(runtime_gpu_to_device_, {gpu_ptr, host_ptr, byte_size});
            array_allocs.push_back({gpu_ptr, host_ptr, byte_size, true, ka.name});
        }
    }

    // ---- Build kernel launch args ----
    // Kernel signature: (i64 n, captured_vars..., i64 pbuf, i64 pcnt, i64 pfail)
    // Args array: pointer to n, pointer to each captured var, 3 staging pointers
    int total_args = 1 + (int)kernel_args_.size() + 3;   // §P5 ② 末尾 3 个 staging 指针

    auto* args_arr = builder_.CreateAlloca(ptr_ty, llvm::ConstantInt::get(i32_ty, total_args),
                                           "gpu_args");

    // Build a map from kernel arg name to its GPU pointer (for arrays)
    std::map<std::string, llvm::Value*> gpu_ptr_map;
    for (auto& aa : array_allocs) {
        gpu_ptr_map[aa.name] = aa.gpu_ptr;
    }

    int arg_pos = 0;
    // Arg 0: n
    {
        auto* n_alloca = builder_.CreateAlloca(i64_ty);
        builder_.CreateStore(n_val, n_alloca);
        llvm::Value* idxs[] = { llvm::ConstantInt::get(i32_ty, arg_pos) };
        auto* ap = builder_.CreateGEP(ptr_ty, args_arr, idxs);
        builder_.CreateStore(builder_.CreateBitCast(n_alloca, ptr_ty), ap);
        arg_pos++;
    }
    // Args 1..N: captured variables
    for (auto& ka : kernel_args_) {
        llvm::Value* store_val = nullptr;
        if (ka.is_array) {
            // For arrays: pass pointer to GPU device pointer
            auto rdit = resident_dev.find(ka.name);
            if (rdit != resident_dev.end()) {
                // M3 设备驻留：设备指针来自 devVar（long，i64）的值
                auto* nv = getNamedValue(rdit->second);
                if (nv) {
                    auto* dev_val = builder_.CreateLoad(builder_.getInt64Ty(), nv,
                                                        ka.name + "_dev");
                    auto* dev_ptr = builder_.CreateIntToPtr(dev_val, ptr_ty, ka.name + "_devptr");
                    auto* tmp = builder_.CreateAlloca(ptr_ty);
                    builder_.CreateStore(dev_ptr, tmp);
                    store_val = tmp;
                }
            } else {
                auto gpit = gpu_ptr_map.find(ka.name);
                if (gpit != gpu_ptr_map.end()) {
                    auto* tmp = builder_.CreateAlloca(ptr_ty);
                    builder_.CreateStore(gpit->second, tmp);
                    store_val = tmp;
                }
            }
        } else {
            // Scalar: store value in temp alloca
            auto* nv = getNamedValue(ka.name);
            if (nv) {
                auto* val = builder_.CreateLoad(ka.type, nv, ka.name);
                auto* tmp = builder_.CreateAlloca(ka.type);
                builder_.CreateStore(val, tmp);
                store_val = tmp;
            }
        }
        if (!store_val) {
            auto* tmp = builder_.CreateAlloca(ka.type);
            builder_.CreateStore(llvm::ConstantInt::get(i64_ty, 0), tmp);
            store_val = tmp;
        }
        llvm::Value* idxs[] = { llvm::ConstantInt::get(i32_ty, arg_pos) };
        auto* ap = builder_.CreateGEP(ptr_ty, args_arr, idxs);
        builder_.CreateStore(builder_.CreateBitCast(store_val, ptr_ty), ap);
        arg_pos++;
    }

    // §P5 ② printk/assert staging 设备指针（kernel 末尾 3 个 i64 参数；非 printk
    // kernel 传 0）。先算好供 launch 与 flush 复用。
    llvm::Value* pf_pbuf = llvm::ConstantInt::get(i64_ty, 0);
    llvm::Value* pf_pcnt = llvm::ConstantInt::get(i64_ty, 0);
    llvm::Value* pf_pfail = llvm::ConstantInt::get(i64_ty, 0);
    if (gpu_kernel_printf_used_ && runtime_gpu_printf_buf_) {
        pf_pbuf = builder_.CreateCall(runtime_gpu_printf_buf_, {});
        pf_pcnt = builder_.CreateCall(runtime_gpu_printf_cnt_, {});
        pf_pfail = builder_.CreateCall(runtime_gpu_printf_fail_, {});
    }
    for (llvm::Value* pv : {pf_pbuf, pf_pcnt, pf_pfail}) {
        auto* tmp = builder_.CreateAlloca(i64_ty);
        builder_.CreateStore(pv, tmp);
        llvm::Value* idxs[] = { llvm::ConstantInt::get(i32_ty, arg_pos) };
        auto* ap = builder_.CreateGEP(ptr_ty, args_arr, idxs);
        builder_.CreateStore(builder_.CreateBitCast(tmp, ptr_ty), ap);
        arg_pos++;
    }

    // §4.1 @gpu stream(s)：stream 句柄（0 = 默认流同步）。
    llvm::Value* stream_h = s.has_stream
        ? emitGpuStreamHandle(s.stream_expr.get())
        : llvm::ConstantInt::get(i64_ty, 0);
    // Launch kernel
    builder_.CreateCall(runtime_gpu_launch_,
        {kernel_ctx, grid_i32, block_i32, builder_.CreateBitCast(args_arr, ptr_ty),
         llvm::ConstantInt::get(i32_ty, total_args), stream_h});

    // ---- Copy back results ----
    for (auto& aa : array_allocs) {
        if (aa.needs_copyback) {
            if (s.has_stream) {
                // stream 模式：异步 D2H 排队到同流（须 s.sync() 后取回）
                builder_.CreateCall(runtime_gpu_to_host_async_,
                    {aa.host_ptr, aa.gpu_ptr, aa.byte_size, stream_h});
            } else {
                builder_.CreateCall(runtime_gpu_to_host_,
                    {aa.host_ptr, aa.gpu_ptr, aa.byte_size});
            }
        }
        builder_.CreateCall(runtime_gpu_free_, {aa.gpu_ptr});
    }

    // §P5 ② kernel printk/assert：回读 staging 记录并打印（宿主侧格式化）。
    // 同步 cuMemcpyDtoH 会等异步 kernel 完成（stream 模式也安全）；assert 失败
    // 在 runtime 内 exit(1)。
    if (gpu_kernel_printf_used_ && runtime_gpu_flush_printf_) {
        int nfmt = (int)gpu_kernel_fmts_.size();
        auto* fmt_arr = builder_.CreateAlloca(ptr_ty,
            llvm::ConstantInt::get(i32_ty, nfmt < 1 ? 1 : nfmt), "pf_fmts");
        for (int i = 0; i < nfmt; i++) {
            auto* g = builder_.CreateGlobalStringPtr(gpu_kernel_fmts_[i], "pf_fmt");
            auto* slotp = builder_.CreateGEP(ptr_ty, fmt_arr,
                llvm::ConstantInt::get(i32_ty, i));
            builder_.CreateStore(g, slotp);
        }
        builder_.CreateCall(runtime_gpu_flush_printf_,
            {pf_pbuf, pf_pcnt, pf_pfail, builder_.CreateBitCast(fmt_arr, ptr_ty),
             llvm::ConstantInt::get(i32_ty, nfmt)});
    }

    // Destroy kernel
    builder_.CreateCall(runtime_gpu_destroy_kernel_, {kernel_ctx});
    builder_.CreateBr(gpu_done_bb);

    // GPU done — skip CPU fallback
    builder_.SetInsertPoint(gpu_done_bb);

    // === CPU fallback ===
    builder_.SetInsertPoint(cpu_bb);
    diag_.warn(s.range, "'@gpu for' GPU fallback — running on CPU");
    const_cast<ForStmt&>(s).gpu = false;
    // CPU 回退模拟 kernel 上下文：gid=p/tx=p%bd/bx=p/bd/bd=block_size/gx=ceil(n/bd)，
    // kernel.sync() 空操作（generateCall/generateMemberAccess 读取这些标志）。
    // loop_var 在 generateGpuKernel 开头已提取。
    gpu_cpu_fallback_ = true;
    gpu_cpu_loop_var_ = loop_var;
    gpu_cpu_bound_ = n_val;
    gpu_cpu_block_ = block_size;   // §3.7 block(n) 供 kernel.bd 模拟（默认 256）
    if (s.stride) {
        // §3.5 CPU 回退：顺序遍历全部 i（step 改 +1；GPU 用 nThreads=ntid*nctaid）
        auto& mut_s = const_cast<ForStmt&>(s);
        auto saved_step = std::move(mut_s.step);
        auto* id1 = new IdentifierExpr(loop_var, s.range);
        auto* one = new IntegerLiteralExpr(1, s.range);
        auto* add = new BinaryOpExpr(std::unique_ptr<Expr>(id1),
                                     BinaryOpKind::Add,
                                     std::unique_ptr<Expr>(one), s.range);
        auto* id2 = new IdentifierExpr(loop_var, s.range);
        mut_s.step = std::make_unique<AssignmentExpr>(std::unique_ptr<Expr>(id2),
                                                      std::unique_ptr<Expr>(add),
                                                      s.range);
        generateForStmt(s);
        mut_s.step = std::move(saved_step);
    } else {
        generateForStmt(s);
    }
    gpu_cpu_fallback_ = false;
    gpu_cpu_loop_var_.clear();
    gpu_cpu_bound_ = nullptr;
    const_cast<ForStmt&>(s).gpu = true;
    builder_.CreateBr(gpu_done_bb);

    // Set insert point to gpu_done_bb (shared continuation)
    builder_.SetInsertPoint(gpu_done_bb);

#ifdef MYP_CUDA_ENABLED
    cuda_enabled_ = true;
#endif

    diag_.warn(s.range, "'@gpu for' PTX kernel generated (" +
               std::to_string(ptx_str.size()) + " bytes)");

    return true;
#else
    // GPU offload 未编译（-DMYP_ENABLE_GPU=OFF）：@gpu for 走 CPU 顺序回退
    (void)s;
    return false;
#endif
}

}  // namespace mylang
