// expression codegen — part of the CodeGen implementation, split from codegen.cpp.
// Pure refactor: member definitions moved out of the 10k-line monolith;
// no behavior change. See codegen.cpp for the class declaration.

#include "mylang/CodeGen.h"
#include <functional>
#include "mylang/MypPasses.h"

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/ADT/SmallString.h>

#include <llvm/IR/IntrinsicsNVPTX.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Instrumentation/ThreadSanitizer.h>

// NVPTX target initialization (must be at global scope)
extern "C" void LLVMInitializeNVPTXTargetInfo(void);
extern "C" void LLVMInitializeNVPTXTarget(void);
extern "C" void LLVMInitializeNVPTXTargetMC(void);
extern "C" void LLVMInitializeNVPTXAsmPrinter(void);

#include <cstdlib>
#include <iostream>

namespace mylang {

// BUG-017: 接口动态分派的返回类型。关联类型方法（`Item getVal()`）的接口声明返回
// 类型是关联类型占位符 → typeNodeToCodegenType 回落 i32，而具体方法（如 StrBox 的
// `string getVal()`）返回 ptr → 分派调用 `call i32 %iface_fn` 把 string 当 i32 传，
// 调用方（如 Test.assertStrEq 期望 ptr）verify 失败。修复：对象已知具体类时解析其
// 同名方法的返回类型（与 vtable 指向的具体方法一致）；未知则回落接口声明类型。
llvm::Type* CodeGen::ifaceDispatchReturnType(const MemberAccessExpr& ma,
                                             const InterfaceMethodInfo* method) {
    std::string concrete;
    if (ma.object->kind == ExprKind::Identifier) {
        auto& oid = static_cast<const IdentifierExpr&>(*ma.object);
        auto vit = var_class_map_.find(oid.name);
        if (vit != var_class_map_.end()) concrete = vit->second;
    } else if (ma.object->kind == ExprKind::Subscript) {
        // 接口数组元素 children[i].method()：元素具体类经 array_elem_class_map_ 记录。
        auto& sub = static_cast<const SubscriptExpr&>(*ma.object);
        if (sub.array && sub.array->kind == ExprKind::Identifier) {
            auto& aid = static_cast<const IdentifierExpr&>(*sub.array);
            auto eit = array_elem_class_map_.find(aid.name);
            if (eit != array_elem_class_map_.end()) concrete = eit->second;
        }
    }
    if (!concrete.empty() && current_tu_) {
        for (auto& cls : current_tu_->classes) {
            if (cls.name != concrete) continue;
            for (auto& a : cls.actions) {
                if (a.name == ma.member_name)
                    return getLLVMType(typeNodeToCodegenType(a.return_type));
            }
            break;
        }
    }
    return getLLVMType(typeNodeToCodegenType(method->action->return_type));
}

llvm::Value* CodeGen::generateExpr(const Expr& e) {
    switch (e.kind) {
        case ExprKind::IntegerLiteral: return generateIntegerLiteral(static_cast<const IntegerLiteralExpr&>(e));
        case ExprKind::FloatLiteral:   return generateFloatLiteral(static_cast<const FloatLiteralExpr&>(e));
        case ExprKind::BoolLiteral:    return generateBoolLiteral(static_cast<const BoolLiteralExpr&>(e));
        case ExprKind::StringLiteral:  return generateStringLiteral(static_cast<const StringLiteralExpr&>(e));
        case ExprKind::NullLiteral:    return generateNullLiteral(static_cast<const NullLiteralExpr&>(e));
        case ExprKind::Identifier:     return generateIdentifier(static_cast<const IdentifierExpr&>(e));
        case ExprKind::BinaryOp:       return generateBinaryOp(static_cast<const BinaryOpExpr&>(e));
        case ExprKind::UnaryOp:        return generateUnaryOp(static_cast<const UnaryOpExpr&>(e));
        case ExprKind::Convert:        return generateConvert(static_cast<const ConvertExpr&>(e));
        case ExprKind::Call:           return generateCall(static_cast<const CallExpr&>(e));
        case ExprKind::MemberAccess:   return generateMemberAccess(static_cast<const MemberAccessExpr&>(e));
        case ExprKind::Subscript:      return generateSubscript(static_cast<const SubscriptExpr&>(e));
        case ExprKind::NewExpr:        return generateNewExpr(static_cast<const NewExpr&>(e));
        case ExprKind::NewArrayExpr:   return generateNewArrayExpr(static_cast<const NewArrayExpr&>(e));
        case ExprKind::ThisExpr:       return generateThisExpr(static_cast<const ThisExpr&>(e));
        case ExprKind::Assignment:     return generateAssignment(static_cast<const AssignmentExpr&>(e));
        case ExprKind::Ternary:        return generateTernary(static_cast<const TernaryExpr&>(e));
        case ExprKind::Try:            return generateTryExpr(static_cast<const TryExpr&>(e));
        case ExprKind::Range:          return generateRange(static_cast<const RangeExpr&>(e));
        case ExprKind::Lambda:         return generateLambda(static_cast<const LambdaExpr&>(e));
        case ExprKind::Pipe:           return generatePipe(static_cast<const PipeExpr&>(e));
        case ExprKind::EnumVariant:    return generateEnumVariant(static_cast<const EnumVariantExpr&>(e));
        case ExprKind::Await:          return generateAwaitExpr(static_cast<const AwaitExpr&>(e));
        case ExprKind::TupleExpr:      return generateTupleExpr(static_cast<const TupleExpr&>(e));
        case ExprKind::NamedArg:       // 防御：sema 已把命名实参重排消除；残留时生成其值
            return generateExpr(*static_cast<const NamedArgExpr&>(e).value);
        case ExprKind::GpuReduceExpr:  // §8.2 @gpu reduce 表达式形式
            return generateGpuReduceExpr(static_cast<const GpuReduceExpr&>(e));
    }
    return nullptr;
}

llvm::Value* CodeGen::generateTupleExpr(const TupleExpr& e) {
    std::vector<llvm::Value*> vals;
    for (auto& el : e.elements) vals.push_back(generateExpr(*el));
    std::vector<llvm::Type*> tys;
    for (auto& v : vals) tys.push_back(v->getType());
    auto* st = llvm::StructType::get(ctx_, tys);
    // Match the cached tuple struct (by type) if present, else use this one.
    llvm::Value* agg = llvm::PoisonValue::get(st);
    for (size_t i = 0; i < vals.size(); i++)
        agg = builder_.CreateInsertValue(agg, vals[i], (unsigned)i);
    return agg;
}

llvm::Value* CodeGen::generateIntegerLiteral(const IntegerLiteralExpr& e) {
    auto val = e.value;
    // 字符字面量 'A' → char（u8）→ i8（0..255 位型；供 s[i]/拼接 char 路径识别）
    if (e.is_char)
        return llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), (uint64_t)val, false);
    // 'u' suffix → 无符号：按值定宽（0xFFu→i8, 0xFFFFu→i16, 0xFFFFFFFFu→i32, 更大→i64）
    if (e.is_unsigned) {
        if (val >= 0 && val <= 0xFF) return llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), (uint64_t)val, false);
        if (val <= 0xFFFF) return llvm::ConstantInt::get(llvm::Type::getInt16Ty(ctx_), (uint64_t)val, false);
        if (val <= 0xFFFFFFFFLL) return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), (uint64_t)val, false);
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), (uint64_t)val, false);
    }
    // An explicit 'L' suffix forces long (i64), regardless of the value range.
    if (e.is_long)
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), val, true);
    // Determine integer width from value range (same logic as sema)
    bool fits_i32 = (val >= -2147483648LL && val <= 2147483647LL);
    if (fits_i32) {
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), val, true);
    }
    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), val, true);
}

llvm::Value* CodeGen::generateFloatLiteral(const FloatLiteralExpr& e) {
    if (e.is_f32) return llvm::ConstantFP::get(llvm::Type::getFloatTy(ctx_), e.value);
    return llvm::ConstantFP::get(llvm::Type::getDoubleTy(ctx_), e.value);
}

llvm::Value* CodeGen::generateBoolLiteral(const BoolLiteralExpr& e) {
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx_), e.value ? 1 : 0);
}

llvm::Value* CodeGen::generateStringLiteral(const StringLiteralExpr& e) {
    // M8 strings: emit an IMMORTAL counted string global —
    // { rc=0x7FFFFFFF, type_id=MYP_STR_TYPE_ID, [N x i8] bytes }. myp_release
    // on a literal decrements the huge rc but never reaches zero, so any ARC
    // slot that aliases a literal releases safely (static, never freed).
    // Type_id 0xFFFFFFFE == MYP_STR_TYPE_ID in runtime.c.
    // NOTE: the global must be WRITABLE (not `constant`), because myp_retain/
    // myp_release bump the header's rc field; a read-only section would fault.
    // The rc starts huge so it never reaches zero — the literal is immortal
    // while still being a uniform counted string for every ARC slot.
    auto* bytes = llvm::ConstantDataArray::getString(ctx_, e.value, true);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    auto* st = llvm::StructType::get(ctx_,
        {i32, i32, bytes->getType()}, false);
    auto* init = llvm::ConstantStruct::get(st, {
        llvm::ConstantInt::get(i32, 0x7FFFFFFF),   // rc — immortal
        llvm::ConstantInt::get(i32, 0xFFFFFFFE),   // MYP_STR_TYPE_ID
        bytes});
    auto* gv = new llvm::GlobalVariable(*module_, st, false /*writable*/,
        llvm::GlobalValue::PrivateLinkage, init, "str");
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    return builder_.CreateConstGEP2_32(st, gv, 0, 2);  // bytes pointer
}

llvm::Value* CodeGen::generateNullLiteral(const NullLiteralExpr&) {
    return llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0));
}

llvm::Value* CodeGen::generateIdentifier(const IdentifierExpr& e) {
    // M-FN-2 递归闭包：lambda __call 体内把"自名"作为值使用（如实参传给
    // 其他函数作为 thunk）→ 生成当前闭包的 fat pointer {this, <hidden>_tramp}。
    // 调用形式（B(...)）已由 sema 标记 __self 在 generateCallImpl 处理；
    // 此处覆盖"作为值"（函数值实参/赋值）的情形。
    if (!current_class_name_.empty() &&
        current_class_name_.rfind("__lambda_", 0) == 0 && current_tu_) {
        for (auto& cls : current_tu_->classes) {
            if (cls.name != current_class_name_) continue;
            if (!cls.lambda_name.empty() && cls.lambda_name == e.name) {
                auto* ta = getNamedValue("this");
                auto* tramp = module_->getFunction(current_class_name_ + "_tramp");
                if (ta && tramp) {
                    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
                    auto* fat_ty = llvm::StructType::get(ctx_, {ptr_ty, ptr_ty});
                    auto* fat = builder_.CreateAlloca(fat_ty);
                    auto* closure = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ta);
                    builder_.CreateStore(closure, builder_.CreateStructGEP(fat_ty, fat, 0));
                    builder_.CreateStore(builder_.CreateBitCast(tramp, ptr_ty),
                                         builder_.CreateStructGEP(fat_ty, fat, 1));
                    return builder_.CreateLoad(fat_ty, fat);
                }
            }
            break;
        }
    }
    auto* a = getNamedValue(e.name);
    if (!a) {
        if (runtime_now_ms_ && e.name == "now") return runtime_now_ms_;
        if (runtime_sleep_ms_ && e.name == "sleep") return runtime_sleep_ms_;
        if (runtime_print_int_ && e.name == "__myp_print_int") return runtime_print_int_;
        if (runtime_print_long_ && e.name == "__myp_print_long") return runtime_print_long_;
        if (runtime_print_ && e.name == "__myp_print") return runtime_print_;
        if (runtime_println_ && e.name == "__myp_println") return runtime_println_;
        if (runtime_print_float_ && e.name == "__myp_print_float") return runtime_print_float_;
        if (runtime_print_bool_ && e.name == "__myp_print_bool") return runtime_print_bool_;
        if (runtime_now_ms_ && e.name == "__myp_now_ms") return runtime_now_ms_;
        if (runtime_sleep_ms_ && e.name == "__myp_sleep_ms") return runtime_sleep_ms_;
        if (runtime_math_sqrt_ && e.name == "__myp_math_sqrt") return runtime_math_sqrt_;
        if (runtime_math_abs_ && e.name == "__myp_math_abs") return runtime_math_abs_;
        if (runtime_math_floor_ && e.name == "__myp_math_floor") return runtime_math_floor_;
        if (runtime_math_ceil_ && e.name == "__myp_math_ceil") return runtime_math_ceil_;
        if (runtime_math_sin_ && e.name == "__myp_math_sin") return runtime_math_sin_;
        if (runtime_math_cos_ && e.name == "__myp_math_cos") return runtime_math_cos_;
        if (runtime_math_tan_ && e.name == "__myp_math_tan") return runtime_math_tan_;
        if (runtime_math_asin_ && e.name == "__myp_math_asin") return runtime_math_asin_;
        if (runtime_math_acos_ && e.name == "__myp_math_acos") return runtime_math_acos_;
        if (runtime_math_atan_ && e.name == "__myp_math_atan") return runtime_math_atan_;
        if (runtime_math_atan2_ && e.name == "__myp_math_atan2") return runtime_math_atan2_;
        if (runtime_math_sinh_ && e.name == "__myp_math_sinh") return runtime_math_sinh_;
        if (runtime_math_cosh_ && e.name == "__myp_math_cosh") return runtime_math_cosh_;
        if (runtime_math_tanh_ && e.name == "__myp_math_tanh") return runtime_math_tanh_;
        if (runtime_math_exp_ && e.name == "__myp_math_exp") return runtime_math_exp_;
        if (runtime_math_log_ && e.name == "__myp_math_log") return runtime_math_log_;
        if (runtime_math_pow_ && e.name == "__myp_math_pow") return runtime_math_pow_;
        if (runtime_math_abs_int_ && e.name == "__myp_math_abs_int") return runtime_math_abs_int_;
        if (runtime_gpu_init_ && e.name == "__myp_cuda_available") return runtime_gpu_init_;
        if (runtime_cuda_count_ && e.name == "__myp_cuda_count") return runtime_cuda_count_;
        if (runtime_cuda_name_ && e.name == "__myp_cuda_name") return runtime_cuda_name_;
        if (runtime_cuda_memory_ && e.name == "__myp_cuda_memory") return runtime_cuda_memory_;
        if (runtime_cuda_capability_ && e.name == "__myp_cuda_capability") return runtime_cuda_capability_;
        if (runtime_cuda_multiprocessors_ && e.name == "__myp_cuda_multiprocessors") return runtime_cuda_multiprocessors_;
        if (runtime_cuda_max_threads_ && e.name == "__myp_cuda_max_threads") return runtime_cuda_max_threads_;
        if (runtime_cuda_warp_ && e.name == "__myp_cuda_warp") return runtime_cuda_warp_;
        if (runtime_gpu_vendor_ && e.name == "__myp_gpu_vendor") return runtime_gpu_vendor_;
        if (runtime_gpu_gfx_arch_ && e.name == "__myp_gpu_gfx_arch") return runtime_gpu_gfx_arch_;
        if (runtime_gpu_shared_per_block_ && e.name == "__myp_gpu_shared_per_block") return runtime_gpu_shared_per_block_;
        if (runtime_gpu_regs_per_block_ && e.name == "__myp_gpu_regs_per_block") return runtime_gpu_regs_per_block_;
        if (runtime_gpu_max_grid_dim_ && e.name == "__myp_gpu_max_grid_dim") return runtime_gpu_max_grid_dim_;
        if (runtime_gpu_max_block_dim_ && e.name == "__myp_gpu_max_block_dim") return runtime_gpu_max_block_dim_;
        if (runtime_gpu_clock_mhz_ && e.name == "__myp_gpu_clock_mhz") return runtime_gpu_clock_mhz_;
        if (runtime_gpu_concurrent_kernels_ && e.name == "__myp_gpu_concurrent_kernels") return runtime_gpu_concurrent_kernels_;
        if (runtime_gpu_mem_alignment_ && e.name == "__myp_gpu_mem_alignment") return runtime_gpu_mem_alignment_;
        if (runtime_gpu_double_precision_ && e.name == "__myp_gpu_double_precision") return runtime_gpu_double_precision_;
        if (runtime_gpu_atomics64_ && e.name == "__myp_gpu_atomics64") return runtime_gpu_atomics64_;
        if (runtime_io_fopen_ && e.name == "__myp_io_fopen") return runtime_io_fopen_;
        if (runtime_io_fclose_ && e.name == "__myp_io_fclose") return runtime_io_fclose_;
        if (runtime_io_read_line_ && e.name == "__myp_io_read_line") return runtime_io_read_line_;
        if (runtime_io_write_ && e.name == "__myp_io_write") return runtime_io_write_;
        if (runtime_io_write_line_ && e.name == "__myp_io_write_line") return runtime_io_write_line_;
        if (runtime_io_has_next_ && e.name == "__myp_io_has_next") return runtime_io_has_next_;
        if (runtime_io_read_byte_ && e.name == "__myp_io_read_byte") return runtime_io_read_byte_;
        if (runtime_io_read_i32be_ && e.name == "__myp_io_read_i32be") return runtime_io_read_i32be_;
        if (runtime_io_seek_ && e.name == "__myp_io_seek") return runtime_io_seek_;
        if (runtime_io_write_byte_ && e.name == "__myp_io_write_byte") return runtime_io_write_byte_;
        if (runtime_io_write_i32be_ && e.name == "__myp_io_write_i32be") return runtime_io_write_i32be_;
        if (runtime_io_write_double_ && e.name == "__myp_io_write_double") return runtime_io_write_double_;
        if (runtime_io_read_double_ && e.name == "__myp_io_read_double") return runtime_io_read_double_;
        if (runtime_io_current_handle_ && e.name == "__myp_io_current_handle") return runtime_io_current_handle_;
        if (runtime_io_select_ && e.name == "__myp_io_select") return runtime_io_select_;
        if (runtime_read_line_ && e.name == "__myp_read_line") return runtime_read_line_;
        if (runtime_kbhit_ && e.name == "__myp_kbhit") return runtime_kbhit_;
        if (runtime_getch_ && e.name == "__myp_getch") return runtime_getch_;
        if (runtime_flush_ && e.name == "__myp_flush") return runtime_flush_;
        if (runtime_term_width_ && e.name == "__myp_term_width") return runtime_term_width_;
        if (runtime_term_height_ && e.name == "__myp_term_height") return runtime_term_height_;
        if (runtime_strlen_ && e.name == "__myp_strlen") return runtime_strlen_;
        if (runtime_chr_ && e.name == "__myp_chr") return runtime_chr_;
        if (runtime_ord_ && e.name == "__myp_ord") return runtime_ord_;
        if (runtime_atof_ && e.name == "__myp_atof") return runtime_atof_;
        // Try class property via 'this'
        if (!current_class_name_.empty()) {
            const ClassDecl* cls = findClass(current_class_name_);
            if (cls) {
                unsigned pi;
                if (getPropertyIndex(cls->name, e.name, pi)) {
                    auto* ta = getNamedValue("this");
                    if (ta) {
                        auto* tp = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ta);
                        auto* st = getClassStruct(cls->name);
                        if (st) {
                            auto* gep = builder_.CreateStructGEP(st, tp, pi);
                            auto* pt = getPropertyType(*cls, e.name);
                            // Array-typed properties: return GEP pointer (cannot load array value)
                            if (pt->isArrayTy()) return gep;
                            return builder_.CreateLoad(pt, gep);
                        }
                    }
                }
            }
        }
        diag_.error(e.range, "undefined variable '" + e.name + "'");
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    }
    // Load named value (could be alloca or struct field GEP)
    if (llvm::isa<llvm::AllocaInst>(a)) {
        return builder_.CreateLoad(llvm::cast<llvm::AllocaInst>(a)->getAllocatedType(), a, e.name);
    }
    if (auto* nty2 = getNamedValueType(e.name)) {
        return builder_.CreateLoad(nty2, a, e.name);
    }
    return builder_.CreateLoad(a->getType(), a, e.name);
}

llvm::Value* CodeGen::generateBinaryOp(const BinaryOpExpr& e) {
    // Operator overloading: dispatch to a user-defined operator (resolved by Sema).
    if (e.op_call) {
        if (e.op_call->kind == "function") {
            auto* fn = module_->getFunction(e.op_call->func_name);
            if (fn) {
                auto* l = generateExpr(*e.lhs);
                auto* r = generateExpr(*e.rhs);
                return builder_.CreateCall(fn, {l, r});
            }
        } else if (e.op_call->kind == "struct_method") {
            auto* st_type = getStructType(e.op_call->struct_key);
            auto* l = generateExpr(*e.lhs);
            auto* r = generateExpr(*e.rhs);
            // Build the 'this' (struct receiver) pointer, mirroring struct
            // method call handling: value → alloca, pointer → reuse.
            llvm::Value* this_ptr = nullptr;
            if (auto* li = llvm::dyn_cast<llvm::LoadInst>(l)) {
                this_ptr = li->getPointerOperand();
            } else if (l->getType()->isPointerTy()) {
                this_ptr = builder_.CreateBitCast(l, llvm::PointerType::get(ctx_, 0));
            } else if (st_type) {
                auto* tmp = builder_.CreateAlloca(st_type);
                builder_.CreateStore(l, tmp);
                this_ptr = tmp;
            }
            if (this_ptr) {
                std::string fn_name = "struct_" + e.op_call->struct_key + "_" + e.op_call->method;
                auto* fn = module_->getFunction(fn_name);
                if (fn) return builder_.CreateCall(fn, {this_ptr, r});
            }
        }
    }

    // 短路逻辑运算：&& / || 在结果已定时不求值右操作数（MYP 原用位运算非短路）
    if (e.op == BinaryOpKind::And || e.op == BinaryOpKind::Or) {
        return generateShortCircuitLogic(e);
    }

    auto* l = generateExpr(*e.lhs);
    auto* r = generateExpr(*e.rhs);
    // §5.1 bitvector<N>：位向量运算保持自身宽度，不做数值提升（v8 << 2 在 8 位内）。
    bool bv_op = (e.lhs->resolved_kind == TypeKind::BitVector ||
                  e.rhs->resolved_kind == TypeKind::BitVector);
    if (l->getType() != r->getType() && !bv_op) {
        if (l->getType()->isDoubleTy() || r->getType()->isDoubleTy()) {
            // 混型提升为 double：int→SIToFP，float→FPExt。此前漏了 float→double，
            // 导致 fsub(float,double) 触发 LLVM verify 失败。
            if (!l->getType()->isDoubleTy()) {
                if (l->getType()->isIntegerTy()) l = builder_.CreateSIToFP(l, llvm::Type::getDoubleTy(ctx_));
                else if (l->getType()->isFloatTy()) l = builder_.CreateFPExt(l, llvm::Type::getDoubleTy(ctx_));
            }
            if (!r->getType()->isDoubleTy()) {
                if (r->getType()->isIntegerTy()) r = builder_.CreateSIToFP(r, llvm::Type::getDoubleTy(ctx_));
                else if (r->getType()->isFloatTy()) r = builder_.CreateFPExt(r, llvm::Type::getDoubleTy(ctx_));
            }
        } else if (l->getType()->isFloatTy() || r->getType()->isFloatTy()) {
            if (!l->getType()->isFloatTy() && l->getType()->isIntegerTy()) l = builder_.CreateSIToFP(l, llvm::Type::getFloatTy(ctx_));
            if (!r->getType()->isFloatTy() && r->getType()->isIntegerTy()) r = builder_.CreateSIToFP(r, llvm::Type::getFloatTy(ctx_));
        } else if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy()) {
            auto lw = l->getType()->getIntegerBitWidth(), rw = r->getType()->getIntegerBitWidth();
            // uint32：无符号操作数加宽用 ZExt（0xFFFFFFFFu 提升到 i64 = 4294967295，不是 -1）
            if (lw < rw) l = e.lhs_unsigned ? builder_.CreateZExt(l, r->getType())
                                            : builder_.CreateSExt(l, r->getType());
            else if (rw < lw) r = e.rhs_unsigned ? builder_.CreateZExt(r, l->getType())
                                                : builder_.CreateSExt(r, l->getType());
        }
    }

    auto fp = l->getType()->isFloatingPointTy();
    // String concatenation with +
    bool is_str_concat = (e.op == BinaryOpKind::Add) &&
                          (l->getType()->isPointerTy() || r->getType()->isPointerTy());
    if (is_str_concat) {
        // Ensure both operands are strings
        auto* ptr_type = llvm::PointerType::get(ctx_, 0);
        bool l_was_ptr = l->getType()->isPointerTy();   // operand already a string?
        bool r_was_ptr = r->getType()->isPointerTy();
        l = stringifyForConcat(l, e.lhs.get());
        r = stringifyForConcat(r, e.rhs.get());
        // Call runtime myp_strcat(l, r)
        auto* sc = module_->getFunction("myp_strcat");
        if (!sc) {
            auto* ft = llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type}, false);
            sc = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_strcat", module_.get());
        }
        auto* cat = builder_.CreateCall(sc, {l, r}, "strcat");
        // M9: myp_strcat reads its operands but does NOT consume them. The
        // integer/float/bool→string conversion temps (myp_to_string_*) are fresh
        // counted strings (rc=1); releasing them here fixes one leaked counted
        // string per non-string concat operand (e.g. `"s" + i` in a loop leaked
        // once per iteration). String operands (already pointers) stay borrowed.
        if (!l_was_ptr && runtime_release_) builder_.CreateCall(runtime_release_, {l});
        if (!r_was_ptr && runtime_release_) builder_.CreateCall(runtime_release_, {r});
        // M10: the concat RESULT is a fresh counted string (rc=1). Register it
        // as a pending temp so a store (arcConsumeTemp) OR the statement-end
        // flush releases it. Without this, a concat used as a FUNCTION-CALL
        // ARGUMENT (e.g. `h.set(Fmt.i(i)+"%")` in a loop) was never released
        // after the call — one leaked counted string per call (BUG-035).
        arcPushTemp(cat);
        return cat;
    }

    // For string equality (== / !=), use myp_str_eq for content comparison.
    // Only when BOTH operands are actually string-typed: a bare isPointerTy()
    // check would also match class refs and array pointers, wrongly doing a
    // strcmp on their memory (class instances with equal first bytes compared
    // "equal"; null comparisons called strcmp(NULL) and crashed).
    if (runtime_str_eq_ && (e.op == BinaryOpKind::Eq || e.op == BinaryOpKind::Ne) &&
        l->getType()->isPointerTy() && r->getType()->isPointerTy() && !fp &&
        exprResolvedString(*e.lhs) && exprResolvedString(*e.rhs)) {
        auto* result = builder_.CreateCall(runtime_str_eq_, {l, r}, "streq");
        if (e.op == BinaryOpKind::Ne) {
            return builder_.CreateICmpEQ(result, llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0));
        } else {
            return builder_.CreateICmpNE(result, llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0));
        }
    }
    // P1 §6.4：string 词法比较（< <= > >=），经 myp_str_cmp（strcmp 语义）。
    // 同样仅限 string 操作数——类/数组指针的 < > 应比较地址（LLVM 整数比较），
    // 而非把指针内容当字符串。
    if ((e.op == BinaryOpKind::Lt || e.op == BinaryOpKind::Gt ||
         e.op == BinaryOpKind::Le || e.op == BinaryOpKind::Ge) &&
        l->getType()->isPointerTy() && r->getType()->isPointerTy() && !fp &&
        exprResolvedString(*e.lhs) && exprResolvedString(*e.rhs)) {
        auto* cmp = module_->getFunction("myp_str_cmp");
        if (!cmp) {
            auto* ft = llvm::FunctionType::get(llvm::Type::getInt32Ty(ctx_),
                {llvm::PointerType::get(ctx_, 0), llvm::PointerType::get(ctx_, 0)}, false);
            cmp = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_str_cmp", module_.get());
        }
        auto* c = builder_.CreateCall(cmp, {l, r}, "strcmp");
        auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
        switch (e.op) {
            case BinaryOpKind::Lt: return builder_.CreateICmpSLT(c, zero);
            case BinaryOpKind::Gt: return builder_.CreateICmpSGT(c, zero);
            case BinaryOpKind::Le: return builder_.CreateICmpSLE(c, zero);
            case BinaryOpKind::Ge: return builder_.CreateICmpSGE(c, zero);
            default: break;
        }
    }

    switch (e.op) {
        case BinaryOpKind::Add: return fp ? builder_.CreateFAdd(l, r) : builder_.CreateAdd(l, r);
        case BinaryOpKind::Sub: return fp ? builder_.CreateFSub(l, r) : builder_.CreateSub(l, r);
        case BinaryOpKind::Mul: return fp ? builder_.CreateFMul(l, r) : builder_.CreateMul(l, r);
        case BinaryOpKind::Div: return fp ? builder_.CreateFDiv(l, r)
                                  : (e.result_unsigned ? builder_.CreateUDiv(l, r)
                                                       : builder_.CreateSDiv(l, r));
        case BinaryOpKind::Mod: return fp ? builder_.CreateFRem(l, r)
                                  : (e.result_unsigned ? builder_.CreateURem(l, r)
                                                       : builder_.CreateSRem(l, r));
        case BinaryOpKind::Eq:
        case BinaryOpKind::Ne:
        case BinaryOpKind::Lt:
        case BinaryOpKind::Gt:
        case BinaryOpKind::Le:
        case BinaryOpKind::Ge: {
            // Enum struct operands: compare by discriminant (payload ignored).
            if (l->getType()->isStructTy() && r->getType()->isStructTy()) {
                auto* ls = llvm::cast<llvm::StructType>(l->getType());
                auto* rs = llvm::cast<llvm::StructType>(r->getType());
                if (enum_struct_set_.count(ls) && enum_struct_set_.count(rs)) {
                    auto* ld = builder_.CreateExtractValue(l, 0, "enum_l_disc");
                    auto* rd = builder_.CreateExtractValue(r, 0, "enum_r_disc");
                    switch (e.op) {
                        case BinaryOpKind::Eq: return builder_.CreateICmpEQ(ld, rd);
                        case BinaryOpKind::Ne: return builder_.CreateICmpNE(ld, rd);
                        case BinaryOpKind::Lt: return builder_.CreateICmpSLT(ld, rd);
                        case BinaryOpKind::Gt: return builder_.CreateICmpSGT(ld, rd);
                        case BinaryOpKind::Le: return builder_.CreateICmpSLE(ld, rd);
                        case BinaryOpKind::Ge: return builder_.CreateICmpSGE(ld, rd);
                        default: break;
                    }
                }
            }
            // Non-enum struct operands: field-by-field value comparison
            // (Eq/Ne). Struct values cannot be fed to ICmp directly (LLVM
            // assertion "Invalid operand types for ICmp"). Ordering ops fall
            // back to memcmp (lexicographic by byte layout).
            if (l->getType()->isStructTy() && r->getType()->isStructTy() &&
                l->getType() == r->getType()) {
                auto* st = llvm::cast<llvm::StructType>(l->getType());
                if (!enum_struct_set_.count(st)) {
                    if (e.op == BinaryOpKind::Eq || e.op == BinaryOpKind::Ne) {
                        std::function<llvm::Value*(llvm::Value*, llvm::Value*, llvm::Type*)> eqRec;
                        eqRec = [&](llvm::Value* a, llvm::Value* b, llvm::Type* ty) -> llvm::Value* {
                            if (ty->isIntegerTy() || ty->isPointerTy())
                                return builder_.CreateICmpEQ(a, b);
                            if (ty->isFloatingPointTy())
                                return builder_.CreateFCmpOEQ(a, b);
                            if (auto* sty = llvm::dyn_cast<llvm::StructType>(ty)) {
                                llvm::Value* acc = nullptr;
                                for (unsigned i = 0; i < sty->getNumElements(); i++) {
                                    auto* fa = builder_.CreateExtractValue(a, i);
                                    auto* fb = builder_.CreateExtractValue(b, i);
                                    llvm::Value* fe = eqRec(fa, fb, sty->getElementType(i));
                                    acc = acc ? builder_.CreateAnd(acc, fe) : fe;
                                }
                                return acc ? acc : llvm::ConstantInt::get(
                                    llvm::Type::getInt1Ty(ctx_), 1);
                            }
                            if (auto* aty = llvm::dyn_cast<llvm::ArrayType>(ty)) {
                                llvm::Value* acc = nullptr;
                                for (unsigned i = 0; i < aty->getNumElements(); i++) {
                                    auto* fa = builder_.CreateExtractValue(a, i);
                                    auto* fb = builder_.CreateExtractValue(b, i);
                                    llvm::Value* fe = eqRec(fa, fb, aty->getElementType());
                                    acc = acc ? builder_.CreateAnd(acc, fe) : fe;
                                }
                                return acc ? acc : llvm::ConstantInt::get(
                                    llvm::Type::getInt1Ty(ctx_), 1);
                            }
                            return builder_.CreateICmpEQ(a, b);
                        };
                        llvm::Value* eq = eqRec(l, r, st);
                        return e.op == BinaryOpKind::Eq ? eq : builder_.CreateNot(eq);
                    }
                    // Ordering on struct values: byte-wise memcmp.
                    auto* memcmp_ft = llvm::FunctionType::get(
                        llvm::Type::getInt32Ty(ctx_),
                        {llvm::PointerType::get(ctx_, 0),
                         llvm::PointerType::get(ctx_, 0),
                         llvm::Type::getInt64Ty(ctx_)},
                        false);
                    auto memcmp = module_->getOrInsertFunction("memcmp", memcmp_ft);
                    auto* la = builder_.CreateAlloca(st);
                    auto* ra = builder_.CreateAlloca(st);
                    builder_.CreateStore(l, la);
                    builder_.CreateStore(r, ra);
                    auto* lap = builder_.CreateBitCast(la, llvm::PointerType::get(ctx_, 0));
                    auto* rap = builder_.CreateBitCast(ra, llvm::PointerType::get(ctx_, 0));
                    auto* sz = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_),
                        module_->getDataLayout().getTypeAllocSize(st));
                    auto* c = builder_.CreateCall(memcmp, {lap, rap, sz});
                    auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
                    switch (e.op) {
                        case BinaryOpKind::Lt: return builder_.CreateICmpSLT(c, zero);
                        case BinaryOpKind::Gt: return builder_.CreateICmpSGT(c, zero);
                        case BinaryOpKind::Le: return builder_.CreateICmpSLE(c, zero);
                        case BinaryOpKind::Ge: return builder_.CreateICmpSGE(c, zero);
                        default: break;
                    }
                }
            }
            // Ensure both operands have same type for comparison
            if (!fp && l->getType() != r->getType()) {
                auto* i32 = llvm::Type::getInt32Ty(ctx_);
                auto* i64 = llvm::Type::getInt64Ty(ctx_);
                if (l->getType() == i64 && r->getType() == i32)
                    r = e.rhs_unsigned ? builder_.CreateZExt(r, i64) : builder_.CreateSExt(r, i64);
                else if (l->getType() == i32 && r->getType() == i64)
                    l = e.lhs_unsigned ? builder_.CreateZExt(l, i64) : builder_.CreateSExt(l, i64);
                else if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy()) {
                    auto lw = l->getType()->getIntegerBitWidth();
                    auto rw = r->getType()->getIntegerBitWidth();
                    if (lw < rw) l = e.lhs_unsigned ? builder_.CreateZExt(l, r->getType())
                                                    : builder_.CreateSExt(l, r->getType());
                    else if (rw < lw) r = e.rhs_unsigned ? builder_.CreateZExt(r, l->getType())
                                                        : builder_.CreateSExt(r, l->getType());
                    else r = builder_.CreateIntCast(r, l->getType(), !e.rhs_unsigned);
                } else if (l->getType()->isPointerTy() && r->getType()->isIntegerTy()) {
                    r = builder_.CreateIntToPtr(r, l->getType());
                } else if (l->getType()->isIntegerTy() && r->getType()->isPointerTy()) {
                    l = builder_.CreateIntToPtr(l, r->getType());
                } else if (l->getType()->isPointerTy() && r->getType()->isPointerTy()) {
                    // Pointers to different types: cast both to i8*
                    auto* i8_ptr = llvm::PointerType::get(ctx_, 0);
                    if (l->getType() != i8_ptr) l = builder_.CreateBitCast(l, i8_ptr);
                    if (r->getType() != i8_ptr) r = builder_.CreateBitCast(r, i8_ptr);
                } else {
                    // Fallback: convert both to i64 for comparison
                    auto* i64 = llvm::Type::getInt64Ty(ctx_);
                    auto valToI64 = [&](llvm::Value* v) -> llvm::Value* {
                        auto* ty = v->getType();
                        if (ty->isIntegerTy()) {
                            return ty == i64 ? v : builder_.CreateSExt(v, i64);
                        } else if (ty->isPointerTy()) {
                            return builder_.CreatePtrToInt(v, i64);
                        } else if (ty->isFloatingPointTy()) {
                            return builder_.CreateFPToSI(v, i64);
                        } else {
                            // Struct, array, or other aggregate: store to alloca and get address
                            auto* alloca = builder_.CreateAlloca(ty);
                            builder_.CreateStore(v, alloca);
                            return builder_.CreatePtrToInt(
                                builder_.CreateBitCast(alloca, llvm::PointerType::get(ctx_, 0)), i64);
                        }
                    };
                    l = valToI64(l);
                    r = valToI64(r);
                }
            }
            if (e.op == BinaryOpKind::Eq)
                return fp ? builder_.CreateFCmpOEQ(l, r) : builder_.CreateICmpEQ(l, r);
            if (e.op == BinaryOpKind::Ne)
                return fp ? builder_.CreateFCmpONE(l, r) : builder_.CreateICmpNE(l, r);
            if (e.op == BinaryOpKind::Lt)
                return fp ? builder_.CreateFCmpOLT(l, r)
                          : (e.result_unsigned ? builder_.CreateICmpULT(l, r)
                                               : builder_.CreateICmpSLT(l, r));
            if (e.op == BinaryOpKind::Gt)
                return fp ? builder_.CreateFCmpOGT(l, r)
                          : (e.result_unsigned ? builder_.CreateICmpUGT(l, r)
                                               : builder_.CreateICmpSGT(l, r));
            if (e.op == BinaryOpKind::Le)
                return fp ? builder_.CreateFCmpOLE(l, r)
                          : (e.result_unsigned ? builder_.CreateICmpULE(l, r)
                                               : builder_.CreateICmpSLE(l, r));
            if (e.op == BinaryOpKind::Ge)
                return fp ? builder_.CreateFCmpOGE(l, r)
                          : (e.result_unsigned ? builder_.CreateICmpUGE(l, r)
                                               : builder_.CreateICmpSGE(l, r));
        }
        case BinaryOpKind::And: return builder_.CreateAnd(l, r);
        case BinaryOpKind::Or:  return builder_.CreateOr(l, r);
        case BinaryOpKind::BitAnd: return builder_.CreateAnd(l, r);
        case BinaryOpKind::BitOr:  return builder_.CreateOr(l, r);
        case BinaryOpKind::BitXor: return builder_.CreateXor(l, r);
        case BinaryOpKind::Shl: {
            // Shift amount must match value type (bitvector: 任意整型移位量截断到同宽)
            if (r->getType() != l->getType())
                r = builder_.CreateZExtOrTrunc(r, l->getType());
            return builder_.CreateShl(l, r);
        }
        case BinaryOpKind::Shr: {
            if (r->getType() != l->getType())
                r = builder_.CreateZExtOrTrunc(r, l->getType());
            // uint32：无符号右移 = 逻辑右移（LShr），有符号 = 算术右移（AShr）
            return e.result_unsigned ? builder_.CreateLShr(l, r) : builder_.CreateAShr(l, r);
        }
    }
    return nullptr;
}

llvm::Value* CodeGen::generateShortCircuitLogic(const BinaryOpExpr& e) {
    auto* l = generateExpr(*e.lhs);
    if (!l->getType()->isIntegerTy(1))
        l = builder_.CreateICmpNE(l, llvm::ConstantInt::get(l->getType(), 0));
    auto* func = builder_.GetInsertBlock()->getParent();
    bool is_and = (e.op == BinaryOpKind::And);
    // 与 generateTernary 一致：rhs_bb / merge_bb 先不挂入函数，后续 insert 一次
    auto* rhs_bb = llvm::BasicBlock::Create(ctx_, is_and ? "and.rhs" : "or.rhs");
    auto* merge_bb = llvm::BasicBlock::Create(ctx_, is_and ? "and.merge" : "or.merge");
    // &&: l true → rhs_bb，l false → merge（结果 false）
    // ||: l true → merge（结果 true），l false → rhs_bb
    builder_.CreateCondBr(l, is_and ? rhs_bb : merge_bb, is_and ? merge_bb : rhs_bb);
    auto* l_bb = builder_.GetInsertBlock();
    // RHS 分支
    func->insert(func->end(), rhs_bb);
    builder_.SetInsertPoint(rhs_bb);
    size_t before_rhs = arc_pending_temps_.size();
    auto* r = generateExpr(*e.rhs);
    if (!r->getType()->isIntegerTy(1))
        r = builder_.CreateICmpNE(r, llvm::ConstantInt::get(r->getType(), 0));
    // ARC: RHS 创建的临时对象在 rhs_bb 内释放（rhs 结果是 i1，绝无临时对象；
    // merge 块可从 entry 直达，语句末释放会违反支配 → LLVM verify 崩溃）。
    arcEndBranch(before_rhs, r);
    if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(merge_bb);
    auto* last_rhs = builder_.GetInsertBlock();
    // Merge
    func->insert(func->end(), merge_bb);
    builder_.SetInsertPoint(merge_bb);
    auto* phi = builder_.CreatePHI(r->getType(), 2, is_and ? "and.res" : "or.res");
    phi->addIncoming(llvm::ConstantInt::get(r->getType(), is_and ? 0 : 1), l_bb);
    phi->addIncoming(r, last_rhs);
    return phi;
}

llvm::Value* CodeGen::generateUnaryOp(const UnaryOpExpr& e) {
    auto* o = generateExpr(*e.operand);
    switch (e.op) {
        case UnaryOpKind::Negate: return o->getType()->isFloatingPointTy() ? builder_.CreateFNeg(o) : builder_.CreateNeg(o);
        case UnaryOpKind::Not:    return builder_.CreateNot(o);
        // §5.1 ~x：位取反（iN 按位非 = xor -1）
        case UnaryOpKind::BitNot: return builder_.CreateNot(o);
    }
    return nullptr;
}

llvm::Value* CodeGen::generateConvert(const ConvertExpr& e) {
    auto* v = generateExpr(*e.operand);
    // §5.1 bit(x)：x≠0（任意数值/bool）；bit→bit 直通。
    if (e.to_kind == TypeKind::Bit) {
        auto* i1 = llvm::Type::getInt1Ty(ctx_);
        if (!v) return llvm::ConstantInt::get(i1, 0);
        if (v->getType() == i1) return v;
        if (v->getType()->isIntegerTy())
            return builder_.CreateICmpNE(v, llvm::ConstantInt::get(v->getType(), 0), "bit");
        if (v->getType()->isFloatingPointTy())
            return builder_.CreateFCmpONE(v, llvm::ConstantFP::get(v->getType(), 0.0), "bit");
        return v;
    }
    // §5.1 bitvector<N>(x)：位保持（截断/零扩展到 N 位）。
    if (e.to_kind == TypeKind::BitVector) {
        TypeInfo bv(TypeKind::BitVector);
        bv.bitvector_width = e.to_bitvector_width;
        llvm::Type* target = getLLVMType(bv);
        if (!v) return llvm::ConstantInt::get(target, 0);
        if (v->getType() == target) return v;
        return builder_.CreateZExtOrTrunc(v, target, "bvcast");
    }
    llvm::Type* target = getLLVMType(TypeInfo(e.to_kind));
    if (!v) return llvm::ConstantInt::get(target, 0);
    // 源 bitvector → 整型/浮点：位保持直通（按无符号零扩展/截断）。
    if (e.operand->resolved_kind == TypeKind::BitVector) {
        if (target->isIntegerTy()) return builder_.CreateZExtOrTrunc(v, target, "bv2int");
        if (target->isFloatingPointTy()) {
            auto* i32 = llvm::Type::getInt32Ty(ctx_);
            auto* z = builder_.CreateZExtOrTrunc(v, i32, "bv2i32");
            return builder_.CreateSIToFP(z, target, "bv2fp");
        }
        return v;
    }
    // 源 bit → 整型/浮点：0/1 零扩展。
    if (e.operand->resolved_kind == TypeKind::Bit &&
        v->getType() == llvm::Type::getInt1Ty(ctx_)) {
        if (target->isIntegerTy()) return builder_.CreateZExt(v, target, "bit2int");
        if (target->isFloatingPointTy()) {
            auto* i32 = llvm::Type::getInt32Ty(ctx_);
            auto* z = builder_.CreateZExt(v, i32, "bit2i32");
            return builder_.CreateSIToFP(z, target, "bit2fp");
        }
        return v;
    }
    // P0：显式 cast 也收敛到单一转换权威（convertIntegerValue），保证无符号源
    // ZExt/UIToFP、bool↔int、fp↔fp 与隐式路径完全一致（docs/type_system_design §7.1）。
    return convertIntegerValue(builder_, v, target, e.operand.get());
}

// P0 bitcast<T,U>(x)：位保持重解释（docs/type_system_design §5.2）。LLVM bitcast
// 同宽直接转换；sema 已校验源/目标同宽（这里再做一次 LLVM 宽度防御）。指针与
// 同宽整数也可 bitcast（方便 `bitcast<long>(ptr)` 之类的位操作）。
llvm::Value* CodeGen::generateBitcast(const CallExpr& e) {
    if (e.args.size() != 1) {
        diag_.error(e.range, "bitcast takes exactly one argument");
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    }
    auto* v = generateExpr(*e.args[0]);
    llvm::Type* target = typeNodeToLLVMType(e.call_type_args.back());
    if (!v) return llvm::ConstantInt::get(target, 0);
    if (v->getType() == target) return v;
    unsigned sw = module_->getDataLayout().getTypeAllocSize(v->getType());
    unsigned tw = module_->getDataLayout().getTypeAllocSize(target);
    if (sw != tw) {
        diag_.error(e.range, "bitcast requires source and target types of the same size (" +
                      std::to_string(sw) + " vs " + std::to_string(tw) + " bytes)");
        return llvm::ConstantInt::get(target, 0);
    }
    return builder_.CreateBitCast(v, target);
}

// P1 bytes(s) / str(bytes)（docs §6.3）：string ↔ ubyte[] 互转。
// bytes(s)：myp_str_to_bytes(s) → 计数 ubyte[] backing data 指针（rc=1，动态数组
// 槽位直接持有）。str(bytes)：myp_bytes_to_str(ubyte[] data) → 计数字符串（rc=1，
// 走字符串 temp/ARC 路径）。sema 已保证实参类型匹配。
llvm::Value* CodeGen::generateBytesStr(const CallExpr& e, const std::string& name) {
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    if (e.args.size() != 1) {
        diag_.error(e.range, name + " takes exactly one argument");
        return llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0));
    }
    auto* arg = generateExpr(*e.args[0]);
    const char* fn_name = (name == "bytes") ? "myp_str_to_bytes" : "myp_bytes_to_str";
    auto* fn = module_->getFunction(fn_name);
    if (!fn) {
        auto* ft = llvm::FunctionType::get(ptr_ty, {ptr_ty}, false);
        fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn_name, module_.get());
    }
    return builder_.CreateCall(fn, {arg}, "bytesstr");
}

// §5.1 bytesOf(bitvector<N>) → ubyte[]：myp_uint_to_bytes(zext(v,i64), N/8)，
// 返回计数 ubyte[] backing（rc=1，动态数组槽位直接持有）。
llvm::Value* CodeGen::generateBytesOf(const CallExpr& e) {
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    if (e.args.size() != 1) {
        diag_.error(e.range, "bytesOf takes exactly one argument");
        return llvm::ConstantPointerNull::get(ptr_ty);
    }
    auto* v = generateExpr(*e.args[0]);
    llvm::Type* vt = v ? v->getType() : llvm::Type::getInt32Ty(ctx_);
    unsigned bits = vt->isIntegerTy() ? vt->getIntegerBitWidth() : 32;
    int nbytes = (int)((bits + 7) / 8);
    if (nbytes < 1) nbytes = 1;
    auto* wide = (vt == i64) ? v : builder_.CreateZExtOrTrunc(v, i64, "bvi64");
    auto* fn = module_->getFunction("myp_uint_to_bytes");
    if (!fn) {
        auto* ft = llvm::FunctionType::get(ptr_ty, {i64, i32}, false);
        fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                    "myp_uint_to_bytes", module_.get());
    }
    return builder_.CreateCall(fn, {wide, llvm::ConstantInt::get(i32, nbytes)}, "bvo");
}

// P2 parse* 全族（docs §6.2）：统一 strtol/strtoull/strtod 语义。runtime helper：
// parseLong→myp_str_to_long；parseUint→myp_str_to_ulong(trunc i32)；parseUlong→
// myp_str_to_ulong；parseFloat→myp_str_to_float；parseDouble→myp_atof；parseInt→
// myp_str_to_long(trunc i32)。
llvm::Value* CodeGen::generateParse(const CallExpr& e, const std::string& name) {
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    if (e.args.size() != 1) {
        diag_.error(e.range, name + " takes exactly one argument");
        return llvm::ConstantInt::get(i32, 0);
    }
    auto* arg = generateExpr(*e.args[0]);
    const char* fn_name = "myp_str_to_long";
    llvm::Type* ret_ty = i64;
    if (name == "parseLong")          { fn_name = "myp_str_to_long"; ret_ty = i64; }
    else if (name == "parseInt")      { fn_name = "myp_str_parse_int"; ret_ty = i32; }
    else if (name == "parseUlong")    { fn_name = "myp_str_to_ulong"; ret_ty = i64; }
    else if (name == "parseUint")     { fn_name = "myp_str_to_uint"; ret_ty = i32; }
    else if (name == "parseFloat")    { fn_name = "myp_str_to_float"; ret_ty = llvm::Type::getFloatTy(ctx_); }
    else if (name == "parseDouble")   { fn_name = "myp_atof"; ret_ty = llvm::Type::getDoubleTy(ctx_); }
    auto* fn = module_->getFunction(fn_name);
    if (!fn) {
        auto* ft = llvm::FunctionType::get(ret_ty, {ptr_ty}, false);
        fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn_name, module_.get());
    }
    llvm::Value* v = builder_.CreateCall(fn->getFunctionType(), fn,
        std::vector<llvm::Value*>{arg}, "parse");
    return v;
}

// §6.2 P4 parseIntOpt(s) → (value:int, ok:bool) 元组。调
// myp_str_parse_int_opt(s, &ok)，打包成 {i32, i1} 结构体（= (int,bool) 元组）。
llvm::Value* CodeGen::generateParseOpt(const CallExpr& e) {
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    auto* i1 = llvm::Type::getInt1Ty(ctx_);
    if (e.args.size() != 1) {
        diag_.error(e.range, "parseIntOpt takes exactly one string argument");
        return llvm::ConstantInt::get(i32, 0);
    }
    auto* sp = generateExpr(*e.args[0]);
    auto* ok_slot = builder_.CreateAlloca(i1);
    auto* fn = module_->getFunction("myp_str_parse_int_opt");
    if (!fn) {
        auto* ft = llvm::FunctionType::get(i32, {ptr_ty, ptr_ty}, false);
        fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
            "myp_str_parse_int_opt", module_.get());
    }
    auto* v = builder_.CreateCall(fn->getFunctionType(), fn,
        std::vector<llvm::Value*>{sp, ok_slot}, "parse");
    auto* ok = builder_.CreateLoad(i1, ok_slot);
    auto* st = llvm::StructType::get(ctx_, {i32, i1});
    llvm::Value* agg = llvm::UndefValue::get(st);
    agg = builder_.CreateInsertValue(agg, v, 0);
    agg = builder_.CreateInsertValue(agg, ok, 1);
    return agg;
}

// P2 §5.3：位操作原语 popcount/clz/ctz/bitreverse/rotl/rotr —— LLVM intrinsic
// 直映（ctpop/ctlz/cttz/bitreverse/fshl/fshr）。多态：返回类型 = 实参整型宽度；
// clz/ctz 用 zero_poison=false（0 时定义为位宽）；rotl/rotr 移位量统一到同宽。
llvm::Value* CodeGen::generateBitOps(const CallExpr& e, const std::string& name) {
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    bool unary = (name == "popcount" || name == "clz" || name == "ctz" ||
                  name == "bitreverse");
    size_t want = unary ? 1 : 2;
    if (e.args.size() != want) {
        diag_.error(e.range, name + " takes " + std::to_string(want) + " argument(s)");
        return llvm::ConstantInt::get(i32, 0);
    }
    auto* v = generateExpr(*e.args[0]);
    auto* ty = v->getType();
    if (!ty->isIntegerTy()) {
        diag_.error(e.range, name + " expects an integer argument");
        return llvm::ConstantInt::get(i32, 0);
    }
    llvm::Function* fn = nullptr;
    llvm::Value* result = nullptr;
    if (name == "popcount") {
        fn = llvm::Intrinsic::getDeclaration(module_.get(), llvm::Intrinsic::ctpop, {ty});
        result = builder_.CreateCall(fn->getFunctionType(), fn,
            std::vector<llvm::Value*>{v}, "popcount");
    } else if (name == "clz") {
        fn = llvm::Intrinsic::getDeclaration(module_.get(), llvm::Intrinsic::ctlz, {ty});
        auto* zp = llvm::ConstantInt::getFalse(ctx_);
        result = builder_.CreateCall(fn->getFunctionType(), fn,
            std::vector<llvm::Value*>{v, zp}, "clz");
    } else if (name == "ctz") {
        fn = llvm::Intrinsic::getDeclaration(module_.get(), llvm::Intrinsic::cttz, {ty});
        auto* zp = llvm::ConstantInt::getFalse(ctx_);
        result = builder_.CreateCall(fn->getFunctionType(), fn,
            std::vector<llvm::Value*>{v, zp}, "ctz");
    } else if (name == "bitreverse") {
        fn = llvm::Intrinsic::getDeclaration(module_.get(), llvm::Intrinsic::bitreverse, {ty});
        result = builder_.CreateCall(fn->getFunctionType(), fn,
            std::vector<llvm::Value*>{v}, "bitreverse");
    } else {  // rotl / rotr
        auto* n = generateExpr(*e.args[1]);
        if (!n->getType()->isIntegerTy()) {
            diag_.error(e.range, name + " shift amount must be an integer");
            return llvm::ConstantInt::get(i32, 0);
        }
        if (n->getType() != ty) n = builder_.CreateZExtOrTrunc(n, ty);
        auto id = (name == "rotl") ? llvm::Intrinsic::fshl : llvm::Intrinsic::fshr;
        fn = llvm::Intrinsic::getDeclaration(module_.get(), id, {ty});
        result = builder_.CreateCall(fn->getFunctionType(), fn,
            std::vector<llvm::Value*>{v, v, n}, name.c_str());
    }
    return result;
}

// §9.5 多态数学 intrinsic：一元实数（sqrt/floor/ceil/trunc/sin/cos/tan/asin/
// acos/atan/sinh/cosh/tanh/exp/log）与 abs 按实参类型发 LLVM 标量 intrinsic，
// 支持 Math 库泛型包装的 float 参数（f32 → llvm.sqrt.f32 等）。abs 整型 →
// llvm.abs.iN。返回 nullptr = 不拦截（pow/atan2/abs_int 走 runtime 调用）。
llvm::Value* CodeGen::generatePolyMathIntrinsic(const CallExpr& e, const std::string& id) {
    std::string fn = id.substr(2);  // "__" → "myp_math_*"（runtime 名）
    if (fn == "myp_math_pow" || fn == "myp_math_atan2" || fn == "myp_math_abs_int")
        return nullptr;
    if (e.args.size() < 1) {
        diag_.error(e.range, id + " requires 1 argument");
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    }
    auto* v = generateExpr(*e.args[0]);
    if (!v) return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    auto* ty = v->getType();
    // abs：整型 → llvm.abs.iN；浮点 → llvm.fabs
    if (fn == "myp_math_abs") {
        if (ty->isIntegerTy())
            return builder_.CreateIntrinsic(llvm::Intrinsic::abs, {ty},
                {v, llvm::ConstantInt::getFalse(ctx_)}, {}, "mypmath");
        return builder_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, v, nullptr, "mypmath");
    }
    llvm::Intrinsic::ID mid = llvm::Intrinsic::not_intrinsic;
    if (fn == "myp_math_sqrt") mid = llvm::Intrinsic::sqrt;
    else if (fn == "myp_math_floor") mid = llvm::Intrinsic::floor;
    else if (fn == "myp_math_ceil") mid = llvm::Intrinsic::ceil;
    else if (fn == "myp_math_trunc") mid = llvm::Intrinsic::trunc;
    else if (fn == "myp_math_sin") mid = llvm::Intrinsic::sin;
    else if (fn == "myp_math_cos") mid = llvm::Intrinsic::cos;
    else if (fn == "myp_math_tan") mid = llvm::Intrinsic::tan;
    else if (fn == "myp_math_asin") mid = llvm::Intrinsic::asin;
    else if (fn == "myp_math_acos") mid = llvm::Intrinsic::acos;
    else if (fn == "myp_math_atan") mid = llvm::Intrinsic::atan;
    else if (fn == "myp_math_sinh") mid = llvm::Intrinsic::sinh;
    else if (fn == "myp_math_cosh") mid = llvm::Intrinsic::cosh;
    else if (fn == "myp_math_tanh") mid = llvm::Intrinsic::tanh;
    else if (fn == "myp_math_exp") mid = llvm::Intrinsic::exp;
    else if (fn == "myp_math_log") mid = llvm::Intrinsic::log;
    if (mid == llvm::Intrinsic::not_intrinsic)
        return nullptr;
    // 整型实参（理论仅泛型体检查/用户直接误用 __myp_math_*）：提为 double
    // 再调用（设计：整型 → f64）。正常路径（where T : Float 实例化）不会到达。
    if (ty->isIntegerTy()) {
        auto* f64 = llvm::Type::getDoubleTy(ctx_);
        auto* d = builder_.CreateSIToFP(v, f64);
        return builder_.CreateUnaryIntrinsic(mid, d, nullptr, "mypmath");
    }
    return builder_.CreateUnaryIntrinsic(mid, v, nullptr, "mypmath");
}

// §4.2 P3：checkedAdd/checkedMul → @llvm.sadd.with.overflow.iN / smul.with.overflow.iN
// （sema 已校验同型有符号整型）。返回 {iN, i1} 结构体，即 MYP 元组 (value, overflow)。
llvm::Value* CodeGen::generateCheckedOp(const CallExpr& e, const std::string& name) {
    if (e.args.size() < 2) {
        diag_.error(e.range, name + " requires 2 arguments");
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    }
    auto* a = generateExpr(*e.args[0]);
    auto* b = generateExpr(*e.args[1]);
    if (!a->getType()->isIntegerTy() || !b->getType()->isIntegerTy()) {
        diag_.error(e.range, name + " expects signed integer arguments");
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    }
    // 公共类型 = 较宽位宽（与 sema commonNumericKind 一致：有符号拓宽）
    unsigned aw = a->getType()->getIntegerBitWidth();
    unsigned bw = b->getType()->getIntegerBitWidth();
    auto* cty = llvm::Type::getIntNTy(ctx_, std::max(aw, bw));
    if (a->getType() != cty) a = builder_.CreateSExtOrTrunc(a, cty);
    if (b->getType() != cty) b = builder_.CreateSExtOrTrunc(b, cty);
    llvm::Intrinsic::ID oid = (name == "checkedAdd")
        ? llvm::Intrinsic::sadd_with_overflow
        : llvm::Intrinsic::smul_with_overflow;
    auto* fn = llvm::Intrinsic::getDeclaration(module_.get(), oid, {cty});
    return builder_.CreateCall(fn->getFunctionType(), fn,
        std::vector<llvm::Value*>{a, b}, "checked");
}

// §3.6 向量打包访问：load4(float[] a, long i) → <4 x float> 打包读（i 为 4 元素
// 组索引，元素偏移 i*4）；store4(a, i, v) → 打包写。CPU 回退与 host 侧共用
// （LLVM 向量 load/store；GPU kernel 走 emitKernelExpr 的等价实现）。
llvm::Value* CodeGen::emitVec4Access(const CallExpr& e, const std::string& name) {
    auto* f32 = llvm::Type::getFloatTy(ctx_);
    auto* f4 = llvm::FixedVectorType::get(f32, 4);
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    if (e.args.size() < 2) {
        diag_.error(e.range, name + " expects (float[] a, long i"
                    + (name == "store4" ? std::string(", float4 v)") : std::string(")")));
        return nullptr;
    }
    auto* ap = generateExpr(*e.args[0]);
    if (!ap) return nullptr;
    auto* idx = generateExpr(*e.args[1]);
    if (idx->getType() != i64) idx = builder_.CreateIntCast(idx, i64, false);
    auto* elem_off = builder_.CreateMul(idx, llvm::ConstantInt::get(i64, 4), "v4off");
    auto* p = builder_.CreateGEP(f32, ap, elem_off, "v4p");
    // 对齐 4（标量对齐）：动态数组数据指针不保证 16B 对齐 → 未对齐向量访问
    //（host 用 movups 安全；GPU NVPTX 对非 16B 对齐拆标量，功能正确，性能收益
    // 需 16B 对齐数组——后续优化）。
    if (name == "store4") {
        if (e.args.size() < 3) {
            diag_.error(e.range, "store4 expects (float[] a, long i, float4 v)");
            return nullptr;
        }
        auto* v = generateExpr(*e.args[2]);
        builder_.CreateAlignedStore(v, p, llvm::Align(4));
        return nullptr;
    }
    return builder_.CreateAlignedLoad(f4, p, llvm::Align(4), "v4");
}

llvm::Value* CodeGen::generateCall(const CallExpr& e) {
    // §3.1 CPU 回退：kernel.sync() 为空操作；§3.4 shfl/块归约无 warp 语义
    // → 返回实参 0（v 本身）（真实 GPU 路径在 emitKernelExpr）。
    if (gpu_cpu_fallback_ && e.callee->kind == ExprKind::MemberAccess) {
        auto& kma = static_cast<const MemberAccessExpr&>(*e.callee);
        if (kma.object->kind == ExprKind::Identifier &&
            static_cast<const IdentifierExpr&>(*kma.object).name == "kernel") {
            if (kma.member_name == "sync")
                return nullptr;
            if (kma.member_name == "shfl_down" ||
                kma.member_name == "block_reduce_sum" ||
                kma.member_name == "block_reduce_max") {
                if (!e.args.empty()) return generateExpr(*e.args[0]);
                return nullptr;
            }
            // §P5 ② kernel.printk / kernel.assert CPU 回退：宿主 printf / 硬失败
            //（调试在双模式均可用，输出与 GPU staging 对齐）。
            if (kma.member_name == "printk" || kma.member_name == "assert")
                return emitCpuPrintk(e, kma.member_name == "assert");
        }
    }
    llvm::Value* r = generateCallImpl(e);
    // ARC: a call that returns a class / class-array reference hands the caller
    // a +1 it OWNS (the callee's retain-at-return / `return new` transfer). If
    // the result is stored into a slot the store site consumes it; if it is
    // used inline (e.g. `obj.take().get()`) and discarded, the statement-end
    // flush releases it. Without this, inline calls leaked the +1.
    if (r && r->getType()->isPointerTy() && callReturnsArcRef(e))
        arcPushTemp(r);
    return r;
}

// §P5 ② kernel.printk / kernel.assert CPU 回退：宿主 printf / 硬失败。
// kernel.printk(fmt, v...) → myp_printf("kernel[gid=%lld] " + fmt + "\n", gid, v...)；
// kernel.assert(cond, fmt, v...) → !cond 时 myp_printf("kernel[gid=%lld] ASSERT FAIL: "
// + fmt + "\n", gid, v...) + myp_assert_abort（exit 1）。
// 前缀与 GPU staging flush 一致（kernel[gid=<循环变量>]）→ 双模式输出逐字节相同。
// args 按实际类型传（%d→i32，%g→double；long→i64 配 %ld）。
llvm::Value* CodeGen::emitCpuPrintk(const CallExpr& e, bool is_assert) {
    int val_start = is_assert ? 2 : 1;   // 值参起点
    std::string fmt = "(fmt?)";
    if ((size_t)val_start - 1 < e.args.size() &&
        e.args[val_start - 1]->kind == ExprKind::StringLiteral) {
        fmt = static_cast<const StringLiteralExpr&>(*e.args[val_start - 1]).value;
    }
    std::string full = (is_assert ? "kernel[gid=%lld] ASSERT FAIL: "
                                  : "kernel[gid=%lld] ") + fmt + "\n";

    // 当前循环变量值 = gid（CPU 回退的 kernel.gid 模拟）
    llvm::Value* gid = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0);
    if (!gpu_cpu_loop_var_.empty()) {
        auto* nv = getNamedValue(gpu_cpu_loop_var_);
        if (nv) gid = builder_.CreateLoad(builder_.getInt64Ty(), nv);
    }

    auto build_call = [&]() {
        std::vector<llvm::Value*> cargs;
        cargs.push_back(builder_.CreateGlobalStringPtr(full, "cpa_fmt"));
        cargs.push_back(gid);
        int nval = (int)e.args.size() - val_start;
        if (nval > 3) nval = 3;
        for (int i = 0; i < nval; i++) {
            auto* av = generateExpr(*e.args[val_start + i]);
            if (av) cargs.push_back(av);
        }
        builder_.CreateCall(runtime_printf_, cargs);
    };

    if (is_assert) {
        auto* cond = generateExpr(*e.args[0]);
        if (!cond) cond = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
        auto* c0 = builder_.CreateICmpEQ(cond,
            llvm::ConstantInt::get(cond->getType(), 0), "cpa_c0");
        auto* fail_bb = llvm::BasicBlock::Create(ctx_, "cpa_fail",
            builder_.GetInsertBlock()->getParent());
        auto* cont_bb = llvm::BasicBlock::Create(ctx_, "cpa_cont",
            builder_.GetInsertBlock()->getParent());
        builder_.CreateCondBr(c0, fail_bb, cont_bb);
        builder_.SetInsertPoint(fail_bb);
        build_call();
        if (runtime_assert_abort_)
            builder_.CreateCall(runtime_assert_abort_,
                {builder_.CreateGlobalStringPtr(fmt, "cpa_msg")});
        builder_.CreateBr(cont_bb);
        builder_.SetInsertPoint(cont_bb);
        return nullptr;
    }
    build_call();
    return nullptr;
}

std::string CodeGen::memberObjectClassName(const Expr& obj) {
    if (obj.kind == ExprKind::Identifier) {
        auto& oi = static_cast<const IdentifierExpr&>(obj);
        auto v = var_class_map_.find(oi.name);
        if (v != var_class_map_.end()) return v->second;
        auto s = var_struct_map_.find(oi.name);
        if (s != var_struct_map_.end()) return s->second;
        if (findClass(oi.name)) return oi.name;   // static call（O(1)）
        // Bare property inside a method (`functions_` → `this.functions_`):
        // resolve the property's concrete class (e.g. ArrayList_AstFunction_inst)
        // so `functions_.get(i).dump(...)` chains dispatch to the property's
        // element class instead of the name-only fallback.
        if (!current_class_name_.empty()) {
            const ClassDecl* c = findClass(current_class_name_);
            if (c) {
                for (auto& p : c->properties) {
                    if (p.name == oi.name && !p.type.class_name.empty())
                        return mangleConcreteTypeNode(p.type);
                }
            }
        }
        return "";
    }
    if (obj.kind == ExprKind::ThisExpr) return current_class_name_;
    if (obj.kind == ExprKind::Subscript) {
        auto& ss = static_cast<const SubscriptExpr&>(obj);
        if (ss.array->kind == ExprKind::Identifier) {
            auto eit = array_elem_class_map_.find(
                static_cast<const IdentifierExpr&>(*ss.array).name);
            if (eit != array_elem_class_map_.end()) return eit->second;
        }
        return "";
    }
    if (obj.kind == ExprKind::NewExpr) {
        auto& ne = static_cast<const NewExpr&>(obj);
        std::string cn = ne.class_name;
        if (!ne.type_args.empty()) {
            cn = ne.class_name;
            for (auto& ta : ne.type_args) cn += "_" + mangleConcreteTypeNode(ta);
            cn += "_inst";
        }
        return cn;
    }
    // Property access (`this.list_` / `obj.list_`): resolve the property's
    // concrete class so `this.list_.method()` and `obj.list_.method()` chains
    // dispatch to the property's type (e.g. ArrayList_AstFunction_inst) instead
    // of the name-only fallback picking the first class with that method name.
    // Without this, `functions_.get(i).dump(...)` inside a dump method resolved
    // `.dump` to the first class defining `dump` (AstParam) — wrong method.
    if (obj.kind == ExprKind::MemberAccess) {
        auto& ma = static_cast<const MemberAccessExpr&>(obj);
        std::string owner;
        if (ma.object->kind == ExprKind::ThisExpr) {
            owner = current_class_name_;
        } else if (ma.object->kind == ExprKind::Identifier) {
            auto& oid = static_cast<const IdentifierExpr&>(*ma.object);
            auto v = var_class_map_.find(oid.name);
            if (v != var_class_map_.end()) owner = v->second;
            else if (findClass(oid.name)) owner = oid.name;   // static（O(1)）
        }
        // Struct field holding a class ref: `node.next.method()`. The object is
        // a struct local (no var_class_map_ entry), but sema recorded its type
        // in resolved_object_class (e.g. "Node") — resolve the field's type so
        // `a.next.get()` dispatches to Option_Node_inst, not the Option
        // template (BUG-004).
        if (owner.empty())
            owner = ma.resolved_object_class;
        if (!owner.empty()) {
            const ClassDecl* c = findClass(owner);
            if (c) {
                for (auto& prop : c->properties) {
                    if (prop.name == ma.member_name) {
                        if (prop.type.class_name.empty()) return "";
                        return mangleConcreteTypeNode(prop.type);
                    }
                }
            }
            // Struct field (public): `s.field` where field is a class ref.
            if (const StructDecl* sd = findStruct(owner)) {
                for (auto& p : sd->properties) {
                    if (p.name == ma.member_name) {
                        if (p.type.class_name.empty()) return "";
                        return mangleConcreteTypeNode(p.type);
                    }
                }
            }
        }
        return "";
    }
    if (obj.kind == ExprKind::Call)
        return callReturnClassName(static_cast<const CallExpr&>(obj));
    return "";
}

std::string CodeGen::callReturnClassName(const CallExpr& e) {
    // Delegate to callReturnTypeNode (which resolves class AND struct methods,
    // and TU/FFI calls). Return the mangled concrete class name for the return
    // type (e.g. Option<int> → Option_int_inst), or "" if not a class type.
    const TypeNode* rt = callReturnTypeNode(e);
    if (!rt || rt->class_name.empty()) return "";
    if (!rt->type_args.empty()) return mangleConcreteTypeNode(*rt);
    return rt->class_name;
}

const TypeNode* CodeGen::callReturnTypeNode(const CallExpr& e) {
    if (e.callee->kind == ExprKind::MemberAccess) {
        auto& ma = static_cast<const MemberAccessExpr&>(*e.callee);
        // Prefer sema's resolved object class (reliable) over the codegen
        // var maps (flat, can be stale across scopes).
        std::string cls = !ma.resolved_object_class.empty()
            ? ma.resolved_object_class : memberObjectClassName(*ma.object);
        if (cls.empty()) return nullptr;
        const ClassDecl* c = findClass(cls);   // O(1)
        if (c) {
            for (auto& a : c->actions)
                if (a.name == ma.member_name) return &a.return_type;
            for (auto& a : c->static_actions)
                if (a.name == ma.member_name) return &a.return_type;
            for (auto& f : c->functions)
                if (f.name == ma.member_name) return &f.return_type;
        }
        // Struct method call: `h.getItems().size()` — resolve the struct
        // method's return type so `.size()` dispatches to the concrete generic
        // instance (ArrayList_int_inst), not the template (ArrayList).
        if (const StructDecl* sd = findStruct(cls)) {
            for (auto& f : sd->functions)
                if (f.name == ma.member_name) return &f.return_type;
        }
        return nullptr;
    }
    if (e.callee->kind == ExprKind::Identifier) {
        // Named call: check TU functions first, then FFI declarations (whose
        // return_type may be a counted string / array / slice).
        std::string name = !e.resolved_call_name.empty()
            ? e.resolved_call_name : static_cast<const IdentifierExpr&>(*e.callee).name;
        if (current_tu_) {
            for (auto& f : current_tu_->functions)
                if (f.name == name) return &f.return_type;
            for (auto& ff : current_tu_->ffis)
                if (ff.name == name) return &ff.return_type;
        }
        return nullptr;
    }
    return nullptr;
}

bool CodeGen::callReturnsArcRef(const CallExpr& e) {
    // A call result is pushed as a statement temp (reclaimed if discarded),
    // and `return <call>` skips retain-at-return. Both require the caller's
    // store path to CONSUME the temp. Class/interface/string var-decls and
    // property stores all call arcConsumeTemp, so those are safe. Slice and
    // dynamic-array var-decls do NOT consume temps — including them here would
    // push an unconsumed temp and double-free the backing at statement end.
    // (Slices/dynamic arrays keep their pre-M8 behavior: no temp, and a
    // `return sliceCall()` retains at return — a known small leak, not UAF.)
    const TypeNode* rt = callReturnTypeNode(e);
    if (!rt) return false;
    // M-FN-2: function-returning calls too — the caller's function-value store
    // consumes the temp (like class refs), so `return f()` transferring rc=1 is
    // safe and skips the extra retain-at-return.
    return isArcRefType(*rt) || isStringType(*rt) || rt->isFunction();
}

// M8 structs: a struct-returning call whose fields hold ARC refs. Used ONLY by
// `return <call>` (arc_skip_retain_return_) so the return doesn't double-retain
// a +1 value. NOT used by generateCall temp-push (structs aren't single refs).
bool CodeGen::callReturnsArcStruct(const CallExpr& e) {
    const TypeNode* rt = callReturnTypeNode(e);
    if (!rt || rt->class_name.empty()) return false;
    const StructDecl* sd = findStruct(rt->class_name);
    return sd && isArcFieldType(*rt);
}

// M8: a slice or dynamic-array-returning call. `return <call>` skips the
// retain-at-return so the fresh rc=1 backing transfers to the caller instead of
// being +1'd and then dropped to 1 by the caller's release (a leak per call).
bool CodeGen::callReturnsArcSliceOrArray(const CallExpr& e) {
    const TypeNode* rt = callReturnTypeNode(e);
    if (!rt) return false;
    if (rt->class_name == "slice") return true;
    return isCountedArrayType(*rt);   // dynamic T[]
}

// M8: does this expression yield a string value? Used to detect string
// concatenation (a fresh counted string) in isFreshArcExpr.
bool CodeGen::exprIsString(const Expr& e) {
    switch (e.kind) {
        case ExprKind::StringLiteral: return true;
        case ExprKind::Identifier: {
            auto& id = static_cast<const IdentifierExpr&>(e);
            // String vars are pointer-typed slots NOT in var_class_map_ (a
            // class ref is also a pointer, but its name IS in var_class_map_).
            if (var_class_map_.find(id.name) != var_class_map_.end()) return false;
            // Dynamic arrays (T[]) are also pointer-typed slots — exclude them
            // (their < > / == compares addresses, not string content).
            if (array_elem_types_.find(id.name) != array_elem_types_.end()) return false;
            auto* v = getNamedValue(id.name);
            if (auto* ai = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                // A pointer-typed non-class local is a string. Slice/interface/
                // function/struct locals are struct-typed, not plain pointers,
                // but exclude them explicitly for safety.
                if (!ai->getAllocatedType()->isPointerTy()) return false;
                if (isArcSliceLocal(ai) || isArcFunctionLocal(ai)) return false;
                if (arc_struct_slot_types_.find(ai) != arc_struct_slot_types_.end()) return false;
                return true;
            }
            auto* t = getNamedValueType(id.name);
            return t && t->isPointerTy();
        }
        case ExprKind::BinaryOp:
            return isStringConcatExpr(static_cast<const BinaryOpExpr&>(e));
        case ExprKind::Call: {
            const TypeNode* rt = callReturnTypeNode(static_cast<const CallExpr&>(e));
            return rt && isStringType(*rt);
        }
        default: return false;
    }
}

// M8: does this expression resolve to a string, accounting for generic
// type-param placeholders? Sema annotates resolved_kind==String on the shared
// template body for everything EXCEPT plain generic params (their resolved_kind
// is the placeholder default, e.g. Int). For those, fall back to exprIsString
// (alloca-type based). This is what string ==/</>/< must use: the bare
// exprIsString misses member access, subscripts and builtin calls (str(), …),
// which sema DID resolve to String.
bool CodeGen::exprResolvedString(const Expr& e) {
    if (e.resolved_kind == TypeKind::String) return true;
    return exprIsString(e);
}

bool CodeGen::isStringConcatExpr(const Expr& e) {
    if (e.kind != ExprKind::BinaryOp) return false;
    auto& b = static_cast<const BinaryOpExpr&>(e);
    if (b.op != BinaryOpKind::Add) return false;
    return exprIsString(*b.lhs) || exprIsString(*b.rhs);
}

// P1（docs/type_system_design §6.1）：把标量操作数转成字符串用于拼接。指针操作数
// （已是字符串）借用直通。仅对转换出的标量返回 FRESH 计数字符串（调用方用后释放）。
// 修 D2（f32 拼接编译崩溃）、D3（无符号拼成有符号）、D4（byte/short 符号不一致）；
// char 按 §4.1 语义输出字符本身（非码值）。src 提供无符号/char 判定。
llvm::Value* CodeGen::stringifyForConcat(llvm::Value* v, const Expr* src) {
    if (!v) return v;
    if (v->getType()->isPointerTy()) return v;   // already a string (borrowed)
    auto* ptr_type = llvm::PointerType::get(ctx_, 0);
    // bool (i1) → myp_to_string_bool(i32)（zext 0/1 → "true"/"false"）
    if (v->getType()->isIntegerTy(1)) {
        auto* ext = builder_.CreateZExt(v, llvm::Type::getInt32Ty(ctx_));
        auto* fn = module_->getFunction("myp_to_string_bool");
        if (!fn) {
            auto* ft = llvm::FunctionType::get(ptr_type, {llvm::Type::getInt32Ty(ctx_)}, false);
            fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_to_string_bool", module_.get());
        }
        return builder_.CreateCall(fn, {ext});
    }
    bool src_unsigned = src &&
        (src->resolved_kind == TypeKind::UByte ||
         src->resolved_kind == TypeKind::UShort ||
         src->resolved_kind == TypeKind::UInt ||
         src->resolved_kind == TypeKind::ULong);
    bool src_is_char = src && src->resolved_kind == TypeKind::Char;
    // char（i8）→ 字符本身（myp_chr），修 D10 的 "x" + 'A' == "65" 语义坑
    if (src_is_char && v->getType()->isIntegerTy(8)) {
        auto* code = builder_.CreateZExt(v, llvm::Type::getInt32Ty(ctx_));
        auto* fn = module_->getFunction("myp_chr");
        if (!fn) {
            auto* ft = llvm::FunctionType::get(ptr_type, {llvm::Type::getInt32Ty(ctx_)}, false);
            fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_chr", module_.get());
        }
        return builder_.CreateCall(fn, {code});
    }
    auto fn_name = std::string("myp_to_string_");
    if (v->getType()->isIntegerTy()) {
        if (src_unsigned) {
            // 无符号：窄型 ZExt 到 i32，再 u32/u64 十进制（修 D3：0xFFFFFFFFu → "4294967295"）
            if (v->getType()->isIntegerTy(8) || v->getType()->isIntegerTy(16))
                v = builder_.CreateZExt(v, llvm::Type::getInt32Ty(ctx_));
            fn_name += v->getType()->isIntegerTy(32) ? "u32" : "u64";
        } else {
            // 有符号：byte/short 一律 SExt 到 i32（修 D4：byte(-1) → "-1"，非 "255"）
            if (v->getType()->isIntegerTy(8) || v->getType()->isIntegerTy(16))
                v = builder_.CreateSExt(v, llvm::Type::getInt32Ty(ctx_));
            fn_name += v->getType()->isIntegerTy(32) ? "i32" : "i64";
        }
    } else if (v->getType()->isFloatTy()) {
        fn_name += "float";               // 修 D2：f32 → myp_to_string_float(%g)
    } else if (v->getType()->isDoubleTy()) {
        fn_name += "double";
    } else {
        fn_name += "i32";
    }
    auto* conv_fn = module_->getFunction(fn_name);
    if (!conv_fn) {
        auto* ft = llvm::FunctionType::get(ptr_type, {v->getType()}, false);
        conv_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn_name, module_.get());
    }
    return builder_.CreateCall(conv_fn, {v});
}

llvm::Value* CodeGen::generateCallImpl(const CallExpr& e) {
    // P0 bitcast<T,U>(x)：位保持重解释（docs/type_system_design §5.2）。sema 已
    // 校验类型实参个数/同宽；这里按最后类型实参（目标）生成位转换。
    if (e.callee->kind == ExprKind::Identifier) {
        auto& bid = static_cast<const IdentifierExpr&>(*e.callee);
        if (bid.name == "bitcast" && !e.call_type_args.empty())
            return generateBitcast(e);
        // P1 bytes(s) / str(bytes)（docs §6.3）：sema 只在实参类型匹配时拦截，
        // 这里同名直接走内建（用户同名函数由 sema 已解析到别的路径）。
        if (bid.name == "bytes" || bid.name == "str")
            return generateBytesStr(e, bid.name);
        // §5.1 bytesOf(bitvector<N>) → ubyte[]
        if (bid.name == "bytesOf")
            return generateBytesOf(e);
        // P2 parse* 全族（docs §6.2）
        if (bid.name == "parseInt" || bid.name == "parseLong" ||
            bid.name == "parseUint" || bid.name == "parseUlong" ||
            bid.name == "parseFloat" || bid.name == "parseDouble")
            return generateParse(e, bid.name);
        // P4 parseIntOpt(s) → (value:int, ok:bool)
        if (bid.name == "parseIntOpt")
            return generateParseOpt(e);
        // §4.2 P3 checked 溢出变体：checkedAdd/checkedMul → overflow intrinsic
        if (bid.name == "checkedAdd" || bid.name == "checkedMul")
            return generateCheckedOp(e, bid.name);
    }
    // M-FN-2 named lambda self-recursion: sema marked `<hidden>__self`; call the
    // lambda's own tramp with `this` as the closure — `tramp(this, args...)`.
    const std::string SELF_SUFFIX = "__self";
    if (!e.resolved_call_name.empty() &&
        e.resolved_call_name.size() > SELF_SUFFIX.size() &&
        e.resolved_call_name.compare(e.resolved_call_name.size() - SELF_SUFFIX.size(),
                                     SELF_SUFFIX.size(), SELF_SUFFIX) == 0) {
        std::string base = e.resolved_call_name.substr(0, e.resolved_call_name.size() - SELF_SUFFIX.size());
        auto* tramp = module_->getFunction(base + "_tramp");
        if (tramp) {
            std::vector<llvm::Value*> args;
            auto* ta = getNamedValue("this");
            if (ta) args.push_back(builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ta));
            for (auto& a : e.args) args.push_back(generateExpr(*a));
            auto* ft = tramp->getFunctionType();
            for (size_t i = 0; i < args.size() && i < ft->getNumParams(); ++i) {
                auto* expected = ft->getParamType(i);
                if (args[i]->getType() != expected)
                    args[i] = convertIntegerValue(builder_, args[i], expected, e.args[i].get());
            }
            bool isv = tramp->getReturnType()->isVoidTy();
            return builder_.CreateCall(tramp->getFunctionType(), tramp, args,
                isv ? "" : "calltmp");
        }
    }
    // Generic function call: sema monomorphized the target to a concrete
    // instance (e.g. id_int_inst); call it directly with arg conversions.
    if (!e.resolved_call_name.empty()) {
        auto* fn = module_->getFunction(e.resolved_call_name);
        if (fn) {
            std::vector<llvm::Value*> args;
            for (auto& a : e.args) args.push_back(generateExpr(*a));
            auto* ft = fn->getFunctionType();
            for (size_t i = 0; i < args.size() && i < ft->getNumParams(); ++i) {
                auto* expected = ft->getParamType(i);
                if (args[i]->getType() != expected)
                    args[i] = convertIntegerValue(builder_, args[i], expected, e.args[i].get());
            }
            bool isv = fn->getReturnType()->isVoidTy();
            return builder_.CreateCall(fn->getFunctionType(), fn, args,
                isv ? "" : "calltmp");
        }
        diag_.error(e.range, "cannot call generic function '" + e.resolved_call_name + "'");
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    }
    // First-class function value call: fn(args) where fn is a function-typed var.
    // Indirect call through the fat pointer's call_fn (tramp): call_fn(closure, args).
    if (e.callee->kind == ExprKind::Identifier) {
        auto& fid = static_cast<const IdentifierExpr&>(*e.callee);
        auto fit = func_val_types_.find(fid.name);
        if (fit != func_val_types_.end()) {
            auto* va = getNamedValue(fid.name);
            if (va) {
                const TypeInfo& ft = fit->second;
                auto* fval = builder_.CreateLoad(getFunctionValueType(), va, fid.name);
                auto* closure = builder_.CreateExtractValue(fval, 0);
                auto* call_fn = builder_.CreateExtractValue(fval, 1);
                std::vector<llvm::Type*> pts;
                pts.push_back(llvm::PointerType::get(ctx_, 0)); // self
                for (auto& pt : ft.param_types) pts.push_back(getLLVMType(pt));
                TypeInfo void_rt(TypeKind::Void);
                auto* rty = getLLVMType(ft.return_type ? *ft.return_type : void_rt);
                auto* callt = llvm::FunctionType::get(rty, pts, false);
                std::vector<llvm::Value*> args;
                args.push_back(closure);
                for (auto& a : e.args) args.push_back(generateExpr(*a));
                for (size_t i = 1; i < args.size() && i < callt->getNumParams(); i++) {
                    auto* expected = callt->getParamType(i);
                    if (args[i]->getType() != expected)
                        args[i] = convertIntegerValue(builder_, args[i], expected, e.args[i - 1].get());
                }
                auto* fnp = builder_.CreateBitCast(call_fn, llvm::PointerType::get(callt, 0));
                bool isv = rty->isVoidTy();
                return builder_.CreateCall(callt, fnp, args, isv ? "" : "calltmp");
            }
        }
    }
    // Struct 函数式构造：StructName(args) → 栈临时 + 构造器，返回 struct 值
    if (!e.resolved_struct_ctor.empty() && !e.resolved_struct_type.empty()) {
        auto* st_ctor = module_->getFunction(e.resolved_struct_ctor);
        auto* st_type = getStructType(e.resolved_struct_type);
        if (st_ctor && st_type) {
            auto* tmp = builder_.CreateAlloca(st_type);
            std::vector<llvm::Value*> ctor_args;
            ctor_args.push_back(tmp);
            auto* ft = st_ctor->getFunctionType();
            size_t idx = 1;
            for (auto& a : e.args) {
                llvm::Value* v = generateExpr(*a);
                // 隐式类型转换：实参 → 形参（int→double 等，与 generateCall 一致；
                // uint32：无符号实参加宽用 ZExt）
                if (idx < ft->getNumParams()) {
                    auto* expected = ft->getParamType(idx);
                    if (v->getType() != expected)
                        v = convertIntegerValue(builder_, v, expected, a.get());
                }
                ctor_args.push_back(v);
                idx++;
            }
            builder_.CreateCall(st_ctor, ctor_args);
            return builder_.CreateLoad(st_type, tmp);
        }
    }
    // slice.size() / slice.length() / slice.data()
    if (e.callee->kind == ExprKind::MemberAccess) {
        auto& ma = static_cast<const MemberAccessExpr&>(*e.callee);
        if (ma.object->kind == ExprKind::Identifier) {
            auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
            auto sit = var_slice_types_.find(oi.name);
            if (sit != var_slice_types_.end()) {
                auto* va = getNamedValue(oi.name);
                if (va) {
                    auto* sval = builder_.CreateLoad(getLLVMType(sit->second), va, oi.name);
                    if (ma.member_name == "size" || ma.member_name == "length") {
                        auto* len = builder_.CreateExtractValue(sval, 1);
                        return builder_.CreateTrunc(len, llvm::Type::getInt32Ty(ctx_));
                    }
                    if (ma.member_name == "data") {
                        return builder_.CreateExtractValue(sval, 0);
                    }
                }
            }
        }
    }
    llvm::Value* callee = nullptr;
    bool is_method = false;
    llvm::Value* mthis = nullptr;

    if (e.callee->kind == ExprKind::MemberAccess) {
        auto& ma = static_cast<const MemberAccessExpr&>(*e.callee);

        if (ma.object->kind == ExprKind::Identifier) {
            auto& object_id = static_cast<const IdentifierExpr&>(*ma.object);
            std::string function_name = object_id.name + "_" + ma.member_name;
            auto static_action = is_static_action_.find(function_name);
            if (static_action != is_static_action_.end() && static_action->second) {
                callee = module_->getFunction(function_name);
                if (callee) {
                    is_method = true;
                    goto call_ready;
                }
            }
        }

        // Enum variant construction: Option.Some(42) → enum struct {disc, payload}
        if (ma.object->kind == ExprKind::Identifier) {
            auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
            const EnumDecl* en = findEnum(oi.name);   // O(1)
            if (en) {
                for (size_t vi = 0; vi < en->variants.size(); vi++) {
                    if (en->variants[vi].name == ma.member_name) {
                        std::vector<llvm::Value*> arg_vals;
                        for (auto& a : e.args)
                            arg_vals.push_back(generateExpr(*a));
                        return buildEnumVariant(en->name, vi, arg_vals);
                    }
                }
            }
        }

        // Generalized interface dispatch: any object expression that evaluates
        // to a {data, vtable} fat pointer (interface var / interface property
        // `this.iface` / interface array element) → vtable dispatch. Bug fix:
        // only identifier-vars were dispatched before; interface properties
        // (root_.method()) fell to the concrete direct-call path and passed the
        // fat pointer as `this` → invalid IR. Safe to evaluate: these are pure
        // loads (no side effects). Non-interface objects evaluate to a ptr and
        // fall through to the concrete paths below.
        if (ma.object->kind == ExprKind::Identifier ||
            ma.object->kind == ExprKind::MemberAccess ||
            ma.object->kind == ExprKind::Subscript ||
            ma.object->kind == ExprKind::ThisExpr) {
            // 静态类调用（Cli.run() / Console.writeLine()）：对象是类名而非值，
            // 求值会报 undefined variable —— 跳过，交给下方静态解析路径。
            if (ma.object->kind == ExprKind::Identifier) {
                auto& oid = static_cast<const IdentifierExpr&>(*ma.object);
                if (findClass(oid.name)) goto skip_generalized_iface;   // O(1)
            }
            auto* obj_val = generateExpr(*ma.object);
            if (obj_val && isInterfaceFatType(obj_val->getType())) {
                auto* data = builder_.CreateExtractValue(obj_val, 0);
                auto* vt = builder_.CreateExtractValue(obj_val, 1);
                auto* method = findInterfaceMethod(
                    ma.resolved_object_class, ma.member_name);
                if (method) {
                    // devirt：接口变量（Identifier 对象）具体类静态已知
                    // （var_class_map_ 声明时记）且从未被重赋值 → 直接调用具体类
                    // 方法（或 trait 默认实现），跳过 vtable 间接调用，让 LLVM
                    // 内联（常量折叠/向量化复利）。返回类型从具体函数取（含
                    // assoc 关联类型的真实类型）。
                    std::string devirt_cls;
                    if (ma.object->kind == ExprKind::Identifier) {
                        auto& oid = static_cast<const IdentifierExpr&>(*ma.object);
                        auto vit = var_class_map_.find(oid.name);
                        if (vit != var_class_map_.end() &&
                            !iface_reassigned_.count(oid.name))
                            devirt_cls = vit->second;
                    }
                    if (!devirt_cls.empty()) {
                        std::string dfn = devirt_cls + "_" + ma.member_name;
                        llvm::Function* direct = module_->getFunction(dfn);
                        if (!direct)
                            direct = findInterfaceDefault(devirt_cls, ma.member_name);
                        if (direct) {
                            std::vector<llvm::Value*> call_args;
                            call_args.push_back(data);
                            for (auto& arg : e.args)
                                call_args.push_back(generateExpr(*arg));
                            return builder_.CreateCall(direct, call_args);
                        }
                    }
                    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
                    auto* func_gep = builder_.CreateGEP(ptr_ty, vt,
                        {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), method->index)},
                        "iface_method");
                    auto* func_ptr = builder_.CreateLoad(ptr_ty, func_gep, "iface_fn");
                    auto* ret_ty = ifaceDispatchReturnType(ma, method);
                    std::vector<llvm::Value*> call_args;
                    call_args.push_back(data);
                    for (auto& arg : e.args) call_args.push_back(generateExpr(*arg));
                    upcastIfaceCallArgs(call_args, e, method);   // BUG-034: 接口形参 → fat
                    std::vector<llvm::Type*> param_types;
                    param_types.push_back(ptr_ty);
                    for (size_t ai = 1; ai < call_args.size(); ai++)
                        param_types.push_back(call_args[ai]->getType());
                    auto* ft = llvm::FunctionType::get(ret_ty, param_types, false);
                    return builder_.CreateCall(ft, func_ptr, call_args);
                }
            }
        }
        skip_generalized_iface:

        // Find the correct class by matching function name AND arg count (fallback)
        std::string best_class;

        // First, try to resolve by variable→class map for identifier objects
        if (ma.object->kind == ExprKind::Identifier) {
            auto& oi = static_cast<const IdentifierExpr&>(*ma.object);

            // Check if this is an interface variable → vtable dispatch
            auto* obj_val = getNamedValue(oi.name);
            if (obj_val) {
                llvm::Type* obj_ty = nullptr;
                if (auto* ovi = llvm::dyn_cast<llvm::AllocaInst>(obj_val)) {
                    obj_ty = ovi->getAllocatedType();
                }
                if (obj_ty && obj_ty->isStructTy()) {
                    auto* st = llvm::cast<llvm::StructType>(obj_ty);
                    std::string st_name = st->getName().str();
                    // {ptr, ptr} = interface fat pointer
                    if (st->getNumElements() == 2 && st->getElementType(0)->isPointerTy()
                        && st->getElementType(1)->isPointerTy()) {
                        // Interface dispatch: load vtable, index method, call indirect
                        auto* data_ptr = builder_.CreateStructGEP(st, obj_val, 0);
                        auto* data = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), data_ptr, "iface_data");
                        auto* vt_ptr = builder_.CreateStructGEP(st, obj_val, 1);
                        auto* vt = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), vt_ptr, "iface_vt");

                        // Find method index in the interface
                        auto* method = findInterfaceMethod(
                            ma.resolved_object_class, ma.member_name);
                        if (method) {
                            // Vtable is [N x ptr], load function pointer by index
                            auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
                            auto* func_gep = builder_.CreateGEP(ptr_ty, vt,
                                {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), method->index)},
                                "iface_method");
                            auto* func_ptr = builder_.CreateLoad(ptr_ty, func_gep, "iface_fn");

                            // Determine return type: resolve assoc-type methods from
                            // the known concrete class (BUG-017), else interface decl.
                            auto* ret_ty = ifaceDispatchReturnType(ma, method);

                            // Build call args: instance + explicit args
                            std::vector<llvm::Value*> call_args;
                            call_args.push_back(data); // 'this' pointer
                            for (auto& arg : e.args)
                                call_args.push_back(generateExpr(*arg));
                            upcastIfaceCallArgs(call_args, e, method);   // BUG-034: 接口形参 → fat

                            // Build parameter types
                            std::vector<llvm::Type*> param_types;
                            param_types.push_back(ptr_ty);
                            for (size_t ai = 1; ai < call_args.size(); ai++)
                                param_types.push_back(call_args[ai]->getType());

                            auto* ft = llvm::FunctionType::get(ret_ty, param_types, false);
                            auto* result = builder_.CreateCall(ft, func_ptr, call_args);
                            return result;
                        }
                    }
                }
            }

            auto vit = var_class_map_.find(oi.name);
            if (vit != var_class_map_.end()) {
                std::string fn = vit->second + "_" + ma.member_name;
                callee = module_->getFunction(fn);
                // trait 默认实现：类未覆盖但接口方法带默认体 → 用本类默认函数
                if (!callee) callee = findInterfaceDefault(vit->second, ma.member_name);
                if (callee) {
                    mthis = generateExpr(*ma.object);
                    is_method = true;
                    goto call_ready;
                }
            } else if (!current_class_name_.empty() && current_tu_) {
                // 本类属性对象：this.prop.method()（如 button.key()，button 是
                // Button 属性）。此前只按方法名兜底匹配，Label/Button 同名方法
                // （key/draw/layout）会解析到先声明的类 → 调错方法。按属性的
                // 具体类解析。
                const ClassDecl* cur = findClass(current_class_name_);   // O(1)
                if (cur) {
                    for (auto& p : cur->properties) {
                        if (p.name != oi.name || p.type.class_name.empty()) continue;
                        std::string cn = p.type.class_name;
                        if (!p.type.type_args.empty()) {
                            std::string m = cn;
                            for (auto& ta : p.type.type_args)
                                m += "_" + mangleConcreteTypeNode(ta);
                            cn = m + "_inst";
                        }
                        std::string fn = cn + "_" + ma.member_name;
                        callee = module_->getFunction(fn);
                        if (!callee) callee = findInterfaceDefault(cn, ma.member_name);
                        if (callee) {
                            mthis = generateExpr(*ma.object);
                            is_method = true;
                            goto call_ready;
                        }
                    }
                }
            }
        }

        // Interface value from a non-identifier expression (e.g. interface array
        // element children[i].method()): the expression evaluates to a
        // {data, vtable} fat pointer → vtable dispatch. Bug fix: this previously
        // fell through to the concrete-class direct-call path and passed the fat
        // pointer as `this`, producing invalid IR (LLVM verify failure).
        if (!callee && ma.object->kind == ExprKind::Subscript) {
            auto* obj_val = generateExpr(*ma.object);
            if (obj_val && isInterfaceFatType(obj_val->getType())) {
                auto* data = builder_.CreateExtractValue(obj_val, 0);
                auto* vt = builder_.CreateExtractValue(obj_val, 1);
                auto* method = findInterfaceMethod(
                    ma.resolved_object_class, ma.member_name);
                if (method) {
                    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
                    auto* func_gep = builder_.CreateGEP(ptr_ty, vt,
                        {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), method->index)},
                        "iface_method");
                    auto* func_ptr = builder_.CreateLoad(ptr_ty, func_gep, "iface_fn");
                    auto* ret_ty = ifaceDispatchReturnType(ma, method);
                    std::vector<llvm::Value*> call_args;
                    call_args.push_back(data);
                    for (auto& arg : e.args) call_args.push_back(generateExpr(*arg));
                    upcastIfaceCallArgs(call_args, e, method);   // BUG-034: 接口形参 → fat
                    std::vector<llvm::Type*> param_types;
                    param_types.push_back(ptr_ty);
                    for (size_t ai = 1; ai < call_args.size(); ai++)
                        param_types.push_back(call_args[ai]->getType());
                    auto* ft = llvm::FunctionType::get(ret_ty, param_types, false);
                    return builder_.CreateCall(ft, func_ptr, call_args);
                }
            }
        }

        // Struct method call: v.method()
        // Check struct type FIRST, before any class name fallback,
        // to avoid resolving h1.init() to Tally::init.
        if (!callee && ma.object->kind == ExprKind::Identifier) {
            auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
            auto* oa = getNamedValue(oi.name);
            if (oa) {
                llvm::StructType* st_for_name_2 = nullptr;
                if (auto* oai = llvm::dyn_cast<llvm::AllocaInst>(oa)) {
                    auto* oat = oai->getAllocatedType();
                    if (oat->isStructTy()) st_for_name_2 = llvm::cast<llvm::StructType>(oat);
                } else {
                    auto* nty = getNamedValueType(oi.name);
                    if (nty && nty->isStructTy()) st_for_name_2 = llvm::cast<llvm::StructType>(nty);
                }
                if (st_for_name_2) {
                    std::string fn = "struct_" + st_for_name_2->getName().str() + "_" + ma.member_name;
                    callee = module_->getFunction(fn);
                    if (callee) {
                        mthis = oa;
                        is_method = true;
                    }
                }
            }
        }
        // Chained struct method call: a.b.method() or ClassName.prop.method()
        // Resolve the struct type from the object's property type
        if (!callee && ma.object->kind == ExprKind::MemberAccess) {
            auto& inner_ma = static_cast<const MemberAccessExpr&>(*ma.object);
            std::string obj_struct_type;
            // Static property chain: ClassName.prop → get property's type
            if (inner_ma.object->kind == ExprKind::Identifier) {
                auto& cls_id = static_cast<const IdentifierExpr&>(*inner_ma.object);
                for (auto& cls : current_tu_->classes) {
                    if (cls.name == cls_id.name) {
                        for (auto& prop : cls.properties) {
                            if (prop.name == inner_ma.member_name &&
                                !prop.type.class_name.empty()) {
                                obj_struct_type = prop.type.class_name;
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            if (!obj_struct_type.empty()) {
                std::string fn = "struct_" + obj_struct_type + "_" + ma.member_name;
                callee = module_->getFunction(fn);
                if (callee) {
                    is_method = true;
                    auto* obj_val = generateExpr(*ma.object);
                    if (obj_val) {
                        auto* st_type = getStructType(obj_struct_type);
                        if (st_type) {
                            if (auto* li = llvm::dyn_cast<llvm::LoadInst>(obj_val)) {
                                mthis = li->getPointerOperand();
                            } else if (obj_val->getType()->isPointerTy()) {
                                mthis = builder_.CreateBitCast(obj_val, llvm::PointerType::get(ctx_, 0));
                            } else {
                                auto* tmp = builder_.CreateAlloca(st_type);
                                builder_.CreateStore(obj_val, tmp);
                                mthis = tmp;
                            }
                        }
                    }
                }
            }
        }

        if (!callee && current_tu_) {
            size_t num_args = e.args.size();
            // Resolve the object's class (O(1))：静态类名（Class.method）/ 局部实例
            //（var_class_map_ 或 sema resolved_object_class）/ 非标识符对象（下标/
            // new/调用/链式——resolved_object_class 或 memberObjectClassName）。
            std::string obj_cls;
            if (ma.object->kind == ExprKind::Identifier) {
                auto& oid = static_cast<const IdentifierExpr&>(*ma.object);
                if (findClass(oid.name)) {
                    obj_cls = oid.name;   // Class.method（静态）
                } else {
                    auto vit = var_class_map_.find(oid.name);
                    if (vit != var_class_map_.end()) obj_cls = vit->second;
                    else if (!ma.resolved_object_class.empty()) obj_cls = ma.resolved_object_class;
                }
            }
            // arr[i].method() / new Box<Node>().method() / f().method() — restrict
            // to the object's (possibly mangled) class so generic element / new /
            // chained-call method dispatch resolves to the concrete instance, not
            // the template's placeholder signature.
            else if (ma.object->kind == ExprKind::Subscript ||
                     ma.object->kind == ExprKind::NewExpr ||
                     ma.object->kind == ExprKind::Call ||
                     ma.object->kind == ExprKind::MemberAccess) {
                // BUG-041 根本修复：优先用 sema 的 resolved_object_class（静态
                // 类型，与文件顺序无关）。
                obj_cls = !ma.resolved_object_class.empty()
                    ? ma.resolved_object_class : memberObjectClassName(*ma.object);
            }
            // 已知对象类 → O(1) 类查找 + O(methods) 方法名/参数匹配（P6 规模修复：
            // 原无论是否已知类都线性扫全部类 × 全部方法 → 每方法调用 O(N×actions)）。
            if (!obj_cls.empty()) {
                const ClassDecl* cls = findClass(obj_cls);
                if (cls) {
                    for (auto& a : cls->actions)
                        if (a.name == ma.member_name && a.params.size() == num_args) {
                            auto fn = cls->name + "_" + a.name;
                            if (module_->getFunction(fn)) { best_class = cls->name; goto found_method; }
                        }
                    for (auto& a : cls->static_actions)
                        if (a.name == ma.member_name && a.params.size() == num_args) {
                            auto fn = cls->name + "_" + a.name;
                            if (module_->getFunction(fn)) { best_class = cls->name; goto found_method; }
                        }
                    for (auto& fn : cls->functions)
                        if (fn.name == ma.member_name && fn.params.size() == num_args) {
                            auto fn_name = cls->name + "_" + fn.name;
                            if (module_->getFunction(fn_name)) { best_class = cls->name; goto found_method; }
                        }
                }
            } else {
                // 无解析类（罕见）——保留全类扫描回退。
                for (auto& cls : current_tu_->classes) {
                    for (auto& a : cls.actions) {
                        if (a.name == ma.member_name && a.params.size() == num_args) {
                            auto fn = cls.name + "_" + a.name;
                            if (module_->getFunction(fn)) { best_class = cls.name; goto found_method; }
                        }
                    }
                    for (auto& a : cls.static_actions) {
                        if (a.name == ma.member_name && a.params.size() == num_args) {
                            auto fn = cls.name + "_" + a.name;
                            if (module_->getFunction(fn)) { best_class = cls.name; goto found_method; }
                        }
                    }
                    for (auto& fn : cls.functions) {
                        if (fn.name == ma.member_name && fn.params.size() == num_args) {
                            auto fn_name = cls.name + "_" + fn.name;
                            if (module_->getFunction(fn_name)) { best_class = cls.name; goto found_method; }
                        }
                    }
                }
            }
        }

        // Fallback: match by name only (only if no struct method was found)
        if (!callee) {
            // Restrict to the object's (possibly mangled) class for non-identifier
            // objects (chained `f().method()` / `arr[i].method()` / generic new),
            // so they don't resolve to a template's placeholder signature.
            std::string fb_obj_cls;
            if (ma.object->kind == ExprKind::Identifier) {
                auto& oid2 = static_cast<const IdentifierExpr&>(*ma.object);
                if (findClass(oid2.name)) fb_obj_cls = oid2.name;   // O(1)
            } else if (ma.object->kind == ExprKind::Subscript ||
                       ma.object->kind == ExprKind::NewExpr ||
                       ma.object->kind == ExprKind::Call ||
                       ma.object->kind == ExprKind::MemberAccess) {
                // BUG-041：同 callee 选择——优先 sema resolved_object_class
                //（类属性数组元素方法调用不依赖文件顺序）。
                fb_obj_cls = !ma.resolved_object_class.empty()
                    ? ma.resolved_object_class : memberObjectClassName(*ma.object);
            }
            // First, if the object is a KNOWN class name (static method call like
            // Vectors.min()), only search that class. Otherwise a method name that
            // collides across classes (e.g. min/max in Math AND Vectors) could
            // resolve to the wrong class.
            if (ma.object->kind == ExprKind::Identifier) {
                auto& oid = static_cast<const IdentifierExpr&>(*ma.object);
                const ClassDecl* scls = findClass(oid.name);   // O(1)
                if (scls) {
                    // static actions
                    for (auto& a : scls->static_actions) {
                        if (a.name == ma.member_name) {
                            auto fn = scls->name + "_" + a.name;
                            if (module_->getFunction(fn)) {
                                best_class = scls->name;
                                goto found_method;
                            }
                        }
                    }
                    // function: section
                    for (auto& fn : scls->functions) {
                        if (fn.name == ma.member_name) {
                            auto fn_name = scls->name + "_" + fn.name;
                            if (module_->getFunction(fn_name)) {
                                best_class = scls->name;
                                goto found_method;
                            }
                        }
                    }
                }
            }
            // Name-only fallback：找第一个定义该成员名的类（O(1) 索引，P6 规模修复：
            // 原线性扫全部类 × 全部方法——每调用 O(N×actions)）。
            std::string fbcls = fb_obj_cls;
            if (fbcls.empty()) {
                auto mit = first_member_class_.find(ma.member_name);
                if (mit != first_member_class_.end()) fbcls = mit->second;
            }
            if (!fbcls.empty() && module_->getFunction(fbcls + "_" + ma.member_name)) {
                best_class = fbcls;
            }
        }
        found_method:

        if (!best_class.empty()) {
            callee = module_->getFunction(best_class + "_" + ma.member_name);
            // trait 默认实现：类未覆盖但接口方法带默认体 → 用本类默认函数
            if (!callee) callee = findInterfaceDefault(best_class, ma.member_name);
            if (callee) {
                // Check if this is a static method
                std::string fn_name = best_class + "_" + ma.member_name;
                auto sit = is_static_action_.find(fn_name);
                bool is_static = (sit != is_static_action_.end() && sit->second);
                if (!is_static) {
                    mthis = generateExpr(*ma.object);
                } else {
                    // Static action: no mthis, and the object is a class name
                    // Don't try to evaluate it as an expression
                }
                is_method = true;
            }
        }
        // 显式 this.method()：best_class 未找到时回退本类接口默认
        if (!callee && ma.object->kind == ExprKind::ThisExpr && !current_class_name_.empty()) {
            callee = findInterfaceDefault(current_class_name_, ma.member_name);
            if (callee) {
                auto* ta = getNamedValue("this");
                if (ta) mthis = builder_.CreateLoad(
                    llvm::cast<llvm::AllocaInst>(ta)->getAllocatedType(), ta, "this");
                is_method = true;
            }
        }
    }

    // Check if this is a struct method self-call (bare method name = this.method())
    if (!callee && e.callee->kind == ExprKind::Identifier && !current_class_name_.empty()) {
        auto& id = static_cast<const IdentifierExpr&>(*e.callee);
        // Check if current class name is a struct type key
        if (struct_types_.count(current_class_name_)) {
            std::string fn = "struct_" + current_class_name_ + "_" + id.name;
            callee = module_->getFunction(fn);
            if (callee) {
                auto* ta = getNamedValue("this");
                if (ta) mthis = ta;
                is_method = true;
            }
        }
    }

    if (!callee && e.callee->kind == ExprKind::Identifier) {
        auto& id = static_cast<const IdentifierExpr&>(*e.callee);
        // §9.5 多态数学 intrinsic：在 resolve 到 runtime 函数（double 签名）之前
        // 拦截，按实参类型发 LLVM 标量 intrinsic（float 实参不被提升成 double）。
        if (id.name.rfind("__myp_math_", 0) == 0) {
            auto* mv = generatePolyMathIntrinsic(e, id.name);
            if (mv) return mv;
        }
        // §3.6 向量打包访问：load4/store4（CPU 回退 + host 侧；GPU 走 emitKernelExpr）。
        if (id.name == "load4" || id.name == "store4")
            return emitVec4Access(e, id.name);
        callee = module_->getFunction(id.name);
        if (!callee) {
            // Intrinsic functions (lookup via map)
            auto it = intrinsic_map_.find(id.name);
            if (it != intrinsic_map_.end()) {
                callee = it->second;
            }
            // Timer intrinsic (special: resolves event name to event_id)
            else if (id.name == "__myp_timer_create") {
                // __myp_timer_create("eventName", delay_ms, interval_ms)
                // We resolve the event name to event_id and generate
                // myp_timer_create(event_id, this, delay_ms, delay_ms, interval_ms)
                if (e.args.size() < 2) {
                    diag_.error(e.range, "__myp_timer_create requires at least 2 arguments");
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
                }
                // First arg: event name — either a string literal (resolved at
                // compile time) or a runtime string (looked up via the global
                // event table, e.g. stdlib Timer.init takes a string param).
                llvm::Value* event_id = nullptr;
                if (e.args[0]->kind == ExprKind::StringLiteral) {
                    std::string event_name = static_cast<const StringLiteralExpr&>(*e.args[0]).value;
                    std::string ekey = current_class_name_ + "::" + event_name;
                    int eid = -1;
                    auto eit = event_id_map_.find(ekey);
                    if (eit != event_id_map_.end()) eid = eit->second;
                    if (eid < 0) {
                        diag_.error(e.range, "unknown event '" + event_name +
                                    "' in class '" + current_class_name_ + "'");
                        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
                    }
                    event_id = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), eid);
                } else {
                    // Runtime event name → look up via the generated table
                    buildEventNameTable();
                    auto* name_val = generateExpr(*e.args[0]);
                    auto find_fn = module_->getOrInsertFunction("myp_event_id_by_name",
                        llvm::FunctionType::get(llvm::Type::getInt32Ty(ctx_),
                            {llvm::PointerType::get(ctx_, 0),
                             llvm::PointerType::get(ctx_, 0),
                             llvm::PointerType::get(ctx_, 0),
                             llvm::Type::getInt32Ty(ctx_)}, false));
                    event_id = builder_.CreateCall(find_fn,
                        {name_val, event_table_names_, event_table_ids_,
                         llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), event_table_count_)},
                        "evid");
                }
                // Get delay_ms and interval_ms args
                auto* delay = generateExpr(*e.args[1]);
                auto* interval = e.args.size() > 2 ? generateExpr(*e.args[2])
                    : llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0);
                // Get 'this' pointer
                auto* ta = getNamedValue("this");
                if (!ta) {
                    diag_.error(e.range, "'this' is not available in this context");
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
                }
                auto* this_ptr = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ta, "this");
                // Generate call: myp_timer_create(event_id, instance, delay_ms, param, interval_ms)
                // param = delay_ms (the requested delay is passed as event data)
                auto* param = builder_.CreateSExtOrTrunc(delay, llvm::Type::getInt64Ty(ctx_));
                auto* delay64 = builder_.CreateSExtOrTrunc(delay, llvm::Type::getInt64Ty(ctx_));
                auto* interval64 = builder_.CreateSExtOrTrunc(interval, llvm::Type::getInt64Ty(ctx_));
                auto* result = builder_.CreateCall(runtime_timer_create_->getFunctionType(),
                    runtime_timer_create_,
                    {event_id, this_ptr, delay64, param, interval64});
                return result;
            }
            // __myp_throw: save error message then longjmp to catch block
            else if (id.name == "__myp_throw") {
                if (e.args.size() < 1) {
                    diag_.error(e.range, "__myp_throw requires 1 argument (string)");
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
                }
                auto* msg = generateExpr(*e.args[0]);
                builder_.CreateCall(runtime_throw_->getFunctionType(),
                    runtime_throw_, {msg});
                // longjmp to the innermost active handler (stack top) — not a
                // global buffer, so nested/cross-function throws resolve right.
                auto* jb = builder_.CreateCall(runtime_exception_get_jmpbuf_->getFunctionType(),
                    runtime_exception_get_jmpbuf_, {}, "cur_handler");
                auto* one = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 1);
                builder_.CreateCall(runtime_longjmp_->getFunctionType(),
                    runtime_longjmp_, {jb, one});
                // Unreachable (longjmp is noreturn, but LLVM needs terminator)
                builder_.CreateUnreachable();
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
            }
            // __myp_trunc: double to int truncation
            else if (id.name == "__myp_trunc") {
                if (e.args.size() < 1) {
                    diag_.error(e.range, "__myp_trunc requires 1 argument");
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
                }
                auto* v = generateExpr(*e.args[0]);
                if (v->getType()->isFloatingPointTy())
                    return builder_.CreateFPToSI(v, llvm::Type::getInt32Ty(ctx_));
                return v;
            }
            // libmyp/freestanding 探针：__myp_syscall(n, a0..a5) → long。
            // x86-64 原始 syscall（rax=号，rdi/rsi/rdx/r10/r8/r9=参数，结果回 rax），
            // 不经 runtime.c / libc——供静态 freestanding（无 CRT、无 libc）使用。
            else if (id.name == "__myp_syscall") {
                if (e.args.size() != 7) {
                    diag_.error(e.range, "__myp_syscall requires 7 arguments (n, a0..a5)");
                    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0);
                }
                auto to_i64 = [&](llvm::Value* v) -> llvm::Value* {
                    if (!v) v = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0);
                    if (v->getType()->isPointerTy())
                        return builder_.CreatePtrToInt(v, llvm::Type::getInt64Ty(ctx_));
                    if (v->getType()->isIntegerTy() &&
                        v->getType()->getIntegerBitWidth() < 64)
                        return builder_.CreateSExt(v, llvm::Type::getInt64Ty(ctx_));
                    return v;
                };
                std::vector<llvm::Value*> av;
                for (auto& a : e.args) av.push_back(to_i64(generateExpr(*a)));
                while (av.size() < 7)
                    av.push_back(llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0));
                auto* i64 = llvm::Type::getInt64Ty(ctx_);
                auto* asm_ft = llvm::FunctionType::get(i64,
                    std::vector<llvm::Type*>(7, i64), false);
                auto* ia = llvm::InlineAsm::get(asm_ft, "syscall",
                    "={rax},{rax},{rdi},{rsi},{rdx},{r10},{r8},{r9},"
                    "~{rcx},~{r11},~{memory}",
                    true, false, llvm::InlineAsm::AD_ATT);
                return builder_.CreateCall(asm_ft, ia, av, "syscall");
            }
            // libmyp/freestanding 探针：__myp_str_ptr(s) → long。
            // 返回 MYP 字符串的数据指针地址（String 值本身即数据字节指针，头部
            // 在 data-8 —— 见 generateStringLiteral 返回的 bytes 指针）。
            else if (id.name == "__myp_str_ptr") {
                if (e.args.size() < 1) {
                    diag_.error(e.range, "__myp_str_ptr requires 1 argument (string)");
                    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0);
                }
                llvm::Value* s = generateExpr(*e.args[0]);
                if (s->getType()->isPointerTy())
                    return builder_.CreatePtrToInt(s, llvm::Type::getInt64Ty(ctx_));
                return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0);
            }
            // P2 §5.3 位操作原语：popcount/clz/ctz/bitreverse/rotl/rotr
            else if (id.name == "popcount" || id.name == "clz" ||
                     id.name == "ctz" || id.name == "bitreverse" ||
                     id.name == "rotl" || id.name == "rotr") {
                return generateBitOps(e, id.name);
            }
            // Atomic intrinsics: generate LLVM atomic instructions directly
            else if (id.name == "__myp_atomic_add_i32" ||
                     id.name == "__myp_atomic_sub_i32" ||
                     id.name == "__myp_atomic_xchg_i32" ||
                     id.name == "__myp_atomic_add_f64" ||
                     id.name == "__myp_atomic_load_i32" ||
                     id.name == "__myp_atomic_store_i32") {
                if (e.args.size() < 2) {
                    diag_.error(e.range, "atomic intrinsic requires at least 2 arguments");
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
                }
                // First arg is the array pointer (int[] or double[])
                auto* arr_ptr = generateExpr(*e.args[0]);
                // Second arg is the index
                auto* index_val = generateExpr(*e.args[1]);

                // Determine element type and generate GEP
                bool is_float = (id.name == "__myp_atomic_add_f64");
                auto* elem_ty = is_float
                    ? llvm::Type::getDoubleTy(ctx_)
                    : llvm::Type::getInt32Ty(ctx_);
                auto* elem_ptr = builder_.CreateGEP(
                    elem_ty, arr_ptr, {index_val}, "atomic_ptr");

                if (id.name == "__myp_atomic_load_i32") {
                    // BUG-014: loadInt 此前编译成普通非原子 load（无内存序）——跨线程
                    // 共享数组读不具原子性/可见性保证。改用 seq_cst 原子 load。
                    // LLVM 21 IRBuilder 无 CreateAtomicLoad → 用 LoadInst 构造器。
                    auto* loaded = new llvm::LoadInst(elem_ty, elem_ptr,
                        "atomic_load", false,
                        module_->getDataLayout().getABITypeAlign(elem_ty),
                        llvm::AtomicOrdering::SequentiallyConsistent);
                    builder_.Insert(loaded);
                    return loaded;
                }

                if (e.args.size() < 3) {
                    diag_.error(e.range, "atomic store/add/sub/xchg requires 3 arguments");
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
                }
                auto* val = generateExpr(*e.args[2]);

                if (id.name == "__myp_atomic_store_i32") {
                    // BUG-014: storeInt 此前编译成普通非原子 store——改用 seq_cst 原子 store。
                    auto* st = new llvm::StoreInst(val, elem_ptr, false,
                        module_->getDataLayout().getABITypeAlign(elem_ty),
                        llvm::AtomicOrdering::SequentiallyConsistent);
                    builder_.Insert(st);
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
                }

                llvm::AtomicRMWInst::BinOp op;
                if (id.name == "__myp_atomic_add_i32") op = llvm::AtomicRMWInst::Add;
                else if (id.name == "__myp_atomic_sub_i32") op = llvm::AtomicRMWInst::Sub;
                else if (id.name == "__myp_atomic_xchg_i32") op = llvm::AtomicRMWInst::Xchg;
                else if (id.name == "__myp_atomic_add_f64") op = llvm::AtomicRMWInst::FAdd;
                else {
                    diag_.error(e.range, "unknown atomic operation");
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
                }

                auto* result = builder_.CreateAtomicRMW(op, elem_ptr, val,
                    llvm::MaybeAlign(),
                    llvm::AtomicOrdering::SequentiallyConsistent);
                return result;
            }
        }
        // Try current class method (bare name without this.)
        if (!callee && !current_class_name_.empty()) {
            const ClassDecl* cls = findClass(current_class_name_);
            if (cls) {
                // Check actions
                for (auto& a : cls->actions) {
                    if (a.name == id.name) {
                        auto fn = cls->name + "_" + a.name;
                        callee = module_->getFunction(fn);
                        if (callee) {
                            is_method = true;
                            auto* ta = getNamedValue("this");
                            if (ta) mthis = builder_.CreateLoad(llvm::cast<llvm::AllocaInst>(ta)->getAllocatedType(), ta, "this");
                            goto found_bare_method;
                        }
                    }
                }
                // Check function: section
                for (auto& fn : cls->functions) {
                    if (fn.name == id.name) {
                        auto fn_name = cls->name + "_" + fn.name;
                        callee = module_->getFunction(fn_name);
                        if (callee) {
                            is_method = true;
                            auto* ta = getNamedValue("this");
                            if (ta) mthis = builder_.CreateLoad(llvm::cast<llvm::AllocaInst>(ta)->getAllocatedType(), ta, "this");
                            goto found_bare_method;
                        }
                    }
                }
                // Check events
                for (auto& ev : cls->events) {
                    if (ev.name == id.name) {
                        auto fn = "fire_" + cls->name + "_" + ev.name;
                        callee = module_->getFunction(fn);
                        if (callee) {
                            is_method = true;
                            auto* ta = getNamedValue("this");
                            if (ta) mthis = builder_.CreateLoad(llvm::cast<llvm::AllocaInst>(ta)->getAllocatedType(), ta, "this");
                            goto found_bare_method;
                        }
                    }
                }
            }
            found_bare_method:;
        }
        // 裸方法调用 = this.method()（trait 默认实现兜底）：类未覆盖的接口默认方法
        if (!callee && !current_class_name_.empty()) {
            callee = findInterfaceDefault(current_class_name_, id.name);
            if (callee) {
                is_method = true;
                auto* ta = getNamedValue("this");
                if (ta) mthis = builder_.CreateLoad(
                    llvm::cast<llvm::AllocaInst>(ta)->getAllocatedType(), ta, "this");
            }
        }
    }
    if (!callee) {
        // Last resort: try to find the function by name
        if (e.callee->kind == ExprKind::MemberAccess) {
            auto& ma = static_cast<const MemberAccessExpr&>(*e.callee);
            // Try getting object name and building function name
            bool ma_is_plain_id = (ma.object->kind == ExprKind::Identifier);
            if (ma_is_plain_id) {
                auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
                // Search all classes for matching method
                if (current_tu_) {
                    for (auto& cls : current_tu_->classes) {
                        for (auto& a : cls.actions) {
                            if (a.name == ma.member_name) {
                                callee = module_->getFunction(cls.name + "_" + a.name);
                                if (callee) {
                                    is_method = true;
                                    std::string fn = cls.name + "_" + a.name;
                                    auto sit = is_static_action_.find(fn);
                                    if (sit == is_static_action_.end() || !sit->second)
                                        mthis = generateExpr(*ma.object);
                                    goto call_ready;
                                }
                            }
                        }
                        // Also check static actions
                        for (auto& a : cls.static_actions) {
                            if (a.name == ma.member_name) {
                                callee = module_->getFunction(cls.name + "_" + a.name);
                                if (callee) {
                                    is_method = true;
                                    // Static — no mthis
                                    goto call_ready;
                                }
                            }
                        }
                    }
                }
            }
            // Chained struct method call: a.b.c.method()
            // Resolve the struct type of the object expression
            if (!callee && current_tu_) {
                std::string obj_struct_type;
                // Struct method call on a call result (`element_.get().str()`):
                // sema recorded the object's resolved struct name (no variable
                // exists for the chain walk below).
                if (!ma.resolved_object_class.empty())
                    obj_struct_type = ma.resolved_object_class;
                // For chained access a.b.c.method(), evaluate the object
                // expression to determine its struct type by walking the chain
                if (obj_struct_type.empty() && ma.object->kind == ExprKind::MemberAccess) {
                    auto& inner_ma = static_cast<const MemberAccessExpr&>(*ma.object);
                    // Check if inner is a known struct field of a known struct type
                    for (auto& [key, st] : struct_types_) {
                        auto idx_it = struct_field_indices_.find(key);
                        if (idx_it != struct_field_indices_.end()) {
                            auto field_it = idx_it->second.find(inner_ma.member_name);
                            if (field_it != idx_it->second.end()) {
                                auto* field_ty = st->getElementType(field_it->second);
                                if (field_ty && field_ty->isStructTy()) {
                                    obj_struct_type = field_ty->getStructName().str();
                                    break;
                                }
                            }
                        }
                    }
                }
                // Static property chain: ClassName.prop
                if (obj_struct_type.empty() && ma.object->kind == ExprKind::MemberAccess) {
                    auto& inner_ma = static_cast<const MemberAccessExpr&>(*ma.object);
                    if (inner_ma.object->kind == ExprKind::Identifier) {
                        auto& cls_id = static_cast<const IdentifierExpr&>(*inner_ma.object);
                        for (auto& cls : current_tu_->classes) {
                            if (cls.name == cls_id.name) {
                                for (auto& prop : cls.properties) {
                                    if (prop.name == inner_ma.member_name &&
                                        !prop.type.class_name.empty()) {
                                        obj_struct_type = prop.type.class_name;
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
                // Look up the method on the resolved struct type
                if (!obj_struct_type.empty()) {
                    std::string sfn = "struct_" + obj_struct_type + "_" + ma.member_name;
                    callee = module_->getFunction(sfn);
                    if (callee) {
                        is_method = true;
                        // Evaluate object to get pointer for mthis
                        auto* obj_val = generateExpr(*ma.object);
                        if (obj_val) {
                            auto* st_type = getStructType(obj_struct_type);
                            if (st_type) {
                                if (auto* li = llvm::dyn_cast<llvm::LoadInst>(obj_val)) {
                                    mthis = li->getPointerOperand();
                                } else if (obj_val->getType()->isPointerTy()) {
                                    mthis = builder_.CreateBitCast(obj_val,
                                        llvm::PointerType::get(ctx_, 0));
                                } else {
                                    auto* tmp = builder_.CreateAlloca(st_type);
                                    builder_.CreateStore(obj_val, tmp);
                                    mthis = tmp;
                                }
                            }
                        }
                        goto call_ready;
                    }
                }
            }
        }
        callee = generateExpr(*e.callee);
    }
    call_ready:

    // ---- @coro method call → spawn coroutine, return handle ----
    if (callee) {
        auto* ccf = llvm::dyn_cast<llvm::Function>(callee);
        if (ccf && coro_methods_.count(ccf->getName().str())) {
            return generateCoroSpawn(ccf, e, mthis, is_method);
        }
    }

    std::vector<llvm::Value*> args;
    // Check if this is a static method — skip mthis for static actions
    bool skip_this = false;
    if (callee && is_method && mthis) {
        auto* cf = llvm::dyn_cast<llvm::Function>(callee);
        if (cf) {
            auto sit = is_static_action_.find(cf->getName().str());
            if (sit != is_static_action_.end() && sit->second)
                skip_this = true;
        }
    }
    if (is_method && mthis && !skip_this) args.push_back(mthis);
    for (auto& a : e.args) args.push_back(generateExpr(*a));

    auto* cf = llvm::dyn_cast<llvm::Function>(callee);
    if (!cf) {
        diag_.error(e.range, "cannot call expression");
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    }
    // Implicit type conversion for arguments
    auto* ft = cf->getFunctionType();
    size_t arg_offset = (is_method && mthis && !skip_this) ? 1 : 0;
    for (size_t i = arg_offset; i < args.size() && i < ft->getNumParams(); ++i) {
        auto* expected = ft->getParamType(i);
        if (args[i]->getType() != expected) {
            // Interface param upcast: a concrete class instance (ptr) passed where
            // an interface fat pointer {data, vtable} is expected → build the fat
            // pointer for (param_interface, concrete_class). Bug fix: previously
            // there was no conversion, so the raw object pointer was passed where
            // the fat pointer was expected → LLVM verify failure / wrong dispatch.
            if (expected->isStructTy() && isInterfaceFatType(expected) &&
                args[i]->getType()->isPointerTy() && current_tu_) {
                std::string cls_name = resolveArgClassName(*e.args[i - arg_offset]);
                std::string iface_name = paramIfaceName(cf, i - arg_offset);
                if (!cls_name.empty() && !iface_name.empty()) {
                    llvm::Value* fp = buildInterfaceFat(args[i], iface_name, cls_name);
                    if (fp && fp->getType()->isStructTy()) {
                        args[i] = fp;
                        continue;
                    }
                }
            }
            args[i] = convertIntegerValue(builder_, args[i], expected, e.args[i - arg_offset].get());
        }
    }
    bool isv = cf->getReturnType()->isVoidTy();
    // Math intrinsics (stdlib Math.sqrt/abs/floor/ceil) → LLVM intrinsics so the
    // backend lowers them to single instructions (sqrtsd / fabs / roundsd) instead
    // of an external runtime call (myp_math_* lives in a separate TU → cannot
    // inline). This matches what C++ -O2 does for std::sqrt/std::fabs/std::floor
    // and removes a call frame from the hottest inner loops (e.g. ray tracing).
    if (args.size() == 1) {
        const std::string& cname = cf->getName().str();
        llvm::Intrinsic::ID mid = llvm::Intrinsic::not_intrinsic;
        if (cname == "myp_math_sqrt") mid = llvm::Intrinsic::sqrt;
        else if (cname == "myp_math_floor") mid = llvm::Intrinsic::floor;
        else if (cname == "myp_math_ceil") mid = llvm::Intrinsic::ceil;
        else if (cname == "myp_math_abs") mid = llvm::Intrinsic::fabs;
        if (mid != llvm::Intrinsic::not_intrinsic)
            return builder_.CreateUnaryIntrinsic(mid, args[0], nullptr, "mypmath");
    }
    return builder_.CreateCall(cf->getFunctionType(), cf, args, isv ? "" : "calltmp");
}

void CodeGen::generateFFIDecl(const FFIDecl& decl) {
    std::vector<llvm::Type*> pts;
    for (auto& p : decl.params) {
        if (p.type.isArray()) {
            pts.push_back(llvm::PointerType::get(ctx_, 0));
        } else {
            pts.push_back(getLLVMType(typeNodeToCodegenType(p.type)));
        }
    }
    auto* rt = getLLVMType(typeNodeToCodegenType(decl.return_type));
    auto* ft = llvm::FunctionType::get(rt, pts, false);
    llvm::Function::Create(ft, llvm::Function::ExternalLinkage, decl.name, module_.get());
}

llvm::Value* CodeGen::generateLambda(const LambdaExpr& e) {
    // Create instance of hidden class: new __lambda_N()
    NewExpr ne(e.hidden_class_name, {}, {}, e.range);
    auto* obj = generateNewExpr(ne);

    // M-FN-2: fill capture slots with the current values of captured outer locals
    // (by value). obj is the hidden-class instance; store each cap_i = outer value.
    // nonlocal 捕获：槽存 cell 对象引用（共享可变，retain 由下方 cap_is_ref 处理）。
    for (size_t i = 0; i < e.capture_names.size(); i++) {
        auto* outer = getNamedValue(e.capture_names[i]);
        if (!outer) continue;
        bool is_nl = i < e.nonlocal_cells.size() && !e.nonlocal_cells[i].empty();
        unsigned pi = 0;
        if (!getPropertyIndex(e.hidden_class_name, e.capture_slots[i], pi)) continue;
        auto* st = getClassStruct(e.hidden_class_name);
        if (!st) continue;
        const ClassDecl* hc = nullptr;
        if (current_tu_) {
            for (auto& c : current_tu_->classes)
                if (c.name == e.hidden_class_name) { hc = &c; break; }
        }
        auto* pt = hc ? getPropertyType(*hc, e.capture_slots[i]) : llvm::Type::getInt32Ty(ctx_);
        // 固定数组捕获：外层局部是数据指针 alloca（`int[N] a` 存 `a_arr[0]` 指针）。
        // 属性类型是 [N x T] → 需先取数据指针，再加载数组值（深拷贝进闭包）。
        llvm::Value* val = nullptr;
        if (is_nl) {
            auto oit = cell_owners_.find(e.capture_names[i]);
            if (oit == cell_owners_.end()) continue;
            val = oit->second;   // 共享 cell 对象
        } else if (pt->isArrayTy()) {
            auto* data = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), outer, e.capture_names[i] + ".data");
            val = builder_.CreateLoad(pt, data, e.capture_names[i] + ".cap");
        } else {
            val = builder_.CreateLoad(pt, outer, e.capture_names[i] + ".cap");
        }
        // ARC: a captured class reference is shared (浅拷贝共享) — the closure's
        // destroy stub will release it, so the closure must RETAIN its own ref
        // (otherwise the outer local's release frees it while the closure holds
        // a dangling borrow). Interface/function captures handled analogously.
        bool cap_is_ref = false;
        if (hc) {
            for (auto& p : hc->properties)
                if (p.name == e.capture_slots[i] && isArcRefType(p.type)) {
                    cap_is_ref = true;
                    break;
                }
        }
        if (cap_is_ref) {
            llvm::Value* cap_data = val;
            if (val->getType()->isStructTy())
                cap_data = builder_.CreateExtractValue(val, 0);
            emitRetain(cap_data);
        }
        auto* gep = builder_.CreateStructGEP(st, obj, pi);
        builder_.CreateStore(val, gep);
    }

    // Build the first-class function value: fat pointer { closure, call_fn }.
    auto* fp_ty = getFunctionValueType();
    auto* fp = builder_.CreateAlloca(fp_ty);
    auto* closure_gep = builder_.CreateStructGEP(fp_ty, fp, 0);
    auto* call_fn_gep = builder_.CreateStructGEP(fp_ty, fp, 1);
    builder_.CreateStore(obj, closure_gep);
    // ARC: the fat-pointer function value now owns the closure — drop it from
    // the statement-end temp list (else it would be freed while still called).
    arcConsumeTemp(obj);
    auto* tramp = module_->getFunction(e.hidden_class_name + "_tramp");
    if (tramp)
        builder_.CreateStore(builder_.CreateBitCast(tramp, llvm::PointerType::get(ctx_, 0)), call_fn_gep);
    else
        builder_.CreateStore(llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)), call_fn_gep);
    return builder_.CreateLoad(fp_ty, fp);
}

llvm::Value* CodeGen::generatePipe(const PipeExpr& e) {
    auto* lhs_val = generateExpr(*e.lhs);
    if (!lhs_val) return nullptr;
    // Resolve the operator instance: reuse a variable ("instance") or
    // instantiate a fresh component from its class name ("class").
    llvm::Value* instance = nullptr;
    if (e.target_kind == "instance") {
        instance = generateExpr(*e.rhs);
    } else {
        NewExpr ne(e.class_name, {}, {}, e.range);
        instance = generateNewExpr(ne);
    }
    if (!instance) return lhs_val;
    std::string fn = e.class_name + "_" + e.method;  // e.g. ScaleOp_transform
    auto* callee = module_->getFunction(fn);
    if (callee)
        return builder_.CreateCall(callee, {instance, lhs_val});
    return lhs_val;
}

llvm::Value* CodeGen::generateArrayElementAddress(const SubscriptExpr& ss) {
    llvm::Type* elem_ty = nullptr;
    const TypeInfo* slice_ti = nullptr;
    if (ss.array->kind == ExprKind::Identifier) {
        auto& id = static_cast<const IdentifierExpr&>(*ss.array);
        auto eit = array_elem_types_.find(id.name);
        if (eit != array_elem_types_.end())
            elem_ty = eit->second;
        // slice<T>[i] — unpack {data,len}, bounds-check, GEP to element.
        auto sit = var_slice_types_.find(id.name);
        if (sit != var_slice_types_.end() && sit->second.element_type)
            slice_ti = &sit->second;
    } else if (ss.array->kind == ExprKind::MemberAccess) {
        // obj.arr[i] — resolve the array field's element type from the struct
        // or class declaration.
        auto& ma = static_cast<const MemberAccessExpr&>(*ss.array);
        if (ma.object->kind == ExprKind::Identifier) {
            auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
            auto* oa = getNamedValue(oi.name);
            llvm::Type* oat = nullptr;
            if (oa) {
                if (auto* oai = llvm::dyn_cast<llvm::AllocaInst>(oa))
                    oat = oai->getAllocatedType();
                else
                    oat = getNamedValueType(oi.name);
            }
            if (oat && oat->isStructTy()) {
                const StructDecl* sd = findStruct(
                    llvm::cast<llvm::StructType>(oat)->getName().str());
                if (sd) {
                    for (auto& pr : sd->properties) {
                        if (pr.name == ma.member_name && pr.type.isArray())
                            elem_ty = typeNodeToLLVMType(*pr.type.element_type);
                    }
                }
            }
        }
    }
    auto* i = generateExpr(*ss.index);
    auto* i64 = i;
    if (i64->getType()->isIntegerTy(32) || i64->getType()->isIntegerTy(8)
        || i64->getType()->isIntegerTy(16))
        i64 = builder_.CreateZExt(i64, llvm::Type::getInt64Ty(ctx_));

    // Slice path: {data, len} fat pointer — bounds check, GEP data[i].
    if (slice_ti) {
        auto* va = getNamedValue(
            static_cast<const IdentifierExpr&>(*ss.array).name);
        if (!va) return nullptr;
        auto* sval = builder_.CreateLoad(getLLVMType(*slice_ti), va);
        auto* data = builder_.CreateExtractValue(sval, 0);
        auto* len = builder_.CreateExtractValue(sval, 1);
        auto* i64ty = llvm::Type::getInt64Ty(ctx_);
        auto* nonneg = builder_.CreateICmpSGE(i64, llvm::ConstantInt::get(i64ty, 0));
        auto* inb = builder_.CreateICmpULT(i64, len);
        auto* ok = builder_.CreateAnd(nonneg, inb);
        auto* be_fn = module_->getFunction("myp_bounds_error");
        if (!be_fn) {
            auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                {i64ty, i64ty}, false);
            be_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                "myp_bounds_error", module_.get());
        }
        auto* cur_fn = builder_.GetInsertBlock()->getParent();
        auto* err_bb = llvm::BasicBlock::Create(ctx_, "slice_oob", cur_fn);
        auto* cont_bb = llvm::BasicBlock::Create(ctx_, "slice_ok", cur_fn);
        builder_.CreateCondBr(ok, cont_bb, err_bb);
        builder_.SetInsertPoint(err_bb);
        builder_.CreateCall(be_fn, {i64, len});
        builder_.CreateBr(cont_bb);
        builder_.SetInsertPoint(cont_bb);
        auto* et = getLLVMType(*slice_ti->element_type);
        return builder_.CreateGEP(et, data, i64);
    }

    if (!elem_ty) return nullptr;
    auto* a = generateExpr(*ss.array);
    return builder_.CreateGEP(elem_ty, a, zextIndexValue(builder_, i));
}

llvm::Value* CodeGen::generateStructMemberAddress(const MemberAccessExpr& e) {
    llvm::StructType* st = nullptr;
    llvm::Value* base = nullptr;
    if (e.object->kind == ExprKind::Identifier) {
        auto& oi = static_cast<const IdentifierExpr&>(*e.object);
        auto* oa = getNamedValue(oi.name);
        if (!oa) return nullptr;
        if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(oa)) {
            auto* at = ai->getAllocatedType();
            if (!at->isStructTy()) return nullptr;
            st = llvm::cast<llvm::StructType>(at);
            base = oa;
        } else {
            auto* nty = getNamedValueType(oi.name);
            if (!nty || !nty->isStructTy()) return nullptr;
            st = llvm::cast<llvm::StructType>(nty);
            base = oa;
        }
    } else if (e.object->kind == ExprKind::ThisExpr) {
        // this.field.subfield inside a struct method: 'this' is the struct ptr
        auto* ta = getNamedValue("this");
        if (!ta) return nullptr;
        auto* tp = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ta);
        if (struct_types_.count(current_class_name_)) {
            st = getStructType(current_class_name_);
            if (!st) return nullptr;
            base = tp;
        } else if (current_tu_) {
            // BUG-010: class method — `this.prop.subfield` where `prop` is a
            // struct-typed property (`property: S s;`). Return the property
            // slot address directly (the chained MemberAccess case then GEPs
            // the subfield into the struct). Previously returned nullptr here,
            // so `this.s.x = v` fell through and failed.
            for (auto& cls : current_tu_->classes) {
                if (cls.name != current_class_name_) continue;
                for (auto& p : cls.properties) {
                    if (p.name != e.member_name) continue;   // 属性不一定是第一个
                    const StructDecl* sd = findStruct(p.type.class_name);
                    if (!sd) break;
                    unsigned pi = 0;
                    auto* cst = getClassStruct(cls.name);
                    if (!cst || !getPropertyIndex(cls.name, p.name, pi)) break;
                    return builder_.CreateStructGEP(cst, tp, pi);
                }
                break;
            }
            return nullptr;
        } else {
            return nullptr;
        }
    } else if (e.object->kind == ExprKind::MemberAccess) {
        // Chained: a.b.c — recurse to get the address of a.b, then GEP c
        base = generateStructMemberAddress(static_cast<const MemberAccessExpr&>(*e.object));
        if (!base) return nullptr;
        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(base)) {
            auto* rt = gep->getResultElementType();
            if (rt && rt->isStructTy()) st = llvm::cast<llvm::StructType>(rt);
        }
        if (!st) return nullptr;
    } else if (e.object->kind == ExprKind::Subscript) {
        // Struct array element: a[i].b — GEP to element, then GEP the field.
        base = generateArrayElementAddress(static_cast<const SubscriptExpr&>(*e.object));
        if (!base) return nullptr;
        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(base)) {
            auto* rt = gep->getResultElementType();
            if (rt && rt->isStructTy()) st = llvm::cast<llvm::StructType>(rt);
        }
        if (!st) return nullptr;
    } else {
        return nullptr;
    }
    unsigned fi = 0;
    if (!getStructFieldIndex(st->getName().str(), e.member_name, fi)) return nullptr;
    return builder_.CreateStructGEP(st, base, fi);
}

llvm::Value* CodeGen::generateMemberAccess(const MemberAccessExpr& e) {
    // §3.1 CPU 回退模拟 kernel 上下文字段（真实 GPU 路径在 emitKernelExpr）：
    // gid=p、tx=p%256、bx=p/256、bd=256、gx=ceil(n/256)。
    if (gpu_cpu_fallback_ && e.object->kind == ExprKind::Identifier) {
        auto& oi = static_cast<const IdentifierExpr&>(*e.object);
        if (oi.name == "kernel") {
            auto* i64_ty = llvm::Type::getInt64Ty(ctx_);
            // §3.7 block(n)：bd = 块大小（默认 256），gx = ceil(bound / bd)
            auto* bd_c = llvm::ConstantInt::get(i64_ty, gpu_cpu_block_);
            auto* va = getNamedValue(gpu_cpu_loop_var_);
            llvm::Value* p;
            if (va)
                p = builder_.CreateLoad(i64_ty, va, gpu_cpu_loop_var_);
            else
                p = llvm::ConstantInt::get(i64_ty, 0);
            if (e.member_name == "gid") return p;
            if (e.member_name == "tx")
                return builder_.CreateURem(p, bd_c, "tx");
            if (e.member_name == "bx")
                return builder_.CreateUDiv(p, bd_c, "bx");
            if (e.member_name == "bd") return bd_c;
            if (e.member_name == "gx") {
                auto* one = llvm::ConstantInt::get(i64_ty, 1);
                auto* num = builder_.CreateAdd(gpu_cpu_bound_,
                    builder_.CreateSub(bd_c, one), "gx_num");
                return builder_.CreateUDiv(num, bd_c, "gx");
            }
            return llvm::ConstantInt::get(i64_ty, 0);
        }
    }
    // Slice field access: s.length / s.size / s.data — value-type {ptr, len}.
    // Must come before the generic field fallback (which treated `s.length`
    // as a pointer-deref and generated `ptrtoint(&s)` garbage as the length).
    if (e.object->kind == ExprKind::Identifier) {
        auto& oi = static_cast<const IdentifierExpr&>(*e.object);
        auto sit = var_slice_types_.find(oi.name);
        if (sit != var_slice_types_.end()) {
            auto* va = getNamedValue(oi.name);
            if (va) {
                auto* sval = builder_.CreateLoad(getLLVMType(sit->second), va, oi.name);
                if (e.member_name == "length" || e.member_name == "size")
                    return builder_.CreateExtractValue(sval, 1, "slen");
                if (e.member_name == "data")
                    return builder_.CreateExtractValue(sval, 0, "sdata");
            }
        }
    }

    // Tuple field access: t.0, t.1 — numeric member name on a tuple value.
    if (!e.member_name.empty() && std::all_of(e.member_name.begin(), e.member_name.end(),
            [](unsigned char c) { return std::isdigit(c); })) {
        auto* objv = generateExpr(*e.object);
        if (objv && objv->getType()->isStructTy()) {
            auto* st = llvm::cast<llvm::StructType>(objv->getType());
            if (st->isLiteral()) {
                unsigned idx = (unsigned)std::stoul(e.member_name);
                if (idx < st->getNumElements())
                    return builder_.CreateExtractValue(objv, idx, "tup_field");
            }
        }
    }

    // §3.6 向量成员访问：float4.x/y/z/w → extractelement（值类型，resolved_kind
    // 由 sema 标记在 object 上）。double2.x/y、int4.x/y/z/w 同机制。
    if (e.object->resolved_kind == TypeKind::Float4 ||
        e.object->resolved_kind == TypeKind::Double2 ||
        e.object->resolved_kind == TypeKind::Int4) {
        auto* objv = generateExpr(*e.object);
        if (!objv || !objv->getType()->isVectorTy())
            return llvm::UndefValue::get(llvm::Type::getFloatTy(ctx_));
        auto* vt = llvm::cast<llvm::FixedVectorType>(objv->getType());
        const std::string names = (e.object->resolved_kind == TypeKind::Double2)
            ? "xy" : "xyzw";
        if (e.member_name.size() == 1 &&
            names.find(e.member_name[0]) != std::string::npos) {
            auto* i32 = llvm::Type::getInt32Ty(ctx_);
            // 组件下标显式映射（'w'-'x' 在 ASCII 里是 -1，不能直接相减）。
            unsigned idx = (e.member_name[0] == 'x') ? 0 :
                           (e.member_name[0] == 'y') ? 1 :
                           (e.member_name[0] == 'z') ? 2 : 3;
            return builder_.CreateExtractElement(objv,
                llvm::ConstantInt::get(i32, idx), "vec_comp");
        }
        const char* vname = (e.object->resolved_kind == TypeKind::Float4) ? "float4"
            : (e.object->resolved_kind == TypeKind::Double2) ? "double2" : "int4";
        diag_.error(e.range, std::string(vname) + " has no component '" +
                     e.member_name + "'");
        return llvm::UndefValue::get(vt->getElementType());
    }

    // §5.1 bitfield field read: f.read : bit（1 位）/ f.reserved : uint（N 位）。
    // 值类型背衬整数，(v >> offset) & mask，1 位截断到 i1，N 位零扩展到 i32。
    if (e.object->resolved_kind == TypeKind::Bitfield) {
        int off = -1, width = 1;
        if (current_tu_) {
            for (auto& bf : current_tu_->bitfields) {
                for (auto& fld : bf.fields) {
                    if (fld.name == e.member_name) { off = fld.offset; width = fld.bit_width; break; }
                }
                if (off >= 0) break;
            }
        }
        if (off < 0) {
            diag_.error(e.range, "unknown bitfield field '" + e.member_name + "'");
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
        }
        auto* bv = generateExpr(*e.object);
        auto* ty = bv->getType();
        if (!ty->isIntegerTy())
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
        auto* shifted = builder_.CreateLShr(bv, llvm::ConstantInt::get(ty, off), "bfld");
        if (width == 1) {
            auto* masked = builder_.CreateAnd(shifted, llvm::ConstantInt::get(ty, 1));
            return builder_.CreateTrunc(masked, llvm::Type::getInt1Ty(ctx_));
        }
        uint64_t mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
        auto* masked = builder_.CreateAnd(shifted, llvm::ConstantInt::get(ty, mask));
        auto* i32 = llvm::Type::getInt32Ty(ctx_);
        if (ty->getIntegerBitWidth() < 32) return builder_.CreateZExt(masked, i32);
        return builder_.CreateZExtOrTrunc(masked, i32);
    }

    // Static class property access: ClassName.property
    if (e.object->kind == ExprKind::Identifier) {
        auto& oi = static_cast<const IdentifierExpr&>(*e.object);
        auto sit = static_property_globals_.find(oi.name);
        if (sit != static_property_globals_.end() && current_tu_) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name == oi.name) {
                    unsigned pi = 0;
                    if (getPropertyIndex(cls.name, e.member_name, pi)) {
                        auto* st = getClassStruct(cls.name);
                        if (st) {
                            auto* gep = builder_.CreateStructGEP(st, sit->second, pi);
                            return loadPropertyField(gep, cls, e.member_name);
                        }
                    }
                }
            }
        }
    }

    // Enum variant access: Color.Red → enum struct {disc, zeros}
    if (e.object->kind == ExprKind::Identifier) {
        auto& oi = static_cast<const IdentifierExpr&>(*e.object);
        if (current_tu_) {
            for (auto& en : current_tu_->enums) {
                if (en.name == oi.name) {
                    for (size_t vi = 0; vi < en.variants.size(); vi++) {
                        if (en.variants[vi].name == e.member_name) {
                            return buildEnumVariant(en.name, vi, {});
                        }
                    }
                }
            }
        }
    }

    // Struct field access: v.field  (struct is value type, allocated on stack)
    // Handle both direct identifier and chained member access (a.b.c)
    llvm::StructType* st2_m = nullptr;
    llvm::Value* bp_m = nullptr;
    if (e.object->kind == ExprKind::Identifier) {
        auto& oi = static_cast<const IdentifierExpr&>(*e.object);
        auto* oa = getNamedValue(oi.name);
        if (oa) {
            if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(oa)) {
                auto* at = ai->getAllocatedType();
                if (at->isStructTy()) { st2_m = llvm::cast<llvm::StructType>(at); bp_m = oa; }
            } else if (oa->getType()->isPointerTy()) {
                auto* nty = getNamedValueType(oi.name);
                if (nty && nty->isStructTy()) { st2_m = llvm::cast<llvm::StructType>(nty); bp_m = oa; }
            }
        }
    } else if (e.object->kind == ExprKind::MemberAccess) {
        // Chained struct field access: a.b.c (e.g., DataManager.mesh.nx)
        auto* inner = generateMemberAccess(static_cast<const MemberAccessExpr&>(*e.object));
        if (inner) {
            if (auto* inner_ld = llvm::dyn_cast<llvm::LoadInst>(inner)) {
                // Loaded a struct value — recover the struct type from the load result
                auto* loaded_ty = inner_ld->getType();
                if (loaded_ty && loaded_ty->isStructTy()) {
                    st2_m = llvm::cast<llvm::StructType>(loaded_ty);
                    bp_m = inner_ld->getPointerOperand();
                }
            } else if (auto* inner_gep = llvm::dyn_cast<llvm::GetElementPtrInst>(inner)) {
                // GEP pointer — try sourced type info
                auto* inner_struct = inner_gep->getResultElementType();
                if (inner_struct && inner_struct->isStructTy()) {
                    st2_m = llvm::cast<llvm::StructType>(inner_struct);
                    bp_m = inner;
                }
            }
            // If still unresolved, try from the value's type
            if (!st2_m && inner->getType()->isPointerTy()) {
                // For opaque pointers in LLVM 21+, try to find the struct type by name
                // by searching the struct field indices for a matching field name
                for (auto& [key, st] : struct_types_) {
                    auto idx_it = struct_field_indices_.find(key);
                    if (idx_it == struct_field_indices_.end()) continue;
                    if (idx_it->second.count(e.member_name)) {
                        st2_m = st;
                        bp_m = inner;
                        break;
                    }
                }
            }
        }
    } else if (e.object->kind == ExprKind::Subscript) {
        // Struct array element field access: v[i].field — GEP to element, then
        // GEP the field. Element type recovered from the GEP result.
        auto* elem_ptr = generateArrayElementAddress(
            static_cast<const SubscriptExpr&>(*e.object));
        if (elem_ptr) {
            if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(elem_ptr)) {
                auto* rt = gep->getResultElementType();
                if (rt && rt->isStructTy()) {
                    st2_m = llvm::cast<llvm::StructType>(rt);
                    bp_m = elem_ptr;
                }
            }
        }
    }
    if (st2_m && bp_m) {
        // Find the field index by name
        std::string st_name = st2_m->getName().str();
        unsigned fi = 0;
        if (getStructFieldIndex(st_name, e.member_name, fi)) {
            auto* gep = builder_.CreateStructGEP(st2_m, bp_m, fi);
            auto* field_type = st2_m->getElementType(fi);
            if (field_type->isArrayTy()) return gep;
            return builder_.CreateLoad(field_type, gep);
        }
    }
    // If the object is a class pointer (identifier), try action/event resolution
    if (e.object->kind == ExprKind::Identifier) {
        auto& oi = static_cast<const IdentifierExpr&>(*e.object);
        auto* oa = getNamedValue(oi.name);
        if (oa) {
            auto* op = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), oa);
            if (op && current_tu_) {
                // Only allow action/event resolution (for mapping calls etc.)
            }
        }
    }
    // this.prop — internal property access (allowed for class AND struct)
    if (e.object->kind == ExprKind::ThisExpr) {
        auto* ta = getNamedValue("this");
        if (ta) {
            // Check if this is a struct method (current_class_name_ is a struct type key)
            if (struct_types_.count(current_class_name_)) {
                auto* st = getStructType(current_class_name_);
                if (st) {
                    auto* tp = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ta);
                    unsigned fi = 0;
                    if (getStructFieldIndex(current_class_name_, e.member_name, fi)) {
                        auto* gep = builder_.CreateStructGEP(st, tp, fi);
                        auto* field_type = st->getElementType(fi);
                        if (field_type->isArrayTy()) return gep;
                        return builder_.CreateLoad(field_type, gep);
                    }
                }
            } else {
                // Class property access — resolve against the CURRENT class
                // (this.prop). Must not scan all classes: capture slots named
                // "cap_i" are shared across lambda hidden classes, so the first
                // match would be a different lambda's struct.
                auto* tp = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ta);
                if (tp && !current_class_name_.empty()) {
                    const ClassDecl* cls = findClass(current_class_name_);
                    if (cls) {
                        unsigned pi;
                        if (getPropertyIndex(cls->name, e.member_name, pi)) {
                            auto* st = getClassStruct(cls->name);
                            if (st) {
                                auto* gep = builder_.CreateStructGEP(st, tp, pi);
                                return loadPropertyField(gep, *cls, e.member_name);
                            }
                        }
                    }
                }
            }
        }
    }
    // Class instance property access: c.prop — c is a local class instance,
    // prop is a member (array/scalar). Without this branch, `c.data_` wrongly
    // resolved to the instance pointer via the fallback below (corrupting
    // writes like `c.data_[i] = v`).
    if (e.object->kind == ExprKind::Identifier) {
        auto& oi = static_cast<const IdentifierExpr&>(*e.object);
        std::string obj_cls;
        auto vcit = var_class_map_.find(oi.name);
        if (vcit != var_class_map_.end()) obj_cls = vcit->second;
        if (obj_cls.empty() && !current_class_name_.empty() && oi.name == current_class_name_)
            obj_cls = current_class_name_;
        if (!obj_cls.empty()) {
            const ClassDecl* cls = findClass(obj_cls);
            if (cls) {
                unsigned pi = 0;
                if (getPropertyIndex(cls->name, e.member_name, pi)) {
                    auto* oa = getNamedValue(oi.name);
                    if (oa) {
                        auto* op = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), oa, oi.name);
                        auto* st = getClassStruct(cls->name);
                        if (st) {
                            auto* gep = builder_.CreateStructGEP(st, op, pi);
                            return loadPropertyField(gep, *cls, e.member_name);
                        }
                    }
                }
            }
        }
    }
    // M8 structs: `structCall().field` — member access on a call that returns a
    // struct VALUE. The call yields a loaded struct; extract the field by index
    // (structs are values, unlike class refs which use GEP via
    // resolved_object_class). Previously this fell through and returned the
    // whole struct, silently dropping the field.
    if (e.object->kind == ExprKind::Call) {
        auto& scol = static_cast<const CallExpr&>(*e.object);
        const TypeNode* srt = callReturnTypeNode(scol);
        if (srt && !srt->class_name.empty() && findStruct(srt->class_name)) {
            const StructDecl* ssd = findStruct(srt->class_name);
            unsigned sfi = 0;
            if (ssd && getStructFieldIndex(ssd->name, e.member_name, sfi)) {
                auto* op = generateExpr(*e.object);
                if (op && op->getType()->isStructTy())
                    return builder_.CreateExtractValue(op, sfi);
            }
        }
    }
    // obj.method().field — member access on a method call that returns a class
    // instance (e.g. `w.get().x`). Previously fell through to the fallback
    // `return generateExpr(*e.object)` below, dropping the field and passing
    // the raw instance pointer to the consumer (LLVM verify error: call
    // parameter type does not match function signature). Resolve the class from
    // the sema-recorded object type and GEP the property. Also covers
    // slice/array-of-class elements (`s[i].field`, where s[i] returns a class
    // ref) and fresh `new Foo().field`.
    if ((e.object->kind == ExprKind::Call ||
         e.object->kind == ExprKind::Subscript ||
         e.object->kind == ExprKind::NewExpr) &&
        !e.resolved_object_class.empty()) {
        auto* op = generateExpr(*e.object);
        if (op) {
            const ClassDecl* cls = findClass(e.resolved_object_class);
            if (cls) {
                unsigned pi = 0;
                if (getPropertyIndex(cls->name, e.member_name, pi)) {
                    auto* st = getClassStruct(cls->name);
                    if (st) {
                        auto* gep = builder_.CreateStructGEP(st, op, pi);
                        return loadPropertyField(gep, *cls, e.member_name);
                    }
                }
            }
        }
    }
    // Bare struct-typed property field read: `s.field` where `s` is
    // `property: S s;` in the current class (i.e. `this.s.field`). The
    // struct-local path above only resolves identifier LOCALS; bare properties
    // fell through and returned the whole struct value, dropping the field
    // (BUG-010: LLVM verify "Function return type does not match operand type
    // of return inst!" — `ret ptr`/struct where a scalar was expected).
    if (e.object->kind == ExprKind::Identifier &&
        !current_class_name_.empty()) {
        auto& bi = static_cast<const IdentifierExpr&>(*e.object);
        if (!getNamedValue(bi.name)) {   // not a local — bare property name
            const ClassDecl* cls = findClass(current_class_name_);
            if (cls) {
                for (auto& p : cls->properties) {
                    if (p.name != bi.name) continue;   // 属性不一定是第一个
                    const StructDecl* sd = findStruct(p.type.class_name);
                    if (!sd) break;
                    auto* ta = getNamedValue("this");
                    if (!ta) break;
                    unsigned pi = 0, fi = 0;
                    auto* st = getClassStruct(cls->name);
                    auto* stt = getStructType(sd->name);
                    if (!st || !stt) break;
                    if (!getPropertyIndex(cls->name, p.name, pi) ||
                        !getStructFieldIndex(sd->name, e.member_name, fi)) break;
                    auto* tp = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ta);
                    auto* sgep = builder_.CreateStructGEP(st, tp, pi);
                    auto* fgep = builder_.CreateStructGEP(stt, sgep, fi);
                    auto* ft = stt->getElementType(fi);
                    if (ft->isArrayTy()) return fgep;
                    return builder_.CreateLoad(ft, fgep);
                }
            }
        }
    }
    return generateExpr(*e.object);
}

const TypeInfo* CodeGen::sliceTypeOfExpr(const Expr* arr) {
    if (!arr) return nullptr;
    if (arr->kind == ExprKind::Identifier) {
        auto& id = static_cast<const IdentifierExpr&>(*arr);
        auto sit = var_slice_types_.find(id.name);
        return sit != var_slice_types_.end() ? &sit->second : nullptr;
    }
    if (arr->kind == ExprKind::Subscript) {
        // rows[i] where rows is slice<slice<int>> → rows[i] : slice<int>
        auto& ss = static_cast<const SubscriptExpr&>(*arr);
        const TypeInfo* inner = sliceTypeOfExpr(ss.array.get());
        if (inner && inner->kind == TypeKind::Slice)
            return inner->element_type.get();
        return nullptr;
    }
    return nullptr;
}

llvm::Value* CodeGen::generateSliceElementAddress(const Expr* arr, llvm::Value* idx) {
    const TypeInfo* sti = sliceTypeOfExpr(arr);
    if (!sti || sti->kind != TypeKind::Slice || !sti->element_type) return nullptr;
    auto* sval = generateExpr(*arr);
    if (!sval || !sval->getType()->isStructTy()) return nullptr;
    auto* data = builder_.CreateExtractValue(sval, 0);
    auto* len = builder_.CreateExtractValue(sval, 1);
    auto* idx64 = idx;
    if (idx64->getType()->isIntegerTy(32) || idx64->getType()->isIntegerTy(8)
        || idx64->getType()->isIntegerTy(16))
        idx64 = builder_.CreateZExt(idx64, llvm::Type::getInt64Ty(ctx_));
    // Bounds check: 0 <= idx < len
    auto* i64ty = llvm::Type::getInt64Ty(ctx_);
    auto* nonneg = builder_.CreateICmpSGE(idx64, llvm::ConstantInt::get(i64ty, 0));
    auto* inb = builder_.CreateICmpULT(idx64, len);
    auto* ok = builder_.CreateAnd(nonneg, inb);
    auto* be_fn = module_->getFunction("myp_bounds_error");
    if (!be_fn) {
        auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
            {i64ty, i64ty}, false);
        be_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
            "myp_bounds_error", module_.get());
    }
    auto* cur_fn = builder_.GetInsertBlock()->getParent();
    auto* err_bb = llvm::BasicBlock::Create(ctx_, "slice_oob", cur_fn);
    auto* cont_bb = llvm::BasicBlock::Create(ctx_, "slice_ok", cur_fn);
    builder_.CreateCondBr(ok, cont_bb, err_bb);
    builder_.SetInsertPoint(err_bb);
    builder_.CreateCall(be_fn, {idx64, len});
    builder_.CreateBr(cont_bb);
    builder_.SetInsertPoint(cont_bb);
    auto* et = getLLVMType(*sti->element_type);
    return builder_.CreateGEP(et, data, idx64);
}

llvm::Value* CodeGen::generateSubscript(const SubscriptExpr& e) {
    // P1 D10（docs §6.3）：string 下标 s[i] : char。用 myp_str_substring(s,i,i+1)
    // + myp_ord 的安全路径（越界钳制，与 Str.* FFI 一致），返回 char（i8）。
    // 用 resolved_kind==String 判定（exprIsString 会把 string[] 也当字符串——两者
    // 都是指针局部，会误伤 files[i] 这类动态数组下标）。
    if (e.array->resolved_kind == TypeKind::String) {
        auto* sptr = generateExpr(*e.array);
        auto* idx = generateExpr(*e.index);
        auto* i32 = llvm::Type::getInt32Ty(ctx_);
        auto* sub = module_->getFunction("myp_str_substring");
        if (!sub) {
            auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0),
                {llvm::PointerType::get(ctx_, 0), i32, i32}, false);
            sub = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_str_substring", module_.get());
        }
        auto* ord = module_->getFunction("myp_ord");
        if (!ord) {
            auto* ft = llvm::FunctionType::get(i32, {llvm::PointerType::get(ctx_, 0)}, false);
            ord = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_ord", module_.get());
        }
        auto* start = (idx->getType() == i32) ? idx : builder_.CreateTrunc(idx, i32);
        auto* one = llvm::ConstantInt::get(i32, 1);
        auto* end = builder_.CreateAdd(start, one);
        auto* ch = builder_.CreateCall(sub, {sptr, start, end}, "substr");
        auto* code = builder_.CreateCall(ord, {ch}, "ord");
        if (runtime_release_) builder_.CreateCall(runtime_release_, {ch});
        return builder_.CreateTrunc(code, llvm::Type::getInt8Ty(ctx_));
    }
    // §5.1 bitvector<N>[i] : bit —— 提取第 i 位（i1）。位向量值 = LLVM iN，
    // (v >> i) & 1 后截断到 i1。
    if (e.array->resolved_kind == TypeKind::BitVector) {
        auto* a = generateExpr(*e.array);
        auto* idx = generateExpr(*e.index);
        auto* ty = a->getType();
        auto* shifted = idx;
        if (shifted->getType() != ty)
            shifted = builder_.CreateZExtOrTrunc(shifted, ty);
        auto* bit = builder_.CreateLShr(a, shifted, "bvbit");
        auto* one = llvm::ConstantInt::get(ty, 1);
        auto* masked = builder_.CreateAnd(bit, one);
        return builder_.CreateTrunc(masked, llvm::Type::getInt1Ty(ctx_));
    }
    // slice<T>[i] (incl. nested slice<slice<T>> rows[i][j]) — unpack the slice
    // value, bounds-check, GEP, load.
    if (const TypeInfo* sti = sliceTypeOfExpr(e.array.get())) {
        if (sti->kind == TypeKind::Slice && sti->element_type) {
            auto* idx = generateExpr(*e.index);
            auto* p = generateSliceElementAddress(e.array.get(), idx);
            if (p) {
                auto* elem_ty = getLLVMType(*sti->element_type);
                return builder_.CreateLoad(elem_ty, p);
            }
        }
    }
    auto* a = generateExpr(*e.array);
    auto* i = generateExpr(*e.index);
    // With opaque pointers in LLVM 21, we determine the element type
    // from the source-level type info rather than the pointer type.
    // If the array expression is a property or variable of known type,
    // we try to access it as a pointer to the element type.
    // Default: assume i32 elements with a simple GEP.
    llvm::Type* elem_ty = llvm::Type::getInt32Ty(ctx_);

    // Try to determine the actual element type from the array expression
    if (e.array->kind == ExprKind::Identifier) {
        auto& id = static_cast<const IdentifierExpr&>(*e.array);
        // Check class properties
        if (!current_class_name_.empty() && current_tu_) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name != current_class_name_) continue;
                for (auto& p : cls.properties) {
                    if (p.name == id.name && p.type.isArray()) {
                        // Get the element type from the property's type node
                        elem_ty = typeNodeToLLVMType(*p.type.element_type);
                        goto do_gep;
                    }
                }
            }
        }
        // Check local variables
        auto* va = getNamedValue(id.name);
        if (va) {
            llvm::Type* at_x = nullptr;
            if (auto* vai = llvm::dyn_cast<llvm::AllocaInst>(va)) {
                at_x = vai->getAllocatedType();
            } else {
                at_x = getNamedValueType(id.name);
            }
            if (at_x && at_x->isArrayTy()) {
                elem_ty = at_x->getArrayElementType();
            } else if (at_x && at_x->isPointerTy()) {
                // Check if this variable has a recorded array element type
                auto eit = array_elem_types_.find(id.name);
                if (eit != array_elem_types_.end()) {
                    elem_ty = eit->second;
                } else {
                    elem_ty = llvm::Type::getInt32Ty(ctx_);
                }
            }
        }
    } else if (e.array->kind == ExprKind::MemberAccess) {
        auto& ma = static_cast<const MemberAccessExpr&>(*e.array);
        if (ma.object->kind == ExprKind::Identifier) {
            auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
            // Struct variable's array field: `bg.nodes[i]` — resolve the field's
            // element type from the struct declaration (var_class_map_ only covers
            // class instances, so struct fields would otherwise fall through to i32).
            auto* oa = getNamedValue(oi.name);
            llvm::Type* oat = nullptr;
            if (oa) {
                if (auto* oai = llvm::dyn_cast<llvm::AllocaInst>(oa))
                    oat = oai->getAllocatedType();
                else
                    oat = getNamedValueType(oi.name);
            }
            if (oat && oat->isStructTy()) {
                std::string sname = llvm::cast<llvm::StructType>(oat)->getName().str();
                const StructDecl* sd = findStruct(sname);
                if (sd) {
                    for (auto& pr : sd->properties) {
                        if (pr.name == ma.member_name && pr.type.isArray()) {
                            elem_ty = typeNodeToLLVMType(*pr.type.element_type);
                            goto do_gep;
                        }
                    }
                }
            }
            // Resolve variable name to class name via var_class_map_
            std::string obj_class = oi.name;
            auto vcit = var_class_map_.find(oi.name);
            if (vcit != var_class_map_.end())
                obj_class = vcit->second;
            if (current_tu_) {
                for (auto& cls : current_tu_->classes) {
                    if (cls.name != obj_class && cls.name != current_class_name_) continue;
                    for (auto& p : cls.properties) {
                        if (p.name == ma.member_name && p.type.isArray()) {
                            elem_ty = typeNodeToLLVMType(*p.type.element_type);
                            goto do_gep;
                        }
                    }
                }
            }
        }
    }

do_gep:
    // 下标窄整数零扩展（ubyte/uint8 值>=128 不符号扩展成负下标）
    auto* p = builder_.CreateGEP(elem_ty, a, zextIndexValue(builder_, i));
    return builder_.CreateLoad(elem_ty, p);
}

// M3: guard a signed length/dimension against negative values. Emits a branch
// to myp_bounds_error (deterministic abort — never a huge/negative allocation)
// and returns a zero-extended i64 copy safe to multiply into a byte size.
// Accepts any integer width. Constant non-negative lengths skip the runtime
// check (common case: `new slice<int>(64)`).
llvm::Value* CodeGen::guardNonNegativeLen(llvm::Value* v) {
    if (!v || !v->getType()->isIntegerTy()) return v;
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* ty = v->getType();
    // Constant fast path: non-negative literals need no runtime branch.
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
        if (ci->getSExtValue() >= 0) {
            return ty->isIntegerTy(64) ? v : builder_.CreateZExt(v, i64);
        }
    }
    auto* neg = builder_.CreateICmpSLT(v, llvm::ConstantInt::get(ty, 0));
    auto* cur_bb = builder_.GetInsertBlock();
    if (!cur_bb || !cur_bb->getParent()) return v;
    auto* fn = cur_bb->getParent();
    auto* ok_bb = llvm::BasicBlock::Create(ctx_, "len_ok", fn);
    auto* err_bb = llvm::BasicBlock::Create(ctx_, "len_neg", fn);
    builder_.CreateCondBr(neg, err_bb, ok_bb);
    builder_.SetInsertPoint(err_bb);
    auto* be = module_->getFunction("myp_bounds_error");
    if (!be) {
        auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
            {i64, i64}, false);
        be = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
            "myp_bounds_error", module_.get());
    }
    auto* val64 = ty->isIntegerTy(64) ? v : builder_.CreateSExt(v, i64);
    builder_.CreateCall(be, {val64, llvm::ConstantInt::get(i64, 0)});
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(ok_bb);
    return ty->isIntegerTy(64) ? v : builder_.CreateZExt(v, i64);
}

llvm::Value* CodeGen::generateNewExpr(const NewExpr& e) {
    // Built-in slice<T>(n): allocate n*elem bytes, return { data, len }
    if (e.class_name == "slice") {
        auto* elem_ty = typeNodeToLLVMType(e.type_args[0]);
        uint64_t es = module_->getDataLayout().getTypeAllocSize(elem_ty);
        // M3: reject negative length deterministically (myp_bounds_error). A
        // negative int otherwise zexts to 0xFFFFFFFF → a ~16GB allocation
        // instead of an error; a negative long → SIZE_MAX-bytes → allocator
        // OOM abort. Both must be clean bounds errors, never an allocation.
        auto* len_val = guardNonNegativeLen(generateExpr(*e.args[0]));
        auto* byte_size = builder_.CreateMul(len_val,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), es));
        // M8: slice backing is ref-counted (MYP_ARR header layout); every slice
        // VALUE holds a counted reference to it, so copies retain and the last
        // release frees the backing (and its elements). elem_kind tells
        // myp_release how to dispose elements: 0=class refs, 1=scalar/struct
        // (none), 2=nested slice fat pointers.
        const TypeNode& et = e.type_args[0];
        bool class_elements = isArcClassType(et);
        uint32_t elem_kind = class_elements || isStringType(et) ? 0
            : (et.class_name == "slice" ? 2 : 1);
        llvm::Function* alloc_fn = module_->getFunction("myp_alloc_slice_backing");
        if (!alloc_fn) {
            auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0),
                {llvm::Type::getInt64Ty(ctx_), llvm::Type::getInt32Ty(ctx_),
                 llvm::Type::getInt32Ty(ctx_)}, false);
            alloc_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                "myp_alloc_slice_backing", module_.get());
        }
        auto* ptr = builder_.CreateCall(alloc_fn,
            {len_val,
             llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), (uint64_t)es),
             llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), elem_kind)},
            "slice_data");
        if (es > 0)
            builder_.CreateMemSet(ptr, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0), byte_size, llvm::Align(8));
        auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
        auto* slt = llvm::StructType::get(ctx_, {ptr_ty, llvm::Type::getInt64Ty(ctx_)});
        llvm::Value* val = llvm::PoisonValue::get(slt);
        val = builder_.CreateInsertValue(val, ptr, 0);
        val = builder_.CreateInsertValue(val, len_val, 1);
        return val;
    }
    std::string cls_name = e.class_name;
    // Mangle name for generic classes: new Box<int>() → Box_int_inst
    // (type-param placeholders like R are resolved via current_type_params_)
    if (!e.type_args.empty()) {
        cls_name = e.class_name;
        for (auto& ta : e.type_args)
            cls_name += "_" + mangleConcreteTypeNode(ta);
        cls_name += "_inst";
    }
    auto* st = getClassStruct(cls_name);
    if (!st) {
        // Struct type not found — allocate 1 byte (minimum valid pointer)
        // The struct has no properties, so no real storage is needed
        auto* alloc_fn = runtime_alloc_object_;
        if (!alloc_fn) {
            auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0),
                {llvm::Type::getInt64Ty(ctx_), llvm::Type::getInt32Ty(ctx_)}, false);
            alloc_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                "myp_alloc_object", module_.get());
        }
        auto tit = class_type_ids_.find(cls_name);
        uint32_t tid = (tit != class_type_ids_.end()) ? (uint32_t)tit->second : 0;
        auto* obj1 = builder_.CreateCall(alloc_fn,
            {llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 1),
             llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), tid)});
        arcPushTemp(obj1);   // statement-end temp release (consumed by stores)
        return obj1;
    }
    const auto& lay = module_->getDataLayout();
    auto sz = lay.getTypeAllocSize(st);
    // ARC: allocate with an 8-byte { rc, type_id } header. myp_alloc_object
    // returns the data pointer (header sits just before it), so all existing
    // field GEPs / this / vtable accesses are unchanged.
    auto* alloc_fn = runtime_alloc_object_;
    if (!alloc_fn) {
        auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0),
            {llvm::Type::getInt64Ty(ctx_), llvm::Type::getInt32Ty(ctx_)}, false);
        alloc_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
            "myp_alloc_object", module_.get());
    }
    auto tit = class_type_ids_.find(cls_name);
    uint32_t tid = (tit != class_type_ids_.end()) ? (uint32_t)tit->second : 0;
    auto* obj = builder_.CreateCall(alloc_fn,
        {llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), sz > 0 ? sz : 1),
         llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), tid)});
    arcPushTemp(obj);   // statement-end temp release (consumed by stores)
    if (sz > 0)
        builder_.CreateMemSet(obj, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0), sz, llvm::Align(8));

    // Apply declared property default values (`int x = 5;`, class-level
    // `const double T = 0.0253;`). The allocator zero-inits; non-default
    // initializers are stored here on every new. Generic instances are skipped
    // (their template's defaults keep the zero value).
    if (const ClassDecl* cls = findClass(cls_name)) {
        auto* st2 = getClassStruct(cls->name);
        if (st2) {
            for (size_t pi = 0; pi < cls->properties.size(); pi++) {
                auto& prop = cls->properties[pi];
                if (!prop.init_expr) continue;
                llvm::Value* v = generateExpr(*prop.init_expr);
                auto* gep = builder_.CreateStructGEP(st2, obj, pi);
                // BUG-028: 属性初始化器持有 ARC 引用（class/interface/string/
                // slice/counted-array）时，此前直接 store——fresh new 的 rc=1 留在
                // 语句末临时释放列表 → 语句末被 release → 属性槽悬垂（任何后续读取
                // 都是 use-after-free；setter 重赋值释放悬垂值 → 双释放段错误）。
                // 与 `this.prop = value` 赋值路径同语义：alias retain、fresh consume。
                bool fresh = isFreshArcExpr(*prop.init_expr);
                if (isArcRefType(prop.type)) {
                    bool iface_prop = isInterfaceName(prop.type.class_name);
                    arcStoreRef(gep, v, iface_prop, fresh);
                    arcConsumeTemp(v);
                } else if (isStringType(prop.type)) {
                    arcStoreRef(gep, v, false, fresh);
                    arcConsumeTemp(v);
                } else if (prop.type.class_name == "slice") {
                    arcStoreSlice(gep, v, fresh);
                    arcConsumeTemp(v);
                } else if (isCountedArrayType(prop.type)) {
                    arcStoreRef(gep, v, false, fresh);
                    arcConsumeTemp(v);
                }
                builder_.CreateStore(v, gep);
            }
        }
    }

    // 调用匹配的构造器（sema 已解析；分配→默认值→构造体，构造体可覆写默认值）。
    // 泛型实例：模板期 resolved_ctor 基于占位符（`Result_T_E_inst` 类当时可能不存在
    // 或名字带占位符）→ 找不到时按「具体实例类名 + 实参个数」重建 ctor 名。
    auto* ctor = module_->getFunction(e.resolved_ctor);
    if (!ctor && !e.type_args.empty()) {
        for (auto& cls : current_tu_->classes) {
            if (cls.name != cls_name) continue;
            for (auto& a : cls.actions) {
                if (!a.has_constructor) continue;
                if (a.params.size() != e.args.size()) continue;
                auto* f2 = module_->getFunction(constructorMangledName(cls_name, a.name, a.params));
                if (f2) { ctor = f2; break; }
            }
            if (ctor) break;
        }
    }
    if (ctor) {
        std::vector<llvm::Value*> ctor_args;
        ctor_args.push_back(obj);
            auto* ft = ctor->getFunctionType();
            size_t idx = 1;
            for (auto& a : e.args) {
                llvm::Value* v = generateExpr(*a);
                // 隐式类型转换：实参 → 形参（int→double 等，与 generateCall 一致）
                if (idx < ft->getNumParams()) {
                    auto* expected = ft->getParamType(idx);
                    if (v->getType() != expected) {
                        // 接口参数 upcast（同 generateCall）：具体实例 → 接口胖指针
                        if (expected->isStructTy() && isInterfaceFatType(expected) &&
                            v->getType()->isPointerTy() && current_tu_) {
                            std::string cls_name = resolveArgClassName(*a);
                            std::string iface_name = paramIfaceName(ctor, idx - 1);
                            if (!cls_name.empty() && !iface_name.empty()) {
                                llvm::Value* fp = buildInterfaceFat(v, iface_name, cls_name);
                                if (fp && fp->getType()->isStructTy()) { v = fp; }
                            }
                        } else if (v->getType()->isIntegerTy() && expected->isIntegerTy())
                            v = builder_.CreateIntCast(v, expected, true);
                        else if (v->getType()->isIntegerTy() && expected->isFloatingPointTy())
                            v = builder_.CreateSIToFP(v, expected);
                        else if (v->getType()->isFloatingPointTy() && expected->isIntegerTy())
                            v = builder_.CreateFPToSI(v, expected);
                        else if (v->getType()->isPointerTy() && expected->isPointerTy())
                            v = builder_.CreateBitCast(v, expected);
                    }
                }
                ctor_args.push_back(v);
                idx++;
            }
            builder_.CreateCall(ctor, ctor_args);
    }
    return obj;
}

llvm::Value* CodeGen::generateNewArrayExpr(const NewArrayExpr& e) {
    llvm::Value* total = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 1);
    for (auto& dim : e.dimensions) {
        // M3: each dimension is a signed length — reject negative at runtime.
        auto* dim_val = guardNonNegativeLen(generateExpr(*dim));
        total = builder_.CreateMul(total, dim_val);
    }
    auto* elem_ty = typeNodeToLLVMType(e.element_type);
    uint64_t elem_size = module_->getDataLayout().getTypeAllocSize(elem_ty);
    auto* byte_size = builder_.CreateMul(total,
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), elem_size));
    // Class-element array → ref-counted class array: a header { count,
    // elem_size, rc=1, type_id=MYP_ARR_TYPE_ID } precedes the data pointer, so
    // myp_release on the array (scope exit / destroy stub / temp flush)
    // releases every element then frees it.
    if (isArcClassType(e.element_type)) {
        auto* ac = module_->getFunction("myp_alloc_class_array");
        if (!ac) {
            auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0),
                {llvm::Type::getInt64Ty(ctx_), llvm::Type::getInt32Ty(ctx_)}, false);
            ac = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                "myp_alloc_class_array", module_.get());
        }
        auto* ptr = builder_.CreateCall(ac,
            {total, llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), (uint64_t)elem_size)},
            "new_arr");
        // Zero the element area so unreleased slots are NULL (release no-op).
        if (elem_size > 0)
            builder_.CreateMemSet(ptr, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0),
                byte_size, llvm::Align(8));
        arcPushTemp(ptr);   // fresh array rc=1 owned by this statement
        return ptr;
    }
    // M8: dynamic T[] backing is ref-counted too (reuse the slice backing
    // layout) so primitive arrays are reclaimed when the last owner goes away,
    // not at process exit. elem_kind: 0=class/string refs (release each),
    // 1=scalar/struct, 2=slice elements.
    const TypeNode& et2 = e.element_type;
    uint32_t elem_kind2 = isArcClassType(et2) || isStringType(et2) ? 0
        : (et2.class_name == "slice" ? 2 : 1);
    llvm::Function* alloc_fn = module_->getFunction("myp_alloc_slice_backing");
    if (!alloc_fn) {
        auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0),
            {llvm::Type::getInt64Ty(ctx_), llvm::Type::getInt32Ty(ctx_),
             llvm::Type::getInt32Ty(ctx_)}, false);
        alloc_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
            "myp_alloc_slice_backing", module_.get());
    }
    auto* ptr = builder_.CreateCall(alloc_fn,
        {total,
         llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), (uint64_t)elem_size),
         llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), elem_kind2)},
        "new_arr");
    if (elem_size > 0)
        builder_.CreateMemSet(ptr, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0), byte_size, llvm::Align(8));
    // NOTE: no arcPushTemp here — a fresh backing is owned by whichever slot
    // stores it (local kind-0 slot / class property / struct field). Pushing a
    // statement temp would release it at statement end even when a struct field
    // or array element holds it (those stores don't consume temps) → use-after-
    // free (two `Buffer(3)` sharing the same freed backing). An unstored temp
    // backing simply stays rc=1 until exit (leak-safe, like slices).
    return ptr;
}

bool CodeGen::typeIsReference(const TypeInfo& t) {
    switch (t.kind) {
        case TypeKind::Slice:
        case TypeKind::Array:
        case TypeKind::Class:
        case TypeKind::Interface:
        case TypeKind::String:
            return true;
        default:
            return false;
    }
}

llvm::Value* CodeGen::generateThisExpr(const ThisExpr&) {
    // BUG-032: `this` 作为值（实参/赋值/返回，如 Holder.set(this)）须返回实例值，
    // 不能返回 alloca 地址——否则 set 把 &栈槽 存进属性，get 当实例读 → 字段错位
    // /段错误（含 event 类共存时栈布局不同必现；无 event 版碰巧栈位为 0 未暴露）。
    // this.field / this.method() 的 this 由 generateMemberAccess 经 getNamedValue
    // 直取（地址用途），不经本函数，不受影响。
    auto* ta = getNamedValue("this");
    if (ta && llvm::isa<llvm::AllocaInst>(ta))
        return builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ta, "this");
    return ta;
}

llvm::Value* CodeGen::castToI64(llvm::Value* v) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    if (!v) return llvm::ConstantInt::get(i64, 0);
    if (v->getType() == i64) return v;
    if (v->getType()->isIntegerTy())
        return builder_.CreateIntCast(v, i64, true);
    if (v->getType()->isFloatingPointTy())
        return builder_.CreateBitCast(v, i64);
    if (v->getType()->isPointerTy())
        return builder_.CreatePtrToInt(v, i64);
    return llvm::ConstantInt::get(i64, 0);
}

void CodeGen::buildEventNameTable() {
    if (event_table_names_) return;  // already built
    auto* ptr = llvm::PointerType::get(ctx_, 0);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    // Sort by event id for a stable, deterministic order.
    std::vector<std::pair<int, std::string>> evs;
    for (auto& kv : event_id_map_) evs.push_back({kv.second, kv.first});
    std::sort(evs.begin(), evs.end());
    std::vector<llvm::Constant*> name_strs;
    std::vector<llvm::Constant*> ids;
    for (auto& p : evs) {
        name_strs.push_back(builder_.CreateGlobalStringPtr(p.second, "evname"));
        ids.push_back(llvm::ConstantInt::get(i32, p.first));
    }
    event_table_count_ = (int)name_strs.size();
    if (event_table_count_ == 0) {
        event_table_names_ = llvm::ConstantPointerNull::get(ptr);
        event_table_ids_ = llvm::ConstantPointerNull::get(ptr);
        return;
    }
    auto* names_arr = llvm::ConstantArray::get(
        llvm::ArrayType::get(ptr, event_table_count_), name_strs);
    auto* names_gv = new llvm::GlobalVariable(*module_, names_arr->getType(), true,
        llvm::GlobalValue::InternalLinkage, names_arr, "__myp_event_names");
    auto* ids_arr = llvm::ConstantArray::get(
        llvm::ArrayType::get(i32, event_table_count_), ids);
    auto* ids_gv = new llvm::GlobalVariable(*module_, ids_arr->getType(), true,
        llvm::GlobalValue::InternalLinkage, ids_arr, "__myp_event_ids");
    event_table_names_ = names_gv;
    event_table_ids_ = ids_gv;
}

llvm::Value* CodeGen::generateAwaitExpr(const AwaitExpr& e) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);

    // §五-5 形态3: await <@async 调用> — 直接调用（@async 函数内部经 park 原语
    // 挂起/恢复当前协程），不做 yield 值握手；await 表达式的值 = 函数返回值。
    if (e.operand && e.operand->kind == ExprKind::Call) {
        auto& call = static_cast<const CallExpr&>(*e.operand);
        if (call.callee && isAsyncCallTarget(call.callee.get())) {
            auto* v = generateExpr(*e.operand);
            // M8 strings: an @async call returning a string/class ref hands
            // the caller a fresh owned reference (rc=1). generateCall pushed
            // it as a statement temp; the i64 round-trip below transfers it to
            // the caller, so consume the temp NOW — otherwise the statement-end
            // flush frees the string the caller just stored.
            if (v && v->getType()->isPointerTy() && callReturnsArcRef(call))
                arcConsumeTemp(v);
            return v ? castToI64(v) : llvm::ConstantInt::get(i64, 0);
        }
    }

    // await ClassName.eventName — block until the event is fired (C4).
    if (e.operand && e.operand->kind == ExprKind::MemberAccess) {
        auto& ma = static_cast<const MemberAccessExpr&>(*e.operand);
        if (ma.object->kind == ExprKind::Identifier) {
            auto& oid = static_cast<const IdentifierExpr&>(*ma.object);
            int event_id = 0;
            bool is_event = false;
            if (current_tu_) {
                for (auto& cls : current_tu_->classes) {
                    if (cls.name != oid.name) continue;
                    for (auto& ev : cls.events) {
                        if (ev.name == ma.member_name) {
                            auto ekey = cls.name + "::" + ev.name;
                            auto eit = event_id_map_.find(ekey);
                            if (eit != event_id_map_.end()) event_id = eit->second;
                            is_event = true;
                            goto event_found;
                        }
                    }
                }
            }
        event_found:
            if (is_event) {
                if (e.timeout) {
                    // await Signal.go timeout N → wait_event_timeout(id, N, 0)
                    auto wt = module_->getOrInsertFunction("__myp_coro_wait_event_timeout",
                        llvm::FunctionType::get(i64, {i64, i64, i64}, false));
                    auto* tms = castToI64(generateExpr(*e.timeout));
                    return builder_.CreateCall(wt,
                        {llvm::ConstantInt::get(i64, event_id), tms,
                         llvm::ConstantInt::get(i64, 0)}, "await_event");
                }
                // __myp_coro_wait_event(event_id, 0)
                auto wait_fn = module_->getOrInsertFunction("__myp_coro_wait_event",
                    llvm::FunctionType::get(i64, {i64, i64}, false));
                return builder_.CreateCall(wait_fn,
                    {llvm::ConstantInt::get(i64, event_id),
                     llvm::ConstantInt::get(i64, 0)}, "await_event");
            }
        }
    }

    // await expr — suspend, passing `expr` out; evaluates to the value passed
    // in by __myp_coro_resume (e.g. `int v = await n * 2;`)
    llvm::Value* out = llvm::ConstantInt::get(i64, 0);
    if (e.operand) {
        out = generateExpr(*e.operand);
        out = castToI64(out);
    }
    auto* yield_fn = module_->getFunction("__myp_coro_yield");
    if (!yield_fn) {
        auto* ft = llvm::FunctionType::get(i64, {i64}, false);
        yield_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                          "__myp_coro_yield", module_.get());
    }
    return builder_.CreateCall(yield_fn, {out}, "await_val");
}

llvm::Value* CodeGen::generateRange(const RangeExpr& e) {
    // Range evaluation: returns the start value (for use in for loops mainly)
    return generateExpr(*e.start);
}

bool CodeGen::isAsyncCallTarget(const Expr* callee) const {
    if (!callee || !current_tu_) return false;
    if (callee->kind == ExprKind::Identifier) {
        auto& id = static_cast<const IdentifierExpr&>(*callee);
        for (auto& f : current_tu_->functions)
            if (f.name == id.name && f.has_async) return true;
        return false;
    }
    if (callee->kind == ExprKind::MemberAccess) {
        auto& ma = static_cast<const MemberAccessExpr&>(*callee);
        if (ma.object && ma.object->kind == ExprKind::Identifier) {
            auto& oid = static_cast<const IdentifierExpr&>(*ma.object);
            // 类名接收者（静态方法）：Class.method
            for (auto& cls : current_tu_->classes) {
                if (cls.name != oid.name) continue;
                for (auto& a : cls.actions)          // action: 段
                    if (a.name == ma.member_name && a.has_async) return true;
                for (auto& a : cls.static_actions)   // static: 段
                    if (a.name == ma.member_name && a.has_async) return true;
            }
            // 实例接收者：obj.method — 经 var_class_map_ 解析变量所属类
            auto vcit = var_class_map_.find(oid.name);
            if (vcit != var_class_map_.end()) {
                for (auto& cls : current_tu_->classes) {
                    if (cls.name != vcit->second) continue;
                    for (auto& a : cls.actions)
                        if (a.name == ma.member_name && a.has_async) return true;
                }
            }
        }
    }
    return false;
}

llvm::Value* CodeGen::generateTryExpr(const TryExpr& e) {
    // Expression try: try <expr> catch (id) <expr> — value on success, fallback on error.
    // Uses the same per-try jmp_buf + handler stack as the try statement, and a
    // PHI to merge the success/fallback values.
    auto* func = builder_.GetInsertBlock()->getParent();

    auto* jb = createEntryBlockAlloca(func, jmp_buf_type_, "try_expr_jmpbuf");
    auto* jb_ptr = builder_.CreateBitCast(jb, llvm::PointerType::get(ctx_, 0));
    builder_.CreateCall(runtime_exception_push_->getFunctionType(),
        runtime_exception_push_, {jb_ptr});
    auto* result = builder_.CreateCall(runtime_setjmp_->getFunctionType(),
        runtime_setjmp_, {jb_ptr}, "setjmp_result");
    auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    auto* is_error = builder_.CreateICmpNE(result, zero, "is_error");

    auto* try_bb = llvm::BasicBlock::Create(ctx_, "try_expr_try", func);
    auto* catch_bb = llvm::BasicBlock::Create(ctx_, "try_expr_catch");
    auto* merge_bb = llvm::BasicBlock::Create(ctx_, "try_expr_merge");
    builder_.CreateCondBr(is_error, catch_bb, try_bb);

    // Success path: evaluate the try expression
    builder_.SetInsertPoint(try_bb);
    auto* v1 = generateExpr(*e.try_expr);
    auto* last_try = builder_.GetInsertBlock();
    if (!last_try->getTerminator()) builder_.CreateBr(merge_bb);

    // Error path: bind catch variable (string message), evaluate fallback
    func->insert(func->end(), catch_bb);
    builder_.SetInsertPoint(catch_bb);
    auto* err_ptr = builder_.CreateCall(runtime_get_error_->getFunctionType(),
        runtime_get_error_, {}, "err_msg");
    // 拷贝消息（共享 myp_error_msg 缓冲会被后续 throw 覆写）
    auto* dup = module_->getFunction("myp_strdup");
    if (!dup) {
        auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0),
            {llvm::PointerType::get(ctx_, 0)}, false);
        dup = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
            "myp_strdup", module_.get());
    }
    err_ptr = builder_.CreateCall(dup->getFunctionType(), dup, {err_ptr}, "err_copy");
    auto* err_var = createEntryBlockAlloca(func, llvm::PointerType::get(ctx_, 0), e.catch_var_name);
    builder_.CreateStore(err_ptr, err_var);
    setNamedValue(e.catch_var_name, err_var);
    auto* v2 = generateExpr(*e.catch_expr);
    auto* last_catch = builder_.GetInsertBlock();
    if (!last_catch->getTerminator()) builder_.CreateBr(merge_bb);

    // Merge via PHI (cast fallback to the success type if needed)
    func->insert(func->end(), merge_bb);
    builder_.SetInsertPoint(merge_bb);
    if (v2->getType() != v1->getType())
        v2 = convertIntegerValue(builder_, v2, v1->getType(), e.catch_expr.get());
    auto* phi = builder_.CreatePHI(v1->getType(), 2, "try_expr_result");
    phi->addIncoming(v1, last_try);
    phi->addIncoming(v2, last_catch);
    // Pop this try's handler (all paths converge here).
    builder_.CreateCall(runtime_exception_pop_->getFunctionType(),
        runtime_exception_pop_, {});
    return phi;
}

llvm::Value* CodeGen::generateTernary(const TernaryExpr& e) {
    auto* cond = generateExpr(*e.condition);
    if (!cond->getType()->isIntegerTy(1))
        cond = builder_.CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0));
    auto* func = builder_.GetInsertBlock()->getParent();
    auto* true_bb = llvm::BasicBlock::Create(ctx_, "tern_true", func);
    auto* false_bb = llvm::BasicBlock::Create(ctx_, "tern_false");
    auto* merge_bb = llvm::BasicBlock::Create(ctx_, "tern_merge");
    builder_.CreateCondBr(cond, true_bb, false_bb);
    // True branch
    builder_.SetInsertPoint(true_bb);
    size_t before_true = arc_pending_temps_.size();
    auto* true_val = generateExpr(*e.true_expr);
    // ARC: 释放本分支创建的中间临时对象；若结果本身是新类引用临时对象则转移
    // 所有权给 phi（见 generateTernary 末尾）。merge 可从另一分支直达，语句末
    // 释放会违反支配。
    llvm::Value* true_owned = arcEndBranch(before_true, true_val);
    auto* last_true = builder_.GetInsertBlock();
    // False branch
    func->insert(func->end(), false_bb);
    builder_.SetInsertPoint(false_bb);
    size_t before_false = arc_pending_temps_.size();
    auto* false_val = generateExpr(*e.false_expr);
    llvm::Value* false_owned = arcEndBranch(before_false, false_val);
    auto* last_false = builder_.GetInsertBlock();

    // Numeric ternary branches can differ in width (e.g. `? 1 : x` — literal 1
    // is i8, x is i32): widen both to a common type before the phi (matches the
    // binary-op SExt promotion).
    llvm::Type* common_ty = true_val->getType();
    if (false_val->getType() != common_ty) {
        // Struct value vs pointer-to-struct (ArrayList<Struct>.get() boxes
        // elements, a struct constructor returns the value): sema types the
        // ternary as the struct value — load the pointer side.
        if (common_ty->isPointerTy() && false_val->getType()->isStructTy())
            common_ty = false_val->getType();
        else if (common_ty->isIntegerTy() && false_val->getType()->isIntegerTy()) {
            unsigned w = std::max(common_ty->getIntegerBitWidth(),
                                  false_val->getType()->getIntegerBitWidth());
            common_ty = llvm::Type::getIntNTy(ctx_, w);
        } else if (common_ty->isFloatingPointTy() && false_val->getType()->isFloatingPointTy()) {
            if (false_val->getType()->getPrimitiveSizeInBits() > common_ty->getPrimitiveSizeInBits())
                common_ty = false_val->getType();
        } else if (common_ty->isIntegerTy() && false_val->getType()->isFloatingPointTy()) {
            common_ty = false_val->getType();
        }
    }
    // Insert the casts in the branch blocks (before their merge branch), then
    // branch to merge. If a branch already returned/terminated, keep its value.
    auto castIn = [&](llvm::Value* v, llvm::BasicBlock* bb) -> llvm::Value* {
        if (!v || v->getType() == common_ty) return v;
        if (bb->getTerminator()) return v;   // dead path — no cast needed
        llvm::IRBuilder<> tb(bb);            // inserts at end of bb
        if (common_ty->isStructTy() && v->getType()->isPointerTy()) {
            auto* sp = llvm::PointerType::get(common_ty, 0);
            if (v->getType() != sp) v = tb.CreateBitCast(v, sp);
            return tb.CreateLoad(common_ty, v);
        }
        if (common_ty->isIntegerTy() && v->getType()->isIntegerTy())
            return tb.CreateSExt(v, common_ty);
        if (common_ty->isFloatingPointTy() && v->getType()->isFloatingPointTy())
            return tb.CreateFPCast(v, common_ty);
        if (common_ty->isFloatingPointTy() && v->getType()->isIntegerTy())
            return tb.CreateSIToFP(v, common_ty);
        if (common_ty->isIntegerTy() && v->getType()->isFloatingPointTy())
            return tb.CreateFPToSI(v, common_ty);
        return v;
    };
    llvm::Value* t_cast = castIn(true_val, last_true);
    llvm::Value* f_cast = castIn(false_val, last_false);
    if (!last_true->getTerminator()) {
        llvm::IRBuilder<> tb(last_true);
        tb.CreateBr(merge_bb);
    }
    if (!last_false->getTerminator()) {
        llvm::IRBuilder<> tb(last_false);
        tb.CreateBr(merge_bb);
    }
    // Merge
    func->insert(func->end(), merge_bb);
    builder_.SetInsertPoint(merge_bb);
    auto* phi = builder_.CreatePHI(common_ty, 2, "ternary");
    phi->addIncoming(t_cast, last_true);
    phi->addIncoming(f_cast, last_false);
    // If both arms produced fresh class-ref temps, the phi owns whichever arm
    // was taken — push it so the statement-end flush releases it once. If only
    // ONE arm was a fresh temp (mixed owned/borrowed), releasing the phi could
    // double-free the borrowed branch, so the fresh temp is consumed above and
    // intentionally leaked (safe; single object, rare pattern).
    if (true_owned && false_owned) arcPushTemp(phi);
    return phi;
}

llvm::Value* CodeGen::generateEnumVariant(const EnumVariantExpr& e) {
    // Enum variant value → enum struct {disc, payload}.
    if (!e.args.empty()) {
        std::vector<llvm::Value*> arg_vals;
        for (auto& a : e.args)
            arg_vals.push_back(generateExpr(*a));
        return buildEnumVariant(e.enum_name, e.variant_index, arg_vals);
    }
    return buildEnumVariant(e.enum_name, e.variant_index, {});
}

}  // namespace mylang
