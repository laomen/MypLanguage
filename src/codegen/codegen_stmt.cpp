// statement codegen — part of the CodeGen implementation, split from codegen.cpp.
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

void CodeGen::generateStmt(const Stmt& s) {
    switch (s.kind) {
        case StmtKind::Block: generateBlock(static_cast<const BlockStmt&>(s)); break;
        case StmtKind::VarDeclStmt: {
            auto& vds = static_cast<const VarDeclStmt&>(s);
            for (auto& d : vds.decls) generateVarDecl(d);
            break;
        }
        case StmtKind::ExprStmt:
            if (static_cast<const ExprStmt&>(s).expression) generateExpr(*static_cast<const ExprStmt&>(s).expression);
            break;
        case StmtKind::IfStmt: generateIfStmt(static_cast<const IfStmt&>(s)); break;
        case StmtKind::WhileStmt: generateWhileStmt(static_cast<const WhileStmt&>(s)); break;
        case StmtKind::ForStmt: generateForStmt(static_cast<const ForStmt&>(s)); break;
        case StmtKind::ForInStmt: generateForInStmt(static_cast<const ForInStmt&>(s)); break;
        case StmtKind::ReturnStmt: generateReturnStmt(static_cast<const ReturnStmt&>(s)); break;
        case StmtKind::BreakStmt:   generateBreakStmt(static_cast<const BreakStmt&>(s)); break;
        case StmtKind::ContinueStmt: generateContinueStmt(static_cast<const ContinueStmt&>(s)); break;
        case StmtKind::AwaitStmt: generateAwaitStmt(static_cast<const AwaitStmt&>(s)); break;
        case StmtKind::MappingStmt: generateMappingDecl(static_cast<const MappingStmt&>(s).decl, builder_.GetInsertBlock()); break;
        case StmtKind::MatchStmt: generateMatchStmt(static_cast<const MatchStmt&>(s)); break;
        case StmtKind::TryStmt: generateTryStmt(static_cast<const TryStmt&>(s)); break;
        case StmtKind::ThrowStmt: generateThrowStmt(static_cast<const ThrowStmt&>(s)); break;
        case StmtKind::DestructureStmt: generateDestructureStmt(static_cast<const DestructureStmt&>(s)); break;
        default: break;
    }
    // ARC: statement-end temporary release — any fresh `new` whose object was
    // not stored into a slot (e.g. passed as a borrow, or discarded) is freed.
    arcFlushTemps();
}

void CodeGen::generateDestructureStmt(const DestructureStmt& ds) {
    if (!current_function_) return;
    auto* rval = generateExpr(*ds.value);
    if (!rval) return;
    auto* st = llvm::dyn_cast<llvm::StructType>(rval->getType());
    if (!st) return;

    std::function<void(const DestructureTarget&, llvm::Value*, llvm::StructType*)> walk =
        [&](const DestructureTarget& t, llvm::Value* agg, llvm::StructType* sty) {
            size_t idx = 0;
            for (auto& c : t.elements) {
                if (idx >= sty->getNumElements()) return;
                // `_` 忽略符：跳过该元素（不生成绑定，仅推进槽位）
                if (c.name == "_") { idx++; continue; }
                auto* ev = builder_.CreateExtractValue(agg, (unsigned)idx, "tup_el");
                auto* et = sty->getElementType(idx);
                if (!c.name.empty()) {
                    // Leaf
                    llvm::Type* lt = c.has_type
                        ? getLLVMType(typeNodeToCodegenType(c.type))
                        : et;
                    if (ds.is_decl) {
                        auto* a = createEntryBlockAlloca(current_function_, lt, c.name);
                        setNamedValue(c.name, a);
                        setNamedTypedValue(c.name, a, lt);
                        if (ev->getType() != lt) {
                            if (lt->isIntegerTy() && ev->getType()->isIntegerTy())
                                ev = builder_.CreateIntCast(ev, lt, true);
                            else if (lt->isPointerTy() && ev->getType()->isPointerTy())
                                ev = builder_.CreateBitCast(ev, lt);
                        }
                        builder_.CreateStore(ev, a);
                    } else {
                        auto* existing = getNamedValue(c.name);
                        if (!existing) return;
                        auto* existing_ty = llvm::dyn_cast<llvm::AllocaInst>(existing)
                            ? llvm::cast<llvm::AllocaInst>(existing)->getAllocatedType()
                            : ev->getType();
                        if (ev->getType() != existing_ty) {
                            if (existing_ty->isIntegerTy() && ev->getType()->isIntegerTy())
                                ev = builder_.CreateIntCast(ev, existing_ty, true);
                            else if (existing_ty->isPointerTy() && ev->getType()->isPointerTy())
                                ev = builder_.CreateBitCast(ev, existing_ty);
                        }
                        builder_.CreateStore(ev, existing);
                    }
                } else {
                    // Nested tuple
                    if (auto* nested_st = llvm::dyn_cast<llvm::StructType>(et))
                        walk(c, ev, nested_st);
                }
                idx++;
            }
        };
    walk(ds.target, rval, st);
}

void CodeGen::generateBlock(const BlockStmt& s) {
    if (!builder_.GetInsertBlock()) return;
    pushScope();
    for (auto& st : s.statements) {
        if (!st) continue;
        // If the current block already has a terminator (e.g., after __myp_throw),
        // create a new dead block to keep LLVM IR valid
        if (builder_.GetInsertBlock()->getTerminator()) {
            auto* func = builder_.GetInsertBlock()->getParent();
            auto* dead_bb = llvm::BasicBlock::Create(ctx_, "dead", func);
            builder_.SetInsertPoint(dead_bb);
        }
        if (debug_mode_) setDebugLoc(st->range);
        generateStmt(*st);
    }
    popScope();
}

void CodeGen::generateVarDecl(const VarDecl& d) {
    if (!current_function_) return;
    // M-FN-2 nonlocal: 被 lambda 按引用捕获的标量局部 → 提升为堆 cell（共享可变）。
    // 读写走 cell 属性 GEP（与栈 alloca 一致）；cell 对象注册 ARC 槽，作用域退出释放。
    if (!current_fn_nonlocal_vars_.empty() &&
        current_fn_nonlocal_vars_.count(d.name) &&
        current_fn_nonlocal_cell_class_.count(d.name)) {
        TypeInfo vt = typeNodeToCodegenType(d.type);
        llvm::Value* init = d.init_expr ? generateExpr(*d.init_expr) : nullptr;
        promoteNonlocalToCell(d.name, getLLVMType(vt), init);
        return;
    }
    bool arc_decl_class = false;      // ARC: local holds a counted class reference
    bool arc_decl_function = false;   // ARC: local holds a closure (function value)
    bool arc_decl_string = false;     // M8: local holds a counted string reference

    if (d.has_thread_annotation) {
        // @thread: create instance + pass startup to dedicated thread
        TypeInfo vt = TypeInfo(TypeKind::Class);
        vt.class_name = d.type.class_name;
        auto* lt = getLLVMType(vt);
        auto* inst_a = createEntryBlockAlloca(current_function_, lt, d.name);
        setNamedValue(d.name, inst_a);

        llvm::Value* startup_fn = llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0));
        llvm::Value* instance_ptr = llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0));

        if (d.init_expr) {
            auto* v = generateExpr(*d.init_expr);
            builder_.CreateStore(v, inst_a);
            instance_ptr = builder_.CreateLoad(lt, inst_a, d.name);
            // ARC: the thread's startup_arg owns the instance (released in
            // myp_thread_destroy) — drop the statement-end `new` temp so it is
            // not freed while the thread still uses it.
            arcConsumeTemp(v);

            // Store in global for mapping handler access
            if (d.init_expr->kind == ExprKind::NewExpr && !d.type.class_name.empty() && current_tu_) {
                auto& ne = static_cast<const NewExpr&>(*d.init_expr);

                // Store to class-name global (used by mapping handlers)
                auto git = class_instance_globals_.find(ne.class_name);
                if (git != class_instance_globals_.end())
                    builder_.CreateStore(instance_ptr, git->second);

                // Also try variable name
                git = class_instance_globals_.find(d.name);
                if (git != class_instance_globals_.end())
                    builder_.CreateStore(instance_ptr, git->second);

                // Also try main class name global (if this is a @startup class)
                if (d.name == ne.class_name) {
                    std::string cls = ne.class_name;
                    git = class_instance_globals_.find(cls);
                    if (git != class_instance_globals_.end())
                        builder_.CreateStore(instance_ptr, git->second);
                }

                // Find @startup action — the thread will call it, not main
                for (auto& cls : current_tu_->classes) {
                    if (cls.name != ne.class_name) continue;
                    for (auto& act : cls.actions) {
                        if (act.has_startup) {
                            auto fn = cls.name + "_" + act.name;
                            auto* sf = module_->getFunction(fn);
                            if (sf) startup_fn = sf;
                        }
                    }
                }
            }
        } else {
            builder_.CreateStore(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(lt)), inst_a);
        }

        // Create the thread with startup function + instance; thread calls startup, then enters event loop
        auto* thread_type = llvm::PointerType::get(ctx_, 0);
        auto* thr_a = createEntryBlockAlloca(current_function_, thread_type, d.name + "@thr");
        setNamedValue(d.name + "@thr", thr_a);
        auto* thr = builder_.CreateCall(runtime_thread_create_, {startup_fn, instance_ptr}, d.name + "_thr");
        builder_.CreateStore(thr, thr_a);
        // Register instance with this thread for async cross-thread event delivery
        if (runtime_thread_assoc_instance_ && d.init_expr &&
            d.init_expr->kind == ExprKind::NewExpr) {
            auto* loaded = builder_.CreateLoad(lt, inst_a, d.name);
            auto* thr_loaded = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), thr_a, d.name + "_thr");
            builder_.CreateCall(runtime_thread_assoc_instance_, {loaded, thr_loaded});
        }
        thread_handle_names_.push_back(d.name + "@thr");
        builder_.CreateCall(runtime_thread_run_loop_, {thr});
        return;
    }

    // @threadpool: class array where each element runs on its own thread
    if (d.has_threadpool_annotation && d.type.isArray() && d.type.element_type &&
        !d.type.element_type->class_name.empty()) {
        int pool_size = d.type.array_size > 0 ? d.type.array_size : 4;
        std::string elem_class = d.type.element_type->class_name;

        TypeInfo elem_vt(TypeKind::Class);
        elem_vt.class_name = elem_class;
        auto* elem_lt = getLLVMType(elem_vt); // ptr type

        // Allocate array of instance pointers on stack
        auto* arr_type = llvm::ArrayType::get(elem_lt, pool_size);
        auto* arr_a = createEntryBlockAlloca(current_function_, arr_type, d.name + "_arr");

        // Store pointer to first element as the variable value (for GEP-based subscript)
        auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
        auto* first_elem = builder_.CreateGEP(arr_type, arr_a, {zero, zero});
        auto* ptr_a = createEntryBlockAlloca(current_function_, elem_lt, d.name);
        builder_.CreateStore(first_elem, ptr_a);
        setNamedValue(d.name, ptr_a);

        for (int i = 0; i < pool_size; i++) {
            // Create instance via myp_alloc_object (ARC header so the thread
            // destroy path can myp_release(startup_arg) safely).
            auto* st = getClassStruct(elem_class);
            if (!st) continue;
            const auto& lay = module_->getDataLayout();
            auto sz = lay.getTypeAllocSize(st);
            auto* alloc_fn = runtime_alloc_object_;
            if (!alloc_fn) {
                auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0),
                    {llvm::Type::getInt64Ty(ctx_), llvm::Type::getInt32Ty(ctx_)}, false);
                alloc_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                    "myp_alloc_object", module_.get());
            }
            auto tit = class_type_ids_.find(elem_class);
            uint32_t tid = (tit != class_type_ids_.end()) ? (uint32_t)tit->second : 0;
            auto* obj = builder_.CreateCall(alloc_fn,
                {llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), sz > 0 ? sz : 1),
                 llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), tid)});
            if (sz > 0)
                builder_.CreateMemSet(obj, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0), sz, llvm::Align(8));

            // Store in array
            auto* idx = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), i);
            auto* gep = builder_.CreateGEP(arr_type, arr_a, {zero, idx});
            builder_.CreateStore(obj, gep);

            // Find @startup action if any
            llvm::Value* startup_fn = llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0));
            for (auto& cls : current_tu_->classes) {
                if (cls.name != elem_class) continue;
                for (auto& act : cls.actions) {
                    if (act.has_startup) {
                        auto fn = cls.name + "_" + act.name;
                        auto* sf = module_->getFunction(fn);
                        if (sf) startup_fn = sf;
                    }
                }
            }

            // Create thread for each instance
            auto* thr = builder_.CreateCall(runtime_thread_create_, {startup_fn, obj}, d.name + "_thr" + std::to_string(i));
            builder_.CreateCall(runtime_thread_run_loop_, {thr});

            // Associate instance with thread
            if (runtime_thread_assoc_instance_) {
                builder_.CreateCall(runtime_thread_assoc_instance_, {obj, thr});
            }
        }
        return;
    }

    // Resolve a generic type-param placeholder (e.g. `V x` in a generic class
    // body) to the concrete type arg for the current instantiation. Without
    // this, a `V` bound to a non-i32 type (string / class / slice / ...) is
    // typed as the placeholder's default Int: wrong size (a 4-byte stack alloca
    // that an 8-byte pointer store overflows — latent corruption in e.g.
    // HashMap<int,string>.remove rehash) and wrong ARC classification.
    const TypeNode* resolved_tp = nullptr;
    if (!d.type.class_name.empty() && !d.type.isArray() && !d.type.isTuple()) {
        for (auto& tp : current_type_params_)
            if (d.type.class_name == tp.first) { resolved_tp = &tp.second; break; }
    }
    const TypeNode& dt = resolved_tp ? *resolved_tp : d.type;

    TypeInfo vt = builtinTypeToInfo(dt.basic_type);
    // Detect class type from parser's type node
    if (!dt.class_name.empty() && getClassStruct(dt.class_name)) {
        vt = TypeInfo(TypeKind::Class); vt.class_name = dt.class_name;
    }
    // Detect interface type
    bool is_interface = false;
    if (!dt.class_name.empty() && current_tu_) {
        for (auto& ifd : current_tu_->interfaces) {
            if (ifd.name == dt.class_name) {
                vt = TypeInfo(TypeKind::Interface); vt.class_name = dt.class_name;
                is_interface = true;
                break;
            }
        }
    }

    // Detect struct type — allocate inline on stack
    bool is_struct = !dt.class_name.empty() && getStructType(dt.class_name) != nullptr;

    // --- Interface variable: fat pointer {ptr data, ptr vtable} ---
    if (is_interface) {
        auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
        auto* fat_ty = llvm::StructType::get(ctx_, {ptr_ty, ptr_ty});
        auto* a = createEntryBlockAlloca(current_function_, fat_ty, d.name);
        setNamedValue(d.name, a);
        // ARC: interface local holds a counted ref → release its data at scope exit.
        registerArcSlot(a, 1);

        // Zero-init
        auto* zero = llvm::ConstantAggregateZero::get(fat_ty);
        builder_.CreateStore(zero, a);

        if (d.init_expr && d.init_expr->kind == ExprKind::NewExpr) {
            auto& ne = static_cast<const NewExpr&>(*d.init_expr);
            std::string cls_name = ne.class_name;

            // Record concrete class name so mapping handlers can resolve it
            var_class_map_[d.name] = cls_name;

            // Create concrete instance
            auto* inst = generateExpr(*d.init_expr);

            // Store concrete instance in global for mapping handler access
            auto iface_git = class_instance_globals_.find(cls_name);
            if (iface_git == class_instance_globals_.end())
                iface_git = class_instance_globals_.find(d.name);
            // Only pre-created mapping globals are genuine long-lived holders.
            bool preexisting = (iface_git != class_instance_globals_.end());
            if (iface_git == class_instance_globals_.end()) {
                auto* gv = new llvm::GlobalVariable(*module_,
                    llvm::PointerType::get(ctx_, 0), false,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)),
                    "__myp_inst_" + d.name);
                class_instance_globals_[d.name] = gv;
                class_inst_globals_transient_.insert(d.name);   // §五-1 收尾
                iface_git = class_instance_globals_.find(d.name);
            }
            if (iface_git != class_instance_globals_.end()) {
                if (preexisting && !class_inst_globals_transient_.count(d.name))
                    emitRetain(inst);
                builder_.CreateStore(inst, iface_git->second);
            }

            // Get or create vtable for (interface, class) pair
            llvm::GlobalVariable* vgv = nullptr;
            std::string vkey = vt.class_name + "_" + cls_name;
            vgv = module_->getGlobalVariable("__myp_vtable_" + vkey);
            if (!vgv) {
                const InterfaceDecl* iface = nullptr;
                for (auto& ifd : current_tu_->interfaces)
                    if (ifd.name == vt.class_name) { iface = &ifd; break; }
                if (iface) {
                    std::vector<llvm::Constant*> func_ptrs;
                    for (auto& ia : iface->actions) {
                        std::string fn = cls_name + "_" + ia.name;
                        auto* callee = module_->getFunction(fn);
                        // trait 默认实现：类未实现但接口方法带默认体 → 回退默认函数
                        if (!callee && ia.body)
                            callee = module_->getFunction(ifaceDefaultName(iface->name, ia.name, cls_name));
                        if (callee)
                            func_ptrs.push_back(llvm::ConstantExpr::getPointerCast(callee, ptr_ty));
                        else
                            func_ptrs.push_back(llvm::ConstantPointerNull::get(ptr_ty));
                    }
                    auto* arr_type = llvm::ArrayType::get(ptr_ty, func_ptrs.size());
                    auto* arr_init = llvm::ConstantArray::get(arr_type, func_ptrs);
                    vgv = new llvm::GlobalVariable(*module_, arr_type, true,
                        llvm::GlobalValue::InternalLinkage, arr_init,
                        "__myp_vtable_" + vkey);
                }
            }

            // Store instance ptr and vtable ptr in the fat pointer
            auto* data_ptr = builder_.CreateStructGEP(fat_ty, a, 0);
            builder_.CreateStore(inst, data_ptr);
            arcConsumeTemp(inst);   // interface local takes the fresh `new` object

            if (vgv) {
                auto* vtable_ptr = builder_.CreateStructGEP(fat_ty, a, 1);
                auto* bitcast_vt = builder_.CreateBitCast(vgv, ptr_ty);
                builder_.CreateStore(bitcast_vt, vtable_ptr);
            }

            // Auto-call @startup for the concrete instance
            if (current_tu_) {
                for (auto& cls : current_tu_->classes) {
                    if (cls.name != cls_name) continue;
                    for (auto& act : cls.actions) {
                        if (act.has_startup) {
                            auto fn = cls.name + "_" + act.name;
                            auto* sf = module_->getFunction(fn);
                            if (sf) builder_.CreateCall(sf, {inst});
                        }
                    }
                }
            }
        } else if (d.init_expr) {
            auto* inst = generateExpr(*d.init_expr);
            // 接口值直通：init 表达式本身是接口值（函数返回接口 / 另一接口变量）→
            // inst 已是 {data, vtable} 胖指针，整体拷入即可（此前误把它当具体实例
            // 指针存进 data 槽 + 空 vtable，导致后续接口分发参数错乱）。
            if (inst && inst->getType()->isStructTy() &&
                inst->getType()->isAggregateType()) {
                auto* data = builder_.CreateExtractValue(inst, 0);
                // ARC：别名持有 → retain data（fresh call 转移，作用域末释放）
                if (!isFreshArcExpr(*d.init_expr)) emitRetain(data);
                auto* dp = builder_.CreateStructGEP(fat_ty, a, 0);
                builder_.CreateStore(data, dp);
                auto* vt = builder_.CreateExtractValue(inst, 1);
                auto* vp = builder_.CreateStructGEP(fat_ty, a, 1);
                builder_.CreateStore(vt, vp);
                return;
            }
            // Assign from an existing concrete variable: IFoo f = impl;
            // ARC: alias of an existing owner → retain its data (fresh new/call
            // transfers; release happens when this interface local's scope exits).
            if (!isFreshArcExpr(*d.init_expr))
                emitRetain(inst);
            std::string cls_name;
            if (d.init_expr->kind == ExprKind::Identifier) {
                auto& id = static_cast<const IdentifierExpr&>(*d.init_expr);
                auto vit = var_class_map_.find(id.name);
                if (vit != var_class_map_.end()) cls_name = vit->second;
            }
            // Look up or find class name from the expression type
            if (cls_name.empty() && d.init_expr->kind == ExprKind::Identifier) {
                auto& id = static_cast<const IdentifierExpr&>(*d.init_expr);
                auto vit = var_class_map_.find(id.name);
                if (vit != var_class_map_.end()) cls_name = vit->second;
            }
            if (!cls_name.empty()) {
                var_class_map_[d.name] = cls_name;
                // Build vtable
                llvm::GlobalVariable* vgv = nullptr;
                std::string vkey = vt.class_name + "_" + cls_name;
                vgv = module_->getGlobalVariable("__myp_vtable_" + vkey);
                if (!vgv) {
                    const InterfaceDecl* iface = nullptr;
                    for (auto& ifd : current_tu_->interfaces)
                        if (ifd.name == vt.class_name) { iface = &ifd; break; }
                    if (iface) {
                        std::vector<llvm::Constant*> func_ptrs;
                        for (auto& ia : iface->actions) {
                            std::string fn = cls_name + "_" + ia.name;
                            auto* callee = module_->getFunction(fn);
                            // trait 默认实现：类未实现但接口方法带默认体 → 回退默认函数
                            if (!callee && ia.body)
                                callee = module_->getFunction(ifaceDefaultName(iface->name, ia.name, cls_name));
                            if (callee)
                                func_ptrs.push_back(llvm::ConstantExpr::getPointerCast(callee, ptr_ty));
                            else
                                func_ptrs.push_back(llvm::ConstantPointerNull::get(ptr_ty));
                        }
                        auto* arr_type = llvm::ArrayType::get(ptr_ty, func_ptrs.size());
                        auto* arr_init = llvm::ConstantArray::get(arr_type, func_ptrs);
                        vgv = new llvm::GlobalVariable(*module_, arr_type, true,
                            llvm::GlobalValue::InternalLinkage, arr_init,
                            "__myp_vtable_" + vkey);
                    }
                }
                auto* data_ptr = builder_.CreateStructGEP(fat_ty, a, 0);
                builder_.CreateStore(inst, data_ptr);
                if (vgv) {
                    auto* vtable_ptr = builder_.CreateStructGEP(fat_ty, a, 1);
                    auto* bitcast_vt = builder_.CreateBitCast(vgv, ptr_ty);
                    builder_.CreateStore(bitcast_vt, vtable_ptr);
                }
            } else {
                // Unknown concrete type — just store instance, null vtable
                auto* data_ptr = builder_.CreateStructGEP(fat_ty, a, 0);
                builder_.CreateStore(inst, data_ptr);
            }
        }
        return;
    }

    // Handle `var` type inference: use init expression's type
    if (d.type.is_inferred && d.init_expr) {
        auto* init_val = generateExpr(*d.init_expr);
        auto* a = createEntryBlockAlloca(current_function_, init_val->getType(), d.name);
        setNamedValue(d.name, a);
        builder_.CreateStore(init_val, a);
        // ARC: `var x = new Foo()` → class ref slot (released at scope exit).
        if (d.init_expr->kind == ExprKind::NewExpr) {
            registerArcSlot(a, 0);
            arcConsumeTemp(init_val);
        }
        return;
    }

    // slice<T> — value type { T* data; int64 len }, allocated on stack
    if (dt.class_name == "slice" && dt.type_args.size() == 1) {
        TypeInfo st(TypeKind::Slice);
        st.element_type = std::make_shared<TypeInfo>(
            typeNodeToCodegenType(dt.type_args[0]));
        auto* slt = getLLVMType(st);
        auto* a = createEntryBlockAlloca(current_function_, slt, d.name);
        setNamedValue(d.name, a);
        if (d.init_expr) {
            auto* v = generateExpr(*d.init_expr);
            if (v && v->getType() == slt) {
                // M8: the slice slot holds a counted reference to the backing.
                // A fresh (new slice / call) result transfers its rc=1; an
                // alias (slice b = a) must retain the shared backing.
                if (!isFreshArcExpr(*d.init_expr))
                    emitRetainSlice(v);
                builder_.CreateStore(v, a);
                registerArcSlot(a, 4);
            }
        } else {
            builder_.CreateStore(llvm::ConstantAggregateZero::get(slt), a);
        }
        var_slice_types_[d.name] = st;
        return;
    }

    llvm::Type* lt;
    if (is_struct) {
        lt = getStructType(dt.class_name);
    } else if (dt.isArray() && dt.element_type) {
        // Array type
        if (dt.array_size > 0) {
            TypeInfo arr_ti = typeNodeToCodegenType(dt);
            lt = getLLVMType(arr_ti);
            auto* arr_a = createEntryBlockAlloca(current_function_, lt, d.name + "_arr");
            auto arr_sz = module_->getDataLayout().getTypeAllocSize(lt);
            if (arr_sz > 0)
                builder_.CreateMemSet(arr_a, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0), arr_sz, llvm::Align(8));
            // 固定数组带初始化：拷贝源数据（lambda 捕获 `T[N] a = this.cap_i;` /
            // `T[N] b = otherArray`）。源可能为 [N x T] 值（属性加载）或指针（首元素
            // 数据指针 / [N x T]*）→ 分别 store / memcpy。
            if (d.init_expr) {
                auto* src = generateExpr(*d.init_expr);
                if (src) {
                    if (src->getType() == lt) {
                        builder_.CreateStore(src, arr_a);
                    } else if (src->getType()->isPointerTy() && arr_sz > 0) {
                        builder_.CreateMemCpy(arr_a, llvm::Align(8), src, llvm::Align(8), arr_sz);
                    }
                }
            }
            auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
            auto* elem_ptr = builder_.CreateGEP(lt, arr_a, {zero, zero});
            auto* ptr_a = createEntryBlockAlloca(current_function_, llvm::PointerType::get(ctx_, 0), d.name);
            builder_.CreateStore(elem_ptr, ptr_a);
            setNamedValue(d.name, ptr_a);
            // 记录固定数组栈变量大小：return 该变量时需堆拷贝避免悬垂指针
            stack_array_sizes_[d.name] = arr_sz;
            // ARC: fixed class/string-array — elements are strong slots;
            // release them at scope exit (the backing stack buffer is not
            // freed). Counted strings share the class release machinery.
            if (isArcClassType(*dt.element_type) || isStringType(*dt.element_type)) {
                registerArcSlot(ptr_a, 3);
                arc_fixed_array_counts_[ptr_a] = (uint64_t)dt.array_size;
            }
        } else {
            auto* ptr_a = createEntryBlockAlloca(current_function_, llvm::PointerType::get(ctx_, 0), d.name);
            llvm::Value* dyn_init_val = nullptr;   // for ARC transfer below
            // Handle initializer: double[] buf = new double[n]
            if (d.init_expr) {
                dyn_init_val = generateExpr(*d.init_expr);
                builder_.CreateStore(dyn_init_val, ptr_a);
                // Track array byte size for GPU data transfer
                if (d.init_expr->kind == ExprKind::NewArrayExpr) {
                    auto& nae = static_cast<const NewArrayExpr&>(*d.init_expr);
                    auto* elem_ty_na = typeNodeToLLVMType(nae.element_type);
                    auto* alloca_lt = llvm::Type::getInt64Ty(ctx_);
                    uint64_t es_na = module_->getDataLayout().getTypeAllocSize(elem_ty_na);
                    llvm::Value* total_na = llvm::ConstantInt::get(alloca_lt, 1);
                    for (auto& dim : nae.dimensions) {
                        auto* dim_val = generateExpr(*dim);
                        if (dim_val->getType()->isIntegerTy(32))
                            dim_val = builder_.CreateZExt(dim_val, alloca_lt);
                        total_na = builder_.CreateMul(total_na, dim_val);
                    }
                    auto* bs = builder_.CreateMul(total_na,
                        llvm::ConstantInt::get(alloca_lt, es_na));
                    array_byte_sizes_[d.name] = bs;
                }
            } else {
                builder_.CreateStore(llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)), ptr_a);
            }
            setNamedValue(d.name, ptr_a);
            // ARC/M8: dynamic T[] backing is ref-counted (class arrays + now
            // primitive arrays via myp_alloc_slice_backing) — the local owns a
            // counted reference released on scope exit.
            registerArcSlot(ptr_a, 0);
            if (d.init_expr) {
                if (isFreshArcExpr(*d.init_expr))
                    arcConsumeTemp(dyn_init_val);   // fresh (new-array/call) rc=1 → local owns
                else if (dyn_init_val)
                    emitRetain(dyn_init_val);       // alias → local retains
            }
        }
        // Record element type for subscript access
        array_elem_types_[d.name] = getLLVMType(typeNodeToCodegenType(*dt.element_type));
        // ARC: record whether elements are class/string references
        // (retain/release on store, release on array free).
        array_elem_is_class_[d.name] = isArcClassType(*dt.element_type) ||
                                       isStringType(*dt.element_type);
        // Record the element class name (mangled for generics) so `arr[i].method()`
        // dispatch resolves to the concrete instance's method.
        if (array_elem_is_class_[d.name]) {
            const TypeNode* et = dt.element_type.get();
            if (!et->type_args.empty())
                array_elem_class_map_[d.name] = mangleConcreteTypeNode(*et);
            else
                array_elem_class_map_[d.name] = et->class_name;
        }
        return;
    } else if (dt.isFunction()) {
        // First-class function value: fat pointer {closure, call_fn}
        TypeInfo ft = typeNodeToCodegenType(dt);
        lt = getLLVMType(ft);
        func_val_types_[d.name] = ft;
        arc_decl_function = true;   // ARC: closure released at scope exit
    } else if (dt.isTuple()) {
        // Tuple value type: anonymous struct { T0, T1, ... }
        TypeInfo tt = typeNodeToCodegenType(dt);
        lt = getLLVMType(tt);
    } else {
        vt = builtinTypeToInfo(dt.basic_type);
        bool arc_class = false;
        if (isStringType(dt)) {
            // M8: strings are counted — the local owns a strong string ref.
            arc_decl_string = true;
        } else if (!dt.class_name.empty() && findEnum(dt.class_name)) {
            // Enum value type: { i32 disc, [N x i8] payload }
            vt = TypeInfo(TypeKind::Enum);
            vt.class_name = dt.class_name;
        } else if (!dt.class_name.empty() && getClassStruct(dt.class_name)) {
            std::string cls_name = dt.class_name;
            // Mangle name for generic classes: Box<int> → Box_int_inst
            // (type-param placeholders like R are resolved via current_type_params_)
            if (!dt.type_args.empty()) {
                cls_name = dt.class_name;
                for (auto& ta : dt.type_args)
                    cls_name += "_" + mangleConcreteTypeNode(ta);
                cls_name += "_inst";
            }
            vt = TypeInfo(TypeKind::Class); vt.class_name = cls_name;
            arc_class = true;
            // Track variable → class mapping for method resolution
            var_class_map_[d.name] = cls_name;
        }
        lt = getLLVMType(vt);
        if (arc_class) arc_decl_class = true;
    }

    auto* a = createEntryBlockAlloca(current_function_, lt, d.name);
    setNamedValue(d.name, a);
    // ARC: a local class reference is released when its scope exits.
    if (arc_decl_class) registerArcSlot(a, 0);
    // M8: a local string reference is released when its scope exits (kind 0
    // shares the class-ptr release machinery — myp_release on a char*).
    if (arc_decl_string) registerArcSlot(a, 0);
    // ARC: a local function-value's closure is released at scope exit (the fat
    // pointer's index 0 is the closure; null closures are no-ops on release).
    if (arc_decl_function) registerArcSlot(a, 2);

    if (is_struct) {
        // Zero-initialize struct
        auto sz = module_->getDataLayout().getTypeAllocSize(lt);
        builder_.CreateMemSet(a, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0),
                              llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), sz), llvm::Align(8));
        // M8 structs: an OWNED struct local whose fields hold ARC references is
        // registered as a kind-5 slot — its fields are released at scope exit.
        // (A struct PARAM is borrowed: not registered, field stores stay plain.)
        const StructDecl* sd = findStruct(dt.class_name);
        if (sd && isArcFieldType(dt)) {
            registerArcSlot(a, 5);
            arc_struct_slot_types_[a] = dt.class_name;
        }
    } else if (lt->isPointerTy()) {
        builder_.CreateStore(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(lt)), a);
    } else if (lt->isStructTy()) {
        // Fat pointer (function value) / interface — zero via memset
        auto sz = module_->getDataLayout().getTypeAllocSize(lt);
        builder_.CreateMemSet(a, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0),
                              llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), sz), llvm::Align(8));
    } else {
        builder_.CreateStore(llvm::ConstantInt::get(lt, 0), a);
    }

    if (d.init_expr) {
        auto* v = generateExpr(*d.init_expr);
        if (v->getType() != lt)
            v = convertIntegerValue(builder_, v, lt, d.init_expr.get());
        // ARC: fresh (new / call) transfers into the slot; an alias (var/prop)
        // must retain because the previous owner keeps its reference.
        if (arc_decl_class && !isFreshArcExpr(*d.init_expr))
            emitRetain(v);
        // M8 strings: same discipline — fresh (call/concat) transfers its rc=1;
        // an alias (string b = s / literal) retains the shared string.
        if (arc_decl_string && !isFreshArcExpr(*d.init_expr))
            emitRetain(v);
        // ARC: function-value alias (`g = f`) → retain the closure (index 0).
        if (arc_decl_function && !isFreshArcExpr(*d.init_expr) &&
            v->getType()->isStructTy()) {
            auto* cl = builder_.CreateExtractValue(v, 0);
            emitRetain(cl);
        }
        // M8 structs: an ALIAS init (`struct S s2 = s1;`) copies the value —
        // retain the ARC fields so the copy owns them (s1 keeps its own). A
        // FRESH init (`var s = makeStruct();`) already has +1 fields from the
        // return retain — no extra retain, ownership transfers to this local.
        if (is_struct && !isFreshArcExpr(*d.init_expr)) {
            const StructDecl* sd = findStruct(dt.class_name);
            if (sd && isArcFieldType(dt))
                emitStructFieldsValue(builder_, v, *sd, true);
        }
        builder_.CreateStore(v, a);
        // §五-1 收尾: mirror this slot's live object into the coroutine frame
        // (released by Coro.destroy / an uncaught exception). Strings are
        // counted refs too — mirror them so early coroutine teardown releases
        // (not leaks) their backing.
        if (current_is_coro_ && (arc_decl_class || arc_decl_function || arc_decl_string))
            emitCoroFrameSet(a, v);
        arcConsumeTemp(v);   // a fresh `new` temp is now owned by the local

        // Store instance in global for mapping handler access
        if (d.init_expr->kind == ExprKind::NewExpr && !d.type.class_name.empty() && current_tu_) {
            auto& ne = static_cast<const NewExpr&>(*d.init_expr);
            // Check both by class name and variable name
            auto git = class_instance_globals_.find(ne.class_name);
            if (git == class_instance_globals_.end())
                git = class_instance_globals_.find(d.name);
            // Only globals pre-created for mapping source nodes are genuine
            // long-lived holders → retain those. On-the-fly convenience globals
            // must NOT keep the object alive (would defeat ARC for plain locals).
            bool preexisting = (git != class_instance_globals_.end());
            if (git == class_instance_globals_.end()) {
                // Create global on-the-fly for function-level mapping access
                auto* gv = new llvm::GlobalVariable(*module_,
                    llvm::PointerType::get(ctx_, 0), false,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)),
                    "__myp_inst_" + d.name);
                class_instance_globals_[d.name] = gv;
                class_inst_globals_transient_.insert(d.name);   // §五-1 收尾
                git = class_instance_globals_.find(d.name);
            }
            if (git != class_instance_globals_.end()) {
                auto* loaded = builder_.CreateLoad(lt, a, d.name);
                if (preexisting && !class_inst_globals_transient_.count(d.name))
                    emitRetain(loaded);
                builder_.CreateStore(loaded, git->second);
            }
            // NOTE: legacy `new C(args)` → auto-call `@startup init` binding was
            // REMOVED (v3.9 migration): constructors handle initialization via
            // generateNewExpr; @startup is strictly a start signal (thread entry).
        }
    }
}

void CodeGen::generateIfStmt(const IfStmt& s) {
    if (!s.condition) return;
    size_t before_cond = arc_pending_temps_.size();
    auto* c = generateExpr(*s.condition);
    if (!c->getType()->isIntegerTy(1)) c = builder_.CreateICmpNE(c, llvm::ConstantInt::get(c->getType(), 0));
    // M7: condition temps (e.g. a @weak upgrade / class-returning call) are
    // used only to derive the branch i1 — release them here so a branch body's
    // statement-end flush cannot grab them in a conditional block (leak).
    arcReleaseConditionTemps(before_cond);
    auto* f = builder_.GetInsertBlock()->getParent();
    auto* tbb = llvm::BasicBlock::Create(ctx_, "then", f);
    auto* ebb = llvm::BasicBlock::Create(ctx_, "else");
    auto* mbb = llvm::BasicBlock::Create(ctx_, "ifcont");
    builder_.CreateCondBr(c, tbb, ebb);
    builder_.SetInsertPoint(tbb);
    if (s.then_block) generateStmt(*s.then_block);
    bool then_terminated = (builder_.GetInsertBlock()->getTerminator() != nullptr);
    if (!then_terminated) builder_.CreateBr(mbb);
    f->insert(f->end(), ebb);
    builder_.SetInsertPoint(ebb);
    if (s.else_block) generateStmt(*s.else_block);
    bool else_terminated = (builder_.GetInsertBlock()->getTerminator() != nullptr);
    if (!else_terminated) builder_.CreateBr(mbb);
    if (then_terminated && else_terminated) {
        // Both branches terminated — mbb is unreachable; leave inserter at else block
        // (it's already terminated, so subsequent code will handle it)
    } else {
        f->insert(f->end(), mbb);
        builder_.SetInsertPoint(mbb);
    }
}

void CodeGen::generateWhileStmt(const WhileStmt& s) {
    auto* f = builder_.GetInsertBlock()->getParent();
    auto* cbb = llvm::BasicBlock::Create(ctx_, "while_cond", f);
    auto* bbb = llvm::BasicBlock::Create(ctx_, "while_body");
    auto* abb = llvm::BasicBlock::Create(ctx_, "while_end");
    builder_.CreateBr(cbb);
    builder_.SetInsertPoint(cbb);
    if (s.condition) {
        size_t before_cond = arc_pending_temps_.size();
        auto* c = generateExpr(*s.condition);
        if (!c->getType()->isIntegerTy(1)) c = builder_.CreateICmpNE(c, llvm::ConstantInt::get(c->getType(), 0));
        arcReleaseConditionTemps(before_cond);
        builder_.CreateCondBr(c, bbb, abb);
    } else builder_.CreateBr(bbb);
    f->insert(f->end(), bbb);
    builder_.SetInsertPoint(bbb);
    loop_context_.push_back({cbb, abb});
    if (s.body) generateStmt(*s.body);
    loop_context_.pop_back();
    if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(cbb);
    f->insert(f->end(), abb);
    builder_.SetInsertPoint(abb);
}

void CodeGen::generateForStmt(const ForStmt& s) {
    if (s.gpu) {
        generateGpuFor(s);
        return;
    }
    if (s.parallel) {
        generateParallelFor(s);
        return;
    }
    pushScope();
    if (s.init) generateStmt(*s.init);
    auto* f = builder_.GetInsertBlock()->getParent();
    auto* cbb = llvm::BasicBlock::Create(ctx_, "for_cond", f);
    auto* bbb = llvm::BasicBlock::Create(ctx_, "for_body");
    auto* sbb = llvm::BasicBlock::Create(ctx_, "for_step");
    auto* abb = llvm::BasicBlock::Create(ctx_, "for_end");
    builder_.CreateBr(cbb);
    builder_.SetInsertPoint(cbb);
    if (s.condition) {
        size_t before_cond = arc_pending_temps_.size();
        auto* c = generateExpr(*s.condition);
        if (!c->getType()->isIntegerTy(1)) c = builder_.CreateICmpNE(c, llvm::ConstantInt::get(c->getType(), 0));
        arcReleaseConditionTemps(before_cond);
        builder_.CreateCondBr(c, bbb, abb);
    } else builder_.CreateBr(bbb);
    f->insert(f->end(), bbb);
    builder_.SetInsertPoint(bbb);
    loop_context_.push_back({sbb, abb});  // continue → sbb (step), break → abb (end)
    if (s.body) generateStmt(*s.body);
    loop_context_.pop_back();
    if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(sbb);
    f->insert(f->end(), sbb);
    builder_.SetInsertPoint(sbb);
    if (s.step) generateExpr(*s.step);
    builder_.CreateBr(cbb);
    f->insert(f->end(), abb);
    builder_.SetInsertPoint(abb);
    popScope();
}

void CodeGen::generateForInStmt(const ForInStmt& s) {
    if (!current_function_) return;
    // 守卫：泛型模板体内 for-in 若未被 sema 注解（iter_kind 默认 0 且 size_fn 空），
    // 给出明确错误而非用 void 循环变量崩溃（§四-2 × 泛型）。
    if (s.iter_kind == 0 && s.size_fn.empty() &&
        !(s.iterable && s.iterable->kind == ExprKind::Range)) {
        diag_.error(s.range, "for-in over a generic-typed iterable was not resolved; "
            "iterate by index (a.size()/a.get(i)) or use a concrete iterable");
        return;
    }
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* f = builder_.GetInsertBlock()->getParent();

    pushScope();
    // 迭代集合求值一次 → 临时（class 集合需要 ARC 管理；数组/slice 为借用）
    auto* iter_val = generateExpr(*s.iterable);
    auto* iter_ty = iter_val->getType();
    auto* iter_a = createEntryBlockAlloca(current_function_, iter_ty, "__for_coll");
    builder_.CreateStore(iter_val, iter_a);
    if (s.iter_kind == 0) {
        // class 集合：临时持有引用——别名需 retain（fresh call 转移），循环末释放
        if (!isFreshArcExpr(*s.iterable))
            emitRetain(iter_val);
        registerArcSlot(iter_a, 0);
    }

    auto* elem_ty = getLLVMType(s.elem_type);

    // 上界
    llvm::Value* size_v = llvm::ConstantInt::get(i32, 0);
    if (s.iter_kind == 0) {
        auto* size_fn = module_->getFunction(s.size_fn);
        if (size_fn) {
            auto* coll = builder_.CreateLoad(iter_ty, iter_a);
            size_v = builder_.CreateCall(size_fn->getFunctionType(), size_fn, {coll}, "for_n");
        }
    } else if (s.iter_kind == 1) {
        size_v = llvm::ConstantInt::get(i32, s.array_size);
    } else if (s.iter_kind == 2) {
        auto* sv = builder_.CreateLoad(iter_ty, iter_a);
        size_v = builder_.CreateTrunc(builder_.CreateExtractValue(sv, 1), i32, "for_n");
    } else if (s.iter_kind == 3) {
        auto& re = static_cast<RangeExpr&>(*s.iterable);
        auto* end_v = generateExpr(*re.end);
        if (end_v->getType() != i32) end_v = builder_.CreateIntCast(end_v, i32, true);
        // 右开 range a..b → 迭代次数 = b - a（循环变量 = start + idx）。
        // 之前用 end 作次数：a=0 时恰好正确，a≠0（如 1..6）会多迭代一次。
        auto* start_v = generateExpr(*re.start);
        if (start_v->getType() != i32) start_v = builder_.CreateIntCast(start_v, i32, true);
        size_v = builder_.CreateSub(end_v, start_v, "for_n");
    }
    if (!size_v) size_v = llvm::ConstantInt::get(i32, 0);

    auto* idx_a = createEntryBlockAlloca(current_function_, i32, "__for_i");
    builder_.CreateStore(llvm::ConstantInt::get(i32, 0), idx_a);

    auto* cbb = llvm::BasicBlock::Create(ctx_, "for_cond", f);
    auto* bbb = llvm::BasicBlock::Create(ctx_, "for_body");
    auto* sbb = llvm::BasicBlock::Create(ctx_, "for_step");
    auto* abb = llvm::BasicBlock::Create(ctx_, "for_end");
    builder_.CreateBr(cbb);
    builder_.SetInsertPoint(cbb);
    auto* idx0 = builder_.CreateLoad(i32, idx_a);
    builder_.CreateCondBr(builder_.CreateICmpSLT(idx0, size_v, "for_cmp"), bbb, abb);

    f->insert(f->end(), bbb);
    builder_.SetInsertPoint(bbb);
    {
        pushScope();
        auto* var_a = createEntryBlockAlloca(current_function_, elem_ty, s.var_name);
        setNamedValue(s.var_name, var_a);
        llvm::Value* elem = nullptr;
        auto* idx1 = builder_.CreateLoad(i32, idx_a);
        auto* coll = builder_.CreateLoad(iter_ty, iter_a);
        if (s.iter_kind == 0) {
            auto* get_fn = module_->getFunction(s.get_fn);
            if (get_fn)
                elem = builder_.CreateCall(get_fn->getFunctionType(), get_fn, {coll, idx1}, "for_el");
        } else if (s.iter_kind == 1) {
            auto* p = builder_.CreateGEP(elem_ty, coll, idx1);
            elem = builder_.CreateLoad(elem_ty, p, "for_el");
        } else if (s.iter_kind == 2) {
            auto* data = builder_.CreateExtractValue(coll, 0);
            auto* idx64 = builder_.CreateZExt(idx1, i64);
            auto* p = builder_.CreateGEP(elem_ty, data, idx64);
            elem = builder_.CreateLoad(elem_ty, p, "for_el");
        } else {  // range
            auto& re = static_cast<RangeExpr&>(*s.iterable);
            auto* start_v = generateExpr(*re.start);
            if (start_v->getType() != i32) start_v = builder_.CreateIntCast(start_v, i32, true);
            elem = builder_.CreateAdd(start_v, idx1, "for_el");
        }
        if (elem && elem->getType() != elem_ty) {
            if (elem_ty->isIntegerTy() && elem->getType()->isIntegerTy())
                elem = builder_.CreateIntCast(elem, elem_ty, true);
            else if (elem_ty->isFloatingPointTy() && elem->getType()->isIntegerTy())
                elem = builder_.CreateSIToFP(elem, elem_ty);
            else if (elem_ty->isPointerTy() && elem->getType()->isPointerTy())
                elem = builder_.CreateBitCast(elem, elem_ty);
        }
        builder_.CreateStore(elem, var_a);
        // ARC：类元素——class get 结果是 fresh(+1) 转移；数组/slice 下标是借用需 retain。
        // 循环变量在迭代作用域末释放（balanced）。
        bool elem_arc = s.elem_type.kind == TypeKind::Class ||
                        s.elem_type.kind == TypeKind::Interface;
        if (elem_arc) {
            if (s.iter_kind != 0 && elem)
                emitRetain(elem);
            registerArcSlot(var_a, s.elem_type.kind == TypeKind::Interface ? 1 : 0);
        }
        loop_context_.push_back({sbb, abb});
        if (s.body) generateStmt(*s.body);
        loop_context_.pop_back();
        popScope();
    }
    if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(sbb);
    f->insert(f->end(), sbb);
    builder_.SetInsertPoint(sbb);
    auto* idx2 = builder_.CreateLoad(i32, idx_a);
    builder_.CreateStore(builder_.CreateAdd(idx2, llvm::ConstantInt::get(i32, 1)), idx_a);
    builder_.CreateBr(cbb);
    f->insert(f->end(), abb);
    builder_.SetInsertPoint(abb);
    popScope();
}

void CodeGen::emitFunctionReturn(llvm::Value* ret_val) {
    // `return voidExpr;` inside a void function (e.g. `return f();` where f
    // returns void) — the expression's void value must NOT be handed to
    // CreateRet ("return instr that returns non-void in Function of void
    // return type"). Discard it so the void-return path emits `ret void`.
    if (ret_val && ret_val->getType()->isVoidTy())
        ret_val = nullptr;
    bool can_emit = builder_.GetInsertBlock() &&
                    !builder_.GetInsertBlock()->getTerminator();

    // ARC: for main, release function-local slots BEFORE myp_free_all() — that
    // call frees every remaining tracking-list object, so releasing a slot
    // after it would read already-freed memory (UAF). For non-main functions
    // the epilogue release runs AFTER retain-at-return (see below), so the
    // returned slot's rc stays balanced (retain +1 then release -1).
    if (can_emit && in_main_)
        arcReleaseAllScopes();

    // For main: cleanup threads + process pending events before returning
    if (in_main_) {
        // Stop and destroy all @thread handles
        for (auto& hname : thread_handle_names_) {
            auto* ha = getNamedValue(hname);
            if (ha && runtime_thread_stop_) {
                auto* thr = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ha, hname);
                builder_.CreateCall(runtime_thread_stop_, {thr});
            }
        }
        for (auto& hname : thread_handle_names_) {
            auto* ha = getNamedValue(hname);
            if (ha && runtime_thread_destroy_) {
                auto* thr = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ha, hname);
                builder_.CreateCall(runtime_thread_destroy_, {thr});
            }
        }
        // Process pending events on main thread
        if (runtime_event_process_all_) {
            builder_.CreateCall(runtime_event_process_all_, {});
        }
        // Free all allocations made by main thread
        if (runtime_free_all_) {
            builder_.CreateCall(runtime_free_all_, {});
        }
    }
    if (can_emit) {
        emitRegionExit();   // @region: release temporaries before returning
        // @coro method: store return value into the coroutine's result slot
        if (current_is_coro_ && ret_val && !ret_val->getType()->isVoidTy()) {
            auto set_result = module_->getOrInsertFunction("__myp_coro_set_result",
                llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                                        {llvm::Type::getInt64Ty(ctx_)}, false));
            builder_.CreateCall(set_result, {castToI64(ret_val)});
        }
        // Cast the return value to the function's declared return type
        // (e.g. `return 0;` in a `long` function needs an int→long extend).
        if (ret_val) {
            llvm::Type* rt = current_function_->getReturnType();
            if (ret_val->getType() != rt) {
                if (rt->isIntegerTy() && ret_val->getType()->isIntegerTy())
                    ret_val = builder_.CreateIntCast(ret_val, rt, true);
                else if (rt->isFloatingPointTy() && ret_val->getType()->isIntegerTy())
                    ret_val = builder_.CreateSIToFP(ret_val, rt);
                else if (rt->isIntegerTy() && ret_val->getType()->isFloatingPointTy())
                    ret_val = builder_.CreateFPToSI(ret_val, rt);
                else if (rt->isPointerTy() && ret_val->getType()->isPointerTy())
                    ret_val = builder_.CreateBitCast(ret_val, rt);
                else if (rt->isStructTy() && ret_val->getType()->isStructTy()) {
                    // Tuple return: rebuild with declared element types
                    // (handles promotions like return (5, "x") in (int, string)).
                    auto* dst_st = llvm::cast<llvm::StructType>(rt);
                    llvm::Value* agg = llvm::PoisonValue::get(dst_st);
                    for (unsigned i = 0; i < dst_st->getNumElements(); i++) {
                        llvm::Value* el = builder_.CreateExtractValue(ret_val, i);
                        auto* el_ty = dst_st->getElementType(i);
                        if (el->getType() != el_ty) {
                            if (el_ty->isIntegerTy() && el->getType()->isIntegerTy())
                                el = builder_.CreateIntCast(el, el_ty, true);
                            else if (el_ty->isPointerTy() && el->getType()->isPointerTy())
                                el = builder_.CreateBitCast(el, el_ty);
                        }
                        agg = builder_.CreateInsertValue(agg, el, i);
                    }
                    ret_val = agg;
                }
            }
            // ARC (§五-1): retain-at-return — a returned class/interface
            // reference is +1'd so the caller transfers it into a strong slot
            // without retaining. `return new T()` is a fresh temporary whose
            // rc=1 already transfers to the caller — skip the extra retain
            // (arc_skip_retain_return_ set by generateReturnStmt). A returned
            // class-array (T[] of class) is likewise +1'd (myp_retain reaches
            // the array header).
            bool skip_retain = arc_skip_retain_return_;
            arc_skip_retain_return_ = false;
            bool ret_is_arc_ref =
                current_ret_ti_.kind == TypeKind::Class ||
                current_ret_ti_.kind == TypeKind::Interface ||
                current_ret_ti_.kind == TypeKind::Slice ||
                current_ret_ti_.kind == TypeKind::String ||   // M8: counted string
                current_ret_ti_.kind == TypeKind::Function || // M-FN-2: closure fat ptr
                (current_ret_ti_.kind == TypeKind::Array &&
                 ((current_ret_ti_.element_type &&
                   current_ret_ti_.element_type->kind == TypeKind::Class) ||
                  current_ret_ti_.array_size == 0));
            if (!skip_retain && ret_is_arc_ref) {
                llvm::Value* rdata = ret_val;
                if ((current_ret_ti_.kind == TypeKind::Interface ||
                     current_ret_ti_.kind == TypeKind::Slice ||
                     current_ret_ti_.kind == TypeKind::Function) &&
                    ret_val->getType()->isStructTy())
                    rdata = builder_.CreateExtractValue(ret_val, 0);
                if (runtime_retain_)
                    builder_.CreateCall(runtime_retain_, {rdata});
            }
            // M8 structs: a returned struct VALUE with ARC-reference fields —
            // retain each field so the caller's copy owns the refs. The callee's
            // struct local is released by the epilogue below (balanced). A
            // `return <structCall>()` skips this via arc_skip_retain_return_
            // (the call already returned a +1 value).
            if (!skip_retain && current_ret_ti_.kind == TypeKind::Struct &&
                !current_ret_ti_.class_name.empty() &&
                ret_val && ret_val->getType()->isStructTy()) {
                const StructDecl* sd = findStruct(current_ret_ti_.class_name);
                if (sd) emitStructFieldsValue(builder_, ret_val, *sd, true);
            }
            // ARC epilogue (non-main): release local slots AFTER retain-at-return
            // so the returned slot's rc is balanced (+1 then -1); for main the
            // release already ran before myp_free_all().
            if (!in_main_)
                arcReleaseAllScopes();
            builder_.CreateRet(ret_val);
        } else {
            // Void return: release local slots (epilogue).
            if (!in_main_)
                arcReleaseAllScopes();
            llvm::Type* rt = current_function_->getReturnType();
            if (rt->isVoidTy())
                builder_.CreateRetVoid();
            else
                // Fall-off-the-end in a non-void function (e.g. empty-body FFI
                // stubs like `int helper() {}`, or an uncaught path): return a
                // zero default instead of emitting `ret void` (LLVM verify
                // "return type does not match").
                builder_.CreateRet(llvm::Constant::getNullValue(rt));
        }
    }
}

void CodeGen::generateReturnStmt(const ReturnStmt& s) {
    // Inside a try-with-finally (but not in its finally body): run the finally
    // first, then return — Java/C#/Python semantics. The return value is stored
    // in a function-level slot so nested finally blocks can forward it.
    if (!finally_ctx_stack_.empty() && !finally_ctx_stack_.back().in_finally) {
        llvm::Value* v = nullptr;
        if (s.value) {
            v = generateExpr(*s.value);
            v = heapCopyArrayReturn(v, s.value.get());
            llvm::Type* rt = current_function_->getReturnType();
            if (v->getType() != rt) {
                if (rt->isIntegerTy() && v->getType()->isIntegerTy())
                    v = builder_.CreateIntCast(v, rt, true);
                else if (rt->isFloatingPointTy() && v->getType()->isIntegerTy())
                    v = builder_.CreateSIToFP(v, rt);
                else if (rt->isIntegerTy() && v->getType()->isFloatingPointTy())
                    v = builder_.CreateFPToSI(v, rt);
                else if (rt->isPointerTy() && v->getType()->isPointerTy())
                    v = builder_.CreateBitCast(v, rt);
            }
            // ARC: `return new T()` / `return new T[n]` / `return f()` (f returns
            // a class ref) in a try-with-finally — consume the fresh temp now;
            // emitFunctionReturn skips the extra retain later.
            if (s.value->kind == ExprKind::NewExpr ||
                s.value->kind == ExprKind::Lambda ||
                s.value->kind == ExprKind::NewArrayExpr ||
                (s.value->kind == ExprKind::Call &&
                 (callReturnsArcRef(static_cast<const CallExpr&>(*s.value)) ||
                  callReturnsArcStruct(static_cast<const CallExpr&>(*s.value)) ||
                  callReturnsArcSliceOrArray(static_cast<const CallExpr&>(*s.value)))) ||
                (s.value->kind == ExprKind::MemberAccess &&
                 isWeakMemberAccess(static_cast<const MemberAccessExpr&>(*s.value)))) {
                arcConsumeTemp(v);
                arc_skip_retain_return_ = true;
            }
            if (!finally_ret_slot_)
                finally_ret_slot_ = createEntryBlockAlloca(current_function_, rt, "finally_ret");
            builder_.CreateStore(v, finally_ret_slot_);
        }
        auto& fc = finally_ctx_stack_.back();
        builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 2), fc.flag_slot);
        builder_.CreateBr(fc.finally_bb);
        return;
    }
    llvm::Value* v = nullptr;
    if (s.value) v = generateExpr(*s.value);
    v = heapCopyArrayReturn(v, s.value.get());
    if (s.value && (s.value->kind == ExprKind::NewExpr ||
                    s.value->kind == ExprKind::Lambda ||
                    s.value->kind == ExprKind::NewArrayExpr ||
                    (s.value->kind == ExprKind::Call &&
                     (callReturnsArcRef(static_cast<const CallExpr&>(*s.value)) ||
                      callReturnsArcStruct(static_cast<const CallExpr&>(*s.value)) ||
                      callReturnsArcSliceOrArray(static_cast<const CallExpr&>(*s.value)))) ||
                    (s.value->kind == ExprKind::MemberAccess &&
                     isWeakMemberAccess(static_cast<const MemberAccessExpr&>(*s.value))))) {
        arcConsumeTemp(v);
        arc_skip_retain_return_ = true;   // fresh `new` rc transfers to caller
    }
    emitFunctionReturn(v);
}

llvm::Value* CodeGen::heapCopyArrayReturn(llvm::Value* v, const Expr* value_expr) {
    if (!v || !value_expr || value_expr->kind != ExprKind::Identifier) return v;
    auto& id = static_cast<const IdentifierExpr&>(*value_expr);
    auto it = stack_array_sizes_.find(id.name);
    if (it == stack_array_sizes_.end()) return v;
    uint64_t byte_size = it->second;
    auto eit = array_elem_types_.find(id.name);
    bool elem_is_class = false;
    auto cit = array_elem_is_class_.find(id.name);
    if (cit != array_elem_is_class_.end()) elem_is_class = cit->second;
    if (elem_is_class && eit != array_elem_types_.end()) {
        // Fixed class array return: deep-copy into a ref-counted array so the
        // returned value owns its elements (each retained from the stack copy).
        // The scope-exit release of the stack copy balances those retains.
        uint64_t elem_size = module_->getDataLayout().getTypeAllocSize(eit->second);
        uint64_t count = elem_size ? byte_size / elem_size : 0;
        auto* ac = module_->getFunction("myp_alloc_class_array");
        if (!ac) {
            auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0),
                {llvm::Type::getInt64Ty(ctx_), llvm::Type::getInt32Ty(ctx_)}, false);
            ac = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                "myp_alloc_class_array", module_.get());
        }
        auto* dest = builder_.CreateCall(ac,
            {llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), count),
             llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), (uint64_t)elem_size)},
            "ret_arr_copy");
        builder_.CreateMemSet(dest, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0),
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), byte_size), llvm::Align(8));
        auto* src_i8 = builder_.CreateBitCast(v, llvm::PointerType::get(ctx_, 0));
        auto* dst_i8 = builder_.CreateBitCast(dest, llvm::PointerType::get(ctx_, 0));
        for (uint64_t i = 0; i < count; i++) {
            auto* off = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), i * elem_size);
            auto* srcp = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), src_i8, off);
            auto* elp = builder_.CreateBitCast(srcp, eit->second->getPointerTo());
            auto* el = builder_.CreateLoad(eit->second, elp);
            auto* dstp = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), dst_i8, off);
            auto* dpe = builder_.CreateBitCast(dstp, eit->second->getPointerTo());
            builder_.CreateStore(el, dpe);
            if (runtime_retain_ && el)   // myp_retain ignores NULL
                builder_.CreateCall(runtime_retain_, {el});
        }
        // The fresh rc=1 array transfers to the caller — skip the extra retain
        // in emitFunctionReturn.
        arc_skip_retain_return_ = true;
        return dest;
    }
    // Non-class fixed-array return: allocate a COUNTED backing (not arena) so
    // the returned pointer can be released by any counted array slot the caller
    // creates — an arena copy here would crash myp_release (reads a garbage
    // header). rc=1 transfers to the caller (skip the emitFunctionReturn retain).
    uint64_t elem_size = (eit != array_elem_types_.end())
        ? module_->getDataLayout().getTypeAllocSize(eit->second)
        : 1;
    uint64_t count = elem_size ? byte_size / elem_size : 0;
    llvm::Function* alloc_fn = module_->getFunction("myp_alloc_slice_backing");
    if (!alloc_fn) {
        auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0),
            {llvm::Type::getInt64Ty(ctx_), llvm::Type::getInt32Ty(ctx_),
             llvm::Type::getInt32Ty(ctx_)}, false);
        alloc_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
            "myp_alloc_slice_backing", module_.get());
    }
    auto* dest = builder_.CreateCall(alloc_fn,
        {llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), count),
         llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), (uint64_t)elem_size),
         llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 1)},  // SCALAR
        "ret_arr_copy");
    auto* dst = builder_.CreateBitCast(dest, llvm::PointerType::get(ctx_, 0));
    auto* src = builder_.CreateBitCast(v, llvm::PointerType::get(ctx_, 0));
    builder_.CreateMemCpy(dst, llvm::Align(8), src, llvm::Align(8), byte_size);
    arc_skip_retain_return_ = true;
    return dest;
}

void CodeGen::emitRegionEnter() {
    if (!current_function_) return;
    auto* mark_fn = module_->getFunction("myp_arena_mark");
    if (!mark_fn) {
        auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0), {}, false);
        mark_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_arena_mark", module_.get());
    }
    current_region_mark_ = createEntryBlockAlloca(current_function_,
        llvm::PointerType::get(ctx_, 0), "region_mark");
    auto* m = builder_.CreateCall(mark_fn, {});
    builder_.CreateStore(m, current_region_mark_);
}

void CodeGen::emitRegionExit() {
    if (!current_region_mark_) return;
    auto* rel_fn = module_->getFunction("myp_arena_release");
    if (!rel_fn) {
        auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
            {llvm::PointerType::get(ctx_, 0)}, false);
        rel_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_arena_release", module_.get());
    }
    auto* m = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), current_region_mark_);
    builder_.CreateCall(rel_fn, {m});
}

void CodeGen::generateBreakStmt(const BreakStmt&) {
    if (loop_context_.empty()) return;
    // Inside a try-with-finally (not in its finally body): run finally first.
    if (!finally_ctx_stack_.empty() && !finally_ctx_stack_.back().in_finally) {
        auto& fc = finally_ctx_stack_.back();
        builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 3), fc.flag_slot);
        builder_.CreateBr(fc.finally_bb);
        return;
    }
    builder_.CreateBr(loop_context_.back().break_bb);
}

void CodeGen::generateContinueStmt(const ContinueStmt&) {
    if (loop_context_.empty()) return;
    // Inside a try-with-finally (not in its finally body): run finally first.
    if (!finally_ctx_stack_.empty() && !finally_ctx_stack_.back().in_finally) {
        auto& fc = finally_ctx_stack_.back();
        builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 4), fc.flag_slot);
        builder_.CreateBr(fc.finally_bb);
        return;
    }
    builder_.CreateBr(loop_context_.back().continue_bb);
}

void CodeGen::generateAwaitStmt(const AwaitStmt& s) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);

    // §五-5 形态3: await <@async 调用> — 直接调用（@async 函数内部经 park 原语
    // 挂起/恢复当前协程），不做 yield 值握手；语句形式丢弃返回值。
    if (s.expr && s.expr->kind == ExprKind::Call) {
        auto& call = static_cast<const CallExpr&>(*s.expr);
        if (call.callee && isAsyncCallTarget(call.callee.get())) {
            generateExpr(*s.expr);
            return;
        }
    }

    // await ClassName.eventName; — block until the event is fired (C4).
    if (s.expr && s.expr->kind == ExprKind::MemberAccess) {
        auto& ma = static_cast<const MemberAccessExpr&>(*s.expr);
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
                            goto stmt_ev_found;
                        }
                    }
                }
            }
        stmt_ev_found:
            if (is_event) {
                if (s.timeout) {
                    // await Signal.go timeout N; → wait_event_timeout(id, N, 0)
                    auto wt = module_->getOrInsertFunction("__myp_coro_wait_event_timeout",
                        llvm::FunctionType::get(i64, {i64, i64, i64}, false));
                    auto* tms = castToI64(generateExpr(*s.timeout));
                    builder_.CreateCall(wt, {llvm::ConstantInt::get(i64, event_id),
                                             tms, llvm::ConstantInt::get(i64, 0)});
                } else {
                    auto wait_fn = module_->getOrInsertFunction("__myp_coro_wait_event",
                        llvm::FunctionType::get(i64, {i64, i64}, false));
                    builder_.CreateCall(wait_fn,
                        {llvm::ConstantInt::get(i64, event_id),
                         llvm::ConstantInt::get(i64, 0)});
                }
                return;
            }
        }
    }

    // await expr; / await; — suspend, passing `expr` value out to the scheduler
    // (statement form; the value passed in by resume is discarded).
    llvm::Value* out = llvm::ConstantInt::get(i64, 0);
    if (s.expr) {
        out = generateExpr(*s.expr);
        out = castToI64(out);
    }
    auto* yield_fn = module_->getFunction("__myp_coro_yield");
    if (!yield_fn) {
        auto* ft = llvm::FunctionType::get(i64, {i64}, false);
        yield_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                          "__myp_coro_yield", module_.get());
    }
    builder_.CreateCall(yield_fn, {out});
}

llvm::Value* CodeGen::generateAssignment(const AssignmentExpr& e) {
    // var = value
    if (e.target->kind == ExprKind::Identifier) {
        auto& id = static_cast<const IdentifierExpr&>(*e.target);
        auto* a = getNamedValue(id.name);
        if (!a) {
            // Try bare property assignment on 'this' (class field without this. prefix)
            if (!current_class_name_.empty() && current_tu_) {
                for (auto& cls : current_tu_->classes) {
                    if (cls.name != current_class_name_) continue;
                    unsigned pi;
                    if (getPropertyIndex(cls.name, id.name, pi)) {
                        auto* ta = getNamedValue("this");
                        if (ta) {
                            auto* tp = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ta);
                            auto* st = getClassStruct(cls.name);
                            if (st) {
                                auto* gep = builder_.CreateStructGEP(st, tp, pi);
                                auto* pt = getPropertyType(cls, id.name);
                                auto* v = generateExpr(*e.value);
                                if (v->getType() != pt) {
                                    if (pt->isIntegerTy() && v->getType()->isIntegerTy())
                                        v = builder_.CreateIntCast(v, pt, true);
                                    else if (pt->isFloatingPointTy() && v->getType()->isIntegerTy())
                                        v = builder_.CreateSIToFP(v, pt);
                                }
                                // M7: weak field → myp_weak_store (no retain/release).
                                if (storePropertyField(gep, v, cls, id.name)) {
                                    builder_.CreateStore(v, gep);
                                    return v;
                                }
                                // ARC: this.prop is a strong slot owned by the object.
                                const TypeNode* prop_tn = nullptr;
                                for (auto& p : cls.properties)
                                    if (p.name == id.name) { prop_tn = &p.type; break; }
                                if (prop_tn && (prop_tn->class_name == "slice" || isCountedArrayType(*prop_tn) || isStringType(*prop_tn))) {
                                    // M8: slice/array/string field — retain new
                                    // (unless fresh), release the old one.
                                    if (prop_tn->class_name == "slice")
                                        arcStoreSlice(gep, v, isFreshArcExpr(*e.value));
                                    else {
                                        arcStoreRef(gep, v, false, isFreshArcExpr(*e.value));
                                        arcConsumeTemp(v);
                                    }
                                } else if (prop_tn && isArcRefType(*prop_tn)) {
                                    bool iface_prop = false;
                                    if (current_tu_)
                                        for (auto& ifd : current_tu_->interfaces)
                                            if (ifd.name == prop_tn->class_name) { iface_prop = true; break; }
                                    arcStoreRef(gep, v, iface_prop, isFreshArcExpr(*e.value));
                                    arcConsumeTemp(v);
                                } else if (prop_tn && !prop_tn->class_name.empty() &&
                                           findStruct(prop_tn->class_name) && isArcFieldType(*prop_tn)) {
                                    // M8 structs: struct property with ARC refs — retain new
                                    // fields (unless fresh), release the old ones.
                                    const StructDecl* psd = findStruct(prop_tn->class_name);
                                    if (!isFreshArcExpr(*e.value))
                                        emitStructFieldsValue(builder_, v, *psd, true);
                                    emitStructFieldsPtr(builder_, gep, *psd, false);
                                    arcConsumeTemp(v);
                                }
                                builder_.CreateStore(v, gep);
                                return v;
                            }
                        }
                    }
                }
            }
            diag_.error(e.range, "undefined variable '" + id.name + "'");
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
        }
        // M4: in-place string append — `s = s + x` reuses s's counted buffer
        // when it is unique (rc==1) via myp_str_append, turning O(n²) string
        // accumulation into O(1)-amortized appends (no full copy per step).
        // Only when the target is a string local and the RHS is `s + x` with
        // the SAME variable on the left (the common accumulation pattern).
        if (e.value && e.value->kind == ExprKind::BinaryOp &&
            exprIsString(*e.target)) {
            auto& bin = static_cast<const BinaryOpExpr&>(*e.value);
            if (bin.op == BinaryOpKind::Add &&
                bin.lhs->kind == ExprKind::Identifier &&
                static_cast<const IdentifierExpr&>(*bin.lhs).name == id.name &&
                exprIsString(*bin.lhs)) {
                if (!runtime_str_append_) {
                    auto* pt = llvm::PointerType::get(ctx_, 0);
                    auto* ft = llvm::FunctionType::get(pt, {pt, pt}, false);
                    runtime_str_append_ = llvm::Function::Create(ft,
                        llvm::Function::ExternalLinkage, "myp_str_append",
                        module_.get());
                }
                auto* cur = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), a);
                llvm::Value* rhs = generateExpr(*bin.rhs);
                bool rhs_was_ptr = rhs->getType()->isPointerTy();
                rhs = stringifyForConcat(rhs);
                auto* app = builder_.CreateCall(
                    runtime_str_append_->getFunctionType(),
                    runtime_str_append_, {cur, rhs});
                // Freshly converted scalar → string temp is not consumed by
                // myp_str_append; release it (same as the concat path).
                if (!rhs_was_ptr && runtime_release_)
                    builder_.CreateCall(runtime_release_, {rhs});
                builder_.CreateStore(app, a);
                // §五-1 收尾: mirror the slot's live object into the coroutine
                // frame (append consumed the old value).
                if (current_is_coro_ && (isArcClassLocal(a) || isArcFunctionLocal(a)))
                    emitCoroFrameSet(a, app);
                return app;
            }
        }
        auto* v = generateExpr(*e.value);
        llvm::Type* at_v = nullptr;
        if (auto* ai_v = llvm::dyn_cast<llvm::AllocaInst>(a)) {
            at_v = ai_v->getAllocatedType();
        } else if (a->getType()->isPointerTy()) {
            at_v = getNamedValueType(id.name);
        }
        if (at_v && v->getType() != at_v) {
            if (at_v->isIntegerTy() && v->getType()->isIntegerTy()) v = builder_.CreateIntCast(v, at_v, true);
            else if (at_v->isFloatingPointTy() && v->getType()->isIntegerTy()) v = builder_.CreateSIToFP(v, at_v);
            else if (at_v->isIntegerTy() && v->getType()->isFloatingPointTy()) v = builder_.CreateFPToSI(v, at_v);
        }
        // ARC: class-local assignment — retain the new owner (unless fresh),
        // release the old value. Self/alias assignment is safe (retain-then-release).
        if (isArcClassLocal(a)) {
            arcStoreRef(a, v, false, isFreshArcExpr(*e.value));
            arcConsumeTemp(v);   // fresh `new` RHS is now owned by the local
        } else if (isArcFunctionLocal(a)) {
            // Function-value slot: release the old closure, retain the new one.
            arcStoreRef(a, v, true, isFreshArcExpr(*e.value));
            arcConsumeTemp(v);   // fresh lambda RHS closure now owned by the local
        } else if (isArcSliceLocal(a)) {
            // M8: slice slot — retain new backing (unless fresh), release old.
            arcStoreSlice(a, v, isFreshArcExpr(*e.value));
        } else if (isOwnedStructLocal(a)) {
            // M8 structs: struct assignment — release the old fields, retain the
            // new value's fields (unless fresh RHS owns them already), store.
            auto sit = arc_struct_slot_types_.find(a);
            if (sit != arc_struct_slot_types_.end()) {
                const StructDecl* sd = findStruct(sit->second);
                if (sd) {
                    emitStructFieldsPtr(builder_, a, *sd, false);
                    if (!isFreshArcExpr(*e.value))
                        emitStructFieldsValue(builder_, v, *sd, true);
                    arcConsumeTemp(v);
                }
            }
        }
        builder_.CreateStore(v, a);
        // §五-1 收尾: mirror the slot's live object into the coroutine frame.
        if (current_is_coro_ && (isArcClassLocal(a) || isArcFunctionLocal(a)))
            emitCoroFrameSet(a, v);
        return v;
    }
    // arr[i] = value
    if (e.target->kind == ExprKind::Subscript) {
        auto& ss = static_cast<const SubscriptExpr&>(*e.target);
        // slice<T>[i] = value (incl. nested slice<slice<T>> rows[i][j] = v) —
        // unpack the slice value, bounds-check, GEP, store.
        if (const TypeInfo* sti = sliceTypeOfExpr(ss.array.get())) {
            if (sti->kind == TypeKind::Slice && sti->element_type) {
                auto* idx = generateExpr(*ss.index);
                auto* p = generateSliceElementAddress(ss.array.get(), idx);
                if (p) {
                    auto* elem_ty = getLLVMType(*sti->element_type);
                    auto* v = generateExpr(*e.value);
                    if (v->getType() != elem_ty) {
                        if (elem_ty->isFloatingPointTy() && v->getType()->isIntegerTy())
                            v = builder_.CreateSIToFP(v, elem_ty);
                        else if (elem_ty->isIntegerTy() && v->getType()->isFloatingPointTy())
                            v = builder_.CreateFPToSI(v, elem_ty);
                        else if (elem_ty->isIntegerTy() && v->getType()->isIntegerTy())
                            v = builder_.CreateIntCast(v, elem_ty, true);
                    }
                    // ARC: slice<T> of classes — element is a strong slot.
                    if (sti->element_type->kind == TypeKind::Class) {
                        auto* old = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), p);
                        if (!isFreshArcExpr(*e.value))
                            emitRetain(v);
                        if (runtime_release_)
                            builder_.CreateCall(runtime_release_, {old});
                        arcConsumeTemp(v);
                    } else if (sti->element_type->kind == TypeKind::Slice) {
                        // M8: slice<slice<T>> — element is a {data,len} fat
                        // pointer holding a counted backing: retain the new
                        // (unless fresh), release the old.
                        auto* slt = getLLVMType(*sti->element_type);
                        auto* old = builder_.CreateLoad(slt, p);
                        auto* old_data = builder_.CreateExtractValue(old, 0);
                        if (!isFreshArcExpr(*e.value)) {
                            auto* new_data = builder_.CreateExtractValue(v, 0);
                            if (runtime_retain_)
                                builder_.CreateCall(runtime_retain_, {new_data});
                        }
                        if (runtime_release_)
                            builder_.CreateCall(runtime_release_, {old_data});
                    }
                    builder_.CreateStore(v, p);
                    return v;
                }
            }
        }
        auto* a = generateExpr(*ss.array);
        auto* i = generateExpr(*ss.index);
        llvm::Type* elem_ty = llvm::Type::getInt32Ty(ctx_);
        bool elem_is_class = false;   // ARC: element is a class reference slot

        // Determine element type from the array expression (see generateSubscript)
        if (ss.array->kind == ExprKind::Identifier) {
            auto& id = static_cast<const IdentifierExpr&>(*ss.array);
            if (!current_class_name_.empty() && current_tu_) {
                for (auto& cls : current_tu_->classes) {
                    if (cls.name != current_class_name_) continue;
                    for (auto& p : cls.properties) {
                        if (p.name == id.name && p.type.isArray()) {
                            elem_ty = typeNodeToLLVMType(*p.type.element_type);
                            elem_is_class = p.type.element_type &&
                                (getClassStruct(p.type.element_type->class_name) != nullptr ||
                                 isStringType(*p.type.element_type));
                            goto assign_gep;
                        }
                    }
                }
            }
            // Check local array variables
            auto eit = array_elem_types_.find(id.name);
            if (eit != array_elem_types_.end()) {
                elem_ty = eit->second;
                auto cit = array_elem_is_class_.find(id.name);
                if (cit != array_elem_is_class_.end()) elem_is_class = cit->second;
                goto assign_gep;
            }
        } else if (ss.array->kind == ExprKind::MemberAccess) {
            auto& ma = static_cast<const MemberAccessExpr&>(*ss.array);
            if (ma.object->kind == ExprKind::Identifier) {
                auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
                // Struct variable's array field: `bg.nodes[i] = ...` — resolve
                // from the struct declaration (var_class_map_ covers classes only).
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
                                elem_is_class = isArcClassType(*pr.type.element_type) ||
                                                isStringType(*pr.type.element_type);
                                goto assign_gep;
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
                                elem_is_class = isArcClassType(*p.type.element_type) ||
                                                isStringType(*p.type.element_type);
                                goto assign_gep;
                            }
                        }
                    }
                }
            }
        }

assign_gep:
        auto* p = builder_.CreateGEP(elem_ty, a, zextIndexValue(builder_, i));
        auto* v = generateExpr(*e.value);
        // Interface array element store: a concrete class instance (raw ptr)
        // stored into an interface fat-pointer slot {data, vtable} → build the
        // fat pointer for (class.interface_class_name, class). Bug fix: the raw
        // pointer was stored before, leaving the vtable half of the slot as
        // garbage → segfault on the later vtable dispatch (children[i].draw()).
        if (isInterfaceFatType(elem_ty) && v->getType()->isPointerTy() && current_tu_) {
            llvm::Value* raw_v = v;
            std::string cls_name;
            if (e.value->kind == ExprKind::NewExpr)
                cls_name = static_cast<const NewExpr&>(*e.value).class_name;
            else if (e.value->kind == ExprKind::Identifier) {
                auto vit = var_class_map_.find(
                    static_cast<const IdentifierExpr&>(*e.value).name);
                if (vit != var_class_map_.end()) cls_name = vit->second;
            }
            std::string iface_name;
            for (auto& cls : current_tu_->classes)
                if (cls.name == cls_name) { iface_name = cls.interface_class_name; break; }
            if (!iface_name.empty()) {
                auto* vgv = getOrCreateVtable(iface_name, cls_name);
                if (vgv) {
                    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
                    auto* fat_ty = llvm::StructType::get(ctx_, {ptr_ty, ptr_ty});
                    auto* fat = builder_.CreateAlloca(fat_ty);
                    builder_.CreateStore(v, builder_.CreateStructGEP(fat_ty, fat, 0));
                    builder_.CreateStore(builder_.CreateBitCast(vgv, ptr_ty),
                        builder_.CreateStructGEP(fat_ty, fat, 1));
                    v = builder_.CreateLoad(fat_ty, fat);
                    // 把 fresh `new` 的所有权转给数组槽（接口数组不释放元素，
                    // 由 arena 回收），否则语句末 temp flush 会释放它 → 悬垂。
                    arcConsumeTemp(raw_v);
                }
            }
        }
        if (v->getType() != elem_ty) {
            if (elem_ty->isIntegerTy() && v->getType()->isIntegerTy())
                v = builder_.CreateIntCast(v, elem_ty, true);
            else if (elem_ty->isFloatingPointTy() && v->getType()->isIntegerTy())
                v = builder_.CreateSIToFP(v, elem_ty);
            else if (elem_ty->isIntegerTy() && v->getType()->isFloatingPointTy())
                v = builder_.CreateFPToSI(v, elem_ty);
        }
        // ARC: T[] of classes — element is a strong slot (retain unless fresh,
        // release the overwritten element). Slice handled above.
        if (elem_is_class) {
            auto* old = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), p);
            if (!isFreshArcExpr(*e.value))
                emitRetain(v);
            if (runtime_release_)
                builder_.CreateCall(runtime_release_, {old});
            arcConsumeTemp(v);
        }
        builder_.CreateStore(v, p);
        return v;
    }
    // obj.prop = value  (only allowed via this.prop — properties are private)
    if (e.target->kind == ExprKind::MemberAccess) {
        auto& ma = static_cast<const MemberAccessExpr&>(*e.target);
        llvm::Value* op = nullptr;
        // Only 'this.prop = value' is allowed — external property assignment is rejected by Sema
        if (ma.object->kind == ExprKind::ThisExpr) {
            auto* ta = getNamedValue("this");
            if (ta) op = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ta);
        }
        if (!op) {
            // Struct field assignment: v.field = value
            if (ma.object->kind == ExprKind::Identifier) {
                auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
                auto* oa = getNamedValue(oi.name);
                if (oa) {
                    llvm::StructType* st3_as = nullptr;
                    if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(oa)) {
                        auto* at = ai->getAllocatedType();
                        if (at->isStructTy()) st3_as = llvm::cast<llvm::StructType>(at);
                    } else {
                        auto* nty = getNamedValueType(oi.name);
                        if (nty && nty->isStructTy()) st3_as = llvm::cast<llvm::StructType>(nty);
                    }
                    if (st3_as) {
                        auto* st = st3_as;
                    std::string st_name = st->getName().str();
                    unsigned fi = 0;
                    if (getStructFieldIndex(st_name, ma.member_name, fi)) {
                        auto* gep = builder_.CreateStructGEP(st, oa, fi);
                        auto* v = generateExpr(*e.value);
                        auto* ft = st3_as->getElementType(fi);
                        if (v->getType() != ft) {
                            if (ft->isIntegerTy() && v->getType()->isIntegerTy())
                                v = builder_.CreateIntCast(v, ft, true);
                            else if (ft->isFloatingPointTy() && v->getType()->isIntegerTy())
                                v = builder_.CreateSIToFP(v, ft);
                            else if (ft->isIntegerTy() && v->getType()->isFloatingPointTy())
                                v = builder_.CreateFPToSI(v, ft);
                            else if (ft->isStructTy() && isInterfaceFatType(ft) &&
                                     v->getType()->isPointerTy() && current_tu_) {
                                // struct field is an interface fat pointer:
                                // upcast the concrete instance (ptr) → {data, vtable}.
                                std::string cls_name = resolveArgClassName(*e.value);
                                std::string iface_name;
                                const StructDecl* fsd = findStruct(st_name);
                                if (fsd && fi < fsd->properties.size())
                                    iface_name = fsd->properties[fi].type.class_name;
                                if (!cls_name.empty() && !iface_name.empty()) {
                                    auto* fp = buildInterfaceFat(v, iface_name, cls_name);
                                    if (fp) v = fp;
                                }
                            }
                        }
                        // M8 structs: an OWNED struct local's ARC field is a
                        // strong slot — retain the new value (unless fresh),
                        // release the overwritten field. Borrowed struct copies
                        // (params) keep plain stores (no release — the caller
                        // still owns the field ref).
                        if (isOwnedStructLocal(oa)) {
                            const StructDecl* fsd = findStruct(st_name);
                            if (fsd && fi < fsd->properties.size() &&
                                isArcFieldType(fsd->properties[fi].type)) {
                                auto* old = builder_.CreateLoad(ft, gep);
                                const TypeNode& ftn = fsd->properties[fi].type;
                                if (!isFreshArcExpr(*e.value))
                                    emitArcFieldOp(builder_, v, ftn, true);
                                emitArcFieldOp(builder_, old, ftn, false);
                                arcConsumeTemp(v);
                            }
                        }
                        builder_.CreateStore(v, gep);
                        return v;
                    }
                }
            }
            // Static class property assignment: ClassName.property = value
            if (ma.object->kind == ExprKind::Identifier) {
                auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
                auto sit = static_property_globals_.find(oi.name);
                if (sit != static_property_globals_.end() && current_tu_) {
                    for (auto& cls : current_tu_->classes) {
                        if (cls.name == oi.name) {
                            unsigned pi = 0;
                            if (getPropertyIndex(cls.name, ma.member_name, pi)) {
                                auto* st = getClassStruct(cls.name);
                                if (st) {
                                    auto* gep = builder_.CreateStructGEP(st, sit->second, pi);
                                    auto* v = generateExpr(*e.value);
                                    auto* pt = getPropertyType(cls, ma.member_name);
                                    if (v->getType() != pt) {
                                        if (pt->isIntegerTy() && v->getType()->isIntegerTy())
                                            v = builder_.CreateIntCast(v, pt, true);
                                        else if (pt->isFloatingPointTy() && v->getType()->isIntegerTy())
                                            v = builder_.CreateSIToFP(v, pt);
                                        else if (pt->isIntegerTy() && v->getType()->isFloatingPointTy())
                                            v = builder_.CreateFPToSI(v, pt);
                                    }
                                    // M7: weak static field → myp_weak_store.
                                    if (storePropertyField(gep, v, cls, ma.member_name)) {
                                        builder_.CreateStore(v, gep);
                                        return v;
                                    }
                                    // ARC: static property = process-global strong slot.
                                    const TypeNode* prop_tn = nullptr;
                                    for (auto& p : cls.properties)
                                        if (p.name == ma.member_name) { prop_tn = &p.type; break; }
                                    if (prop_tn && (prop_tn->class_name == "slice" || isCountedArrayType(*prop_tn) || isStringType(*prop_tn))) {
                                        // M8: static slice/array/string field — retain/release.
                                        if (prop_tn->class_name == "slice")
                                            arcStoreSlice(gep, v, isFreshArcExpr(*e.value));
                                        else {
                                            arcStoreRef(gep, v, false, isFreshArcExpr(*e.value));
                                            arcConsumeTemp(v);
                                        }
                                    } else if (prop_tn && isArcRefType(*prop_tn)) {
                                        bool iface_prop = false;
                                        if (current_tu_)
                                            for (auto& ifd : current_tu_->interfaces)
                                                if (ifd.name == prop_tn->class_name) { iface_prop = true; break; }
                                        arcStoreRef(gep, v, iface_prop, isFreshArcExpr(*e.value));
                                        arcConsumeTemp(v);
                                    } else if (prop_tn && !prop_tn->class_name.empty() &&
                                               findStruct(prop_tn->class_name) && isArcFieldType(*prop_tn)) {
                                        // M8 structs: static struct property — retain/release fields.
                                        const StructDecl* psd = findStruct(prop_tn->class_name);
                                        if (!isFreshArcExpr(*e.value))
                                            emitStructFieldsValue(builder_, v, *psd, true);
                                        emitStructFieldsPtr(builder_, gep, *psd, false);
                                        arcConsumeTemp(v);
                                    }
                                    builder_.CreateStore(v, gep);
                                    return v;
                                }
                            }
                            diag_.error(e.range, "static class '" + oi.name +
                                "' has no property '" + ma.member_name + "'");
                            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
                        }
                    }
                }
            }
            // External obj.prop — Sema should have already rejected this, but for safety:
            if (ma.object->kind == ExprKind::Identifier) {
                auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
                diag_.error(e.range, "cannot assign to property '" + ma.member_name +
                    "' of '" + oi.name + "' from outside the class");
            } else {
                diag_.error(e.range, "cannot determine assignment target");
            }
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
        }
        // Chained struct field assignment: a.b.c = value (obj is itself a
        // member access, e.g. v.field.subfield) — at the if(!op) level so it
        // fires for non-Identifier objects (Identifier targets were handled
        // above inside the class property path).
        if (ma.object->kind == ExprKind::MemberAccess) {
            auto* addr = generateStructMemberAddress(ma);
            if (addr) {
                llvm::Type* ft = nullptr;
                if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(addr))
                    ft = gep->getResultElementType();
                if (ft) {
                    auto* v = generateExpr(*e.value);
                    if (v->getType() != ft) {
                        if (ft->isIntegerTy() && v->getType()->isIntegerTy())
                            v = builder_.CreateIntCast(v, ft, true);
                        else if (ft->isFloatingPointTy() && v->getType()->isIntegerTy())
                            v = builder_.CreateSIToFP(v, ft);
                        else if (ft->isIntegerTy() && v->getType()->isFloatingPointTy())
                            v = builder_.CreateFPToSI(v, ft);
                        else if (ft->isStructTy() && isInterfaceFatType(ft) &&
                                 v->getType()->isPointerTy() && current_tu_) {
                            // Chained struct field store into an interface field:
                            // upcast the concrete instance (ptr) → {data, vtable}.
                            // (The single-level path above already handles
                            // `h.s = c`; this covers `w.h.s = c`.)
                            std::string cls_name = resolveArgClassName(*e.value);
                            std::string iface_name;
                            for (auto& sd : current_tu_->structs)
                                for (auto& p : sd.properties)
                                    if (p.name == ma.member_name) {
                                        bool is_iface = false;
                                        for (auto& ifd : current_tu_->interfaces)
                                            if (ifd.name == p.type.class_name) { is_iface = true; break; }
                                        if (is_iface) { iface_name = p.type.class_name; break; }
                                    }
                            if (!cls_name.empty() && !iface_name.empty()) {
                                auto* fp = buildInterfaceFat(v, iface_name, cls_name);
                                if (fp) v = fp;
                            }
                        }
                    }
                    builder_.CreateStore(v, addr);
                    return v;
                }
            }
        }
        // Struct array element field: v[i].field = value — GEP to element, GEP
        // the field, store (struct elements are value slots, no ARC).
        if (ma.object->kind == ExprKind::Subscript) {
            auto* elem_ptr = generateArrayElementAddress(
                static_cast<const SubscriptExpr&>(*ma.object));
            if (elem_ptr) {
                llvm::Type* rt = nullptr;
                if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(elem_ptr))
                    rt = gep->getResultElementType();
                if (rt && rt->isStructTy()) {
                    auto* st = llvm::cast<llvm::StructType>(rt);
                    unsigned fi = 0;
                    if (getStructFieldIndex(st->getName().str(), ma.member_name, fi)) {
                        auto* fgep = builder_.CreateStructGEP(st, elem_ptr, fi);
                        auto* ft = st->getElementType(fi);
                        auto* v = generateExpr(*e.value);
                        if (v->getType() != ft) {
                            if (ft->isIntegerTy() && v->getType()->isIntegerTy())
                                v = builder_.CreateIntCast(v, ft, true);
                            else if (ft->isFloatingPointTy() && v->getType()->isIntegerTy())
                                v = builder_.CreateSIToFP(v, ft);
                            else if (ft->isIntegerTy() && v->getType()->isFloatingPointTy())
                                v = builder_.CreateFPToSI(v, ft);
                        }
                        builder_.CreateStore(v, fgep);
                        return v;
                    }
                }
            }
        }
        // Struct method: this.field = value
        if (struct_types_.count(current_class_name_)) {
            auto* st = getStructType(current_class_name_);
            if (st) {
                unsigned fi = 0;
                if (getStructFieldIndex(current_class_name_, ma.member_name, fi)) {
                    auto* gep = builder_.CreateStructGEP(st, op, fi);
                    auto* v = generateExpr(*e.value);
                    auto* ft = st->getElementType(fi);
                    if (v->getType() != ft) {
                        if (ft->isIntegerTy() && v->getType()->isIntegerTy())
                            v = builder_.CreateIntCast(v, ft, true);
                        else if (ft->isFloatingPointTy() && v->getType()->isIntegerTy())
                            v = builder_.CreateSIToFP(v, ft);
                        else if (ft->isIntegerTy() && v->getType()->isFloatingPointTy())
                            v = builder_.CreateFPToSI(v, ft);
                    }
                    builder_.CreateStore(v, gep);
                    return v;
                }
            }
            diag_.error(e.range, "struct '" + current_class_name_ +
                        "' has no field '" + ma.member_name + "'");
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
        }
        for (auto& cls : current_tu_->classes) {
            unsigned pi;
            if (getPropertyIndex(cls.name, ma.member_name, pi)) {
                auto* st = getClassStruct(cls.name);
                if (st) {
                    auto* gep = builder_.CreateStructGEP(st, op, pi);
                    auto* v = generateExpr(*e.value);
                    auto* pt = getPropertyType(cls, ma.member_name);
                    if (v->getType() != pt) {
                        if (pt->isIntegerTy() && v->getType()->isIntegerTy()) v = builder_.CreateIntCast(v, pt, true);
                        else if (pt->isFloatingPointTy() && v->getType()->isIntegerTy()) v = builder_.CreateSIToFP(v, pt);
                        else if (pt->isIntegerTy() && v->getType()->isFloatingPointTy()) v = builder_.CreateFPToSI(v, pt);
                    }
                    // M7: weak field → myp_weak_store (no retain/release).
                    if (storePropertyField(gep, v, cls, ma.member_name)) {
                        builder_.CreateStore(v, gep);
                        return v;
                    }
                    // ARC: obj.prop is a strong slot owned by the object.
                    const TypeNode* prop_tn = nullptr;
                    for (auto& p : cls.properties)
                        if (p.name == ma.member_name) { prop_tn = &p.type; break; }
                    if (prop_tn && (prop_tn->class_name == "slice" || isCountedArrayType(*prop_tn) || isStringType(*prop_tn))) {
                        // M8: instance slice/array/string field — retain/release.
                        if (prop_tn->class_name == "slice")
                            arcStoreSlice(gep, v, isFreshArcExpr(*e.value));
                        else {
                            arcStoreRef(gep, v, false, isFreshArcExpr(*e.value));
                            arcConsumeTemp(v);
                        }
                    } else if (prop_tn && isArcRefType(*prop_tn)) {
                        bool iface_prop = false;
                        if (current_tu_)
                            for (auto& ifd : current_tu_->interfaces)
                                if (ifd.name == prop_tn->class_name) { iface_prop = true; break; }
                        arcStoreRef(gep, v, iface_prop, isFreshArcExpr(*e.value));
                        arcConsumeTemp(v);
                    } else if (prop_tn && !prop_tn->class_name.empty() &&
                               findStruct(prop_tn->class_name) && isArcFieldType(*prop_tn)) {
                        // M8 structs: instance struct property — retain/release fields.
                        const StructDecl* psd = findStruct(prop_tn->class_name);
                        if (!isFreshArcExpr(*e.value))
                            emitStructFieldsValue(builder_, v, *psd, true);
                        emitStructFieldsPtr(builder_, gep, *psd, false);
                        arcConsumeTemp(v);
                    }
                    builder_.CreateStore(v, gep);
                    return v;
                }
            }
        }
        diag_.error(e.range, "unknown property '" + ma.member_name + "'");
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    }
    diag_.error(e.range, "not a valid assignment target");
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
}
}

void CodeGen::generateMatchStmt(const MatchStmt& s) {
    auto* subject_val = generateExpr(*s.subject);
    auto* func = builder_.GetInsertBlock()->getParent();

    // Resolve the enum name (for payload extraction) from the subject's type.
    std::string enum_name;
    if (s.subject->type && !s.subject->type->class_name.empty())
        enum_name = s.subject->type->class_name;
    else if (!s.arms.empty())
        enum_name = s.arms[0].enum_name;

    // Ensure the subject is the enum struct, then extract the i32 discriminant.
    llvm::Value* disc = nullptr;
    llvm::StructType* subject_st = nullptr;
    if (subject_val->getType()->isStructTy()) {
        subject_st = llvm::cast<llvm::StructType>(subject_val->getType());
        disc = builder_.CreateExtractValue(subject_val, 0, "match_disc");
    } else if (subject_val->getType()->isIntegerTy()) {
        disc = subject_val;
        if (!disc->getType()->isIntegerTy(32))
            disc = builder_.CreateIntCast(disc, llvm::Type::getInt32Ty(ctx_), false);
    } else {
        disc = builder_.CreatePtrToInt(subject_val, llvm::Type::getInt32Ty(ctx_));
    }

    size_t n = s.arms.size();
    auto* merge_bb = llvm::BasicBlock::Create(ctx_, "match_end", func);

    if (n == 0) {
        // No arms — just continue
        return;
    }

    // Create arm blocks
    std::vector<llvm::BasicBlock*> arm_blocks;
    for (size_t i = 0; i < n; i++) {
        arm_blocks.push_back(llvm::BasicBlock::Create(ctx_,
            "match_arm" + std::to_string(i), func));
    }

    // Create the first comparison block and branch to it from current position
    auto* start_bb = llvm::BasicBlock::Create(ctx_, "match_check_0", func);
    builder_.CreateBr(start_bb);

    for (size_t i = 0; i < n; i++) {
        auto& arm = s.arms[i];
        builder_.SetInsertPoint(start_bb);

        auto* variant_const = llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(ctx_), (uint64_t)arm.variant_index, false);
        auto* cmp = builder_.CreateICmpEQ(disc, variant_const, "match_cmp");

        llvm::BasicBlock* next_bb = nullptr;
        if (i + 1 < n) {
            next_bb = llvm::BasicBlock::Create(ctx_,
                "match_check" + std::to_string(i + 1), func);
        } else {
            next_bb = merge_bb;
        }

        builder_.CreateCondBr(cmp, arm_blocks[i], next_bb);

        // Generate arm body
        builder_.SetInsertPoint(arm_blocks[i]);

        // If arm has data bindings, extract the packed payload bytes from the
        // subject and bind each field into a local variable.
        pushScope();
        if (!arm.bindings.empty()) {
            const EnumDecl* ed = findEnum(enum_name);
            if (ed && arm.variant_index >= 0 &&
                (size_t)arm.variant_index < ed->variants.size() && subject_st) {
                auto& v = ed->variants[arm.variant_index];
                // Materialize the subject into an alloca so we can GEP into it.
                auto* subj_a = createEntryBlockAlloca(func, subject_st, "match_subject");
                builder_.CreateStore(subject_val, subj_a);
                auto* i8_ptr = llvm::PointerType::get(ctx_, 0);
                llvm::Value* base = nullptr;
                if (subject_st->getNumElements() > 1) {
                    auto* payload_ptr = builder_.CreateStructGEP(subject_st, subj_a, 1);
                    base = builder_.CreateBitCast(payload_ptr, i8_ptr);
                }
                for (size_t bi = 0; bi < arm.bindings.size() && bi < v.params.size(); bi++) {
                    auto* bind_ty = typeNodeToLLVMType(v.params[bi].type);
                    auto* a = createEntryBlockAlloca(func, bind_ty, arm.bindings[bi]);
                    if (base) {
                        uint64_t off = enumPayloadOffset(v, bi);
                        auto* field_ptr = builder_.CreateGEP(
                            llvm::Type::getInt8Ty(ctx_), base,
                            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), off));
                        auto* typed_ptr = builder_.CreateBitCast(field_ptr, bind_ty->getPointerTo());
                        auto* loaded = builder_.CreateLoad(bind_ty, typed_ptr);
                        builder_.CreateStore(loaded, a);
                    } else {
                        // No payload available — zero-initialize.
                        builder_.CreateStore(llvm::Constant::getNullValue(bind_ty), a);
                    }
                    setNamedValue(arm.bindings[bi], a);
                }
            } else {
                // Unknown enum/arm — fall back to zero bindings.
                for (auto& bname : arm.bindings) {
                    auto* a = createEntryBlockAlloca(func,
                        llvm::Type::getInt32Ty(ctx_), bname);
                    builder_.CreateStore(
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0), a);
                    setNamedValue(bname, a);
                }
            }
        }

        if (arm.body) generateStmt(*arm.body);

        // Branch to merge block if body didn't return/break/continue
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(merge_bb);
        popScope();

        start_bb = next_bb;
    }

    builder_.SetInsertPoint(merge_bb);
}

void CodeGen::generateTryStmt(const TryStmt& s) {
    auto* func = builder_.GetInsertBlock()->getParent();
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    auto* i32_ty = llvm::Type::getInt32Ty(ctx_);
    auto* i8_ty = llvm::Type::getInt8Ty(ctx_);

    // Per-try jmp_buf, allocated in the entry block so it stays live across the
    // longjmp back to the setjmp (fixes nested tries / cross-function throws).
    auto* jb = createEntryBlockAlloca(func, jmp_buf_type_, "try_jmpbuf");
    auto* jb_ptr = builder_.CreateBitCast(jb, ptr_ty);

    // Register this try's handler before setjmp (innermost-active handler stack).
    builder_.CreateCall(runtime_exception_push_->getFunctionType(),
        runtime_exception_push_, {jb_ptr});
    // Exception unwinding: collect ARC slots registered inside this try so the
    // dispatch/propagate paths can release them when the longjmp skips scope exit.
    try_ctx_stack_.emplace_back();

    auto* result = builder_.CreateCall(runtime_setjmp_->getFunctionType(),
        runtime_setjmp_, {jb_ptr}, "setjmp_result");
    auto* zero = llvm::ConstantInt::get(i32_ty, 0);
    auto* is_error = builder_.CreateICmpNE(result, zero, "is_error");

    auto* try_bb = llvm::BasicBlock::Create(ctx_, "try_block", func);
    llvm::BasicBlock* dispatch_bb = s.catches.empty()
        ? nullptr : llvm::BasicBlock::Create(ctx_, "catch_dispatch", func);
    auto* finally_bb = s.finally_block
        ? llvm::BasicBlock::Create(ctx_, "finally_block", func) : nullptr;
    auto* merge_bb = llvm::BasicBlock::Create(ctx_, "try_end", func);
    // 1 = an exception is propagating through finally (then rethrow after).
    llvm::Value* finally_flag = finally_bb
        ? createEntryBlockAlloca(func, i8_ty, "finally_flag") : nullptr;
    llvm::BasicBlock* propagate_bb = nullptr;  // exception path: set flag=1, br finally
    llvm::BasicBlock* rethrow_bb = nullptr;    // pop handler + longjmp to outer

    // Branch from the entry: error → dispatch / finally / merge; normal → try.
    if (dispatch_bb)
        builder_.CreateCondBr(is_error, dispatch_bb, try_bb);
    else if (finally_bb) {
        // No catch: an exception propagates — run finally, then rethrow.
        propagate_bb = llvm::BasicBlock::Create(ctx_, "try_propagate", func);
        builder_.CreateCondBr(is_error, propagate_bb, try_bb);
    } else
        builder_.CreateCondBr(is_error, merge_bb, try_bb);

    // === Set up finally control-flow context (only when a finally exists) ===
    // return/break/continue inside the try/catch will store an exit mode and
    // branch here, so the finally body runs before the real exit.
    if (finally_bb) {
        FinallyCtx fc;
        fc.flag_slot = finally_flag;
        fc.finally_bb = finally_bb;
        fc.merge_bb = merge_bb;
        if (!finally_ctx_stack_.empty()) {
            fc.outer_finally_bb = finally_ctx_stack_.back().finally_bb;
            fc.outer_flag_slot = finally_ctx_stack_.back().flag_slot;
        } else {
            fc.outer_finally_bb = nullptr;
            fc.outer_flag_slot = nullptr;
        }
        if (!loop_context_.empty()) {
            fc.break_bb = loop_context_.back().break_bb;
            fc.continue_bb = loop_context_.back().continue_bb;
        } else {
            fc.break_bb = nullptr;
            fc.continue_bb = nullptr;
        }
        finally_ctx_stack_.push_back(fc);
    }

    // === Generate try block ===
    builder_.SetInsertPoint(try_bb);
    if (s.try_block) generateBlock(*s.try_block);
    if (!builder_.GetInsertBlock()->getTerminator()) {
        if (finally_bb) {
            builder_.CreateStore(llvm::ConstantInt::get(i8_ty, 0), finally_flag);
            builder_.CreateBr(finally_bb);
        } else {
            // No finally: the try body completed normally — pop the handler so
            // a throw in a later catch body targets the outer handler, not this
            // try. (With a finally, the pop happens at finally-block entry.)
            builder_.CreateCall(runtime_exception_pop_->getFunctionType(),
                runtime_exception_pop_, {});
            builder_.CreateBr(merge_bb);
        }
    }

    // === Dispatch: match the exception type against each catch clause ===
    if (dispatch_bb) {
        builder_.SetInsertPoint(dispatch_bb);
        // The longjmp skipped the try block's scope-exit releases — free the
        // abandoned inner slots before running the catch bodies.
        emitReleaseTryInnerSlots();
        auto* etype = builder_.CreateCall(runtime_exception_get_type_->getFunctionType(),
            runtime_exception_get_type_, {}, "exc_type");
        std::vector<llvm::BasicBlock*> catch_bbs;
        for (size_t i = 0; i < s.catches.size(); ++i)
            catch_bbs.push_back(llvm::BasicBlock::Create(ctx_,
                "catch_" + std::to_string(i), func));
        // "no match" target: if there is a finally, run it then rethrow;
        // otherwise rethrow directly.
        if (finally_bb) {
            if (!propagate_bb)
                propagate_bb = llvm::BasicBlock::Create(ctx_, "try_propagate", func);
        } else {
            rethrow_bb = llvm::BasicBlock::Create(ctx_, "catch_rethrow", func);
        }
        llvm::BasicBlock* nomatch = finally_bb ? propagate_bb : rethrow_bb;

        llvm::BasicBlock* chk = dispatch_bb;
        for (size_t i = 0; i < s.catches.size(); ++i) {
            auto& cc = s.catches[i];
            llvm::BasicBlock* nxt = (i + 1 < s.catches.size())
                ? llvm::BasicBlock::Create(ctx_, "catch_chk_" + std::to_string(i + 1), func)
                : nomatch;
            builder_.SetInsertPoint(chk);
            if (cc.var_type.empty()) {
                builder_.CreateBr(catch_bbs[i]);
            } else if (cc.var_type == "string") {
                auto* m = builder_.CreateICmpEQ(etype, llvm::ConstantInt::get(i32_ty, 0));
                builder_.CreateCondBr(m, catch_bbs[i], nxt);
            } else if (isErrorInterface(cc.var_type)) {
                // Match any class implementing the Error interface: the
                // __myp_error_vtables[type_id] entry is non-null for such classes.
                auto* idx = builder_.CreateSExt(etype, llvm::Type::getInt64Ty(ctx_));
                auto* zero64 = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0);
                auto* gep = builder_.CreateGEP(error_vtables_gv_->getValueType(),
                    error_vtables_gv_, {zero64, idx}, "err_vt_gep");
                auto* vt = builder_.CreateLoad(ptr_ty, gep, "err_vt");
                auto* m = builder_.CreateICmpNE(vt, llvm::ConstantPointerNull::get(ptr_ty));
                builder_.CreateCondBr(m, catch_bbs[i], nxt);
            } else {
                auto it = class_type_ids_.find(cc.var_type);
                int tid = (it != class_type_ids_.end()) ? it->second : 0;
                auto* m = builder_.CreateICmpEQ(etype, llvm::ConstantInt::get(i32_ty, tid));
                builder_.CreateCondBr(m, catch_bbs[i], nxt);
            }
            chk = nxt;

            // Generate this catch body: bind the variable, run the block.
            builder_.SetInsertPoint(catch_bbs[i]);
            // A throw inside a catch body must propagate to the OUTER handler,
            // not re-trigger this try (previously: infinite loop). Pop this
            // try's handler before running the catch body — but only when there
            // is no finally (the finally-block entry pop already covers the
            // catch-end path when a finally exists; popping here too would
            // double-pop).
            if (!finally_bb)
                builder_.CreateCall(runtime_exception_pop_->getFunctionType(),
                    runtime_exception_pop_, {});
            bool iface_catch = isErrorInterface(cc.var_type);
            bool obj_catch = !cc.var_type.empty() && cc.var_type != "string" && !iface_catch;
            llvm::Value* caught_slot = nullptr;   // non-null → release at catch end
            if (iface_catch) {
                // e is an Error interface fat pointer { data=object, vtable }.
                auto* obj = builder_.CreateCall(runtime_exception_get_object_->getFunctionType(),
                    runtime_exception_get_object_, {}, "exc_obj");
                auto* etype2 = builder_.CreateCall(runtime_exception_get_type_->getFunctionType(),
                    runtime_exception_get_type_, {}, "exc_type2");
                auto* idx = builder_.CreateSExt(etype2, llvm::Type::getInt64Ty(ctx_));
                auto* zero64 = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0);
                auto* gep = builder_.CreateGEP(error_vtables_gv_->getValueType(),
                    error_vtables_gv_, {zero64, idx}, "err_vt_gep");
                auto* vt = builder_.CreateLoad(ptr_ty, gep, "err_vt");
                auto* fat_ty = llvm::StructType::get(ctx_, {ptr_ty, ptr_ty});
                auto* ev = createEntryBlockAlloca(func, fat_ty, cc.var_name);
                builder_.CreateStore(obj, builder_.CreateStructGEP(fat_ty, ev, 0));
                builder_.CreateStore(vt, builder_.CreateStructGEP(fat_ty, ev, 1));
                setNamedValue(cc.var_name, ev);
                caught_slot = ev;
            } else {
                llvm::Value* bound = nullptr;
                if (obj_catch) {
                    bound = builder_.CreateCall(runtime_exception_get_object_->getFunctionType(),
                        runtime_exception_get_object_, {}, "exc_obj");
                } else {
                    bound = builder_.CreateCall(runtime_get_error_->getFunctionType(),
                        runtime_get_error_, {}, "err_msg");
                    // 拷贝消息：myp_get_error 返回共享 myp_error_msg 缓冲指针，
                    // 后续 throw 会覆写它 → 存储的字符串漂移（Result 等存错误即踩中）。
                    auto* dup = module_->getFunction("myp_strdup");
                    if (!dup) {
                        auto* ft = llvm::FunctionType::get(ptr_ty, {ptr_ty}, false);
                        dup = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                            "myp_strdup", module_.get());
                    }
                    bound = builder_.CreateCall(dup->getFunctionType(), dup, {bound}, "err_copy");
                }
                auto* ev = createEntryBlockAlloca(func, ptr_ty, cc.var_name);
                builder_.CreateStore(bound, ev);
                setNamedValue(cc.var_name, ev);
                if (obj_catch) {
                    var_class_map_[cc.var_name] = cc.var_type;
                    caught_slot = ev;
                }
            }
            if (cc.block) generateBlock(*cc.block);
            // ARC ownership: the catch takes the exception's reference. Release
            // it at NORMAL catch end; a rethrow/return/break (dead path) skips
            // this, so ownership flows to the next catch (or leaks if uncaught,
            // safe). Non-matching catches never run → no uninitialized slot.
            if (caught_slot && builder_.GetInsertBlock() &&
                !builder_.GetInsertBlock()->getTerminator() && runtime_release_) {
                llvm::Value* rel = nullptr;
                if (iface_catch) {
                    auto* fat = builder_.CreateLoad(getLLVMType(TypeInfo(TypeKind::Interface)), caught_slot);
                    rel = builder_.CreateExtractValue(fat, 0);
                } else {
                    rel = builder_.CreateLoad(ptr_ty, caught_slot);
                }
                builder_.CreateCall(runtime_release_, {rel});
            }
            if (!builder_.GetInsertBlock()->getTerminator()) {
                if (finally_bb) {
                    builder_.CreateStore(llvm::ConstantInt::get(i8_ty, 0), finally_flag);
                    builder_.CreateBr(finally_bb);
                } else {
                    builder_.CreateBr(merge_bb);
                }
            }
        }
    }

    // === Exception propagating through finally (flag=1) ===
    if (propagate_bb) {
        builder_.SetInsertPoint(propagate_bb);
        // No matching catch: the try's abandoned inner slots must be freed
        // before the finally body runs (then rethrow frees the outer ones).
        emitReleaseTryInnerSlots();
        builder_.CreateStore(llvm::ConstantInt::get(i8_ty, 1), finally_flag);
        builder_.CreateBr(finally_bb);
    }

    // === Generate finally block ===
    if (finally_bb) {
        finally_ctx_stack_.back().in_finally = true;
        builder_.SetInsertPoint(finally_bb);
        // Pop this try's handler BEFORE running the finally body: a throw here
        // (or in a nested finally) must go to the OUTER handler, not re-trigger
        // this try (infinite loop). All finally entry paths (try end / catch
        // end / propagate / return/break/continue forwarding) converge here.
        builder_.CreateCall(runtime_exception_pop_->getFunctionType(),
            runtime_exception_pop_, {});
        if (s.finally_block) generateBlock(*s.finally_block);
        finally_ctx_stack_.back().in_finally = false;
        if (!builder_.GetInsertBlock()->getTerminator()) {
            auto& fc = finally_ctx_stack_.back();
            if (!rethrow_bb)
                rethrow_bb = llvm::BasicBlock::Create(ctx_, "catch_rethrow", func);
            auto* flag = builder_.CreateLoad(i8_ty, finally_flag, "finally_mode");
            // Dispatch on exit mode: 0→merge, 1→rethrow, 2→return,
            // 3→break, 4→continue (2/3/4 forward to an enclosing finally first).
            auto* fin_merge = llvm::BasicBlock::Create(ctx_, "fin_merge", func);
            llvm::BasicBlock* fin_ret = llvm::BasicBlock::Create(ctx_, "fin_ret", func);
            llvm::BasicBlock* fin_break = llvm::BasicBlock::Create(ctx_, "fin_break", func);
            llvm::BasicBlock* fin_cont = llvm::BasicBlock::Create(ctx_, "fin_cont", func);
            auto* c0 = llvm::ConstantInt::get(i8_ty, 0);
            auto* c1 = llvm::ConstantInt::get(i8_ty, 1);
            auto* c2 = llvm::ConstantInt::get(i8_ty, 2);
            auto* c3 = llvm::ConstantInt::get(i8_ty, 3);
            auto* e0 = builder_.CreateICmpEQ(flag, c0);
            auto* chk1 = llvm::BasicBlock::Create(ctx_, "fin_chk1", func);
            builder_.CreateCondBr(e0, fin_merge, chk1);
            builder_.SetInsertPoint(chk1);
            auto* e1 = builder_.CreateICmpEQ(flag, c1);
            auto* chk2 = llvm::BasicBlock::Create(ctx_, "fin_chk2", func);
            builder_.CreateCondBr(e1, rethrow_bb, chk2);
            builder_.SetInsertPoint(chk2);
            auto* e2 = builder_.CreateICmpEQ(flag, c2);
            auto* chk3 = llvm::BasicBlock::Create(ctx_, "fin_chk3", func);
            builder_.CreateCondBr(e2, fin_ret, chk3);
            builder_.SetInsertPoint(chk3);
            auto* e3 = builder_.CreateICmpEQ(flag, c3);
            builder_.CreateCondBr(e3, fin_break, fin_cont);

            // mode 0 → this try's normal end
            builder_.SetInsertPoint(fin_merge);
            builder_.CreateBr(merge_bb);

            // mode 2 → return: forward to enclosing finally, else do the real return
            builder_.SetInsertPoint(fin_ret);
            if (fc.outer_finally_bb) {
                builder_.CreateStore(c2, fc.outer_flag_slot);
                builder_.CreateBr(fc.outer_finally_bb);
            } else {
                // flag==2 (return) can only be set by an actual return inside the
                // try, which always creates finally_ret_slot_ (for non-void). If
                // there was none, this block is unreachable — emit a well-typed
                // unreachable instead of `ret void` in a non-void function
                // (pre-existing bug: try/finally in an int-returning fn failed
                // LLVM verify with "return type does not match").
                if (finally_ret_slot_ || current_function_->getReturnType()->isVoidTy()) {
                    llvm::Value* rv = nullptr;
                    if (finally_ret_slot_ && !current_function_->getReturnType()->isVoidTy())
                        rv = builder_.CreateLoad(current_function_->getReturnType(), finally_ret_slot_);
                    emitFunctionReturn(rv);
                } else {
                    builder_.CreateUnreachable();
                }
            }

            // mode 3 → break: forward to enclosing finally, else to loop break target
            builder_.SetInsertPoint(fin_break);
            if (fc.outer_finally_bb) {
                builder_.CreateStore(c3, fc.outer_flag_slot);
                builder_.CreateBr(fc.outer_finally_bb);
            } else if (fc.break_bb) {
                builder_.CreateBr(fc.break_bb);
            } else {
                builder_.CreateBr(merge_bb);
            }

            // mode 4 → continue
            builder_.SetInsertPoint(fin_cont);
            if (fc.outer_finally_bb) {
                builder_.CreateStore(llvm::ConstantInt::get(i8_ty, 4), fc.outer_flag_slot);
                builder_.CreateBr(fc.outer_finally_bb);
            } else if (fc.continue_bb) {
                builder_.CreateBr(fc.continue_bb);
            } else {
                builder_.CreateBr(merge_bb);
            }
        }
        finally_ctx_stack_.pop_back();
    }

    // === rethrow: pop this handler and longjmp to the next (outer) handler ===
    if (rethrow_bb) {
        builder_.SetInsertPoint(rethrow_bb);
        // With a finally, the handler was already popped at finally-block entry
        // (mode-1 dispatch happens there); popping again would pop the outer
        // try's handler. Without a finally (catch-only no-match), this is the
        // only pop on the path.
        if (!finally_bb)
            builder_.CreateCall(runtime_exception_pop_->getFunctionType(),
                runtime_exception_pop_, {});
        // Leaving the function (no enclosing same-function try): free the
        // remaining outer slots before the outward longjmp.
        emitUnwindRelease(true);
        auto* jb2 = builder_.CreateCall(runtime_exception_get_jmpbuf_->getFunctionType(),
            runtime_exception_get_jmpbuf_, {}, "outer_handler");
        auto* one = llvm::ConstantInt::get(i32_ty, 1);
        builder_.CreateCall(runtime_longjmp_->getFunctionType(), runtime_longjmp_, {jb2, one});
        builder_.CreateUnreachable();
    }

    builder_.SetInsertPoint(merge_bb);
    // (Handler is popped on every path into here: try-body end when there is no
    // finally, catch-body entry, or finally-block entry. An unconditional pop
    // here would double-pop the outer try's handler.)
    // Pop the unwind-collection context (must match the emplace_back at entry).
    if (!try_ctx_stack_.empty()) try_ctx_stack_.pop_back();
}

void CodeGen::emitExceptionRethrow() {
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    auto* i32_ty = llvm::Type::getInt32Ty(ctx_);
    // NOTE: no myp_exception_pop here — `throw;` is only valid inside a catch
    // body, and the handler was already popped at catch-body entry (or at the
    // enclosing finally-block entry). Popping again would pop the OUTER try's
    // handler and misdirect the rethrow (caught as "uncaught").
    // Rethrow leaving the function: free the still-live outer slots first.
    emitUnwindRelease(true);
    auto* jb2 = builder_.CreateCall(runtime_exception_get_jmpbuf_->getFunctionType(),
        runtime_exception_get_jmpbuf_, {}, "outer_handler");
    auto* one = llvm::ConstantInt::get(i32_ty, 1);
    builder_.CreateCall(runtime_longjmp_->getFunctionType(), runtime_longjmp_, {jb2, one});
    builder_.CreateUnreachable();
}

void CodeGen::generateThrowStmt(const ThrowStmt& s) {
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    auto* i32_ty = llvm::Type::getInt32Ty(ctx_);
    if (s.throw_type == "rethrow") {
        // throw; — rethrow the current exception. If inside a try-with-finally,
        // run the finally body first (mode=1), then propagate outward; otherwise
        // propagate directly (the current exception state stays in the runtime).
        // (Ownership model: the exception keeps the object; the rethrowing catch's
        // normal-end release is skipped because this path ends in longjmp.)
        if (!finally_ctx_stack_.empty() && !finally_ctx_stack_.back().in_finally) {
            auto& fc = finally_ctx_stack_.back();
            builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 1), fc.flag_slot);
            builder_.CreateBr(fc.finally_bb);
        } else {
            emitExceptionRethrow();
        }
        return;
    }
    if (s.throw_type == "string") {
        auto* msg = generateExpr(*s.expr);
        builder_.CreateCall(runtime_throw_->getFunctionType(), runtime_throw_, {msg});
    } else {
        auto* obj = generateExpr(*s.expr);
        auto it = class_type_ids_.find(s.throw_type);
        int tid = (it != class_type_ids_.end()) ? it->second : 0;
        // The exception owns a reference independent of any local slot: a fresh
        // (new/call/lambda) value transfers its +1 to the exception; an alias
        // (identifier / member access) must be retained so the unwind release of
        // its slot doesn't free the object before the catch binds it.
        if (obj && !isFreshArcExpr(*s.expr) && runtime_retain_)
            builder_.CreateCall(runtime_retain_->getFunctionType(), runtime_retain_, {obj});
        builder_.CreateCall(runtime_throw_object_->getFunctionType(),
            runtime_throw_object_, {obj, llvm::ConstantInt::get(i32_ty, tid)});
    }
    // This throw leaves the function (no same-function try): free the abandoned
    // local slots before the longjmp skips their scope-exit releases.
    emitUnwindRelease(false);
    // longjmp to the innermost active handler (stack top).
    auto* jb = builder_.CreateCall(runtime_exception_get_jmpbuf_->getFunctionType(),
        runtime_exception_get_jmpbuf_, {}, "cur_handler");
    auto* one = llvm::ConstantInt::get(i32_ty, 1);
    builder_.CreateCall(runtime_longjmp_->getFunctionType(), runtime_longjmp_, {jb, one});
    builder_.CreateUnreachable();
}

}  // namespace mylang
