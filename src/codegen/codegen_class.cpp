// class/interface/function declaration codegen — part of the CodeGen implementation, split from codegen.cpp.
// Pure refactor: member definitions moved out of the 10k-line monolith;
// no behavior change. See codegen.cpp for the class declaration.

#include "mylang/CodeGen.h"
#include "mylang/MypPasses.h"
#include "escape_analysis.h"

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

std::string CodeGen::mangleConcreteTypeNode(const TypeNode& node) {
    // Type-param placeholder → concrete arg (e.g. R → string), then recurse.
    if (!node.class_name.empty() && !node.type_args.empty() && !node.isArray()) {
        // Generic class: Option<int> → "Option_int_inst"
        for (auto& tp : current_type_params_)
            if (node.class_name == tp.first)
                return mangleConcreteTypeNode(tp.second);
    }
    for (auto& tp : current_type_params_) {
        if (!node.class_name.empty() && node.class_name == tp.first)
            return mangleConcreteTypeNode(tp.second);
    }
    if (node.element_type)
        return mangleConcreteTypeNode(*node.element_type) + "[]";
    if (node.isFunction()) {
        std::string s = "(";
        for (size_t i = 0; i < node.func_param_types.size(); i++) {
            if (i) s += ", ";
            s += mangleConcreteTypeNode(node.func_param_types[i]);
        }
        s += ") -> ";
        s += node.func_return_type ? mangleConcreteTypeNode(*node.func_return_type) : "void";
        return s;
    }
    if (node.isTuple()) {
        std::string s = "(";
        for (size_t i = 0; i < node.func_param_types.size(); i++) {
            if (i) s += ", ";
            s += mangleConcreteTypeNode(node.func_param_types[i]);
        }
        s += ")";
        return s;
    }
    if (!node.class_name.empty()) {
        std::string s = node.class_name;
        bool is_generic = !node.type_args.empty();
        for (auto& ta : node.type_args) { s += "_"; s += mangleConcreteTypeNode(ta); }
        if (is_generic) s += "_inst";
        return s;
    }
    return mangleTypeNode(node);
}

void CodeGen::generateErrorVTables() {
    // Only when the Error interface is declared in this TU.
    const InterfaceDecl* error_iface = nullptr;
    for (auto& ifd : current_tu_->interfaces)
        if (ifd.name == "Error") { error_iface = &ifd; break; }
    if (!error_iface) return;

    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    int max_tid = 0;
    for (auto& kv : class_type_ids_)
        if (kv.second > max_tid) max_tid = kv.second;
    std::vector<llvm::Constant*> entries(max_tid + 1,
        llvm::ConstantPointerNull::get(ptr_ty));

    for (auto& cls : current_tu_->classes) {
        if (cls.interface_class_name != "Error") continue;
        auto tit = class_type_ids_.find(cls.name);
        if (tit == class_type_ids_.end()) continue;

        // Build (or reuse) the Error vtable for this class.
        std::string vkey = "Error_" + cls.name;
        auto* vgv = module_->getGlobalVariable("__myp_vtable_" + vkey);
        if (!vgv) {
            std::vector<llvm::Constant*> func_ptrs;
            for (auto& ia : error_iface->actions) {
                std::string fn = cls.name + "_" + ia.name;
                auto* callee = module_->getFunction(fn);
                // trait 默认实现：类未实现但接口方法带默认体 → 回退默认函数
                if (!callee && ia.body)
                    callee = module_->getFunction(ifaceDefaultName(error_iface->name, ia.name, cls.name));
                func_ptrs.push_back(callee
                    ? llvm::ConstantExpr::getPointerCast(callee, ptr_ty)
                    : llvm::ConstantPointerNull::get(ptr_ty));
            }
            auto* arr_type = llvm::ArrayType::get(ptr_ty, func_ptrs.size());
            auto* arr_init = llvm::ConstantArray::get(arr_type, func_ptrs);
            vgv = new llvm::GlobalVariable(*module_, arr_type, true,
                llvm::GlobalValue::InternalLinkage, arr_init,
                "__myp_vtable_" + vkey);
        }
        entries[tit->second] = llvm::ConstantExpr::getPointerCast(vgv, ptr_ty);
    }

    auto* arr_ty = llvm::ArrayType::get(ptr_ty, entries.size());
    auto* arr_init = llvm::ConstantArray::get(arr_ty, entries);
    error_vtables_gv_ = new llvm::GlobalVariable(*module_, arr_ty, true,
        llvm::GlobalValue::InternalLinkage, arr_init, "__myp_error_vtables");
}

bool CodeGen::isErrorInterface(const std::string& name) {
    if (name != "Error" || !current_tu_) return false;
    for (auto& ifd : current_tu_->interfaces)
        if (ifd.name == "Error") return true;
    return false;
}

bool CodeGen::isArcClassType(const TypeNode& tn) {
    if (!tn.class_name.empty() && tn.class_name.find("::") != std::string::npos) {
        auto ra = const_cast<CodeGen*>(this)->resolveAssocType(
            tn.class_name.substr(0, tn.class_name.find("::")),
            tn.class_name.substr(tn.class_name.find("::") + 2));
        if (ra) return isArcClassType(*ra);
        return false;
    }
    // Generic type-param placeholder → concrete arg for current instance.
    for (auto& tp : current_type_params_) {
        if (!tn.class_name.empty() && tn.class_name == tp.first)
            return isArcClassType(tp.second);
    }
    if (!tn.class_name.empty()) {
        // Generic class instance (Box<int>) → mangled concrete class ref.
        if (!tn.type_args.empty()) {
            std::string m = mangleConcreteTypeNode(tn);
            return getClassStruct(m) != nullptr;
        }
        return getClassStruct(tn.class_name) != nullptr;
    }
    return false;
}

bool CodeGen::isArcRefType(const TypeNode& tn) {
    if (!tn.class_name.empty() && tn.class_name.find("::") != std::string::npos) {
        auto ra = const_cast<CodeGen*>(this)->resolveAssocType(
            tn.class_name.substr(0, tn.class_name.find("::")),
            tn.class_name.substr(tn.class_name.find("::") + 2));
        if (ra) return isArcRefType(*ra);
        return false;
    }
    // Generic type-param placeholder → concrete arg for current instance.
    for (auto& tp : current_type_params_) {
        if (!tn.class_name.empty() && tn.class_name == tp.first)
            return isArcRefType(tp.second);
    }
    if (tn.isArray()) {
        // Dynamic class-element arrays (T[] of class) are ref-counted
        // (myp_alloc_class_array); their elements are strong slots released
        // when the array is released. Fixed arrays ([N x T]) are stack/field
        // VALUES — not ARC-managed (lambda captures / returns must not treat
        // them as refs). Interface arrays hold fat-pointer elements — skipped.
        if (tn.element_type && tn.array_size == 0)
            return isArcClassType(*tn.element_type);
        return false;
    }
    if (tn.isTuple() || tn.isFunction()) return false;
    if (!tn.class_name.empty()) {
        // Class instance?
        if (getClassStruct(tn.class_name)) return true;
        // Interface fat pointer (data part is the object)?
        if (current_tu_)
            for (auto& ifd : current_tu_->interfaces)
                if (ifd.name == tn.class_name) return true;
        return false;
    }
    return false;
}

bool CodeGen::isCountedArrayType(const TypeNode& tn) {
    return tn.isArray() && tn.array_size == 0;
}

// See header comment. Retain an @coro param / `this` so the object the
// coroutine holds stays alive across yields (the caller may release it while
// the coroutine is parked), and release it at normal completion (scope exit)
// or on destroy / uncaught exception (frame registry).
void CodeGen::registerCoroParam(const TypeNode& tn, const TypeInfo& ti,
                                llvm::Value* alloca, llvm::Value* val) {
    if (!current_is_coro_ || !alloca || !val) return;
    int kind = -1;
    if (ti.kind == TypeKind::String) {
        kind = 0;
        emitRetain(val);
    } else if (ti.kind == TypeKind::Interface) {
        kind = 1;
        emitRetain(builder_.CreateExtractValue(val, 0));
    } else if (ti.kind == TypeKind::Function) {
        kind = 2;
        emitRetain(builder_.CreateExtractValue(val, 0));
    } else if (ti.kind == TypeKind::Slice) {
        kind = 4;
        emitRetainSlice(val);
    } else if (ti.kind == TypeKind::Array && ti.array_size == 0) {
        // Dynamic T[]: counted backing, local owns a counted reference.
        kind = 0;
        emitRetain(val);
    } else if (ti.kind == TypeKind::Class) {
        kind = 0;
        emitRetain(val);
    } else if (ti.kind == TypeKind::Struct && isArcFieldType(tn)) {
        const StructDecl* sd = findStruct(tn.class_name);
        if (!sd) return;
        kind = 5;
        emitStructFieldsValue(builder_, val, *sd, true);
        arc_struct_slot_types_[alloca] = tn.class_name;
    } else {
        return;
    }
    registerArcSlot(alloca, kind);
    // Mirror into the frame registry for destroy/exception teardown (class /
    // interface / function-value — same set as var-decl; slices and structs are
    // only released at normal completion).
    if (kind == 0 || kind == 1 || kind == 2)
        emitCoroFrameSet(alloca, val);
}

bool CodeGen::isStringType(const TypeNode& tn) {
    return tn.class_name.empty() && !tn.isArray() && !tn.isTuple() &&
           tn.basic_type == BuiltinType::String;
}

bool CodeGen::isArcReturnType(const TypeNode& tn) {
    if (isArcRefType(tn)) return true;        // class / interface / dyn class array
    if (isCountedArrayType(tn)) return true;  // dynamic T[] (all element kinds)
    if (isStringType(tn)) return true;        // M8 counted string
    if (tn.class_name == "slice") return true; // slice value -> counted backing
    return false;
}

// 动态数组 new T[N]（常量维度 + 元素非 ARC）→ 可栈上化（alloca backing，
// 跳过 ARC 头释放）。维度须为 IntegerLiteral 常量（alloca 固定大小）；元素
// 非 ARC（int/long/double/float/bool/char 等值类型——ARC 元素需作用域末逐元素
// 释放，第一版保守排除）。
bool CodeGen::isStackArrayCandidate(const Expr* e) {
    if (!e || e->kind != ExprKind::NewArrayExpr) return false;
    auto& na = static_cast<const NewArrayExpr&>(*e);
    if (na.dimensions.empty()) return false;
    for (auto& d : na.dimensions)
        if (!d || d->kind != ExprKind::IntegerLiteral) return false;
    if (isArcReturnType(na.element_type)) return false;
    return true;
}

// M8 structs: a struct field that holds an ARC reference (class/interface/
// slice/dynamic-array/string), OR is a nested struct that transitively holds
// one. Such fields need retain/release whenever the struct value is copied or
// discarded.
bool CodeGen::isArcFieldType(const TypeNode& tn) {
    if (tn.isArray() && tn.array_size > 0 && tn.element_type)
        return isArcFieldType(*tn.element_type);
    if (isArcReturnType(tn)) return true;
    if (!tn.class_name.empty() && !tn.isArray() && findStruct(tn.class_name)) {
        const StructDecl* nsd = findStruct(tn.class_name);
        if (nsd)
            for (auto& p : nsd->properties)
                if (isArcFieldType(p.type)) return true;
    }
    return false;
}

// 类是否含 ARC 属性（string/数组/类/接口/slice，或嵌套 struct 递归含 ARC 字段）。
// 栈上化对象作用域结束只回收对象本身、不释放 ARC 属性 → 泄漏（esc_leak 实测
// 10 万 string 泄漏）→ 含这类属性的类不栈上化（第一版保守）。
bool CodeGen::classHasArcProps(const std::string& cls) {
    const ClassDecl* c = findClass(cls);
    if (!c) return true;  // 找不到类（泛型实例名等）→ 保守
    auto* st = getClassStruct(cls);
    for (size_t i = 0; i < c->properties.size(); i++) {
        auto& p = c->properties[i];
        if (p.weak || isArcFieldType(p.type)) return true;
        if (p.type.isArray() && p.type.array_size > 0 && st && i < st->getNumElements()) {
            auto* at = llvm::dyn_cast<llvm::ArrayType>(st->getElementType(i));
            if (at && at->getElementType()->isPointerTy()) return true;
        }
    }
    return false;
}

// Operate (retain/release) on one loaded field value. field_val is a plain
// pointer (string/class/counted-array), a fat-pointer struct (slice/interface/
// function value -> index 0 is the data ptr), or a nested struct value (recurse).
void CodeGen::emitArcFieldOp(llvm::IRBuilderBase& b, llvm::Value* field_val,
                             const TypeNode& tn, bool retain) {
    if (!field_val) return;
    // Nested user struct -> recurse into its ARC fields.
    if (!tn.class_name.empty() && !tn.isArray() && findStruct(tn.class_name)) {
        const StructDecl* nsd = findStruct(tn.class_name);
        if (nsd && field_val->getType()->isStructTy()) {
            emitStructFieldsValue(b, field_val, *nsd, retain);
            return;
        }
    }
    if (field_val->getType()->isStructTy()) {
        // slice / interface / function value -> index 0 is the data pointer.
        auto* data = b.CreateExtractValue(field_val, 0);
        if (retain) emitRetain(data);
        else if (runtime_release_) b.CreateCall(runtime_release_, {data});
    } else if (field_val->getType()->isPointerTy()) {
        if (retain) emitRetain(field_val);
        else if (runtime_release_) b.CreateCall(runtime_release_, {field_val});
    }
}

void CodeGen::emitStructFieldsValue(llvm::IRBuilderBase& b, llvm::Value* struct_val,
                                    const StructDecl& sd, bool retain) {
    auto* st = getStructType(sd.name);
    if (!st || !struct_val || !struct_val->getType()->isStructTy()) return;
    unsigned n = (unsigned)std::min<size_t>(sd.properties.size(), st->getNumElements());
    for (unsigned i = 0; i < n; i++) {
        const TypeNode& ftn = sd.properties[i].type;
        if (!isArcFieldType(ftn)) continue;
        auto* fv = b.CreateExtractValue(struct_val, i);
        emitArcFieldOp(b, fv, ftn, retain);
    }
}

void CodeGen::emitStructFieldsPtr(llvm::IRBuilderBase& b, llvm::Value* struct_ptr,
                                  const StructDecl& sd, bool retain) {
    auto* st = getStructType(sd.name);
    if (!st || !struct_ptr) return;
    unsigned n = (unsigned)std::min<size_t>(sd.properties.size(), st->getNumElements());
    for (unsigned i = 0; i < n; i++) {
        const TypeNode& ftn = sd.properties[i].type;
        if (!isArcFieldType(ftn)) continue;
        auto* gep = b.CreateStructGEP(st, struct_ptr, i);
        auto* fv = b.CreateLoad(st->getElementType(i), gep);
        emitArcFieldOp(b, fv, ftn, retain);
    }
}

bool CodeGen::isOwnedStructLocal(llvm::Value* alloca) {
    if (!alloca) return false;
    for (auto& scope : arc_scope_slots_)
        for (auto& s : scope)
            if (s.alloca == alloca && s.kind == 5) return true;
    return false;
}

void CodeGen::maybeReleaseLocal(const std::string& name, llvm::Value* alloca) {
    if (!runtime_release_ || !alloca) return;
    if (builder_.GetInsertBlock()->getTerminator()) return;  // dead path — skip
    auto* slot = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), alloca, name + ".ref");
    builder_.CreateCall(runtime_release_, {slot});
}

void CodeGen::generateArcSupport(TranslationUnit& tu) {
    auto* p = llvm::PointerType::get(ctx_, 0);
    int max_tid = 0;
    for (auto& kv : class_type_ids_)
        if (kv.second > max_tid) max_tid = kv.second;
    std::vector<llvm::Constant*> table(max_tid + 1,
        llvm::ConstantPointerNull::get(p));

    for (auto& cls : tu.classes) {
        auto tit = class_type_ids_.find(cls.name);
        if (tit == class_type_ids_.end()) continue;
        current_type_params_.clear();
        for (size_t i = 0; i < cls.type_params.size() && i < cls.inst_type_args.size(); i++)
            current_type_params_.emplace_back(cls.type_params[i], cls.inst_type_args[i]);
        if (!classHasArcProps(cls.name)) {
            table[tit->second] = llvm::ConstantExpr::getPointerCast(runtime_free_object_, p);
            continue;
        }
        std::string dname = "__myp_destroy_" + cls.name;
        auto* fn = module_->getFunction(dname);
        if (!fn) {
            auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {p}, false);
            fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, dname, module_.get());
        }
        auto* bb = llvm::BasicBlock::Create(ctx_, "entry", fn);
        llvm::IRBuilder<> b(ctx_);
        b.SetInsertPoint(bb);
        auto* self = fn->getArg(0);
        auto* st = getClassStruct(cls.name);
        bool is_iface_field = false;
        if (st) {
            for (size_t pi = 0; pi < cls.properties.size(); pi++) {
                auto& prop = cls.properties[pi];
                if (prop.weak) {
                    // M7: weak field — unregister + null the slot (the holder
                    // is being destroyed; the slot address must not dangle in
                    // the target's weak registry).
                    auto* gep = b.CreateStructGEP(st, self, pi);
                    if (!runtime_weak_clear_) {
                        auto* pt = llvm::PointerType::get(ctx_, 0);
                        auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                            {pt}, false);
                        runtime_weak_clear_ = llvm::Function::Create(ft,
                            llvm::Function::ExternalLinkage, "myp_weak_clear",
                            module_.get());
                    }
                    b.CreateCall(runtime_weak_clear_->getFunctionType(),
                                 runtime_weak_clear_, {gep});
                    continue;
                }
                if (isCountedArrayType(prop.type)) {
                    // M8: dynamic T[] field — release the counted backing.
                    auto* gep = b.CreateStructGEP(st, self, pi);
                    auto* data = b.CreateLoad(llvm::PointerType::get(ctx_, 0), gep);
                    b.CreateCall(runtime_release_, {data});
                    continue;
                }
                if (prop.type.isArray() && prop.type.array_size > 0 && prop.type.element_type) {
                    auto* field = b.CreateStructGEP(st, self, pi);
                    auto* array_ty = llvm::dyn_cast<llvm::ArrayType>(st->getElementType(pi));
                    if (array_ty && (array_ty->getElementType()->isPointerTy() ||
                        isArcFieldType(*prop.type.element_type))) {
                        for (int i = 0; i < prop.type.array_size; i++) {
                            auto* slot = b.CreateInBoundsGEP(array_ty, field,
                                {b.getInt32(0), b.getInt32(i)});
                            auto* value = b.CreateLoad(array_ty->getElementType(), slot);
                            emitArcFieldOp(b, value, *prop.type.element_type, false);
                        }
                    }
                    continue;
                }
                if (prop.type.class_name == "slice") {
                    // M8: slice field → release the counted backing via data
                    // (index 0 of the {data,len} fat pointer).
                    auto* gep = b.CreateStructGEP(st, self, pi);
                    auto* fat = b.CreateLoad(getLLVMType(typeNodeToCodegenType(prop.type)), gep);
                    auto* data = b.CreateExtractValue(fat, 0);
                    b.CreateCall(runtime_release_, {data});
                    continue;
                }
                if (isStringType(prop.type)) {
                    // M8: string field → release the counted string.
                    auto* gep = b.CreateStructGEP(st, self, pi);
                    auto* s = b.CreateLoad(llvm::PointerType::get(ctx_, 0), gep);
                    b.CreateCall(runtime_release_, {s});
                    continue;
                }
                // M8 structs: a struct-typed field holding ARC refs (incl.
                // nested structs) → release its ARC fields on destroy.
                if (!prop.type.class_name.empty()) {
                    const StructDecl* fnsd = findStruct(prop.type.class_name);
                    if (fnsd && isArcFieldType(prop.type)) {
                        auto* gep = b.CreateStructGEP(st, self, pi);
                        auto* fv = b.CreateLoad(
                            getLLVMType(typeNodeToCodegenType(prop.type)), gep);
                        emitStructFieldsValue(b, fv, *fnsd, false);
                        continue;
                    }
                }
                if (!isArcRefType(prop.type)) continue;
                // Interface slot = fat pointer { data, vtable } → release data.
                bool iface = false;
                if (!prop.type.class_name.empty() && current_tu_)
                    for (auto& ifd : current_tu_->interfaces)
                        if (ifd.name == prop.type.class_name) { iface = true; break; }
                is_iface_field = iface;
                auto* gep = b.CreateStructGEP(st, self, pi);
                llvm::Value* slot;
                if (iface) {
                    auto* fat = b.CreateLoad(getLLVMType(typeNodeToCodegenType(prop.type)), gep);
                    slot = b.CreateExtractValue(fat, 0);
                } else {
                    slot = b.CreateLoad(llvm::PointerType::get(ctx_, 0), gep);
                }
                b.CreateCall(runtime_release_, {slot});
            }
        }
        (void)is_iface_field;
        b.CreateCall(runtime_free_object_, {self});
        b.CreateRetVoid();
        table[tit->second] = llvm::ConstantExpr::getPointerCast(fn, p);
    }

    // __myp_release_table: ExternalLinkage so the runtime's myp_release can
    // dispatch by type_id. Sized to max_tid+1; null entries fall back to a
    // plain myp_free_object in myp_release.
    if (table.empty()) table.push_back(llvm::ConstantPointerNull::get(p));
    auto* arr_ty = llvm::ArrayType::get(p, table.size());
    auto* init = llvm::ConstantArray::get(arr_ty, table);
    release_table_gv_ = new llvm::GlobalVariable(*module_, arr_ty, false,
        llvm::GlobalValue::ExternalLinkage, init, "__myp_release_table");

    // M9: max type_id — lets the runtime validate object headers in strict
    // mode (a live header's type_id must be STR/ARR magic or 1..max_tid).
    // v3.15.175: non-constant (writable) — a constant global gets linked/
    // optimized differently, so the MYP runtime's `__myp_fn_addr` external ref
    // (ccClassOk) reads a shifted/garbage value (e.g. Box<Node> tid=6 > fake
    // maxv=3) and rejects generic-instance objects → generic cycles/arrays leak.
    // Writable, like __myp_release_table, resolves correctly.
    new llvm::GlobalVariable(*module_, llvm::Type::getInt32Ty(ctx_), false,
        llvm::GlobalValue::ExternalLinkage,
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), max_tid),
        "__myp_max_type_id");

    // §五-4 RTTI: __myp_type_name_table — type_id → class name string
    // (index 0 = "" for string messages / non-class objects). ExternalLinkage
    // + constant so the runtime's myp_obj_type_name can index it directly.
    // ALWAYS emitted (a bare [null] table when no class got a type_id) because
    // the runtime's myp_type_name references the symbol unconditionally.
    {
        auto* i32ty = llvm::Type::getInt32Ty(ctx_);
        size_t n = (size_t)max_tid + 1;
        std::vector<llvm::Constant*> names(n,
            llvm::ConstantPointerNull::get(p));
        for (auto& kv : class_type_ids_) {
            if (kv.second <= 0 || kv.second > (int)max_tid) continue;
            auto* str = llvm::ConstantDataArray::getString(ctx_, kv.first);
            auto* gv = new llvm::GlobalVariable(*module_, str->getType(), true,
                llvm::GlobalValue::PrivateLinkage, str, "__myp_tn_" + kv.first);
            std::vector<llvm::Constant*> sidx = {
                llvm::ConstantInt::get(i32ty, 0), llvm::ConstantInt::get(i32ty, 0)};
            names[kv.second] = llvm::ConstantExpr::getInBoundsGetElementPtr(
                str->getType(), gv, sidx);
        }
        auto* name_arr_ty = llvm::ArrayType::get(p, names.size());
        auto* name_init = llvm::ConstantArray::get(name_arr_ty, names);
        new llvm::GlobalVariable(*module_, name_arr_ty, true,
            llvm::GlobalValue::ExternalLinkage, name_init, "__myp_type_name_table");
    }
}

void CodeGen::createClassActionDecl(const ClassDecl& cls, const ActionDecl& action) {
    auto fn = action.has_constructor
        ? constructorMangledName(cls.name, action.name, action.params)
        : cls.name + "_" + action.name;
    if (module_->getFunction(fn)) return;
    std::vector<llvm::Type*> pts = {llvm::PointerType::get(ctx_, 0)};
    for (auto& p : action.params) {
        TypeInfo pt = typeNodeToCodegenType(p.type);
        pts.push_back(getLLVMType(pt));
    }
    auto* ft = llvm::FunctionType::get(getLLVMType(typeNodeToCodegenType(action.return_type)), pts, false);
    llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn, module_.get());
    // Track if this is a static action for later use
    is_static_action_[fn] = false;
}

void CodeGen::createStaticActionDecl(const ClassDecl& cls, const ActionDecl& action) {
    // Generic static method templates (List.map<T,R>) are not emitted directly;
    // only their monomorphized instances (appended to tu.functions by sema) are.
    if (!action.type_params.empty()) return;
    auto fn = cls.name + "_" + action.name;
    if (module_->getFunction(fn)) return;
    std::vector<llvm::Type*> pts;
    for (auto& p : action.params) {
        TypeInfo pt = typeNodeToCodegenType(p.type);
        pts.push_back(getLLVMType(pt));
    }
    auto* ft = llvm::FunctionType::get(getLLVMType(typeNodeToCodegenType(action.return_type)), pts, false);
    llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn, module_.get());
    is_static_action_[fn] = true;
}

std::string CodeGen::ifaceDefaultName(const std::string& iface,
                                      const std::string& method,
                                      const std::string& cls) {
    return "__ifdef_" + iface + "_" + method + "_" + cls;
}

std::optional<TypeNode> CodeGen::resolveAssocType(const std::string& owner,
                                                  const std::string& member) {
    std::string cls_name = owner;
    for (auto& tp : current_type_params_)
        if (tp.first == owner) { cls_name = tp.second.class_name; break; }
    if (cls_name.empty() || !current_tu_) return std::nullopt;
    for (auto& cls : current_tu_->classes) {
        if (cls.name != cls_name) continue;
        auto bit = cls.associated_type_bindings.find(member);
        if (bit != cls.associated_type_bindings.end()) return bit->second;
        break;
    }
    return std::nullopt;
}

llvm::Function* CodeGen::findInterfaceDefault(const std::string& cls_name,
                                              const std::string& method) {
    if (!current_tu_) return nullptr;
    const ClassDecl* cls = nullptr;
    for (auto& c : current_tu_->classes)
        if (c.name == cls_name) { cls = &c; break; }
    if (!cls || cls->interface_class_name.empty()) return nullptr;
    const InterfaceDecl* iface = nullptr;
    for (auto& ifd : current_tu_->interfaces)
        if (ifd.name == cls->interface_class_name) { iface = &ifd; break; }
    if (!iface) return nullptr;
    for (auto& ia : iface->actions)
        if (ia.name == method && ia.body)
            return module_->getFunction(ifaceDefaultName(iface->name, method, cls_name));
    return nullptr;
}

bool CodeGen::isInterfaceFatType(llvm::Type* ty) {
    if (!ty || !ty->isStructTy()) return false;
    auto* st = llvm::dyn_cast<llvm::StructType>(ty);
    if (!st || st->getNumElements() != 2) return false;
    return st->getElementType(0)->isPointerTy() &&
           st->getElementType(1)->isPointerTy();
}

llvm::GlobalVariable* CodeGen::getOrCreateVtable(const std::string& iface_name,
                                                 const std::string& cls_name) {
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    std::string vkey = iface_name + "_" + cls_name;
    auto* vgv = module_->getGlobalVariable("__myp_vtable_" + vkey);
    if (vgv) return vgv;
    if (!current_tu_) return nullptr;
    const InterfaceDecl* iface = nullptr;
    for (auto& ifd : current_tu_->interfaces)
        if (ifd.name == iface_name) { iface = &ifd; break; }
    if (!iface) return nullptr;
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
    vtables_[vkey] = vgv;
    return vgv;
}

llvm::Value* CodeGen::buildInterfaceFat(llvm::Value* inst,
                                        const std::string& iface_name,
                                        const std::string& cls_name) {
    auto* vgv = getOrCreateVtable(iface_name, cls_name);
    if (!vgv) return inst;
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    auto* fat_ty = llvm::StructType::get(ctx_, {ptr_ty, ptr_ty});
    auto* fat = builder_.CreateAlloca(fat_ty);
    builder_.CreateStore(inst, builder_.CreateStructGEP(fat_ty, fat, 0));
    builder_.CreateStore(builder_.CreateBitCast(vgv, ptr_ty),
        builder_.CreateStructGEP(fat_ty, fat, 1));
    return builder_.CreateLoad(fat_ty, fat);
}

void CodeGen::upcastIfaceCallArgs(std::vector<llvm::Value*>& call_args,
                                  const CallExpr& e,
                                  const InterfaceMethodInfo* method) {
    if (!method || !method->action || !current_tu_) return;
    for (size_t ai = 0; ai < e.args.size() && ai < method->action->params.size(); ++ai) {
        const TypeNode& ptn = method->action->params[ai].type;
        bool is_iface = false;
        for (auto& ifd : current_tu_->interfaces)
            if (ifd.name == ptn.class_name) { is_iface = true; break; }
        if (!is_iface) continue;
        llvm::Value* a = call_args[1 + ai];   // [0] 是 this（接口 data）
        if (a && a->getType()->isPointerTy()) {
            std::string cls_name = resolveArgClassName(*e.args[ai]);
            if (!cls_name.empty()) {
                llvm::Value* fp = buildInterfaceFat(a, ptn.class_name, cls_name);
                if (fp && fp->getType()->isStructTy()) call_args[1 + ai] = fp;
            }
        }
    }
}

const CodeGen::InterfaceMethodInfo* CodeGen::findInterfaceMethod(
        const std::string& iface_name, const std::string& method) const {
    if (!iface_name.empty()) {
        auto interface_it = interface_methods_.find(iface_name);
        if (interface_it != interface_methods_.end()) {
            auto method_it = interface_it->second.find(method);
            if (method_it != interface_it->second.end())
                return &method_it->second;
            return nullptr;
        }
    }
    auto fallback_it = interface_method_fallback_.find(method);
    return fallback_it != interface_method_fallback_.end() ? &fallback_it->second : nullptr;
}

std::string CodeGen::resolveArgClassName(const Expr& arg) {
    if (arg.kind == ExprKind::NewExpr)
        return static_cast<const NewExpr&>(arg).class_name;
    // `this` evaluates to the current concrete instance. It can enter an
    // interface parameter context just like a local class variable.
    if (arg.kind == ExprKind::ThisExpr && !current_class_name_.empty())
        return current_class_name_;
    if (arg.kind == ExprKind::Identifier) {
        auto& id = static_cast<const IdentifierExpr&>(arg);
        auto vit = var_class_map_.find(id.name);
        if (vit != var_class_map_.end()) return vit->second;
        // 本类裸属性名（this.label）：按当前类属性类型解析具体类
        if (!current_class_name_.empty() && current_tu_) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name != current_class_name_) continue;
                for (auto& p : cls.properties) {
                    if (p.name == id.name && !p.type.class_name.empty()) {
                        std::string cn = p.type.class_name;
                        if (!p.type.type_args.empty()) {
                            std::string m = cn;
                            for (auto& ta : p.type.type_args)
                                m += "_" + mangleConcreteTypeNode(ta);
                            m += "_inst";
                            cn = m;
                        }
                        return cn;
                    }
                }
                break;
            }
        }
    }
    return "";
}

std::string CodeGen::paramIfaceName(llvm::Function* cf, size_t rel) {
    if (!current_tu_ || !cf) return "";
    std::string fn = cf->getName().str();
    // Event fire functions: fire_<Class>_<Event>(this, args...) — the param
    // types come from the class's `event:` declarations. Without this, calling
    // `ev(concreteInstance)` where the event takes an interface param passed
    // the raw object pointer where a {data, vtable} fat pointer was expected
    // → LLVM verify failure.
    if (fn.compare(0, 5, "fire_") == 0) {
        std::string rest = fn.substr(5);
        for (auto& cls : current_tu_->classes) {
            std::string prefix = cls.name + "_";
            if (rest.compare(0, prefix.size(), prefix) != 0) continue;
            std::string ename = rest.substr(prefix.size());
            for (auto& ev : cls.events) {
                if (ev.name == ename && rel < ev.params.size() &&
                    !ev.params[rel].type.class_name.empty())
                    return ev.params[rel].type.class_name;
            }
        }
        return "";
    }
    for (auto& cls : current_tu_->classes) {
        std::string prefix = cls.name + "_";
        if (fn.compare(0, prefix.size(), prefix) != 0) continue;
        std::string mname = fn.substr(prefix.size());
        auto scan = [&](const auto& actions) -> std::string {
            for (auto& a : actions) {
                if (a.name == mname && rel < a.params.size() &&
                    !a.params[rel].type.class_name.empty())
                    return a.params[rel].type.class_name;
            }
            return "";
        };
        std::string s = scan(cls.actions);
        if (!s.empty()) return s;
        s = scan(cls.functions);
        if (!s.empty()) return s;
        s = scan(cls.static_actions);
        if (!s.empty()) return s;
        // 构造器：函数名带参数类型后缀，如 `C_C_IWidget_IRenderer_int`。
        // mname = `C_<mangled-params>`；匹配任何构造器（has_constructor）并
        // 检查 rel 位置的形参是否为接口类型。
        if (mname.compare(0, cls.name.size() + 1, cls.name + "_") == 0) {
            for (auto& a : cls.actions) {
                if (!a.has_constructor) continue;
                if (rel < a.params.size() && !a.params[rel].type.class_name.empty())
                    return a.params[rel].type.class_name;
            }
        }
        break;
    }
    return "";
}

void CodeGen::createClassDefaultDecl(const ClassDecl& cls, const InterfaceDecl& iface,
                                     const ActionDecl& action) {
    auto fn = ifaceDefaultName(iface.name, action.name, cls.name);
    if (module_->getFunction(fn)) return;
    std::vector<llvm::Type*> pts = {llvm::PointerType::get(ctx_, 0)}; // this
    for (auto& p : action.params)
        pts.push_back(getLLVMType(typeNodeToCodegenType(p.type)));
    auto* ft = llvm::FunctionType::get(
        getLLVMType(typeNodeToCodegenType(action.return_type)), pts, false);
    llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn, module_.get());
    is_static_action_[fn] = false;
}

void CodeGen::generateClassDefaultAction(const ClassDecl& cls, const InterfaceDecl& iface,
                                         const ActionDecl& action) {
    std::string fn = ifaceDefaultName(iface.name, action.name, cls.name);
    auto* func = module_->getFunction(fn);
    if (!func) return;
    current_function_ = func;
    current_class_name_ = cls.name;
    current_is_coro_ = false;
    finally_ret_slot_ = nullptr;
    finally_ctx_stack_.clear();
    // 接口默认实现 stub 也必须设置返回类型：generateClassAction 等正常路径都
    // 设置了 current_ret_ti_，这里漏设会导致 return 语句用「上一函数残留的
    // 返回类型」判断是否需 ARC retain —— 例如 `int enabled() { return 1; }`
    // 若残留类型为 string/Interface 会错误生成 myp_retain(i32 1) → LLVM
    // verify 失败（2026-08-20 import 合并时类生成顺序不同暴露）。
    current_ret_ti_ = typeNodeToCodegenType(action.return_type);
    arc_skip_retain_return_ = false;
    arc_pending_temps_.clear();
    stack_array_sizes_.clear();
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);
    pushScope();
    if (debug_mode_) beginFunctionDebug(func, fn, action.range);

    auto* this_a = createEntryBlockAlloca(func, llvm::PointerType::get(ctx_, 0), "this");
    builder_.CreateStore(func->getArg(0), this_a);
    setNamedValue("this", this_a);

    for (size_t i = 0; i < action.params.size(); ++i) {
        TypeInfo pt = typeNodeToCodegenType(action.params[i].type);
        auto* a = createEntryBlockAlloca(func, getLLVMType(pt), action.params[i].name);
        builder_.CreateStore(func->getArg((unsigned)(i + 1)), a);
        setNamedValue(action.params[i].name, a);
        if (action.params[i].type.isArray() && action.params[i].type.element_type)
            array_elem_types_[action.params[i].name] =
                getLLVMType(typeNodeToCodegenType(*action.params[i].type.element_type));
    }

    if (action.body) {
        // 逃逸分析：设置当前 action 可栈上分配的局部变量集合。
        auto* bb = dynamic_cast<const BlockStmt*>(action.body.get());
        current_escape_stack_vars_ = bb ? analyzeEscapeStackVars(bb) : std::set<std::string>{};
        current_stack_promotion_bytes_ = 0;
        generateBlock(static_cast<const BlockStmt&>(*action.body));
    }
    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator()) {
        builder_.CreateRetVoid();
    }
    current_is_coro_ = false;
    popScope();
    if (debug_mode_) endFunctionDebug();
}

void CodeGen::createClassFunctionDecl(const ClassDecl& cls, const FuncDecl& fn) {
    auto name = fn.has_constructor
        ? constructorMangledName(cls.name, fn.name, fn.params)
        : cls.name + "_" + fn.name;
    if (module_->getFunction(name)) return;
    std::vector<llvm::Type*> pts = {llvm::PointerType::get(ctx_, 0)};
    for (auto& p : fn.params) {
        TypeInfo pt = typeNodeToCodegenType(p.type);
        pts.push_back(getLLVMType(pt));
    }
    auto* ft = llvm::FunctionType::get(getLLVMType(typeNodeToCodegenType(fn.return_type)), pts, false);
    llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, module_.get());
}

void CodeGen::generateClass(const ClassDecl& cls) {
    // Set generic type-param mapping: for a monomorphized instance (e.g.
    // ArrayList_int_inst), type_params_ = {"T"} and inst_type_args_ = {int}, so
    // `new T[n]` / `(T)x` inside the shared template body resolves T → int.
    // Template classes (no inst args) leave the map empty (T → i32 placeholder,
    // never called at runtime).
    current_type_params_.clear();
    for (size_t i = 0; i < cls.type_params.size() && i < cls.inst_type_args.size(); i++) {
        current_type_params_.emplace_back(cls.type_params[i], cls.inst_type_args[i]);
    }

    // Generate function: section bodies FIRST so actions can call them
    for (auto& fn : cls.functions) {
        generateClassFunction(cls, fn);
    }

    // Generate static action bodies (no 'this' pointer needed); generic static
    // method templates are skipped (their instances are in tu.functions).
    for (auto& a : cls.static_actions) {
        if (!a.type_params.empty()) continue;
        generateStaticAction(cls, a);
    }

    // trait 默认实现：类省略了带默认体的接口方法时，生成按类特化的默认函数
    //（this 绑定本类，默认体内 this.method() 静态解析到 <Class>_<method>）。
    if (!cls.interface_class_name.empty() && !cls.is_generic_inst) {
        const InterfaceDecl* iface = nullptr;
        if (current_tu_)
            for (auto& ifd : current_tu_->interfaces)
                if (ifd.name == cls.interface_class_name) { iface = &ifd; break; }
        if (iface) {
            for (auto& ia : iface->actions) {
                if (!ia.body) continue;
                bool has = false;
                for (auto& ca : cls.actions)
                    if (ca.name == ia.name && ca.return_type.basic_type == ia.return_type.basic_type) { has = true; break; }
                if (!has) generateClassDefaultAction(cls, *iface, ia);
            }
        }
    }

    // Generate all action bodies (including stdlib intrinsics with no body)
    for (auto& a : cls.actions) generateClassAction(cls, a);

    // Generate coroutine entry wrappers for @coro methods (after bodies exist)
    for (auto& a : cls.actions) {
        if (a.has_coro) {
            coro_methods_.insert(cls.name + "_" + a.name);
            coro_stack_map_[cls.name + "_" + a.name] = a.coro_stack_kb;
            generateCoroEntry(cls, a);
        }
    }

    // Generate event fire functions for each event using global event ID
    for (auto& ev : cls.events) {
        std::string ekey = cls.name + "::" + ev.name;
        int eid = event_id_map_[ekey];
        generateEventFire(cls, ev, eid);
    }

    // Lambda hidden class → uniform tramp for first-class function values.
    if (cls.name.rfind("__lambda_", 0) == 0)
        generateLambdaTramp(cls);
}

void CodeGen::generateLambdaTramp(const ClassDecl& cls) {
    if (module_->getFunction(cls.name + "_tramp")) return; // idempotent
    const ActionDecl* call = nullptr;
    for (auto& a : cls.actions)
        if (a.name == "__call") { call = &a; break; }
    if (!call) return;

    std::vector<llvm::Type*> pts;
    pts.push_back(llvm::PointerType::get(ctx_, 0)); // self (closure)
    for (auto& p : call->params)
        pts.push_back(getLLVMType(typeNodeToCodegenType(p.type)));
    TypeInfo rt = typeNodeToCodegenType(call->return_type);
    auto* ft = llvm::FunctionType::get(getLLVMType(rt), pts, false);
    auto* tramp = llvm::Function::Create(ft, llvm::Function::InternalLinkage,
        cls.name + "_tramp", module_.get());
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", tramp);
    builder_.SetInsertPoint(bb);

    auto* call_fn = module_->getFunction(cls.name + "___call");
    if (!call_fn) { builder_.CreateUnreachable(); return; }
    std::vector<llvm::Value*> args;
    auto* self = tramp->getArg(0);
    args.push_back(builder_.CreateBitCast(self, call_fn->getFunctionType()->getParamType(0)));
    for (size_t i = 1; i < tramp->arg_size(); i++)
        args.push_back(tramp->getArg(i));
    if (rt.kind == TypeKind::Void) {
        builder_.CreateCall(call_fn, args);
        builder_.CreateRetVoid();
    } else {
        auto* r = builder_.CreateCall(call_fn, args);
        builder_.CreateRet(r);
    }
}

void CodeGen::generateEventFire(const ClassDecl& cls, const EventDecl& ev, int event_id) {
    std::string fn = "fire_" + cls.name + "_" + ev.name;
    auto* func = module_->getFunction(fn);
    if (!func) {
        // Create if not already declared
        std::vector<llvm::Type*> pts;
        pts.push_back(llvm::PointerType::get(ctx_, 0));
        for (auto& p : ev.params)
            pts.push_back(getLLVMType(typeNodeToCodegenType(p.type)));
        auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), pts, false);
        func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn, module_.get());
    } else {
        func->deleteBody();
    }
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);

    if (!ev.params.empty()) {
        std::vector<llvm::Type*> data_types;
        for (size_t i = 0; i < ev.params.size(); i++)
            data_types.push_back(getLLVMType(typeNodeToCodegenType(ev.params[i].type)));
        auto* data_struct = llvm::StructType::create(ctx_, data_types, fn + "_data");
        auto* data_alloca = builder_.CreateAlloca(data_struct);
        for (size_t i = 0; i < ev.params.size(); i++) {
            auto* gep = builder_.CreateStructGEP(data_struct, data_alloca, i);
            builder_.CreateStore(func->getArg(i + 1), gep);
        }
        // data_size 第 4 参：跨线程路由时按字节数深拷贝载荷（BUG-005）
        auto* dsize = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_),
            (uint64_t)module_->getDataLayout().getTypeAllocSize(data_struct));
        builder_.CreateCall(runtime_event_fire_, {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), event_id),
            func->getArg(0),
            builder_.CreateBitCast(data_alloca, llvm::PointerType::get(ctx_, 0)),
            dsize
        });
    } else {
        builder_.CreateCall(runtime_event_fire_, {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), event_id),
            func->getArg(0),
            llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)),
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0)
        });
    }
    // Process same-thread events immediately (data is stack-allocated in this function)
    // Cross-thread events are handled by the target thread's event loop
    if (runtime_event_process_all_) {
        builder_.CreateCall(runtime_event_process_all_, {});
    }
    builder_.CreateRetVoid();
}

void CodeGen::generateClassAction(const ClassDecl& cls, const ActionDecl& action) {
    std::string fn = action.has_constructor
        ? constructorMangledName(cls.name, action.name, action.params)
        : cls.name + "_" + action.name;
    auto* func = module_->getFunction(fn);
    if (!func) return; // declaration not found (shouldn't happen)

    current_function_ = func;
    current_class_name_ = cls.name;
    current_is_coro_ = action.has_coro;
    finally_ret_slot_ = nullptr;
    finally_ctx_stack_.clear();
    current_ret_ti_ = typeNodeToCodegenType(action.return_type);
    arc_skip_retain_return_ = false;
    arc_pending_temps_.clear();
    // 固定数组栈变量表按函数隔离（否则不同函数同名变量互相污染）
    stack_array_sizes_.clear();
    // 签名声明（无实现）：只保留外部声明、不生成 body/stub，链接器从预编译库
    // （.so/.a，MYP_BRIDGES）解析——MYP 闭源分发（实现编译进 .so，分发签名 .myp）。
    if (!action.body) return;
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);
    pushScope();
    if (debug_mode_) beginFunctionDebug(func, fn, action.range);
    // M-FN-2 nonlocal: 本 action 被 lambda 按引用捕获的参数 → 序言提升为堆 cell。
    current_fn_nonlocal_vars_ = action.nonlocal_captures;
    current_fn_nonlocal_cell_class_ = action.nonlocal_cell_class;
    cell_owners_.clear();

    auto* this_a = createEntryBlockAlloca(func, llvm::PointerType::get(ctx_, 0), "this");
    builder_.CreateStore(func->getArg(0), this_a);
    setNamedValue("this", this_a);
    // BUG-030: 构造器入口把 this 注册进类的 mapping 全局（__myp_inst_<Class>），
    // 使构造器内触发的 mapping 事件能派发到当前实例。此前该全局在构造完成后才由
    // 调用方 generateVarDecl 写入 → 构造器内派发读空/旧指针 → 段错误。
    if (action.has_constructor) {
        auto git = class_instance_globals_.find(cls.name);
        if (git != class_instance_globals_.end())
            builder_.CreateStore(func->getArg(0), git->second);
    }
    // BUG-002: @coro 协程比调用方作用域长寿——`this`（类引用）被借用，调用方
    // 释放后协程仍持有悬垂指针。retain 并注册为 ARC 槽（协程完成时释放）。
    if (current_is_coro_) {
        TypeNode this_tn;
        this_tn.class_name = cls.name;
        TypeInfo this_ti(TypeKind::Class);
        this_ti.class_name = cls.name;
        registerCoroParam(this_tn, this_ti, this_a, func->getArg(0));
    }
    if (debug_mode_)
        emitParamDebug(this_a, "this", llvm::PointerType::get(ctx_, 0),
                       action.range.begin.line ? action.range.begin.line : 1, 0);

    for (size_t i = 0; i < action.params.size(); ++i) {
        TypeInfo pt = typeNodeToCodegenType(action.params[i].type);
        // nonlocal 参数：提升为堆 cell（共享可变），读写走 cell 属性 GEP。
        if (current_fn_nonlocal_vars_.count(action.params[i].name)) {
            promoteNonlocalToCell(action.params[i].name, getLLVMType(pt), func->getArg(i + 1));
            continue;
        }
        auto* a = createEntryBlockAlloca(func, getLLVMType(pt), action.params[i].name);
        builder_.CreateStore(func->getArg(i + 1), a);
        setNamedValue(action.params[i].name, a);
        // 类参数：注册 var_class_map_（含类型参数 T→具体类，经 current_type_params_
        // 解析）→ `c.method()` 精确解析到 c 的具体类，而非 best-class 误选同名方法。
        if (pt.kind == TypeKind::Class) {
            std::string cn = pt.class_name;
            if (!action.params[i].type.type_args.empty()) {
                cn = action.params[i].type.class_name;
                for (auto& ta : action.params[i].type.type_args)
                    cn += "_" + mangleConcreteTypeNode(ta);
                cn += "_inst";
            }
            var_class_map_[action.params[i].name] = cn;
        }
        if (debug_mode_)
            emitParamDebug(a, action.params[i].name, getLLVMType(pt),
                           action.range.begin.line ? action.range.begin.line : 1, (unsigned)(i + 1));
        // Record array element type for subscript access
        if (action.params[i].type.isArray() && action.params[i].type.element_type) {
            array_elem_types_[action.params[i].name] = getLLVMType(typeNodeToCodegenType(*action.params[i].type.element_type));
        }
        // Record slice element type for slice operations (must mirror
        // generateStaticAction / generateClassFunction — a slice parameter's
        // `a[i]`/`a.length` were otherwise compiled as a plain pointer GEP on
        // the {ptr,len} value → LLVM verify failure).
        if (pt.kind == TypeKind::Slice)
            var_slice_types_[action.params[i].name] = pt;
        // Struct param: record type name for struct-method dispatch.
        if (pt.kind == TypeKind::Struct)
            var_struct_map_[action.params[i].name] = pt.class_name;
        // BUG-002: retain ARC 参数——协程挂起期间调用方可能释放实参对象。
        registerCoroParam(action.params[i].type, pt, a, func->getArg(i + 1));
    }

    // @region: enter arena (skipped if return type is a reference — it escapes)
    bool fn_region = action.has_region &&
        !typeIsReference(typeNodeToCodegenType(action.return_type)) &&
        (!action.body || !regionBodyMayEscape(*action.body));
    if (fn_region) { in_region_function_ = true; emitRegionEnter(); }

    // Generate action body (stdlib actions use __myp_* intrinsics in their source code)
    // M-FN-2 nonlocal: lambda __call 开头注入 cell 属性别名（body 读写直达共享 cell）。
    if (action.body && cls.name.rfind("__lambda_", 0) == 0)
        setupNonlocalAliases(cls);
    // 逃逸分析：设置当前 action/构造器可栈上分配的局部变量集合。
    current_escape_stack_vars_.clear();
    current_stack_promotion_bytes_ = 0;
    if (action.body) {
        auto* ebb = dynamic_cast<const BlockStmt*>(action.body.get());
        if (ebb) current_escape_stack_vars_ = analyzeEscapeStackVars(ebb);
    }
    if (action.body)
        generateBlock(static_cast<const BlockStmt&>(*action.body));
    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator()) {
        if (scope_functions_.count(func))
            builder_.CreateCall(runtime_event_pop_scope_, {});
        if (fn_region) emitRegionExit();
        // Fall-off-the-end: return the declared type's zero default (e.g. empty
        // `int test() {}` stubs) instead of `ret void` (LLVM verify).
        auto* rty = func->getReturnType();
        if (rty->isVoidTy()) builder_.CreateRetVoid();
        else builder_.CreateRet(llvm::Constant::getNullValue(rty));
    }
    in_region_function_ = false;
    current_region_mark_ = nullptr;
    current_is_coro_ = false;
    popScope();
    current_fn_nonlocal_vars_.clear();
    current_fn_nonlocal_cell_class_.clear();
    if (debug_mode_) endFunctionDebug();
}

void CodeGen::generateCoroBuiltin(const ClassDecl& cls, const ActionDecl& action) {
    std::string fn = cls.name + "_" + action.name;
    auto* func = module_->getFunction(fn);
    if (!func) return;
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* v = llvm::Type::getVoidTy(ctx_);

    // Coro.waitAny(long[] ids, long count, long timeoutMs, long val) → long
    // ids is an unsized-array parameter (a bare pointer; MYP arrays carry no
    // length across calls), so the caller passes the element count explicitly.
    if (action.name == "waitAny") {
        auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
        auto wfn = module_->getOrInsertFunction("__myp_coro_wait_any",
            llvm::FunctionType::get(i64, {ptr_ty, i64, i64, i64}, false));
        auto* ids = func->getArg(0);
        auto* cnt = castToI64(func->getArg(1));
        auto* tms = castToI64(func->getArg(2));
        auto* aval = castToI64(func->getArg(3));
        auto* r = builder_.CreateCall(wfn, {ids, cnt, tms, aval});
        builder_.CreateRet(r);
        return;
    }

    // Coro.waitAnyOf(long[] spec, long count, long timeoutMs, long val) → long
    // (§五-5 P4): unified waitAny — spec is a flat long[] of count*3 entries
    // (kind/id/flag per spec, see __myp_coro_wait_any_of). Returns fired spec
    // index, -1 overall timeout, -2 not in a coroutine.
    if (action.name == "waitAnyOf") {
        auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
        auto wfn = module_->getOrInsertFunction("__myp_coro_wait_any_of",
            llvm::FunctionType::get(i64, {ptr_ty, i64, i64, i64}, false));
        auto* spec = func->getArg(0);
        auto* cnt = castToI64(func->getArg(1));
        auto* tms = castToI64(func->getArg(2));
        auto* aval = castToI64(func->getArg(3));
        auto* r = builder_.CreateCall(wfn, {spec, cnt, tms, aval});
        builder_.CreateRet(r);
        return;
    }

    const char* rt = nullptr;
    llvm::Type* ret = v;
    std::vector<llvm::Type*> pts;
    if (action.name == "scheduler") { rt = "__myp_coro_scheduler"; }
    else if (action.name == "sleep") { rt = "__myp_coro_sleep"; ret = i64; pts = {i64}; }
    else if (action.name == "resume")    { rt = "__myp_coro_resume"; ret = i64; pts = {i64, i64}; }
    else if (action.name == "yield")     { rt = "__myp_coro_yield"; ret = i64; pts = {i64}; }
    else if (action.name == "isActive")  { rt = "__myp_coro_is_active"; ret = i64; pts = {i64}; }
    else if (action.name == "current")   { rt = "__myp_coro_current_handle"; ret = i64; }
    else if (action.name == "count")    { rt = "__myp_coro_count"; ret = i64; }
    else if (action.name == "status")   { rt = "__myp_coro_status"; ret = i64; pts = {i64}; }
    else if (action.name == "destroy")   { rt = "__myp_coro_destroy"; pts = {i64}; }
    else if (action.name == "result")    { rt = "__myp_coro_result"; ret = i64; pts = {i64}; }
    else if (action.name == "waitEvent") { rt = "__myp_coro_wait_event"; ret = i64; pts = {i64, i64}; }
    else if (action.name == "waitEventTimeout") { rt = "__myp_coro_wait_event_timeout"; ret = i64; pts = {i64, i64, i64}; }
    else if (action.name == "waitFd")   { rt = "__myp_coro_wait_fd"; ret = i64; pts = {i64, i64, i64, i64}; }
    else if (action.name == "requestCancel") { rt = "__myp_coro_request_cancel"; pts = {i64}; }
    else if (action.name == "cancelRequested") { rt = "__myp_coro_cancel_requested"; ret = i64; }
    else if (action.name == "clearCancel") { rt = "__myp_coro_cancel_clear"; }
    else {
        builder_.CreateRetVoid();
        return;
    }
    auto rf = module_->getOrInsertFunction(rt, llvm::FunctionType::get(ret, pts, false));
    std::vector<llvm::Value*> args;
    for (size_t i = 0; i < pts.size(); ++i)
        args.push_back(func->getArg((unsigned)i));
    if (ret->isVoidTy()) {
        builder_.CreateCall(rf, args);
        builder_.CreateRetVoid();
    } else {
        auto* r = builder_.CreateCall(rf, args);
        builder_.CreateRet(r);
    }
}

void CodeGen::generateStaticAction(const ClassDecl& cls, const ActionDecl& action) {
    // Coro built-in static methods: body is empty in stdlib; codegen emits the
    // runtime call directly so __myp_coro_* stays hidden from user code.
    if (cls.name == "Coro") {
        generateCoroBuiltin(cls, action);
        return;
    }
    std::string fn = cls.name + "_" + action.name;
    auto* func = module_->getFunction(fn);
    if (!func) return;

    current_function_ = func;
    current_class_name_ = cls.name;
    current_is_coro_ = false;
    finally_ret_slot_ = nullptr;
    finally_ctx_stack_.clear();
    current_ret_ti_ = typeNodeToCodegenType(action.return_type);
    arc_skip_retain_return_ = false;
    arc_pending_temps_.clear();
    // 固定数组栈变量表按函数隔离
    stack_array_sizes_.clear();
    // 签名声明（无实现）：保持外部声明，链接预编译库（闭源分发，同 generateClassAction）
    if (!action.body) return;
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);
    pushScope();
    if (debug_mode_) beginFunctionDebug(func, fn, action.range);
    // M-FN-2 nonlocal: 本 static action 被 lambda 按引用捕获的参数 → 序言提升为 cell。
    current_fn_nonlocal_vars_ = action.nonlocal_captures;
    current_fn_nonlocal_cell_class_ = action.nonlocal_cell_class;
    cell_owners_.clear();

    for (size_t i = 0; i < action.params.size(); ++i) {
        TypeInfo pt = typeNodeToCodegenType(action.params[i].type);
        // nonlocal 参数：提升为堆 cell（共享可变）。
        if (current_fn_nonlocal_vars_.count(action.params[i].name)) {
            promoteNonlocalToCell(action.params[i].name, getLLVMType(pt), func->getArg(i));
            continue;
        }
        auto* a = createEntryBlockAlloca(func, getLLVMType(pt), action.params[i].name);
        builder_.CreateStore(func->getArg(i), a);
        setNamedValue(action.params[i].name, a);
        if (debug_mode_)
            emitParamDebug(a, action.params[i].name, getLLVMType(pt),
                           action.range.begin.line ? action.range.begin.line : 1, (unsigned)i);
        // Record array element type for subscript access
        if (action.params[i].type.isArray() && action.params[i].type.element_type) {
            array_elem_types_[action.params[i].name] = getLLVMType(typeNodeToCodegenType(*action.params[i].type.element_type));
        }
        // Record slice element type for slice operations
        if (pt.kind == TypeKind::Slice)
            var_slice_types_[action.params[i].name] = pt;
        // Struct param: record type name for struct-method dispatch
        // (mirrors generateClassAction / generateClassFunction).
        if (pt.kind == TypeKind::Struct)
            var_struct_map_[action.params[i].name] = pt.class_name;
        // 类参数：注册 var_class_map_（含类型参数 T→具体类）→ 静态方法内
        // `list.method()`（如 ArrayList<T> 参数 .add/.get）精确解析到具体实例
        // 类，而非 best-class 误选模板签名（此前漏注册 → LLVM verify 失败）。
        if (pt.kind == TypeKind::Class) {
            std::string cn = pt.class_name;
            if (!action.params[i].type.type_args.empty()) {
                cn = action.params[i].type.class_name;
                for (auto& ta : action.params[i].type.type_args)
                    cn += "_" + mangleConcreteTypeNode(ta);
                cn += "_inst";
            }
            var_class_map_[action.params[i].name] = cn;
        }
    }

    // @region (static action)
    bool fn_region = action.has_region &&
        !typeIsReference(typeNodeToCodegenType(action.return_type)) &&
        (!action.body || !regionBodyMayEscape(*action.body));
    if (fn_region) { in_region_function_ = true; emitRegionEnter(); }

    if (action.body)
        generateBlock(static_cast<const BlockStmt&>(*action.body));
    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator()) {
        if (scope_functions_.count(func))
            builder_.CreateCall(runtime_event_pop_scope_, {});
        if (fn_region) emitRegionExit();
        // Fall-off-the-end: return the declared type's zero default (e.g. empty
        // `@async long sleep() {}` stubs) instead of `ret void` (LLVM verify).
        auto* rty = func->getReturnType();
        if (rty->isVoidTy()) builder_.CreateRetVoid();
        else builder_.CreateRet(llvm::Constant::getNullValue(rty));
    }
    in_region_function_ = false;
    current_region_mark_ = nullptr;
    popScope();
    current_fn_nonlocal_vars_.clear();
    current_fn_nonlocal_cell_class_.clear();
    if (debug_mode_) endFunctionDebug();
}

void CodeGen::generateCoroEntry(const ClassDecl& cls, const ActionDecl& action) {
    std::string method_fn = cls.name + "_" + action.name;
    auto* target = module_->getFunction(method_fn);
    if (!target) return;

    std::string entry_fn = "__myp_coro_entry_" + cls.name + "_" + action.name;
    if (module_->getFunction(entry_fn)) return;  // already created (pre-scan)
    auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {}, false);
    auto* func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, entry_fn, module_.get());
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);

    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto get_arg = module_->getOrInsertFunction("__myp_coro_get_entry_arg",
        llvm::FunctionType::get(i64, {i64}, false));
    auto* slot0 = builder_.CreateCall(get_arg, {llvm::ConstantInt::get(i64, 0)}, "this_raw");
    auto* this_ptr = builder_.CreateIntToPtr(slot0, llvm::PointerType::get(ctx_, 0), "this");

    std::vector<llvm::Value*> call_args;
    call_args.push_back(this_ptr);
    for (size_t i = 0; i < action.params.size(); ++i) {
        TypeInfo pt = typeNodeToCodegenType(action.params[i].type);
        auto* slot = builder_.CreateCall(get_arg,
            {llvm::ConstantInt::get(i64, (int64_t)(i + 1))}, "arg_raw");
        auto* want = getLLVMType(pt);
        llvm::Value* v = slot;
        if (want->isIntegerTy()) {
            v = builder_.CreateIntCast(slot, want, true);
        } else if (want->isFloatingPointTy()) {
            v = builder_.CreateBitCast(slot, want);
        } else if (want->isPointerTy()) {
            v = builder_.CreateIntToPtr(slot, want);
        }
        call_args.push_back(v);
    }
    builder_.CreateCall(target, call_args);
    builder_.CreateRetVoid();
}

void CodeGen::generateCoroFuncEntry(const FuncDecl& decl) {
    std::string entry_fn = "__myp_coro_entry_" + decl.name;
    auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {}, false);
    auto* func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, entry_fn, module_.get());
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);

    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto get_arg = module_->getOrInsertFunction("__myp_coro_get_entry_arg",
        llvm::FunctionType::get(i64, {i64}, false));

    // Resolve (or declare) the actual top-level function — body filled later.
    std::vector<llvm::Type*> pts;
    for (auto& p : decl.params)
        pts.push_back(getLLVMType(typeNodeToCodegenType(p.type)));
    auto* rty = getLLVMType(typeNodeToCodegenType(decl.return_type));
    auto* ft2 = llvm::FunctionType::get(rty, pts, false);
    auto* target = llvm::dyn_cast<llvm::Function>(
        module_->getOrInsertFunction(decl.name, ft2).getCallee());

    std::vector<llvm::Value*> call_args;
    for (size_t i = 0; i < decl.params.size(); ++i) {
        TypeInfo pt = typeNodeToCodegenType(decl.params[i].type);
        auto* slot = builder_.CreateCall(get_arg,
            {llvm::ConstantInt::get(i64, (int64_t)(i + 1))}, "arg_raw");
        auto* want = getLLVMType(pt);
        llvm::Value* v = slot;
        if (want->isIntegerTy()) v = builder_.CreateIntCast(slot, want, true);
        else if (want->isFloatingPointTy()) v = builder_.CreateBitCast(slot, want);
        else if (want->isPointerTy()) v = builder_.CreateIntToPtr(slot, want);
        call_args.push_back(v);
    }
    builder_.CreateCall(target, call_args);
    builder_.CreateRetVoid();
}

llvm::Value* CodeGen::generateCoroSpawn(llvm::Function* target, const CallExpr& e,
                                        llvm::Value* mthis, bool is_method) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);

    // Stack size: @coro(stack=N) KB, default 128KB (0 → runtime default).
    int64_t stack_bytes = 128 * 1024;
    auto sit = coro_stack_map_.find(target->getName().str());
    if (sit != coro_stack_map_.end() && sit->second > 0) {
        stack_bytes = (int64_t)sit->second * 1024;
        // Guard against a too-small stack (silent corruption on overflow).
        if (sit->second < 16) {
            diag_.warn(e.range, "@coro(stack=" + std::to_string(sit->second) +
                "): very small coroutine stack — risk of stack overflow " +
                "(recommend >= 16KB, or raise with @coro(stack=N))");
        }
    }

    auto idx = [&](uint64_t v) { return llvm::ConstantInt::get(i64, v); };
    std::string entry_name = "__myp_coro_entry_" + target->getName().str();
    llvm::Value* entry_i = idx(0);
    if (auto* entry = module_->getFunction(entry_name))
        entry_i = builder_.CreatePtrToInt(entry, i64);
    auto create_fn = module_->getOrInsertFunction("__myp_coro_create_entry",
        llvm::FunctionType::get(i64, {i64, i64}, false));
    auto* handle = builder_.CreateCall(create_fn,
        {llvm::ConstantInt::get(i64, stack_bytes), entry_i}, "coro_handle");

    size_t arg_count = e.args.size() + 1;
    auto* pack_ty = llvm::ArrayType::get(i64, arg_count);
    auto* pack = createEntryBlockAlloca(current_function_, pack_ty, "coro_args");
    auto store_arg = [&](size_t pos, llvm::Value* value) {
        auto* ptr = builder_.CreateInBoundsGEP(pack_ty, pack,
            {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0),
             llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), pos)});
        builder_.CreateStore(value, ptr);
    };

    // Slot 0: 'this'
    llvm::Value* this_i = idx(0);
    if (is_method && mthis)
        this_i = builder_.CreatePtrToInt(mthis, i64);
    store_arg(0, this_i);

    // Slots 1..N: explicit arguments
    for (size_t i = 0; i < e.args.size(); ++i) {
        auto* arg = generateExpr(*e.args[i]);
        llvm::Value* slot = arg;
        if (arg->getType()->isPointerTy())
            slot = builder_.CreatePtrToInt(arg, i64);
        else if (arg->getType()->isIntegerTy())
            slot = builder_.CreateIntCast(arg, i64, true);
        else if (arg->getType()->isFloatingPointTy())
            slot = builder_.CreateBitCast(arg, i64);
        store_arg(i + 1, slot);
    }

    auto start_fn = module_->getOrInsertFunction("__myp_coro_start_args",
        llvm::FunctionType::get(i64, {i64, i64, i64}, false));
    builder_.CreateCall(start_fn,
        {handle, idx(arg_count), builder_.CreatePtrToInt(pack, i64)});

    return handle;
}

void CodeGen::declareStructMethods(const StructDecl& st) {
    std::string type_key = st.parent_class.empty()
        ? st.name : st.parent_class + "::" + st.name;
    auto* st_type = getStructType(type_key);
    if (!st_type) return;

    for (auto& method : st.functions) {
        if (!method.body) continue;
        // 构造器用重载 mangling（struct_<key>_<name>_<paramtypes>）
        std::string fn = method.has_constructor
            ? "struct_" + constructorMangledName(type_key, method.name, method.params)
            : "struct_" + type_key + "_" + method.name;
        if (module_->getFunction(fn)) continue;

        std::vector<llvm::Type*> pts;
        pts.push_back(llvm::PointerType::get(ctx_, 0)); // struct ptr
        for (auto& p : method.params)
            pts.push_back(typeNodeToLLVMType(p.type));

        auto* ret_ty = typeNodeToLLVMType(method.return_type);
        auto* ft = llvm::FunctionType::get(ret_ty, pts, false);
        llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn, module_.get());
    }
}

void CodeGen::generateStructMethods(const StructDecl& st) {
    std::string type_key = st.parent_class.empty()
        ? st.name : st.parent_class + "::" + st.name;
    auto* st_type = getStructType(type_key);
    if (!st_type) return;

    for (auto& method : st.functions) {
        if (!method.body) continue;
        // 构造器用重载 mangling（struct_<key>_<name>_<paramtypes>）
        std::string fn = method.has_constructor
            ? "struct_" + constructorMangledName(type_key, method.name, method.params)
            : "struct_" + type_key + "_" + method.name;
        auto* func = module_->getFunction(fn);
        if (func) {
            func->deleteBody();
        } else {
            std::vector<llvm::Type*> pts;
            pts.push_back(llvm::PointerType::get(ctx_, 0));
            for (auto& p : method.params) {
                pts.push_back(typeNodeToLLVMType(p.type));
            }
            auto* ret_ty = typeNodeToLLVMType(method.return_type);
            auto* ft = llvm::FunctionType::get(ret_ty, pts, false);
            func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn, module_.get());
        }

        current_function_ = func;
        current_class_name_ = type_key; // for error messages
        current_ret_ti_ = typeNodeToCodegenType(method.return_type);
        arc_skip_retain_return_ = false;
        arc_pending_temps_.clear();
        auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
        builder_.SetInsertPoint(bb);
        pushScope();

        // 'this' pointer for struct = the struct pointer (alloca address)
        auto* this_a = createEntryBlockAlloca(func, llvm::PointerType::get(ctx_, 0), "this");
        builder_.CreateStore(func->getArg(0), this_a);
        setNamedValue("this", this_a);

        // Register struct fields as named GEP values so bare field names resolve
        for (auto& prop : st.properties) {
            unsigned fi = 0;
            if (getStructFieldIndex(type_key, prop.name, fi)) {
                auto* gep = builder_.CreateStructGEP(st_type, func->getArg(0), fi);
                auto* ft5 = st_type->getElementType(fi);
                setNamedTypedValue(prop.name, gep, ft5);
                struct_field_types_[prop.name] = prop.type;
                // For array fields, record the element type for subscript access
                if (prop.type.isArray() && prop.type.element_type) {
                    array_elem_types_[prop.name] = typeNodeToLLVMType(*prop.type.element_type);
                }
            }
        }

        // Method parameters
        for (size_t i = 0; i < method.params.size(); ++i) {
            auto& p = method.params[i];
            auto* p_ty = typeNodeToLLVMType(p.type);
            auto* a = createEntryBlockAlloca(func, p_ty, p.name);
            builder_.CreateStore(func->getArg(i + 1), a);
            setNamedValue(p.name, a);
            if (p.type.isArray() && p.type.element_type) {
                array_elem_types_[p.name] = typeNodeToLLVMType(*p.type.element_type);
            }
            TypeInfo pti = typeNodeToCodegenType(p.type);
            if (pti.kind == TypeKind::Slice)
                var_slice_types_[p.name] = pti;
        }

        if (method.body)
            generateBlock(static_cast<const BlockStmt&>(*method.body));
        if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator()) {
            auto* rty = typeNodeToLLVMType(method.return_type);
            if (rty->isVoidTy())
                builder_.CreateRetVoid();
            else if (rty->isIntegerTy())
                builder_.CreateRet(llvm::ConstantInt::get(rty, 0));
            else if (rty->isFloatingPointTy())
                builder_.CreateRet(llvm::Constant::getNullValue(rty));
            else
                builder_.CreateRet(llvm::Constant::getNullValue(rty));
        }
        popScope();
    }
}

void CodeGen::generateClassFunction(const ClassDecl& cls, const FuncDecl& fn_decl) {
    std::string fn = fn_decl.has_constructor
        ? constructorMangledName(cls.name, fn_decl.name, fn_decl.params)
        : cls.name + "_" + fn_decl.name;
    auto* existing = module_->getFunction(fn);
    // 签名声明（无实现）：保持 ExternalLinkage 纯声明，链接器从预编译库解析
    // （闭源分发）。声明由 createClassFunctionDecl 预创建；无则补建外部声明。
    if (!fn_decl.body) {
        if (!existing) {
            std::vector<llvm::Type*> pts0 = {llvm::PointerType::get(ctx_, 0)};
            for (auto& p : fn_decl.params)
                pts0.push_back(getLLVMType(typeNodeToCodegenType(p.type)));
            auto* ft0 = llvm::FunctionType::get(
                getLLVMType(typeNodeToCodegenType(fn_decl.return_type)), pts0, false);
            llvm::Function::Create(ft0, llvm::Function::ExternalLinkage, fn, module_.get());
        }
        return;
    }
    if (existing) existing->deleteBody();

    std::vector<llvm::Type*> pts = {llvm::PointerType::get(ctx_, 0)};
    for (auto& p : fn_decl.params) {
        pts.push_back(getLLVMType(typeNodeToCodegenType(p.type)));
    }

    TypeInfo rt = typeNodeToCodegenType(fn_decl.return_type);
    auto* ft = llvm::FunctionType::get(getLLVMType(rt), pts, false);
    // Reuse the pre-created declaration (createClassFunctionDecl) so cross-calls
    // in the section resolve; otherwise fall back to creating it here.
    llvm::Function* func = existing;
    if (!func) {
        func = llvm::Function::Create(ft, llvm::Function::InternalLinkage, fn, module_.get());
    } else {
        func->setLinkage(llvm::Function::InternalLinkage);
    }

    current_function_ = func;
    current_class_name_ = cls.name;
    current_ret_ti_ = rt;
    arc_skip_retain_return_ = false;
    arc_pending_temps_.clear();
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);
    pushScope();
    if (debug_mode_) beginFunctionDebug(func, fn, fn_decl.range);
    // M-FN-2 nonlocal: 本函数被 lambda 按引用捕获的参数 → 序言提升为堆 cell。
    current_fn_nonlocal_vars_ = fn_decl.nonlocal_captures;
    current_fn_nonlocal_cell_class_ = fn_decl.nonlocal_cell_class;
    cell_owners_.clear();

    auto* this_a = createEntryBlockAlloca(func, llvm::PointerType::get(ctx_, 0), "this");
    builder_.CreateStore(func->getArg(0), this_a);
    setNamedValue("this", this_a);
    if (debug_mode_)
        emitParamDebug(this_a, "this", llvm::PointerType::get(ctx_, 0),
                       fn_decl.range.begin.line ? fn_decl.range.begin.line : 1, 0);

    for (size_t i = 0; i < fn_decl.params.size(); ++i) {
        TypeInfo pt = typeNodeToCodegenType(fn_decl.params[i].type);
        // nonlocal 参数：提升为堆 cell（共享可变），读写走 cell 属性 GEP。
        if (current_fn_nonlocal_vars_.count(fn_decl.params[i].name)) {
            promoteNonlocalToCell(fn_decl.params[i].name, getLLVMType(pt), func->getArg(i + 1));
            continue;
        }
        auto* a = createEntryBlockAlloca(func, getLLVMType(pt), fn_decl.params[i].name);
        builder_.CreateStore(func->getArg(i + 1), a);
        setNamedValue(fn_decl.params[i].name, a);
        if (debug_mode_)
            emitParamDebug(a, fn_decl.params[i].name, getLLVMType(pt),
                           fn_decl.range.begin.line ? fn_decl.range.begin.line : 1, (unsigned)(i + 1));
        // Record slice element type for slice operations
        if (pt.kind == TypeKind::Slice)
            var_slice_types_[fn_decl.params[i].name] = pt;
    }

    // @region (class function)
    bool fn_region = fn_decl.has_region &&
        !typeIsReference(typeNodeToCodegenType(fn_decl.return_type)) &&
        (!fn_decl.body || !regionBodyMayEscape(*fn_decl.body));
    if (fn_region) { in_region_function_ = true; emitRegionEnter(); }

    if (fn_decl.body)
        generateBlock(static_cast<const BlockStmt&>(*fn_decl.body));
    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator()) {
        if (fn_region) emitRegionExit();
        // Fall-off-the-end: return the declared type's zero default (e.g. empty
        // `int helper() {}` FFI stubs) instead of `ret void` (LLVM verify).
        auto* rty = getLLVMType(rt);
        if (rt.kind == TypeKind::Void) builder_.CreateRetVoid();
        else if (rty->isIntegerTy()) builder_.CreateRet(llvm::ConstantInt::get(rty, 0));
        else builder_.CreateRet(llvm::Constant::getNullValue(rty));
    }
    in_region_function_ = false;
    current_region_mark_ = nullptr;
    popScope();
    current_fn_nonlocal_vars_.clear();
    current_fn_nonlocal_cell_class_.clear();
    if (debug_mode_) endFunctionDebug();
}

void CodeGen::declareFuncSignature(const FuncDecl& decl) {
    if (decl.has_proc_macro) return;
    std::vector<llvm::Type*> pts;
    for (auto& p : decl.params) {
        TypeInfo pt = typeNodeToCodegenType(p.type);
        pts.push_back(getLLVMType(pt));
    }
    TypeInfo rt = typeNodeToCodegenType(decl.return_type);
    auto* ft = llvm::FunctionType::get(getLLVMType(rt), pts, false);
    module_->getOrInsertFunction(decl.name, ft);
}

void CodeGen::generateFuncDecl(const FuncDecl& decl) {
    // @macro (M4 proc-macro): compile-time only — never emitted as runtime code.
    if (decl.has_proc_macro) return;
    std::vector<llvm::Type*> pts;
    for (auto& p : decl.params) {
        TypeInfo pt = typeNodeToCodegenType(p.type);
        pts.push_back(getLLVMType(pt));
    }

    TypeInfo rt = typeNodeToCodegenType(decl.return_type);
    auto* ft = llvm::FunctionType::get(getLLVMType(rt), pts, false);
    // Reuse if a declaration was already inserted (top-level @coro entry
    // wrapper pre-creates it via getOrInsertFunction); otherwise create fresh.
    auto* func = llvm::dyn_cast<llvm::Function>(
        module_->getOrInsertFunction(decl.name, ft).getCallee());
    if (!func) {
        func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                      decl.name, module_.get());
    }
    if (func->size() != 0) {
        // Already has a body (e.g. duplicate declaration) — skip.
        current_function_ = nullptr;
        return;
    }

    current_is_coro_ = decl.has_coro;

    size_t i = 0;
    for (auto& arg : func->args()) { if (i < decl.params.size()) arg.setName(decl.params[i].name); ++i; }

    current_function_ = func;
    finally_ret_slot_ = nullptr;
    finally_ctx_stack_.clear();
    current_ret_ti_ = rt;
    arc_skip_retain_return_ = false;
    arc_pending_temps_.clear();
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);
    pushScope();
    if (debug_mode_) beginFunctionDebug(func, decl.name, decl.range);
    // M-FN-2 nonlocal: 本函数被 lambda 按引用捕获的参数/局部 → 序言提升为堆 cell。
    current_fn_nonlocal_vars_ = decl.nonlocal_captures;
    current_fn_nonlocal_cell_class_ = decl.nonlocal_cell_class;
    cell_owners_.clear();

    i = 0;
    for (auto& arg : func->args()) {
        if (i < decl.params.size()) {
            TypeInfo pt = typeNodeToCodegenType(decl.params[i].type);
            // nonlocal 参数：提升为堆 cell（共享可变），读写走 cell 属性 GEP。
            if (current_fn_nonlocal_vars_.count(decl.params[i].name)) {
                promoteNonlocalToCell(decl.params[i].name, getLLVMType(pt), &arg);
                ++i;
                continue;
            }
            auto* a = createEntryBlockAlloca(func, getLLVMType(pt), decl.params[i].name);
            builder_.CreateStore(&arg, a);
            if (pt.kind == TypeKind::Slice)
                var_slice_types_[decl.params[i].name] = pt;
            if (pt.kind == TypeKind::Function)
                func_val_types_[decl.params[i].name] = pt;
            if (pt.kind == TypeKind::Struct)
                var_struct_map_[decl.params[i].name] = pt.class_name;
            setNamedValue(decl.params[i].name, a);
            // 类参数：注册 var_class_map_（含类型参数 T→具体类型）→ 泛型函数实例内
            // `opt.method()` 精确解析到具体实例类，而非 best-class 误选模板名。
            if (pt.kind == TypeKind::Class) {
                std::string cn = pt.class_name;
                if (!decl.params[i].type.type_args.empty()) {
                    cn = decl.params[i].type.class_name;
                    for (auto& ta : decl.params[i].type.type_args)
                        cn += "_" + mangleConcreteTypeNode(ta);
                    cn += "_inst";
                }
                var_class_map_[decl.params[i].name] = cn;
            }
            if (debug_mode_)
                emitParamDebug(a, decl.params[i].name, getLLVMType(pt),
                               decl.range.begin.line ? decl.range.begin.line : 1, (unsigned)i);
            // Record array element type for subscript access
            if (decl.params[i].type.isArray() && decl.params[i].type.element_type) {
                array_elem_types_[decl.params[i].name] = getLLVMType(typeNodeToCodegenType(*decl.params[i].type.element_type));
            }
            // BUG-002: retain ARC 参数——协程挂起期间调用方可能释放实参对象。
            registerCoroParam(decl.params[i].type, pt, a, &arg);
        }
        ++i;
    }

    // For main: call init mappings and track for cleanup
    if (decl.name == "main") {
        // In test mode, skip user's main - test runner main will be generated.
        // BUG-026: the user's `int main()` placeholder was left in the module
        // with an empty (terminator-less) entry block — LLVM verify failed with
        // "Basic Block in function 'main' does not have terminator", and had it
        // survived, generateTestRunner's `main` would be auto-renamed to
        // "main.1" and the runner would silently never run. Erase the empty
        // placeholder so the test-runner `main` keeps its name and is the real
        // entry point.
        if (test_mode_) {
            popScope();
            current_function_ = nullptr;
            current_fn_nonlocal_vars_.clear();
            current_fn_nonlocal_cell_class_.clear();
            if (func) func->eraseFromParent();
            return;
        }
        in_main_ = true;
        if (init_func_) {
            builder_.CreateCall(init_func_, {});
        }
    }

    // @region (top-level function)
    bool fn_region = decl.has_region &&
        !typeIsReference(rt) &&
        (!decl.body || !regionBodyMayEscape(*decl.body));
    if (fn_region) { in_region_function_ = true; emitRegionEnter(); }

    // 逃逸分析：设置当前函数可栈上分配的局部变量集合（generateVarDecl 使用）。
    current_escape_stack_vars_.clear();
    current_stack_promotion_bytes_ = 0;
    if (decl.body) {
        auto* bb = dynamic_cast<const BlockStmt*>(decl.body.get());
        if (bb) current_escape_stack_vars_ = analyzeEscapeStackVars(bb);
    }

    if (decl.body) generateBlock(*decl.body);

    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator()) {
        if (scope_functions_.count(func))
            builder_.CreateCall(runtime_event_pop_scope_, {});
        if (fn_region) emitRegionExit();
        auto* rty = getLLVMType(rt);
        if (rt.kind == TypeKind::Void) builder_.CreateRetVoid();
        else if (rty->isIntegerTy()) builder_.CreateRet(llvm::ConstantInt::get(rty, 0));
        else builder_.CreateRet(llvm::Constant::getNullValue(rty));
    }
    in_region_function_ = false;
    current_region_mark_ = nullptr;
    current_is_coro_ = false;
    popScope();
    current_fn_nonlocal_vars_.clear();
    current_fn_nonlocal_cell_class_.clear();
    if (decl.name == "main") {
        in_main_ = false;
    }
    if (debug_mode_) endFunctionDebug();
}

void CodeGen::generateMappingDecl(const MappingDecl& decl, llvm::BasicBlock* insert_bb) {
    if (!runtime_event_register_) return;

    // If @scope: push a scope marker BEFORE registrations so that pop_scope
    // restores to this count, effectively unregistering all handlers below.
    if (decl.has_scope && runtime_event_push_scope_) {
        builder_.CreateCall(runtime_event_push_scope_, {});
        if (current_function_)
            scope_functions_.insert(current_function_);
    }

    for (auto& chain : decl.chains) {
        if (chain.nodes.size() < 2) continue;
        auto& src = chain.nodes[0];

        // Find source class and event to determine data struct layout
        // First try to resolve source_name as a variable → class name
        const ClassDecl* src_cls = nullptr;
        const EventDecl* src_ev = nullptr;
        int event_id = 0;
        std::string src_class = src.source_name;
        if (current_tu_) {
            auto vcit = var_class_map_.find(src.source_name);
            if (vcit != var_class_map_.end())
                src_class = vcit->second;
            for (auto& cls : current_tu_->classes) {
                if (cls.name != src_class) continue;
                for (auto& ev : cls.events) {
                    if (ev.name == src.member_name) {
                        src_cls = &cls; src_ev = &ev;
                        auto ekey = cls.name + "::" + ev.name;
                        auto eit = event_id_map_.find(ekey);
                        if (eit != event_id_map_.end()) event_id = eit->second;
                        goto ev_found;
                    }
                }
            }
            // Fallback: search all classes by event name
            if (!src_cls) {
                for (auto& cls : current_tu_->classes) {
                    for (auto& ev : cls.events) {
                        if (ev.name == src.member_name) {
                            src_cls = &cls; src_ev = &ev;
                            auto ekey = cls.name + "::" + ev.name;
                            auto eit = event_id_map_.find(ekey);
                            if (eit != event_id_map_.end()) event_id = eit->second;
                            goto ev_found;
                        }
                    }
                }
            }
        }
        ev_found:

        // Create the dispatch handler: void handler(void* instance, void* data)
        std::string hname = "handler_" + src.source_name + "_" + src.member_name;
        auto* handler_ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
            {llvm::PointerType::get(ctx_, 0), llvm::PointerType::get(ctx_, 0)}, false);
        auto* handler = llvm::Function::Create(handler_ft,
            llvm::Function::InternalLinkage, hname, module_.get());

        auto* saved_bb = builder_.GetInsertBlock();
        auto* hbb = llvm::BasicBlock::Create(ctx_, "entry", handler);
        builder_.SetInsertPoint(hbb);

        // Store the previous call result for chain forwarding
        llvm::Value* prev_result = handler->getArg(1); // event data as default

        // ---- Where clause: evaluate condition, skip if false ----
        if (chain.where_expr && src_ev && !src_ev->params.empty()) {
            pushScope();
            // Unpack event data into named values for the where expression
            std::vector<llvm::Type*> ev_types;
            for (auto& p : src_ev->params)
                ev_types.push_back(getLLVMType(typeNodeToCodegenType(p.type)));
            auto* ev_struct = llvm::StructType::create(ctx_, ev_types, "evdata_w");
            auto* ev_ptr = builder_.CreateBitCast(handler->getArg(1),
                llvm::PointerType::get(ctx_, 0));
            for (size_t pi = 0; pi < src_ev->params.size(); pi++) {
                auto* gep = builder_.CreateStructGEP(ev_struct, ev_ptr, pi);
                auto* loaded = builder_.CreateLoad(ev_types[pi], gep);
                auto* a = createEntryBlockAlloca(handler, ev_types[pi], src_ev->params[pi].name);
                builder_.CreateStore(loaded, a);
                setNamedValue(src_ev->params[pi].name, a);
            }
            // Evaluate the where expression
            auto* cond_val = generateExpr(*chain.where_expr);
            popScope();
            if (cond_val) {
                auto* cond_i1 = cond_val->getType()->isIntegerTy(1)
                    ? cond_val : builder_.CreateICmpNE(cond_val,
                        llvm::ConstantInt::get(cond_val->getType(), 0));
                auto* cont_bb = llvm::BasicBlock::Create(ctx_, "w_cont", handler);
                auto* skip_bb = llvm::BasicBlock::Create(ctx_, "w_skip", handler);
                builder_.CreateCondBr(cond_i1, cont_bb, skip_bb);
                builder_.SetInsertPoint(skip_bb);
                builder_.CreateRetVoid();
                builder_.SetInsertPoint(cont_bb);
            }
        }

        for (size_t i = 1; i < chain.nodes.size(); ++i) {
            auto& tgt = chain.nodes[i];
            llvm::Function* callee = nullptr;
            bool routed_checked = false;   // BUG-005: 首个非静态目标已做线程路由检查

            // --- Lambda node: call the hidden class __call method ---
            if (tgt.is_lambda && tgt.lambda) {
                std::string cls_name = tgt.lambda->hidden_class_name;
                std::string fn_name = cls_name + "___call";
                callee = module_->getFunction(fn_name);
                if (callee) {
                    std::vector<llvm::Value*> call_args;

                    // Load lambda instance from global
                    std::string lg = "__myp_lambda_" + cls_name;
                    auto* lgv = module_->getGlobalVariable(lg);
                    if (!lgv) {
                        lgv = new llvm::GlobalVariable(*module_,
                            llvm::PointerType::get(ctx_, 0), false,
                            llvm::GlobalValue::InternalLinkage,
                            llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)), lg);
                    }
                    auto* li = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), lgv, "li");
                    call_args.push_back(li);

                    // Forward input (prev_result or event data)
                    auto* ft = callee->getFunctionType();
                    if (ft->getNumParams() > (unsigned)call_args.size()) {
                        if (i == 1 && src_ev && src_ev->params.size() > 0) {
                            // First target: unpack event data
                            std::vector<llvm::Type*> ev_param_types;
                            for (auto& p : src_ev->params)
                                ev_param_types.push_back(getLLVMType(typeNodeToCodegenType(p.type)));
                            auto* ev_struct = llvm::StructType::create(ctx_, ev_param_types, "evdata");
                            auto* ev_ptr = builder_.CreateBitCast(handler->getArg(1),
                                llvm::PointerType::get(ctx_, 0));
                            for (size_t pi = 0; pi < src_ev->params.size() && call_args.size() < ft->getNumParams(); pi++) {
                                auto* gep = builder_.CreateStructGEP(ev_struct, ev_ptr, pi);
                                auto* loaded = builder_.CreateLoad(getLLVMType(typeNodeToCodegenType(src_ev->params[pi].type)), gep);
                                call_args.push_back(loaded);
                            }
                        } else if (prev_result) {
                            auto* expected_type = ft->getParamType(call_args.size());
                            auto* arg = prev_result;
                            if (arg->getType() != expected_type) {
                                if (expected_type->isIntegerTy() && arg->getType()->isIntegerTy())
                                    arg = builder_.CreateIntCast(arg, expected_type, true);
                                else if (expected_type->isPointerTy())
                                    arg = builder_.CreateBitCast(arg, expected_type);
                            }
                            call_args.push_back(arg);
                        }
                    }

                    auto* result = builder_.CreateCall(callee, call_args);
                    if (!result->getType()->isVoidTy()) prev_result = result;
                }
                continue;
            }

            // --- Transformer node: delay(ms) or throttle(ms) ---
            if (tgt.is_transformer) {
                if (tgt.transformer_kind == 0) {
                    // delay(ms): sleep before forwarding
                    auto* sleep_fn = module_->getFunction("myp_sleep_ms");
                    if (sleep_fn) {
                        builder_.CreateCall(sleep_fn, {
                            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), tgt.transformer_param)
                        });
                    }
                } else if (tgt.transformer_kind == 1) {
                    // throttle(ms): skip if too soon since last fire
                    std::string gname = "__myp_throttle_" + hname;
                    auto* gv = module_->getGlobalVariable(gname);
                    if (!gv) {
                        gv = new llvm::GlobalVariable(*module_,
                            llvm::Type::getInt64Ty(ctx_), false,
                            llvm::GlobalValue::InternalLinkage,
                            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0), gname);
                    }
                    auto* now_fn = module_->getFunction("myp_now_ms");
                    if (now_fn) {
                        auto* now = builder_.CreateCall(now_fn, {});
                        auto* last = builder_.CreateLoad(llvm::Type::getInt64Ty(ctx_), gv, "thr_last");
                        auto* diff = builder_.CreateSub(now, last, "thr_diff");
                        auto* thr_ms = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), tgt.transformer_param);
                        auto* enough = builder_.CreateICmpSGE(diff, thr_ms, "thr_enough");
                        auto* hf = handler;
                        auto* fire_bb = llvm::BasicBlock::Create(ctx_, "thr_fire", hf);
                        auto* drop_bb = llvm::BasicBlock::Create(ctx_, "thr_drop", hf);
                        builder_.CreateCondBr(enough, fire_bb, drop_bb);
                        builder_.SetInsertPoint(drop_bb);
                        builder_.CreateRetVoid();
                        builder_.SetInsertPoint(fire_bb);
                        builder_.CreateStore(now, gv);
                    }
                }
                // Unpack event data into prev_result so the next node receives the value
                if (i == 1 && src_ev && src_ev->params.size() > 0) {
                    std::vector<llvm::Type*> ev_param_types;
                    for (auto& p : src_ev->params)
                        ev_param_types.push_back(getLLVMType(builtinTypeToInfo(p.type.basic_type)));
                    auto* ev_struct = llvm::StructType::create(ctx_, ev_param_types, "evdata");
                    auto* ev_ptr = builder_.CreateBitCast(handler->getArg(1),
                        llvm::PointerType::get(ctx_, 0));
                    for (size_t pi = 0; pi < src_ev->params.size(); pi++) {
                        auto* gep = builder_.CreateStructGEP(ev_struct, ev_ptr, pi);
                        auto* loaded = builder_.CreateLoad(ev_param_types[pi], gep);
                        // If transformer is the first target, store unpacked value as prev_result
                        if (pi == 0) prev_result = loaded;
                    }
                }
                continue;
            }

            // --- Normal (non-lambda, non-transformer) target ---
            // Resolve target: for class-level mappings, source_name is the class name.
            // For function-level mappings (local variables), resolve variable → class.
            std::string tgt_class = tgt.source_name;
            if (!tgt.is_function) {
                auto vcit = var_class_map_.find(tgt.source_name);
                if (vcit != var_class_map_.end())
                    tgt_class = vcit->second;
            }

            if (tgt.is_function) {
                // File-level function: look up by name directly
                callee = module_->getFunction(tgt.source_name);
            } else {
                // Class action: look up with ClassName_actionName
                std::string tf = tgt_class + "_" + tgt.member_name;
                callee = module_->getFunction(tf);
            }

            if (callee) {
                std::vector<llvm::Value*> call_args;

                if (!tgt.is_function) {
                    // Check if target is a static action (→ no instance pointer needed)
                    std::string tf = tgt_class + "_" + tgt.member_name;
                    auto sit = is_static_action_.find(tf);
                    bool is_static = (sit != is_static_action_.end() && sit->second);

                    if (!is_static) {
                        // Instance pointer: use global if available, otherwise fallback
                        // Try variable name first, then class name
                        auto git = class_instance_globals_.find(tgt.source_name);
                        if (git == class_instance_globals_.end())
                            git = class_instance_globals_.find(tgt_class);
                        if (git != class_instance_globals_.end()) {
                            auto* inst_p = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), git->second, "tgt_inst");
                            call_args.push_back(inst_p);
                        } else {
                            auto* inst_a = getNamedValue(tgt.source_name);
                            if (inst_a) {
                                auto* inst_p = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), inst_a, "tgt");
                                call_args.push_back(inst_p);
                            } else {
                                call_args.push_back(handler->getArg(0));
                            }
                        }
                        // BUG-005：若首个非静态目标实例位于其他线程 → 把事件路由到
                        // 该线程执行（否则 action 在事件源线程跑，B 的 @thread 归属被
                        // 忽略）。handler 自身检查：myp_thread_is_current(inst)==0 →
                        // myp_event_route_to_instance(inst, eid, data, size) 后返回。
                        if (!routed_checked && !call_args.empty() &&
                            call_args[0]->getType()->isPointerTy()) {
                            routed_checked = true;
                            auto* tgt_inst = call_args[0];
                            auto* cur_ok = builder_.CreateCall(runtime_thread_is_current_, {tgt_inst});
                            auto* cur_i1 = builder_.CreateICmpNE(cur_ok,
                                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0));
                            auto* run_bb = llvm::BasicBlock::Create(ctx_, "tgt_run", handler);
                            auto* route_bb = llvm::BasicBlock::Create(ctx_, "tgt_route", handler);
                            builder_.CreateCondBr(cur_i1, run_bb, route_bb);
                            builder_.SetInsertPoint(route_bb);
                            // 事件载荷字节数（0 = 无参）
                            uint64_t dsz = 0;
                            if (src_ev && !src_ev->params.empty()) {
                                std::vector<llvm::Type*> dts;
                                for (auto& p : src_ev->params)
                                    dts.push_back(getLLVMType(typeNodeToCodegenType(p.type)));
                                auto* dst = llvm::StructType::get(ctx_, dts);
                                dsz = module_->getDataLayout().getTypeAllocSize(dst);
                            }
                            builder_.CreateCall(runtime_event_route_inst_, {
                                tgt_inst,
                                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), event_id),
                                handler->getArg(1),
                                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), dsz)
                            });
                            builder_.CreateRetVoid();
                            builder_.SetInsertPoint(run_bb);
                        }
                    }
                }

                // Forward event data or previous return value
                auto* ft = callee->getFunctionType();
                size_t num_expected = ft->getNumParams();

                if (num_expected > call_args.size()) {
                    // Unpack event data from struct if this is the first target
                    if (i == 1 && src_ev && src_ev->params.size() > 0) {
                        // Reconstruct the struct type to GEP into it
                        std::vector<llvm::Type*> param_types;
                        for (auto& p : src_ev->params)
                            param_types.push_back(getLLVMType(typeNodeToCodegenType(p.type)));
                        auto* data_struct = llvm::StructType::create(ctx_, param_types, "evdata");
                        auto* data_ptr = builder_.CreateBitCast(handler->getArg(1),
                            llvm::PointerType::get(ctx_, 0));

                        for (size_t pi = 0; pi < src_ev->params.size() && call_args.size() < num_expected; pi++) {
                            auto* gep = builder_.CreateStructGEP(data_struct, data_ptr, pi);
                            auto* loaded = builder_.CreateLoad(getLLVMType(typeNodeToCodegenType(src_ev->params[pi].type)), gep);
                            call_args.push_back(loaded);
                        }
                    } else if (prev_result) {
                        // Forward previous return value
                        auto* expected_type = ft->getParamType(call_args.size());
                        if (prev_result->getType() != expected_type) {
                            if (expected_type->isIntegerTy() && prev_result->getType()->isIntegerTy())
                                prev_result = builder_.CreateIntCast(prev_result, expected_type, true);
                            else if (expected_type->isPointerTy())
                                prev_result = builder_.CreateBitCast(prev_result, expected_type);
                        }
                        call_args.push_back(prev_result);
                    }
                }

                auto* result = builder_.CreateCall(callee, call_args);
                // Save non-void result for chain forwarding
                if (!result->getType()->isVoidTy()) prev_result = result;
            }
        }
        builder_.CreateRetVoid();

        // Registration in init function
        if (insert_bb) builder_.SetInsertPoint(insert_bb);
        else if (saved_bb) builder_.SetInsertPoint(saved_bb);

        // BUG-031：handler 注册 instance=目标实例全局地址 &__myp_inst_X（非 NULL、
        // 非值）——注册发生在 __myp_init（早于实例 new），值不可行；传地址后 runtime
        // 在 dispatch 时解引用得实例，按线程归属路由副本，routed 副本只跑归属本线程
        // 的 handler，跨线程多目标不再互相 route 乒乓。lambda/transformer 复杂链或无
        // 类级全局实例时回落 NULL（保持原行为）。
        llvm::Value* reg_inst = llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0));
        if (chain.nodes.size() == 2 && !chain.nodes[1].is_lambda && !chain.nodes[1].is_transformer) {
            auto& tgt = chain.nodes[1];
            std::string tgt_class = tgt.source_name;
            if (!tgt.is_function) {
                auto vcit = var_class_map_.find(tgt.source_name);
                if (vcit != var_class_map_.end()) tgt_class = vcit->second;
            }
            // 与 handler 内实例解析同一来源：class_instance_globals_（变量名→类级
            // 实例全局）。getGlobalVariable 按名查在 codegen 顺序上不可靠（@thread
            // 实例全局可能晚于 mapping 注册生成）。
            auto git = class_instance_globals_.find(tgt.source_name);
            if (git == class_instance_globals_.end())
                git = class_instance_globals_.find(tgt_class);
            if (git != class_instance_globals_.end())
                reg_inst = git->second;
        }
        builder_.CreateCall(runtime_event_register_, {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), event_id),
            reg_inst,
            builder_.CreateBitCast(handler, llvm::PointerType::get(ctx_, 0))
        });

        // Allocate lambda instances used in this mapping chain
        for (auto& node : chain.nodes) {
            if (!node.is_lambda || !node.lambda) continue;
            std::string cls_name = node.lambda->hidden_class_name;
            if (cls_name.empty()) continue;
            auto* cls_st = class_structs_[cls_name];
            if (!cls_st) continue;
            auto* lgv = module_->getGlobalVariable("__myp_lambda_" + cls_name);
            if (!lgv) {
                lgv = new llvm::GlobalVariable(*module_,
                    llvm::PointerType::get(ctx_, 0), false,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)),
                    "__myp_lambda_" + cls_name);
            }
            auto sz = module_->getDataLayout().getTypeAllocSize(cls_st);
            auto* inst = builder_.CreateCall(runtime_alloc_, {
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), sz > 0 ? sz : 1)
            });
            if (sz > 0)
                builder_.CreateMemSet(inst, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0),
                                      sz, llvm::Align(8));
            builder_.CreateStore(inst, lgv);
        }
    }
}

}  // namespace mylang
