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
                           val_type->isDoubleTy() || val_type->isPointerTy()) {
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
                            auto* alloca = kb.CreateAlloca(ty);
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
                    static_cast<const IdentifierExpr&>(*kma.object).name == "kernel" &&
                    kma.member_name == "sync") {
                    llvm::Module* cur_mod = kb.GetInsertBlock()->getParent()->getParent();
                    // kernel.sync() → PTX bar.sync 0。LLVM 21 把旧 llvm.nvvm.barrier0
                    // 升级为 llvm.nvvm.barrier.cta.sync.aligned.all(i32 0)，NVPTX 后端
                    // 降级为 bar.sync 0（必须用真实 intrinsic，手动函数会变 extern call）。
                    auto* bar = llvm::Intrinsic::getDeclaration(cur_mod,
                        llvm::Intrinsic::nvvm_barrier_cta_sync_aligned_all);
                    auto* zero = llvm::ConstantInt::get(
                        llvm::Type::getInt32Ty(cur_mod->getContext()), 0);
                    return kb.CreateCall(bar, {zero});
                }
            }
            // Handle math functions
            std::string callee_name;
            if (e.callee->kind == ExprKind::Identifier)
                callee_name = static_cast<const IdentifierExpr&>(*e.callee).name;

            if (!callee_name.empty()) {
                // GPU math: emit calls to CUDA libdevice device functions (__nv_*).
                // generateGpuKernel links NVIDIA's libdevice.10.bc bitcode into the
                // kernel module at compile time, so __nv_sin/__nv_cos/__nv_exp/...
                // are resolved and the emitted PTX is fully self-contained.
                auto emit_math_gpu = [&](const char* nv) -> llvm::Value* {
                    if (e.args.size() < 1) return nullptr;
                    auto* a = emitKernelExpr(*e.args[0], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    if (!a) return nullptr;
                    gpu_math_used_ = true;
                    llvm::Module* cur_mod = kb.GetInsertBlock()->getParent()->getParent();
                    // §9.5 GPU：按实参类型选 libdevice 变体——float 实参走
                    // __nv_xf（float 版，返回 float），double 走 __nv_x；整型
                    // abs 内联 select（返回同宽整型，与 Math.abs<T:Numeric> 的
                    // int 实例返回类型匹配），其余整型实数提升 f64。
                    std::string fname = nv;
                    llvm::Type* arg_ty = double_ty;
                    if (a->getType()->isFloatTy()) {
                        arg_ty = float_ty;
                        if (fname.back() != 'f') fname += 'f';
                    } else if (a->getType()->isIntegerTy()) {
                        if (fname == "__nv_fabs") {
                            auto* zero = llvm::ConstantInt::get(a->getType(), 0);
                            auto* neg = kb.CreateSub(zero, a);
                            auto* isneg = kb.CreateICmpSLT(a, zero);
                            return kb.CreateSelect(isneg, neg, a);
                        }
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
                    gpu_math_used_ = true;
                    llvm::Module* cur_mod = kb.GetInsertBlock()->getParent()->getParent();
                    // §9.5：双 float → __nv_powf；否则提升 f64 → __nv_pow
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
                // Generic 2-argument GPU math (atan2)
                auto emit_math_gpu_2 = [&](const char* nv) -> llvm::Value* {
                    if (e.args.size() < 2) return nullptr;
                    auto* a = emitKernelExpr(*e.args[0], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    auto* b = emitKernelExpr(*e.args[1], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    if (!a || !b) return nullptr;
                    gpu_math_used_ = true;
                    llvm::Module* cur_mod = kb.GetInsertBlock()->getParent()->getParent();
                    // §9.5：双 float → __nv_atan2f；否则提升 f64 → __nv_atan2
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
                    auto* fn = cur_mod->getFunction(nv);
                    if (!fn)
                        fn = llvm::Function::Create(
                            llvm::FunctionType::get(double_ty, {double_ty, double_ty}, false),
                            llvm::Function::ExternalLinkage, nv, cur_mod);
                    return kb.CreateCall(fn, {a, b});
                };
                // Math functions
                auto emit_math_1 = [&](const char* n) -> llvm::Value* {
                    if (e.args.size() < 1) return nullptr;
                    auto* a = emitKernelExpr(*e.args[0], kb, kernel_vars,
                                              kernel_arg_values, loop_var_name, tid_val);
                    if (!a) return nullptr;
                    // GPU mode: call CUDA libdevice device functions
                    if (gpu_for_stmt_) {
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
                    if (gpu_for_stmt_) return emit_math_gpu_2("__nv_atan2");
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
                    if (gpu_for_stmt_) {
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
                    if (gpu_for_stmt_) return emit_math_pow_gpu();
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
                    if (!gpu_for_stmt_) {
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
            // Three cases:
            // 1. ClassName.property (static class) — look up in static_property_globals_
            // 2. obj.property (struct field) — struct GEP access
            // 3. this.property — this pointer struct access

            if (e.object->kind == ExprKind::Identifier) {
                auto& oi = static_cast<const IdentifierExpr&>(*e.object);
                auto sit = static_property_globals_.find(oi.name);
                if (sit != static_property_globals_.end()) {
                    // Static class property: access via global struct pointer
                    auto cvit = kernel_vars.find(oi.name);
                    if (cvit != kernel_vars.end()) {
                        auto* st_ptr = cvit->second;
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
                auto eit = array_elem_types_.find(id.name);
                if (eit != array_elem_types_.end())
                    elem_ty = eit->second;
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
                    auto eit = array_elem_types_.find(id.name);
                    if (eit != array_elem_types_.end())
                        elem_ty = eit->second;
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
                // Create alloca for mutable local variable
                llvm::BasicBlock& entry_b = kernel_func->getEntryBlock();
                llvm::IRBuilder<> entry_kb(&entry_b, entry_b.getFirstInsertionPt());
                auto* alloca_p = entry_kb.CreateAlloca(var_ty, nullptr, d.name);
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

bool CodeGen::generateGpuKernel(const ForStmt& s) {
#ifdef MYP_ENABLE_GPU
    // Create a new module for PTX generation
    auto ptx_mod = std::make_unique<llvm::Module>("myp_gpu_kernel", ctx_);
    ptx_mod->setTargetTriple(llvm::Triple("nvptx64-nvidia-cuda"));

    auto* i64_ty = llvm::Type::getInt64Ty(ctx_);
    auto* i32_ty = llvm::Type::getInt32Ty(ctx_);
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    auto* double_ty = llvm::Type::getDoubleTy(ctx_);

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

    // Build kernel function signature: (i64 n, captured_vars...)
    // tid is NOT a parameter — computed from blockIdx/threadIdx NVVM intrinsics
    std::vector<llvm::Type*> kernel_param_types;
    kernel_param_types.push_back(i64_ty);  // n (loop bound)

    std::map<std::string, llvm::Value*> kernel_vars_map;
    std::vector<llvm::Value*> kernel_arg_values;

    for (auto& ka : kernel_args_) {
        kernel_param_types.push_back(ka.type);
    }

    auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), kernel_param_types, false);
    auto* kernel_func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                                "myp_kernel", ptx_mod.get());
    // Mark as PTX kernel entry point (generates .entry in PTX, not .func)
    kernel_func->setCallingConv(llvm::CallingConv::PTX_Kernel);

    int arg_idx = 0;
    auto* n_arg = kernel_func->getArg(arg_idx++);
    n_arg->setName("n");

    for (auto& ka : kernel_args_) {
        auto* arg = kernel_func->getArg(arg_idx++);
        arg->setName(ka.name);
        kernel_vars_map[ka.name] = arg;
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

    // Compute tid = blockIdx.x * blockDim.x + threadIdx.x using NVVM intrinsics
    auto* tid_x = kb.CreateIntCast(
        kb.CreateIntrinsic(llvm::Intrinsic::nvvm_read_ptx_sreg_tid_x,
            llvm::ArrayRef<llvm::Type*>(), llvm::ArrayRef<llvm::Value*>()),
        i64_ty, false, "tid_x");
    auto* ntid = kb.CreateIntCast(
        kb.CreateIntrinsic(llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_x,
            llvm::ArrayRef<llvm::Type*>(), llvm::ArrayRef<llvm::Value*>()),
        i64_ty, false, "ntid");
    auto* ctaid = kb.CreateIntCast(
        kb.CreateIntrinsic(llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_x,
            llvm::ArrayRef<llvm::Type*>(), llvm::ArrayRef<llvm::Value*>()),
        i64_ty, false, "ctaid");
    auto* bid_offset = kb.CreateMul(ctaid, ntid, "bid_off");
    auto* tid_val = kb.CreateAdd(bid_offset, tid_x, "tid");

    // §3.1 kernel 执行上下文：存当前块/线程值供 emitKernelExpr 的 kernel.xxx 读取
    gpu_ctx_tid_x_ = tid_x;
    gpu_ctx_ntid_ = ntid;
    gpu_ctx_ctaid_ = ctaid;
    gpu_ctx_tid_ = tid_val;
    gpu_ctx_n_arg_ = n_arg;

    // Generate tid < n check
    auto* cond_check = kb.CreateICmpSLT(tid_val, n_arg);
    auto* body_bb = llvm::BasicBlock::Create(ctx_, "body", kernel_func);
    auto* end_bb = llvm::BasicBlock::Create(ctx_, "end", kernel_func);
    kb.CreateCondBr(cond_check, body_bb, end_bb);

    // Compile loop body into kernel
    kb.SetInsertPoint(body_bb);
    gpu_for_stmt_ = &s;  // Mark GPU kernel mode (affects emitKernelExpr)
    gpu_math_unsupported_ = false;
    gpu_math_used_ = false;
    if (s.body) {
        emitKernelStmt(*s.body, kb, kernel_vars_map, kernel_arg_values,
                       loop_var, tid_val);
    }
    gpu_for_stmt_ = nullptr;
    if (gpu_math_unsupported_) {
        // Kernel uses transcendental math (sin/cos/tan/exp/log/pow) which needs
        // CUDA libdevice — the runtime doesn't link libdevice, so fall back to CPU.
        diag_.warn(s.range, "'@gpu for' uses libdevice math (sin/cos/tan/exp/log/pow); falling back to CPU");
        return false;
    }
    if (!kb.GetInsertBlock()->getTerminator())
        kb.CreateBr(end_bb);

    kb.SetInsertPoint(end_bb);
    kb.CreateRetVoid();

    // Store PTX for later use
    std::string ts = "nvptx64-nvidia-cuda";
    std::string err;

    // Initialize NVPTX target
    static bool nvptx_initialized = false;
    if (!nvptx_initialized) {
        LLVMInitializeNVPTXTargetInfo();
        LLVMInitializeNVPTXTarget();
        LLVMInitializeNVPTXTargetMC();
        LLVMInitializeNVPTXAsmPrinter();
        nvptx_initialized = true;
    }

    auto* tgt = llvm::TargetRegistry::lookupTarget(ts, err);
    if (!tgt) {
        diag_.warn(SourceRange{}, "NVPTX target not available: " + err);
        return false;
    }

    auto* tm = tgt->createTargetMachine(
        llvm::Triple(std::string(ts)), "", "", llvm::TargetOptions{}, llvm::Reloc::PIC_);
    if (!tm) {
        diag_.warn(SourceRange{}, "NVPTX target machine creation failed");
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
    // self-contained in the emitted PTX. If libdevice is unavailable, fall back
    // to CPU execution (the loop still runs correctly, just not on the GPU).
    if (gpu_math_used_) {
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

    // Emit PTX
    llvm::legacy::PassManager pm;
    llvm::SmallString<16384> ptx_buf;
    llvm::raw_svector_ostream ptx_os(ptx_buf);

    if (tm->addPassesToEmitFile(pm, ptx_os, nullptr, llvm::CodeGenFileType::AssemblyFile)) {
        diag_.warn(SourceRange{}, "NVPTX cannot emit PTX");
        delete tm;
        return false;
    }
    pm.run(*ptx_mod);
    delete tm;

    std::string ptx_str(std::string(ptx_buf.data(), ptx_buf.size()));
    if (ptx_str.empty()) {
        diag_.warn(SourceRange{}, "NVPTX emitted empty PTX");
        return false;
    }

    ptx_code_ = ptx_str;

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
    builder_.CreateCondBr(kernel_ok, launch_bb, gpu_done_bb);

    // Launch kernel
    builder_.SetInsertPoint(launch_bb);
    auto* grid_val = builder_.CreateUDiv(
        builder_.CreateAdd(n_val, llvm::ConstantInt::get(i64_ty, 255)),
        llvm::ConstantInt::get(i64_ty, 256), "grid");
    auto* grid_i32 = builder_.CreateIntCast(grid_val, i32_ty, false);
    auto* block_i32 = llvm::ConstantInt::get(i32_ty, 256);

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
    // Kernel signature: (i64 n, captured_vars...)
    // Args array: pointer to n, pointer to each captured var
    int total_args = 1 + (int)kernel_args_.size();

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

    // Launch kernel
    builder_.CreateCall(runtime_gpu_launch_,
        {kernel_ctx, grid_i32, block_i32, builder_.CreateBitCast(args_arr, ptr_ty),
         llvm::ConstantInt::get(i32_ty, total_args)});

    // ---- Copy back results ----
    for (auto& aa : array_allocs) {
        if (aa.needs_copyback) {
            builder_.CreateCall(runtime_gpu_to_host_,
                {aa.host_ptr, aa.gpu_ptr, aa.byte_size});
        }
        builder_.CreateCall(runtime_gpu_free_, {aa.gpu_ptr});
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
    // CPU 回退模拟 kernel 上下文：gid=p/tx=p%256/bx=p/256/bd=256/gx=ceil(n/256)，
    // kernel.sync() 空操作（generateCall/generateMemberAccess 读取这些标志）。
    // loop_var 在 generateGpuKernel 开头已提取。
    gpu_cpu_fallback_ = true;
    gpu_cpu_loop_var_ = loop_var;
    gpu_cpu_bound_ = n_val;
    generateForStmt(s);
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
