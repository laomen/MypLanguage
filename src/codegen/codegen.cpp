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

CodeGen::CodeGen(DiagnosticEngine& diag) : diag_(diag), builder_(ctx_) {}
CodeGen::~CodeGen() = default;

// uint32：按源表达式类型做整数转换——无符号源加宽用 ZExt（0xFFFFFFFFu → i64 = 4294967295，
// 不是 -1）。arg 可为空（回退有符号 SExt，保持旧行为）。其余转换同原内联逻辑。
llvm::Value* convertIntegerValue(llvm::IRBuilder<>& b, llvm::Value* v,
                                        llvm::Type* expected, const Expr* src) {
    if (!v || v->getType() == expected) return v;
    if (v->getType()->isIntegerTy() && expected->isIntegerTy()) {
        bool src_unsigned = src &&
            (src->resolved_kind == TypeKind::UByte ||
             src->resolved_kind == TypeKind::UShort ||
             src->resolved_kind == TypeKind::UInt ||
             src->resolved_kind == TypeKind::ULong);
        return b.CreateIntCast(v, expected, !src_unsigned);
    }
    if (v->getType()->isIntegerTy() && expected->isFloatingPointTy())
        return b.CreateSIToFP(v, expected);
    if (v->getType()->isFloatingPointTy() && expected->isIntegerTy())
        return b.CreateFPToSI(v, expected);
    if (v->getType()->isFloatTy() && expected->isDoubleTy())
        return b.CreateFPExt(v, expected);       // float → double（调用实参）
    if (v->getType()->isDoubleTy() && expected->isFloatTy())
        return b.CreateFPTrunc(v, expected);     // double → float（调用实参）
    if (v->getType()->isPointerTy() && expected->isPointerTy())
        return b.CreateBitCast(v, expected);
    return v;
}

// 数组下标：窄整数（i8/i16/i32）一律**零扩展**到 i64 再 GEP——否则 `cnt[msg[i]]`
// 里 msg[i] 是 uint8(i8)，值 >=128（如 190=0xBE 即 i8 -66）被 LLVM GEP 符号扩展成
// 负下标 → 计数丢失 + 越界写（huffman 基准暴露的 bug）。与 slice 路径的
// zext 约定一致（数组下标视为非负）。
llvm::Value* zextIndexValue(llvm::IRBuilder<>& b, llvm::Value* v) {
    if (!v) return v;
    if (v->getType()->isIntegerTy(8) || v->getType()->isIntegerTy(16) ||
        v->getType()->isIntegerTy(32))
        return b.CreateZExt(v, llvm::Type::getInt64Ty(v->getContext()));
    return v;
}

bool CodeGen::saveIR(const std::string& path) const {
    std::error_code ec;
    llvm::raw_fd_ostream dest(path, ec);
    if (ec) return false;
    module_->print(dest, nullptr);
    dest.flush();
    return true;
}

std::string CodeGen::generate(TranslationUnit& tu, const std::string& output_fn, int opt_level) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    module_ = std::make_unique<llvm::Module>("myp_module", ctx_);
    current_tu_ = &tu;
    interface_methods_.clear();
    interface_method_fallback_.clear();
    for (auto& iface : tu.interfaces) {
        auto& methods = interface_methods_[iface.name];
        for (size_t index = 0; index < iface.actions.size(); ++index) {
            InterfaceMethodInfo info{static_cast<unsigned>(index), &iface.actions[index]};
            methods.emplace(iface.actions[index].name, info);
            interface_method_fallback_.emplace(iface.actions[index].name, info);
        }
    }
    // Opt-in IEEE relaxation (like -ffast-math): MYP_FAST_MATH=1 marks all FP
    // ops with fast-math flags (reassoc/contract/nnan/...) so LLVM can vectorize
    // FP reductions (e.g. matmul) and contract FMAs. Default OFF keeps strict
    // IEEE semantics (matches the -O0/-O2 regression baseline).
    if (const char* fm = getenv("MYP_FAST_MATH"); fm && fm[0] == '1') {
        llvm::FastMathFlags fmf;
        fmf.setFast();
        builder_.setFastMathFlags(fmf);
    }
    if (debug_mode_) initDebugInfo(output_fn);
    declareRuntimeFunctions();
    buildStructTypes(tu);
    buildClassStructTypes(tu);
    // Create init function before body generation (so main can call it)
    createInitFunction();
    generateTranslationUnit(tu);
    emitInitMappingCalls();

    // In test mode, generate test runner main
    if (test_mode_) {
        generateTestRunner();
    }

    // Non-library builds: internalize all function definitions except `main`
    // so LLVM IPO can constant-specialize + inline hot kernels.
    if (!library_mode_) {
        markNonMainFunctionsInternal();
    }

    finalizeDebugInfo();

    // If codegen-level semantic errors were emitted (e.g. "undefined variable"
    // in a generic template body that sema skips — template bodies aren't fully
    // type-checked), abort cleanly instead of letting the type-incompatible
    // placeholder (i32 0) reach LLVM verify and crash with a confusing message.
    if (diag_.hasErrors())
        return "";

    std::string err_str;
    llvm::raw_string_ostream err_os(err_str);
    if (llvm::verifyModule(*module_, &err_os)) {
        diag_.error(SourceRange{}, "LLVM verify failed: " + err_str);
        return "";
    }

    // --emit-llvm: save IR text and skip object output
    if (emit_llvm_) {
        std::string ll_path = output_fn + ".ll";
        std::error_code ec;
        llvm::raw_fd_ostream dest(ll_path, ec, llvm::sys::fs::OF_None);
        if (!ec) {
            module_->print(dest, nullptr);
            dest.flush();
        }
        return ll_path;  // Return .ll path instead of .o
    }

    std::string obj_path = output_fn + ".o";
    return writeObjectFile(obj_path, opt_level) ? obj_path : "";
}

// ===================== DWARF Debug Info (M3-M5) =====================

void CodeGen::initDebugInfo(const std::string& filename) {
    dbg_builder_ = std::make_unique<llvm::DIBuilder>(*module_);
    std::string dir;
    auto pos = filename.find_last_of('/');
    if (pos != std::string::npos)
        dir = filename.substr(0, pos);
    dbg_file_ = dbg_builder_->createFile(filename, dir);
    dbg_cu_ = dbg_builder_->createCompileUnit(
        llvm::dwarf::DW_LANG_C_plus_plus, dbg_file_,
        "mypc v3.2.0", false, "", 0);
}

llvm::DIType* CodeGen::getDebugType(llvm::Type* ty, unsigned line) {
    if (!dbg_builder_ || !ty) return nullptr;
    if (ty->isIntegerTy(1))
        return dbg_builder_->createBasicType("bool", 1, llvm::dwarf::DW_ATE_boolean);
    if (ty->isIntegerTy(8))
        return dbg_builder_->createBasicType("char", 8, llvm::dwarf::DW_ATE_signed_char);
    if (ty->isIntegerTy(16))
        return dbg_builder_->createBasicType("short", 16, llvm::dwarf::DW_ATE_signed);
    if (ty->isIntegerTy(32))
        return dbg_builder_->createBasicType("int", 32, llvm::dwarf::DW_ATE_signed);
    if (ty->isIntegerTy(64))
        return dbg_builder_->createBasicType("long", 64, llvm::dwarf::DW_ATE_signed);
    if (ty->isFloatTy())
        return dbg_builder_->createBasicType("float", 32, llvm::dwarf::DW_ATE_float);
    if (ty->isDoubleTy())
        return dbg_builder_->createBasicType("double", 64, llvm::dwarf::DW_ATE_float);
    if (ty->isPointerTy()) {
        // LLVM 21 opaque pointers: element type is unavailable.
        return dbg_builder_->createPointerType(
            dbg_builder_->createBasicType("void", 8, llvm::dwarf::DW_ATE_unsigned), 64);
    }
    if (auto* st = llvm::dyn_cast<llvm::StructType>(ty)) {
        // DICompositeType with members (offsets from DataLayout)
        std::vector<llvm::Metadata*> elems;
        const llvm::StructLayout* sl =
            module_->getDataLayout().getStructLayout(st);
        unsigned idx = 0;
        for (llvm::Type* et : st->elements()) {
            llvm::DIType* dt = getDebugType(et, line);
            if (!dt)
                dt = dbg_builder_->createBasicType("void", 8, llvm::dwarf::DW_ATE_unsigned);
            elems.push_back(dbg_builder_->createMemberType(
                dbg_file_, "f" + std::to_string(idx), dbg_file_, line,
                module_->getDataLayout().getTypeAllocSizeInBits(et),
                sl->getElementOffsetInBits(idx), 0, llvm::DINode::FlagZero, dt));
            idx++;
        }
        return dbg_builder_->createStructType(
            dbg_file_, st->getName().str(), dbg_file_, line,
            module_->getDataLayout().getTypeAllocSizeInBits(st), 0,
            llvm::DINode::FlagZero, nullptr,
            dbg_builder_->getOrCreateArray(elems));
    }
    if (ty->isArrayTy()) {
        llvm::DIType* elem = getDebugType(ty->getArrayElementType(), line);
        if (!elem)
            elem = dbg_builder_->createBasicType("void", 8, llvm::dwarf::DW_ATE_unsigned);
        llvm::DINodeArray subs = dbg_builder_->getOrCreateArray({
            dbg_builder_->getOrCreateSubrange(0, (int64_t)ty->getArrayNumElements())});
        return dbg_builder_->createArrayType(
            module_->getDataLayout().getTypeAllocSizeInBits(ty), 0, elem, subs);
    }
    return nullptr;
}

void CodeGen::setDebugLoc(const SourceRange& r) {
    if (!debug_mode_ || !debug_scope_ || !dbg_builder_) return;
    uint32_t line = r.begin.line ? r.begin.line : 1;
    uint32_t col = r.begin.column ? r.begin.column : 1;
    builder_.SetCurrentDebugLocation(
        llvm::DILocation::get(ctx_, line, col, debug_scope_));
}

void CodeGen::beginFunctionDebug(llvm::Function* func, const std::string& name,
                                 const SourceRange& r) {
    if (!debug_mode_ || !dbg_builder_ || !func) return;
    unsigned line = r.begin.line ? r.begin.line : 1;
    auto* ret_ty = getDebugType(func->getReturnType(), line);
    if (!ret_ty)
        ret_ty = dbg_builder_->createBasicType("void", 8, llvm::dwarf::DW_ATE_unsigned);
    llvm::DISubroutineType* st = dbg_builder_->createSubroutineType(
        dbg_builder_->getOrCreateTypeArray({ret_ty}));
    debug_scope_ = dbg_builder_->createFunction(
        dbg_cu_->getFile(), name, name, dbg_file_, line,
        st, line, llvm::DINode::FlagPrototyped,
        llvm::DISubprogram::SPFlagDefinition);
    func->setSubprogram(debug_scope_);
    debug_declared_.clear();
    builder_.SetCurrentDebugLocation(llvm::DILocation::get(
        ctx_, line, r.begin.column ? r.begin.column : 1, debug_scope_));
}

void CodeGen::endFunctionDebug() {
    if (!debug_mode_) return;
    debug_scope_ = nullptr;
}

void CodeGen::emitParamDebug(llvm::Value* alloca, const std::string& name,
                             llvm::Type* ty, unsigned line, unsigned arg_idx) {
    if (!debug_mode_ || !debug_scope_ || !alloca || !dbg_builder_) return;
    llvm::DIType* dt = getDebugType(ty, line);
    if (!dt)
        dt = dbg_builder_->createBasicType("void", 8, llvm::dwarf::DW_ATE_unsigned);
    auto* dv = dbg_builder_->createParameterVariable(
        debug_scope_, name, arg_idx, dbg_file_, line, dt);
    dbg_builder_->insertDeclare(
        alloca, dv, dbg_builder_->createExpression(),
        llvm::DILocation::get(ctx_, line, 1, debug_scope_), builder_.GetInsertBlock());
    debug_declared_.insert(name);
}

void CodeGen::emitScopeLocalsDebug() {
    if (!debug_mode_ || !debug_scope_ || !dbg_builder_) return;
    if (named_values_.empty()) return;
    auto& scope = named_values_.back();
    unsigned line = 1;
    if (auto loc = builder_.getCurrentDebugLocation()) line = loc->getLine();
    for (auto& [name, val] : scope) {
        if (debug_declared_.count(name)) continue;
        auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(val);
        if (!alloca) continue;
        llvm::Type* ty = alloca->getAllocatedType();
        llvm::DIType* dt = getDebugType(ty, line);
        if (!dt)
            dt = dbg_builder_->createBasicType("void", 8, llvm::dwarf::DW_ATE_unsigned);
        auto* dv = dbg_builder_->createAutoVariable(
            debug_scope_, name, dbg_file_, line, dt);
        dbg_builder_->insertDeclare(
            alloca, dv, dbg_builder_->createExpression(),
            llvm::DILocation::get(ctx_, line, 1, debug_scope_), builder_.GetInsertBlock());
        debug_declared_.insert(name);
    }
}

void CodeGen::finalizeDebugInfo() {
    if (dbg_builder_) dbg_builder_->finalize();
}

// -- Class struct builder --
void CodeGen::buildClassStructTypes(TranslationUnit& tu) {
    // Pass 1: create ALL class struct types (empty) so self- and cross-class
    // property references (`Node next;`) resolve to ptr via getClassStruct
    // instead of falling through to an i32 placeholder. (One-pass building made
    // a class referencing its own type emit `{ i32, i32, ... }`, corrupting
    // pointer fields — exposed by ARC tests.)
    for (auto& cls : tu.classes)
        class_structs_[cls.name] = llvm::StructType::create(ctx_, cls.name);
    // Pass 2: fill members.
    for (auto& cls : tu.classes) {
        std::vector<llvm::Type*> members;
        unsigned idx = 0;
        for (auto& prop : cls.properties) {
            auto* prop_type = typeNodeToLLVMType(prop.type);
            members.push_back(prop_type);
            property_indices_[cls.name][prop.name] = idx++;
            property_types_[cls.name][prop.name] = prop_type;
        }
        class_structs_[cls.name]->setBody(members);
    }
}

// -- Struct type builder --
void CodeGen::buildStructTypes(TranslationUnit& tu) {
    // Two-pass build: Pass 1 creates ALL struct types as empty named types
    // (top-level + nested) BEFORE any body is computed, so a struct referencing
    // another struct declared later in the merged TU resolves to the named type
    // instead of falling through to an i32 placeholder. (Single-pass building
    // made `struct Holder { Item it; } struct Item {...}` lay out Holder's
    // field as i32 → LLVM verify "Call parameter type does not match function
    // signature!" when passing h.it as an Item argument. Mirrors the sema
    // two-phase struct registration fix.)
    for (auto& st : tu.structs) {
        std::string key = st.parent_class.empty()
            ? st.name : st.parent_class + "::" + st.name;
        struct_types_[key] = llvm::StructType::create(ctx_, key);
    }
    for (auto& cls : tu.classes) {
        for (auto& st : cls.structs) {
            std::string key = st.parent_class.empty()
                ? st.name : st.parent_class + "::" + st.name;
            struct_types_[key] = llvm::StructType::create(ctx_, key);
        }
    }
    // Pass 2: fill bodies. Struct-typed fields resolve via getStructType to the
    // (shared) named type object; bodies are set after all types exist so the
    // final layout is correct regardless of declaration order.
    for (auto& st : tu.structs) {
        std::string key = st.parent_class.empty()
            ? st.name : st.parent_class + "::" + st.name;
        std::vector<llvm::Type*> members;
        unsigned idx = 0;
        for (auto& prop : st.properties) {
            members.push_back(typeNodeToLLVMType(prop.type));
            struct_field_indices_[key][prop.name] = idx++;
        }
        struct_types_[key]->setBody(members);
    }
    for (auto& cls : tu.classes) {
        for (auto& st : cls.structs) {
            std::string key = st.parent_class.empty()
                ? st.name : st.parent_class + "::" + st.name;
            std::vector<llvm::Type*> members;
            unsigned idx = 0;
            for (auto& prop : st.properties) {
                members.push_back(typeNodeToLLVMType(prop.type));
                struct_field_indices_[key][prop.name] = idx++;
            }
            struct_types_[key]->setBody(members);
        }
    }
}

llvm::StructType* CodeGen::getEnumStructType(const std::string& name) {
    auto it = enum_structs_.find(name);
    if (it != enum_structs_.end()) return it->second;
    const EnumDecl* ed = findEnum(name);
    uint64_t max_payload = 0;
    if (ed) {
        for (auto& v : ed->variants) {
            uint64_t sz = 0;
            for (auto& p : v.params) {
                auto* t = typeNodeToLLVMType(p.type);
                sz += module_->getDataLayout().getTypeAllocSize(t).getFixedValue();
            }
            if (sz > max_payload) max_payload = sz;
        }
    }
    std::vector<llvm::Type*> elems;
    elems.push_back(llvm::Type::getInt32Ty(ctx_)); // disc
    if (max_payload > 0)
        elems.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx_), max_payload));
    auto* st = llvm::StructType::get(ctx_, elems, /*isPacked=*/false);
    enum_structs_[name] = st;
    enum_struct_set_.insert(st);
    return st;
}

const EnumDecl* CodeGen::findEnum(const std::string& name) const {
    if (!current_tu_) return nullptr;
    for (auto& e : current_tu_->enums)
        if (e.name == name) return &e;
    return nullptr;
}

uint64_t CodeGen::enumPayloadOffset(const EnumVariant& v, size_t field_idx) {
    uint64_t off = 0;
    auto& dl = module_->getDataLayout();
    for (size_t i = 0; i < field_idx && i < v.params.size(); i++)
        off += dl.getTypeAllocSize(typeNodeToLLVMType(v.params[i].type)).getFixedValue();
    return off;
}

llvm::Value* CodeGen::buildEnumVariant(const std::string& enum_name, size_t variant_index,
                                       const std::vector<llvm::Value*>& args) {
    auto* st = getEnumStructType(enum_name);
    auto* func = builder_.GetInsertBlock()->getParent();
    const EnumDecl* ed = findEnum(enum_name);
    llvm::Value* result = llvm::UndefValue::get(st);
    auto* i32_ty = llvm::Type::getInt32Ty(ctx_);
    result = builder_.CreateInsertValue(
        result, llvm::ConstantInt::get(i32_ty, variant_index, false), 0);

    if (st->getNumElements() > 1 && ed && variant_index < ed->variants.size()) {
        auto& v = ed->variants[variant_index];
        size_t n = v.params.size();
        if (n > 0 && args.size() >= n) {
            // Materialize each arg into a byte buffer at its packed offset,
            // then store the buffer as the payload array.
            auto* buf = createEntryBlockAlloca(
                func, st->getElementType(1), "enum_payload");
            auto& dl = module_->getDataLayout();
            auto* i8_ptr = llvm::PointerType::get(ctx_, 0);
            for (size_t i = 0; i < n; i++) {
                uint64_t off = enumPayloadOffset(v, i);
                auto* base = builder_.CreateBitCast(buf, i8_ptr);
                auto* field_ptr = builder_.CreateGEP(
                    llvm::Type::getInt8Ty(ctx_), base,
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), off));
                field_ptr = builder_.CreateBitCast(field_ptr, args[i]->getType()->getPointerTo());
                builder_.CreateStore(args[i], field_ptr);
            }
            auto* payload = builder_.CreateLoad(st->getElementType(1), buf, "enum_payload_val");
            result = builder_.CreateInsertValue(result, payload, 1);
        }
    }
    return result;
}

llvm::StructType* CodeGen::getStructType(const std::string& name) {
    auto it = struct_types_.find(name);
    return it != struct_types_.end() ? it->second : nullptr;
}

bool CodeGen::getStructFieldIndex(const std::string& sn, const std::string& fn, unsigned& idx) {
    auto si = struct_field_indices_.find(sn);
    if (si == struct_field_indices_.end()) return false;
    auto fi = si->second.find(fn);
    if (fi == si->second.end()) return false;
    idx = fi->second;
    return true;
}

llvm::Type* CodeGen::getStructFieldType(const StructDecl& st, const std::string& fn) {
    for (auto& p : st.properties)
        if (p.name == fn) return typeNodeToLLVMType(p.type);
    return llvm::Type::getInt32Ty(ctx_);
}

const StructDecl* CodeGen::findStruct(const std::string& name) const {
    if (!current_tu_) return nullptr;
    for (auto& st : current_tu_->structs) {
        std::string key = st.parent_class.empty()
            ? st.name : st.parent_class + "::" + st.name;
        if (key == name) return &st;
    }
    for (auto& cls : current_tu_->classes) {
        for (auto& st : cls.structs) {
            std::string key = st.parent_class.empty()
                ? st.name : st.parent_class + "::" + st.name;
            if (key == name) return &st;
        }
    }
    return nullptr;
}

llvm::StructType* CodeGen::getClassStruct(const std::string& n) {
    auto it = class_structs_.find(n);
    return it != class_structs_.end() ? it->second : nullptr;
}

bool CodeGen::getPropertyIndex(const std::string& cn, const std::string& pn, unsigned& idx) {
    auto ci = property_indices_.find(cn);
    if (ci == property_indices_.end()) return false;
    auto pi = ci->second.find(pn);
    if (pi == ci->second.end()) return false;
    idx = pi->second;
    return true;
}

llvm::Type* CodeGen::getPropertyType(const ClassDecl& cls, const std::string& pn) {
    auto class_it = property_types_.find(cls.name);
    if (class_it != property_types_.end()) {
        auto property_it = class_it->second.find(pn);
        if (property_it != class_it->second.end()) return property_it->second;
    }
    return llvm::Type::getInt32Ty(ctx_);
}

llvm::Type* CodeGen::typeNodeToLLVMType(const TypeNode& tn) {
    // Type alias expansion: `type Name = Type;`
    if (!tn.class_name.empty() && tn.type_args.empty()) {
        if (auto* alias = findAlias(tn.class_name))
            return typeNodeToLLVMType(alias->alias_type);
    }
    // Function type: (A,B)->R → fat pointer {closure, call_fn}
    if (tn.isFunction())
        return getFunctionValueType();
    // slice<T> — fat pointer { T* data; int64 len }. Missing this, a slice type
    // falls through to the builtin default (i32, 4 bytes), which mis-sizes
    // `new slice<slice<T>>(n)` (element should be 16 bytes, not 4) and corrupts
    // the outer slice's data region.
    if (tn.class_name == "slice" && !tn.type_args.empty()) {
        TypeInfo st(TypeKind::Slice);
        st.element_type = std::make_shared<TypeInfo>(
            typeNodeToCodegenType(tn.type_args[0]));
        return getLLVMType(st);
    }
    // Check for array type
    if (tn.isArray() && tn.element_type) {
        auto* elem = typeNodeToLLVMType(*tn.element_type);
        if (tn.array_size > 0)
            return llvm::ArrayType::get(elem, tn.array_size);
        return llvm::PointerType::get(ctx_, 0);
    }
    // Check for class type
    if (!tn.class_name.empty()) {
        // 关联类型 X::Item → 绑定类型（递归解析）
        auto pos = tn.class_name.find("::");
        if (pos != std::string::npos) {
            auto ra = resolveAssocType(tn.class_name.substr(0, pos),
                                       tn.class_name.substr(pos + 2));
            if (ra) return typeNodeToLLVMType(*ra);
        }
        // Resolve generic type param (e.g. T in `new T[n]`) to its concrete arg
        // for the currently generated generic instance.
        for (auto& tp : current_type_params_) {
            if (tn.class_name == tp.first) return typeNodeToLLVMType(tp.second);
        }
        if (getClassStruct(tn.class_name))
            return llvm::PointerType::get(ctx_, 0);
        // Class reference is always a pointer. Fall back to a TU class-name
        // lookup so struct bodies still map class fields to ptr when
        // buildStructTypes runs BEFORE buildClassStructTypes has filled
        // class_structs_ — otherwise `struct Holder { Payload p; }` laid the
        // field out as i32 → `h.p.get()` compiled as Payload_get(i32).
        if (current_tu_) {
            for (auto& c : current_tu_->classes)
                if (c.name == tn.class_name)
                    return llvm::PointerType::get(ctx_, 0);
        }
        if (auto* st = getStructType(tn.class_name))
            return st;
        if (findEnum(tn.class_name))
            return getEnumStructType(tn.class_name);
        // Interface type → fat pointer { ptr data, ptr vtable }. Bug fix: this
        // previously fell through to the builtin default (i32, 4 bytes), so
        // `new IWidget[n]` allocated 4 bytes/elem while GEPs (which use the
        // correct {ptr,ptr} element type) stepped 16 → out-of-bounds writes
        // into the heap (corruption / double-free).
        if (current_tu_)
            for (auto& ifd : current_tu_->interfaces)
                if (ifd.name == tn.class_name) {
                    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
                    return llvm::StructType::get(ctx_, {ptr_ty, ptr_ty});
                }
        // Generic class instance (Box<int>) → mangled concrete class ref.
        if (!tn.type_args.empty()) {
            std::string m = mangleConcreteTypeNode(tn);
            if (getClassStruct(m))
                return llvm::PointerType::get(ctx_, 0);
            if (current_tu_)
                for (auto& c : current_tu_->classes)
                    if (c.name == m)
                        return llvm::PointerType::get(ctx_, 0);
        }
    }
    return getLLVMType(builtinTypeToInfo(tn.basic_type));
}

const ClassDecl* CodeGen::findClass(const std::string& n) {
    if (!current_tu_) return nullptr;
    for (auto& c : current_tu_->classes) if (c.name == n) return &c;
    return nullptr;
}

const TypeAliasDecl* CodeGen::findAlias(const std::string& name) const {
    if (!current_tu_) return nullptr;
    for (auto& a : current_tu_->type_aliases)
        if (a.name == name) return &a;
    return nullptr;
}

TypeInfo CodeGen::builtinTypeToInfo(BuiltinType bt) const {
    switch (bt) {
        case BuiltinType::Int:    return TypeInfo(TypeKind::Int);
        case BuiltinType::Float:  return TypeInfo(TypeKind::Float);
        case BuiltinType::Double: return TypeInfo(TypeKind::Double);
        case BuiltinType::Bool:   return TypeInfo(TypeKind::Bool);
        case BuiltinType::Byte:   return TypeInfo(TypeKind::Byte);
        case BuiltinType::Short:  return TypeInfo(TypeKind::Short);
        case BuiltinType::Long:   return TypeInfo(TypeKind::Long);
        case BuiltinType::Char:   return TypeInfo(TypeKind::Char);
        case BuiltinType::String: return TypeInfo(TypeKind::String);
        case BuiltinType::UInt:   return TypeInfo(TypeKind::UInt);
        case BuiltinType::UByte:  return TypeInfo(TypeKind::UByte);
        case BuiltinType::UShort: return TypeInfo(TypeKind::UShort);
        case BuiltinType::ULong:  return TypeInfo(TypeKind::ULong);
        case BuiltinType::Void:   return TypeInfo(TypeKind::Void);
    }
    return TypeInfo(TypeKind::Int);
}

llvm::Type* CodeGen::getLLVMType(const TypeInfo& t) {
    switch (t.kind) {
        case TypeKind::Void:   return llvm::Type::getVoidTy(ctx_);
        case TypeKind::Bool:   return llvm::Type::getInt1Ty(ctx_);
        case TypeKind::Byte:   case TypeKind::UByte: case TypeKind::Char:
            return llvm::Type::getInt8Ty(ctx_);
        case TypeKind::Short:  case TypeKind::UShort:
            return llvm::Type::getInt16Ty(ctx_);
        case TypeKind::Int:    case TypeKind::UInt:
            return llvm::Type::getInt32Ty(ctx_);
        case TypeKind::Long:   case TypeKind::ULong:
            return llvm::Type::getInt64Ty(ctx_);
        case TypeKind::Float:  return llvm::Type::getFloatTy(ctx_);
        case TypeKind::Double: return llvm::Type::getDoubleTy(ctx_);
        case TypeKind::String:    return llvm::PointerType::get(ctx_, 0);
        case TypeKind::Null:
        case TypeKind::Function:  return getFunctionValueType();
        case TypeKind::Tuple: {
            // Anonymous struct { T0, T1, ... } — cached by element-type signature.
            std::vector<llvm::Type*> elems;
            std::string sig;
            for (auto& et : t.tuple_types) {
                elems.push_back(getLLVMType(et));
                sig += std::to_string((uintptr_t)elems.back());
                sig += ";";
            }
            auto it = tuple_structs_.find(sig);
            if (it != tuple_structs_.end()) return it->second;
            auto* st = llvm::StructType::get(ctx_, elems);
            tuple_structs_[sig] = st;
            return st;
        }
        case TypeKind::Assoc: {
            // 抽象关联类型：仅接口签名静态用途；运行时参数经调用方实参类型生成。
            // 兜底 i32 占位（实际不会在运行时物化）。
            return llvm::Type::getInt32Ty(ctx_);
        }
        case TypeKind::Class: {
            auto* st = getClassStruct(t.class_name);
            (void)st;
            return llvm::PointerType::get(ctx_, 0);
        }
        case TypeKind::Interface: {
            // Fat pointer: { ptr data, ptr vtable }
            auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
            return llvm::StructType::get(ctx_, {ptr_ty, ptr_ty});
        }
        case TypeKind::Struct:
            return getStructType(t.class_name);
        case TypeKind::Enum:
            return getEnumStructType(t.class_name);
        case TypeKind::Array: {
            if (t.array_size > 0 && t.element_type) {
                auto* elem = getLLVMType(*t.element_type);
                return llvm::ArrayType::get(elem, t.array_size);
            }
            // Unsized array (int[]) — pass as pointer
            return llvm::PointerType::get(ctx_, 0);
        }
        case TypeKind::Slice: {
            // slice<T> = { T* data; int64 len }
            auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
            return llvm::StructType::get(ctx_, {ptr_ty, llvm::Type::getInt64Ty(ctx_)});
        }
    }
    return llvm::Type::getVoidTy(ctx_);
}

llvm::StructType* CodeGen::getFunctionValueType() {
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    return llvm::StructType::get(ctx_, {ptr_ty, ptr_ty});
}

/// Convert a TypeNode (from AST) to TypeInfo for codegen use.
/// Properly handles array types (both sized and unsized).
TypeInfo CodeGen::typeNodeToCodegenType(const TypeNode& node) {
    // Type alias expansion: `type Name = Type;`
    if (!node.class_name.empty() && node.type_args.empty()) {
        if (auto* alias = findAlias(node.class_name))
            return typeNodeToCodegenType(alias->alias_type);
    }
    // Function type: (A,B)->R
    if (node.isFunction()) {
        TypeInfo ft(TypeKind::Function);
        ft.return_type = std::make_shared<TypeInfo>(typeNodeToCodegenType(*node.func_return_type));
        for (auto& p : node.func_param_types)
            ft.param_types.push_back(typeNodeToCodegenType(p));
        return ft;
    }
    // Tuple type: (A,B)
    if (node.isTuple()) {
        TypeInfo tt(TypeKind::Tuple);
        for (auto& p : node.func_param_types)
            tt.tuple_types.push_back(typeNodeToCodegenType(p));
        return tt;
    }
    if (node.isArray() && node.element_type) {
        TypeInfo result(TypeKind::Array);
        result.array_size = node.array_size;
        result.element_type = std::make_shared<TypeInfo>(
            typeNodeToCodegenType(*node.element_type));
        return result;
    }
    if (!node.class_name.empty()) {
        // 关联类型 X::Item → 绑定类型（递归解析）
        auto pos = node.class_name.find("::");
        if (pos != std::string::npos) {
            auto ra = resolveAssocType(node.class_name.substr(0, pos),
                                       node.class_name.substr(pos + 2));
            if (ra) return typeNodeToCodegenType(*ra);
        }
        // Resolve generic type param (e.g. T in `new T[n]`) to its concrete arg
        // for the currently generated generic instance.
        for (auto& tp : current_type_params_) {
            if (node.class_name == tp.first) return typeNodeToCodegenType(tp.second);
        }
        // Built-in slice<T>
        if (node.class_name == "slice" && node.type_args.size() == 1) {
            TypeInfo result(TypeKind::Slice);
            result.element_type = std::make_shared<TypeInfo>(
                typeNodeToCodegenType(node.type_args[0]));
            return result;
        }
        if (getClassStruct(node.class_name)) {
            TypeInfo result(TypeKind::Class);
            result.class_name = node.class_name;
            return result;
        }
        if (getStructType(node.class_name)) {
            TypeInfo result(TypeKind::Struct);
            result.class_name = node.class_name;
            return result;
        }
        if (findEnum(node.class_name)) {
            TypeInfo result(TypeKind::Enum);
            result.class_name = node.class_name;
            return result;
        }
        // Generic class instance (Box<int>) → mangled concrete class ref.
        if (!node.type_args.empty()) {
            std::string m = mangleConcreteTypeNode(node);
            if (getClassStruct(m)) {
                TypeInfo result(TypeKind::Class);
                result.class_name = m;
                return result;
            }
        }
        // Check if this is an interface type
        if (current_tu_) {
            for (auto& ifd : current_tu_->interfaces) {
                if (ifd.name == node.class_name) {
                    TypeInfo result(TypeKind::Interface);
                    result.class_name = node.class_name;
                    return result;
                }
            }
        }
    }
    return builtinTypeToInfo(node.basic_type);
}

// Resolve generic type-param placeholders against current_type_params_, then
// mangle to the same string sema uses (typeName(typeNodeToTypeInfo(...))).
// Handles builtins, arrays, function types and generic classes (nested generics
// get one "_inst" per level, matching sema's per-arg mangling).

// -- Symbol table --
void CodeGen::pushScope() {
    named_values_.emplace_back();
    arc_scope_slots_.emplace_back();
}
void CodeGen::popScope() {
    if (debug_mode_) emitScopeLocalsDebug();
    // ARC (§五-1): release every local class/interface reference declared in the
    // scope being popped. Params / `this` are never registered → borrowed from
    // the caller, not released here. If the insert point is dead (after a
    // return/throw) we skip the release — safe leak, never a premature free.
    if (!arc_scope_slots_.empty()) {
        auto& slots = arc_scope_slots_.back();
        // Dead path (after return/throw): the normal release is skipped (safe
        // leak) — keep the slot in the try's unwind list so the exception
        // dispatch releases it. Live path: release and drop from the unwind
        // list so the dispatch never double-releases it.
        bool dead = !builder_.GetInsertBlock() ||
                    builder_.GetInsertBlock()->getTerminator();
        for (auto& s : slots) {
            releaseArcSlot(s.alloca, s.kind);
            if (!dead) removeTryUnwindSlot(s.alloca);
        }
        arc_scope_slots_.pop_back();
    }
    if (!named_values_.empty()) named_values_.pop_back();
}
void CodeGen::removeTryUnwindSlot(llvm::Value* alloca) {
    if (try_ctx_stack_.empty() || !alloca) return;
    auto& slots = try_ctx_stack_.back().inner_slots;
    for (auto it = slots.begin(); it != slots.end(); ++it) {
        if (it->alloca == alloca) { slots.erase(it); return; }
    }
}
// Register a local reference slot (class instance / interface fat pointer /
// function-value closure) so its referenced object is myp_release'd when the
// scope exits. kind: 0=class ptr, 1=interface, 2=function value.
void CodeGen::registerArcSlot(llvm::Value* alloca, int kind) {
    if (!alloca || arc_scope_slots_.empty()) return;
    arc_scope_slots_.back().push_back({alloca, kind});
    // Exception unwinding: also collect into the innermost active try so the
    // dispatch path can release it (the longjmp skips the normal scope exit).
    if (!try_ctx_stack_.empty()) {
        try_ctx_stack_.back().inner_slots.push_back({alloca, kind});
        // §五-3 exception × -O: the dispatch/propagate paths release inner
        // slots AFTER a longjmp, but their only reaching defs live in try_block
        // (which does NOT dominate the longjmp path). Without an escape, LLVM
        // folds the dispatch load to undef → myp_release(garbage) at -O2
        // (result segfault) or release(null) (arc_throw leak). Passing the slot
        // address to the myp_try_escape no-op marks it escaped memory, so the
        // -O pipeline keeps both the try-block stores and the dispatch load and
        // reads the true physical value on the longjmp path.
        if (!builder_.GetInsertBlock() ||
            builder_.GetInsertBlock()->getTerminator())
            return;   // dead path — nothing further to do
        if (!runtime_try_escape_)
            runtime_try_escape_ = llvm::Function::Create(
                llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                                        {llvm::PointerType::get(ctx_, 0)}, false),
                llvm::Function::ExternalLinkage, "myp_try_escape", module_.get());
        builder_.CreateCall(runtime_try_escape_->getFunctionType(),
                            runtime_try_escape_, {alloca});
    }
}

// Emit myp_release for every slot collected in the innermost try's unwind list.
// Called at the top of the exception paths (dispatch / propagate) — only runs
// when the try block was abandoned, so no double release with normal scope exit.
// The slot is read PHYSICALLY by myp_release_slot (opaque to LLVM): after the
// longjmp, the slot's memory holds the true object (try-block null-init or the
// constructed object), but an LLVM-side load here would be folded to undef at
// -O2 because the try_block defs do not dominate the longjmp path (§五-3).
// Passing the slot address to the opaque myp_release_slot also escapes it,
// keeping the try-block stores alive.
void CodeGen::emitReleaseTryInnerSlots() {
    if (try_ctx_stack_.empty()) return;
    auto& slots = try_ctx_stack_.back().inner_slots;
    if (slots.empty()) return;
    if (!builder_.GetInsertBlock() ||
        builder_.GetInsertBlock()->getTerminator())
        return;   // dead path — skip
    if (!runtime_release_slot_)
        runtime_release_slot_ = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                                    {llvm::PointerType::get(ctx_, 0),
                                     llvm::Type::getInt32Ty(ctx_)}, false),
            llvm::Function::ExternalLinkage, "myp_release_slot", module_.get());
    auto* i32ty = llvm::Type::getInt32Ty(ctx_);
    for (auto& s : slots) {
        // Drop the coroutine-frame mirror for this slot first (same as
        // releaseArcSlot does) so a later Coro.destroy can't double-release.
        emitCoroFrameClear(s.alloca);
        if (s.kind == 3) {
            // Fixed class-array slot: release `count` element refs directly
            // (myp_release_slot has no count).
            auto it = arc_fixed_array_counts_.find(s.alloca);
            uint64_t cnt = (it != arc_fixed_array_counts_.end()) ? it->second : 0;
            auto* data = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), s.alloca);
            auto* fn = module_->getFunction("myp_release_fixed_class_array");
            if (!fn) {
                auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                    {llvm::PointerType::get(ctx_, 0), llvm::Type::getInt64Ty(ctx_)}, false);
                fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                    "myp_release_fixed_class_array", module_.get());
            }
            builder_.CreateCall(fn, {data,
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), cnt)});
            continue;
        }
        llvm::Value* kind = llvm::ConstantInt::get(i32ty, s.kind);
        builder_.CreateCall(runtime_release_slot_->getFunctionType(),
                            runtime_release_slot_, {s.alloca, kind});
    }
}

// §五-1 收尾: coroutine-frame ARC registry (see header comment). set/clear mirror
// the object currently held by a local ARC slot into the current coroutine's
// runtime frame list. Only meaningful inside @coro bodies (current_is_coro_);
// elsewhere the runtime calls are no-ops anyway.
void CodeGen::emitCoroFrameSet(llvm::Value* alloca, llvm::Value* obj) {
    if (!current_is_coro_ || !alloca || !obj) return;
    if (!builder_.GetInsertBlock() || builder_.GetInsertBlock()->getTerminator())
        return;   // dead path — never runs
    if (!runtime_coro_frame_set_)
        runtime_coro_frame_set_ = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                                    {llvm::Type::getInt64Ty(ctx_),
                                     llvm::Type::getInt64Ty(ctx_)}, false),
            llvm::Function::ExternalLinkage, "__myp_coro_frame_set", module_.get());
    auto* slot = builder_.CreatePtrToInt(alloca, llvm::Type::getInt64Ty(ctx_));
    llvm::Value* data = obj;
    if (obj->getType()->isStructTy())   // interface / function-value fat pointer
        data = builder_.CreateExtractValue(obj, 0);
    auto* obj_i = builder_.CreatePtrToInt(data, llvm::Type::getInt64Ty(ctx_));
    builder_.CreateCall(runtime_coro_frame_set_, {slot, obj_i});
}
void CodeGen::emitCoroFrameClear(llvm::Value* alloca) {
    if (!current_is_coro_ || !alloca) return;
    if (!builder_.GetInsertBlock() || builder_.GetInsertBlock()->getTerminator())
        return;   // dead path — never runs
    if (!runtime_coro_frame_clear_)
        runtime_coro_frame_clear_ = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                                    {llvm::Type::getInt64Ty(ctx_)}, false),
            llvm::Function::ExternalLinkage, "__myp_coro_frame_clear", module_.get());
    auto* slot = builder_.CreatePtrToInt(alloca, llvm::Type::getInt64Ty(ctx_));
    builder_.CreateCall(runtime_coro_frame_clear_, {slot});
}
// Release the current function's still-live slots before an outward longjmp.
// Fresh-throw sites: only when no same-function try exists (its dispatch would
// otherwise handle the inner slots). Rethrow sites: when no ENCLOSING
// same-function try exists (only the current one, or none) — the outer slots
// would otherwise leak when the exception leaves the function.
void CodeGen::emitUnwindRelease(bool rethrow_site) {
    bool release = rethrow_site
        ? (try_ctx_stack_.size() <= 1)
        : try_ctx_stack_.empty();
    if (release) arcReleaseAllScopes();
}
void CodeGen::releaseArcSlot(llvm::Value* alloca, int kind) {
    if (!runtime_release_ || !alloca) return;
    if (!builder_.GetInsertBlock() ||
        builder_.GetInsertBlock()->getTerminator()) return;  // dead path — skip
    // §五-1 收尾: remove this slot from the coroutine's runtime frame list
    // BEFORE the release — a normally-released slot's object must not be
    // released again if the coroutine is destroyed later.
    emitCoroFrameClear(alloca);
    if (kind == 5) {
        // M8 structs: an owned struct local — release every ARC-reference field
        // (string / class / interface / slice / dynamic array / nested struct).
        auto it = arc_struct_slot_types_.find(alloca);
        if (it != arc_struct_slot_types_.end()) {
            const StructDecl* sd = findStruct(it->second);
            if (sd) emitStructFieldsPtr(builder_, alloca, *sd, false);
        }
        return;
    }
    if (kind == 3) {
        // Fixed (stack) class-array slot: release `count` element refs.
        auto it = arc_fixed_array_counts_.find(alloca);
        uint64_t cnt = (it != arc_fixed_array_counts_.end()) ? it->second : 0;
        auto* data = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), alloca);
        auto* fn = module_->getFunction("myp_release_fixed_class_array");
        if (!fn) {
            auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                {llvm::PointerType::get(ctx_, 0), llvm::Type::getInt64Ty(ctx_)}, false);
            fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                "myp_release_fixed_class_array", module_.get());
        }
        builder_.CreateCall(fn, {data, llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), cnt)});
        return;
    }
    if (kind == 4) {
        // M8: slice slot holds a { T* data; int64 len } fat pointer — release
        // the counted backing via data (index 0).
        auto* slt = llvm::StructType::get(ctx_,
            {llvm::PointerType::get(ctx_, 0), llvm::Type::getInt64Ty(ctx_)});
        auto* fat = builder_.CreateLoad(slt, alloca);
        auto* data = builder_.CreateExtractValue(fat, 0);
        builder_.CreateCall(runtime_release_, {data});
        return;
    }
    llvm::Value* v;
    if (kind == 1 || kind == 2) {
        // Interface / function-value fat pointer {obj, ...} → release index 0.
        auto* fat = builder_.CreateLoad(getLLVMType(TypeInfo(TypeKind::Interface)), alloca);
        v = builder_.CreateExtractValue(fat, 0);
    } else {
        v = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), alloca);
    }
    builder_.CreateCall(runtime_release_, {v});
}
llvm::Value* CodeGen::emitRetain(llvm::Value* data) {
    if (!data) return data;
    // Null-safe: myp_retain ignores NULL. But a garbage/poison pointer would
    // read a bogus header — only call on values we know are object refs.
    // Only retain POINTER values (class/string refs). A non-pointer (e.g. a
    // generic type-param placeholder that resolved to a primitive in a
    // template body) is not an ARC ref — passing it to myp_retain would fail
    // LLVM verify ("Call parameter type does not match").
    if (data->getType()->isPointerTy() && runtime_retain_)
        builder_.CreateCall(runtime_retain_, {data});
    return data;
}
// M8: retain the counted backing of a slice value ({data, len} fat pointer).
llvm::Value* CodeGen::emitRetainSlice(llvm::Value* slice_val) {
    if (!slice_val || !slice_val->getType()->isStructTy()) return slice_val;
    auto* data = builder_.CreateExtractValue(slice_val, 0);
    if (runtime_retain_)
        builder_.CreateCall(runtime_retain_, {data});
    return slice_val;
}
bool CodeGen::isArcClassLocal(llvm::Value* alloca) {
    if (!alloca) return false;
    for (auto& scope : arc_scope_slots_)
        for (auto& s : scope)
            if (s.alloca == alloca && s.kind == 0) return true;
    return false;
}
bool CodeGen::isArcSliceLocal(llvm::Value* alloca) {
    if (!alloca) return false;
    for (auto& scope : arc_scope_slots_)
        for (auto& s : scope)
            if (s.alloca == alloca && s.kind == 4) return true;
    return false;
}
bool CodeGen::isArcFunctionLocal(llvm::Value* alloca) {
    if (!alloca) return false;
    for (auto& scope : arc_scope_slots_)
        for (auto& s : scope)
            if (s.alloca == alloca && s.kind == 2) return true;
    return false;
}
// ARC strong-slot store: retain(new) unless it is a fresh (new/call) value,
// then release the previous slot value. The caller stores `new_val` afterward.
void CodeGen::arcStoreRef(llvm::Value* slot, llvm::Value* new_val,
                          bool is_interface, bool is_fresh) {
    if (!slot) return;
    llvm::Value* new_data = new_val;
    llvm::Value* old_data = nullptr;
    if (is_interface) {
        auto* fat_ty = getLLVMType(TypeInfo(TypeKind::Interface));
        auto* old = builder_.CreateLoad(fat_ty, slot);
        old_data = builder_.CreateExtractValue(old, 0);
        if (new_val && new_val->getType()->isStructTy())
            new_data = builder_.CreateExtractValue(new_val, 0);
    } else {
        old_data = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), slot);
    }
    if (!is_fresh && new_data && runtime_retain_)
        builder_.CreateCall(runtime_retain_, {new_data});
    if (old_data && runtime_release_)
        builder_.CreateCall(runtime_release_, {old_data});
}
// M8: strong-store into a slice slot ({data,len} fat pointer): retain the new
// backing (unless the RHS is a fresh new slice), release the old backing.
void CodeGen::arcStoreSlice(llvm::Value* slot, llvm::Value* new_val, bool is_fresh) {
    if (!slot || !new_val) return;
    auto* slt = llvm::StructType::get(ctx_,
        {llvm::PointerType::get(ctx_, 0), llvm::Type::getInt64Ty(ctx_)});
    auto* old = builder_.CreateLoad(slt, slot);
    auto* old_data = builder_.CreateExtractValue(old, 0);
    if (!is_fresh) {
        auto* new_data = builder_.CreateExtractValue(new_val, 0);
        if (runtime_retain_)
            builder_.CreateCall(runtime_retain_, {new_data});
    }
    if (old_data && runtime_release_)
        builder_.CreateCall(runtime_release_, {old_data});
}
// A fresh `new` object is owned by the current statement until a store takes it.
void CodeGen::arcPushTemp(llvm::Value* v) {
    if (v) arc_pending_temps_.push_back(v);
}
// A store site takes ownership of a temporary (transfer) — drop it from the
// pending list so the statement-end flush does not release it again.
void CodeGen::arcConsumeTemp(llvm::Value* v) {
    if (!v) return;
    for (auto it = arc_pending_temps_.begin(); it != arc_pending_temps_.end(); ++it) {
        if (*it == v) { arc_pending_temps_.erase(it); return; }
    }
}
// End of a conditional branch block (short-circuit RHS / ternary arm): the merge
// block is reachable from sibling blocks (short-circuit/other arm), so the
// statement-end flush — emitted in merge — would release temps created here in
// a block they do NOT dominate (LLVM verify: "Instruction does not dominate all
// uses"). Release branch-local intermediates HERE (the current block dominates
// its own instructions). If the branch result itself is a freshly-created class-
// ref temp, ownership transfers to the merge phi: consume it from the pending
// list and return it so the caller can push the phi as the surviving temp.
llvm::Value* CodeGen::arcEndBranch(size_t before, llvm::Value* branch_result) {
    llvm::Value* owned = nullptr;
    if (arc_pending_temps_.size() <= before) return owned;
    for (auto it = arc_pending_temps_.begin() + (ptrdiff_t)before;
         it != arc_pending_temps_.end(); ) {
        if (branch_result && *it == branch_result) {
            owned = *it;
            it = arc_pending_temps_.erase(it);
        } else {
            if (runtime_release_ && builder_.GetInsertBlock() &&
                !builder_.GetInsertBlock()->getTerminator())
                builder_.CreateCall(runtime_release_, {*it});
            it = arc_pending_temps_.erase(it);
        }
    }
    return owned;
}
// M7/ARC: release temps created during a branch-condition evaluation (a
// class-returning call, a @weak upgrade, `new`, etc.). The branch uses only the
// derived i1, never the temps, so they can be released here (current, always-
// executed block) instead of being grabbed by a branch body's statement-end
// flush in a CONDITIONAL block — that would release them on only one path and
// leak on the other.
void CodeGen::arcReleaseConditionTemps(size_t before) {
    if (arc_pending_temps_.size() <= before) return;
    for (auto it = arc_pending_temps_.begin() + (ptrdiff_t)before;
         it != arc_pending_temps_.end(); ++it) {
        if (runtime_release_ && builder_.GetInsertBlock() &&
            !builder_.GetInsertBlock()->getTerminator())
            builder_.CreateCall(runtime_release_, {*it});
    }
    arc_pending_temps_.erase(arc_pending_temps_.begin() + (ptrdiff_t)before,
                             arc_pending_temps_.end());
}
// End of statement: release any `new` temporaries nobody stored.
void CodeGen::arcFlushTemps() {
    if (arc_pending_temps_.empty()) return;
    // Dead path (after return/throw): leak the temps rather than emit into a
    // dead block (the return path already consumed/retained its value).
    if (!builder_.GetInsertBlock() || builder_.GetInsertBlock()->getTerminator()) {
        arc_pending_temps_.clear();
        return;
    }
    for (auto* v : arc_pending_temps_) {
        if (runtime_release_)
            builder_.CreateCall(runtime_release_, {v});
    }
    arc_pending_temps_.clear();
}
// Function epilogue: release every live scope's local reference slots before a
// return. Normally popScope handles this, but when the last statement is a
// `return` the block is already terminated and popScope's release is skipped
// (leaking the locals). retain-at-return already +1'd the returned value, so
// releasing the returned slot too is balanced.
void CodeGen::arcReleaseAllScopes() {
    if (!runtime_release_) return;
    if (!builder_.GetInsertBlock() || builder_.GetInsertBlock()->getTerminator())
        return;
    for (auto& scope : arc_scope_slots_) {
        for (auto& s : scope)
            releaseArcSlot(s.alloca, s.kind);
    }
}
// NewExpr / CallExpr produce a fresh (+1) reference → transfer into a strong
// slot without retaining. Everything else (identifier / member access /
// subscript …) is an alias of an existing owner → must retain. NewArrayExpr of
// a class element is likewise a fresh ref-counted array (temp rc=1).
bool CodeGen::isFreshArcExpr(const Expr& e) {
    // NewExpr / CallExpr / LambdaExpr produce a fresh reference (the lambda's
    // closure is a freshly allocated class instance held by its fat pointer).
    // NewArrayExpr of a class element is a fresh ref-counted array.
    if (e.kind == ExprKind::NewExpr || e.kind == ExprKind::Call ||
        e.kind == ExprKind::Lambda || e.kind == ExprKind::NewArrayExpr)
        return true;
    // M7: a @weak property read performs a weak→strong upgrade at runtime
    // (myp_weak_load retains), so it yields a FRESH strong reference — stores
    // / var-decls must not retain it a second time.
    if (e.kind == ExprKind::MemberAccess)
        return isWeakMemberAccess(static_cast<const MemberAccessExpr&>(e));
    // M8: string concatenation `a + b` is a BinaryOp whose result is a fresh
    // counted string (rc=1). Without this, `s = s + "x"` / `string t = a + b`
    // RETAINED the concat result (rc 1→2), so releasing the slot only dropped
    // it to 1 — one leaked reference per concatenation.
    return isStringConcatExpr(e);
}

// M7: is `obj.weakProp` a read of a @weak property? A weak read upgrades to a
// fresh strong ref at runtime, so it must be treated as fresh by the ARC store
// / var-decl paths (no extra retain).
bool CodeGen::isWeakMemberAccess(const MemberAccessExpr& e) {
    if (!current_tu_) return false;
    std::string cls;
    if (e.object->kind == ExprKind::ThisExpr) {
        cls = current_class_name_;
    } else if (e.object->kind == ExprKind::Identifier) {
        auto& id = static_cast<const IdentifierExpr&>(*e.object);
        auto vc = var_class_map_.find(id.name);
        if (vc != var_class_map_.end()) cls = vc->second;
        else if (static_property_globals_.find(id.name) != static_property_globals_.end())
            cls = id.name;   // static class: ClassName.prop
    } else if (!e.resolved_object_class.empty()) {
        cls = e.resolved_object_class;
    }
    if (cls.empty()) return false;
    for (auto& c : current_tu_->classes) {
        if (c.name != cls) continue;
        for (auto& p : c.properties)
            if (p.name == e.member_name) return p.weak;
        return false;
    }
    return false;
}

// M7: load a class property field. Weak fields go through myp_weak_load — a
// weak→strong upgrade returning a FRESH strong ref (rc+1), pushed as a temp the
// store/var-decl consumes or the statement-end flush releases. Strong fields
// load directly (borrowed, no retain).
llvm::Value* CodeGen::loadPropertyField(llvm::Value* gep, const ClassDecl& cls,
                                        const std::string& member_name) {
    if (!gep) return nullptr;
    bool weak = false;
    for (auto& p : cls.properties)
        if (p.name == member_name) { weak = p.weak; break; }
    if (weak) {
        if (!runtime_weak_load_) {
            auto* pt = llvm::PointerType::get(ctx_, 0);
            auto* ft = llvm::FunctionType::get(pt, {pt}, false);
            runtime_weak_load_ = llvm::Function::Create(ft,
                llvm::Function::ExternalLinkage, "myp_weak_load", module_.get());
        }
        auto* obj = builder_.CreateCall(runtime_weak_load_->getFunctionType(),
                                        runtime_weak_load_, {gep});
        arcPushTemp(obj);
        return obj;
    }
    auto* pt = getPropertyType(cls, member_name);
    if (pt->isArrayTy()) return gep;
    return builder_.CreateLoad(pt, gep);
}

// M7: store into a class property field. Weak fields go through myp_weak_store
// (no retain/release; the registry keeps the slot's weak observer updated) and
// the caller skips the strong arcStoreRef. Returns true when the field is weak.
bool CodeGen::storePropertyField(llvm::Value* gep, llvm::Value* v,
                                 const ClassDecl& cls, const std::string& member_name) {
    if (!gep || !v) return false;
    for (auto& p : cls.properties) {
        if (p.name != member_name) continue;
        if (!p.weak) return false;
        if (!runtime_weak_store_) {
            auto* pt = llvm::PointerType::get(ctx_, 0);
            auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                {pt, pt}, false);
            runtime_weak_store_ = llvm::Function::Create(ft,
                llvm::Function::ExternalLinkage, "myp_weak_store", module_.get());
        }
        builder_.CreateCall(runtime_weak_store_->getFunctionType(),
                            runtime_weak_store_, {gep, v});
        return true;
    }
    return false;
}
void CodeGen::setNamedValue(const std::string& n, llvm::Value* a) {
    if (named_values_.empty()) named_values_.emplace_back();
    named_values_.back()[n] = a;
}
void CodeGen::setNamedTypedValue(const std::string& n, llvm::Value* ptr, llvm::Type* ty) {
    setNamedValue(n, ptr);
    named_value_types_[n] = ty;
}
llvm::Value* CodeGen::getNamedValue(const std::string& n) {
    for (auto it = named_values_.rbegin(); it != named_values_.rend(); ++it) {
        auto f = it->find(n);
        if (f != it->end()) return f->second;
    }
    return nullptr;
}

llvm::Type* CodeGen::getNamedValueType(const std::string& n) {
    auto it = named_value_types_.find(n);
    return it != named_value_types_.end() ? it->second : nullptr;
}

llvm::AllocaInst* CodeGen::createEntryBlockAlloca(llvm::Function* f, llvm::Type* t, const std::string& n) {
    llvm::IRBuilder<> tb(&f->getEntryBlock(), f->getEntryBlock().begin());
    return tb.CreateAlloca(t, nullptr, n);
}

// -- Top level --
void CodeGen::generateTranslationUnit(TranslationUnit& tu) {
    // Create ALL class action function declarations first
    for (auto& cls : tu.classes) {
        for (auto& action : cls.actions)
            createClassActionDecl(cls, action);
        for (auto& action : cls.static_actions)
            createStaticActionDecl(cls, action);
        // Pre-declare function: section methods so cross-calls in the section
        // resolve regardless of declaration order.
        for (auto& fn : cls.functions)
            createClassFunctionDecl(cls, fn);
    }

    // trait 默认实现：预声明接口默认方法（按实现类特化），使虚表构建时能找到。
    for (auto& cls : tu.classes) {
        if (cls.interface_class_name.empty() || cls.is_generic_inst) continue;
        const InterfaceDecl* iface = nullptr;
        for (auto& ifd : tu.interfaces)
            if (ifd.name == cls.interface_class_name) { iface = &ifd; break; }
        if (!iface) continue;
        for (auto& ia : iface->actions) {
            if (!ia.body) continue;
            bool has = false;
            for (auto& ca : cls.actions)
                if (ca.name == ia.name && ca.return_type.basic_type == ia.return_type.basic_type) { has = true; break; }
            if (has) continue;   // 类已覆盖 → 不需要默认
            createClassDefaultDecl(cls, *iface, ia);
        }
    }

    // Create ALL event fire function declarations
    for (auto& cls : tu.classes) {
        for (auto& ev : cls.events) {
            std::string fn = "fire_" + cls.name + "_" + ev.name;
            if (module_->getFunction(fn)) continue;
            std::vector<llvm::Type*> pts;
            pts.push_back(llvm::PointerType::get(ctx_, 0)); // instance ptr
            for (auto& p : ev.params)
                pts.push_back(getLLVMType(typeNodeToCodegenType(p.type)));
            auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), pts, false);
            llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn, module_.get());
        }
    }

    // Assign global event IDs (unique across all classes)
    int global_event_id = 0;
    for (auto& cls : tu.classes) {
        for (auto& ev : cls.events) {
            event_id_map_[cls.name + "::" + ev.name] = global_event_id++;
        }
    }

    // Generate FFI function declarations
    for (auto& ff : current_tu_->ffis) {
        generateFFIDecl(ff);
    }

    // Create global variables for @static class instances (accessible via ClassName.property)
    for (auto& cls : tu.classes) {
        if (!cls.is_static) continue;
        if (cls.properties.empty()) continue;
        auto* st = getClassStruct(cls.name);
        if (!st) continue;
        auto* gv = new llvm::GlobalVariable(*module_, st, false,
            llvm::GlobalValue::ExternalLinkage,
            llvm::ConstantAggregateZero::get(st),
            "__myp_static_" + cls.name);
        static_property_globals_[cls.name] = gv;
    }



    // Create global instance pointers for classes used in mappings
    // MUST be done before generateClass() so @thread variables can store
    // instance pointers that mapping handlers need
    for (auto& m : tu.mappings) {
        for (auto& chain : m.chains) {
            for (auto& node : chain.nodes) {
                if (class_instance_globals_.find(node.source_name) == class_instance_globals_.end()) {
                    auto* gv = new llvm::GlobalVariable(*module_,
                        llvm::PointerType::get(ctx_, 0), false,
                        llvm::GlobalValue::InternalLinkage,
                        llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)),
                        "__myp_inst_" + node.source_name);
                    class_instance_globals_[node.source_name] = gv;
                }
            }
        }
    }

    // Forward-declare ALL struct methods so they can be called from class bodies
    // (must be done BEFORE generateClass)
    for (auto& st : tu.structs) declareStructMethods(st);
    for (auto& cls : tu.classes) {
        for (auto& st : cls.structs) declareStructMethods(st);
    }

    // Assign compile-time exception type IDs to every class (ID 0 = string).
    {
        int tid = 1;
        for (auto& cls : tu.classes)
            class_type_ids_[cls.name] = tid++;
    }

    // Build Error-interface vtables + the __myp_error_vtables lookup array.
    // Done BEFORE function bodies so catch (Error e) dispatch can reference it.
    generateErrorVTables();

    // ARC (§五-1): per-class destroy stubs + __myp_release_table. Stubs are
    // dormant until myp_release drops an rc to 0; objects live until exit
    // until the instrumentation (M-ARC-2) starts releasing them.
    generateArcSupport(tu);

    // Pre-create entry wrappers for top-level @coro functions so spawn sites in
    // any order (e.g. main defined before the coroutine) can resolve them.
    for (auto& f : tu.functions) {
        if (f.has_coro) {
            generateCoroFuncEntry(f);
            coro_methods_.insert(f.name);
            coro_stack_map_[f.name] = f.coro_stack_kb;
        }
    }
    // Pre-scan class @coro methods BEFORE generating any body: a coroutine may
    // spawn another @coro method of the same class, and coro_methods_ must be
    // populated before that body is emitted (otherwise the call falls through
    // to a plain call and the void→long assignment crashes codegen). The
    // entry wrapper is created here too so spawn sites in any order resolve it.
    for (auto& cls : tu.classes) {
        for (auto& a : cls.actions) {
            if (a.has_coro) {
                std::string key = cls.name + "_" + a.name;
                coro_methods_.insert(key);
                coro_stack_map_[key] = a.coro_stack_kb;
                generateCoroEntry(cls, a);
            }
        }
    }

    // Pre-declare ALL top-level function signatures (including monomorphized
    // generic instances, which sema appends at the end of tu.functions) so a
    // body generated earlier in the vector can call one defined later.
    // Generic templates (type_params, not an instance) are skipped.
    for (auto& f : tu.functions) {
        if (!f.type_params.empty() && !f.is_generic_inst) continue;
        current_type_params_.clear();
        for (size_t i = 0; i < f.type_params.size() && i < f.inst_type_args.size(); i++)
            current_type_params_.emplace_back(f.type_params[i], f.inst_type_args[i]);
        declareFuncSignature(f);
    }

    // Pre-declare lambda __call methods + generate tramps so lambda expressions
    // inside function bodies can build fat pointers regardless of class order.
    for (auto& cls : tu.classes) {
        if (cls.name.rfind("__lambda_", 0) != 0) continue;
        for (auto& a : cls.actions)
            if (a.name == "__call") createClassActionDecl(cls, a);
        generateLambdaTramp(cls);
    }

    // Generate function bodies.
    // Generic function templates (type_params, not an instance) are skipped —
    // only monomorphized instances are emitted (like generic classes).
    for (auto& f : tu.functions) {
        if (!f.type_params.empty() && !f.is_generic_inst) continue;
        current_type_params_.clear();
        for (size_t i = 0; i < f.type_params.size() && i < f.inst_type_args.size(); i++)
            current_type_params_.emplace_back(f.type_params[i], f.inst_type_args[i]);
        generateFuncDecl(f);
    }
    // Generic class templates are skipped (like generic function templates):
    // their bodies use type-param placeholders that would resolve method calls
    // on T to arbitrary classes (best-class search). Only monomorphized
    // instances (with substituted param types + current_type_params_) are emitted.
    for (auto& c : tu.classes) {
        if (!c.type_params.empty() && !c.is_generic_inst) continue;
        generateClass(c);
    }

    // Generate struct method function bodies (file-level)
    for (auto& st : tu.structs) {
        generateStructMethods(st);
    }
    // Generate struct method function bodies (nested in classes)
    for (auto& cls : tu.classes) {
        for (auto& st : cls.structs) {
            generateStructMethods(st);
        }
    }
}



// ---- ARC (§五-1) ----

// Is a TypeNode an ARC-counted reference slot? Only class instances and
// interface fat pointers count (§2 of docs/arc.md). string / arrays / slices /
// structs do NOT (value or arena-managed). Generic type-param placeholders are
// resolved via current_type_params_ before classifying.


// Emit myp_release(load(alloca)) for a local class-ref slot. Used at scope exit.




// trait 默认实现（§三-5）：接口方法带默认体 → 为省略该方法的实现类生成特化默认函数
// `__ifdef_<Iface>_<method>_<Class>`。this 绑定具体类，故 `this.method()` 在默认体
// 内静态解析到 `<Class>_<method>`（无需 vtable 查找）。类必须实现全部无体方法，
// 由 sema checkInterfaceImpl 保证（默认体引用的抽象方法必存在）。

// 关联类型（§三-5）：X::Item → 绑定类型。X 为具体类，或当前类型参数（T → 具体类，
// 经 current_type_params_ 解析）。无绑定 → nullopt（回落嵌套 struct 等既有路径）。

// 类未覆盖的接口默认方法 → 该类的特化默认函数（无则 nullptr）。








// Pre-declare a function: section method so its symbol exists before any body
// references it (cross-calls in the section were previously order-dependent:
// a method could only call methods declared earlier in the section).



// __lambda_N_tramp(void* self, A, B) -> R : calls __lambda_N___call((__lambda_N*)self, A, B)



// -- Generate a built-in Coro static method (direct runtime call) --
// The stdlib `Coro` class declares these methods with empty bodies; codegen
// emits the runtime call here so the __myp_coro_* intrinsics are never exposed
// to user code (sema does not register them either).

// -- Generate static action (no 'this' pointer) --

// -- Generate coroutine entry wrapper for an @coro method --
// void __myp_coro_entry_<Class>_<method>(void)
// Reads 'this' (slot 0) and params (slots 1..N) from the thread-local entry
// arg table and calls Class_method(this, params...). ucontext trampolines call
// this with no arguments.

// -- Entry wrapper for a top-level @coro function (no 'this' slot) --
// void __myp_coro_entry_name() {
//     args = __myp_coro_get_entry_arg(1..N);
//     name(args...);
// }
// h = __myp_coro_create();
// __myp_coro_set_entry_arg(0, this);
// __myp_coro_set_entry_arg(1+i, arg_i);
// __myp_coro_set_entry(h, ptrtoint(__myp_coro_entry_Class_method));
// __myp_coro_resume(h);   // first start
// return h;               // handle

// -- Generate struct methods --


// -- Generate class function: section --




// -- Mapping --

void CodeGen::createInitFunction() {
    if (!current_tu_ || current_tu_->mappings.empty()) return;
    auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {}, false);
    init_func_ = llvm::Function::Create(ft, llvm::Function::InternalLinkage, "__myp_init", module_.get());
}

void CodeGen::emitInitMappingCalls() {
    if (!init_func_) return;
    auto* bb = &init_func_->getEntryBlock();
    if (bb->empty()) {
        builder_.SetInsertPoint(bb);
    } else {
        bb = llvm::BasicBlock::Create(ctx_, "body", init_func_);
        builder_.SetInsertPoint(bb);
    }

    // Generate all global mapping registrations
    for (auto& m : current_tu_->mappings) generateMappingDecl(m, bb);

    builder_.CreateRetVoid();
}

void CodeGen::markNonMainFunctionsInternal() {
    for (auto& fn : module_->functions()) {
        if (fn.isDeclaration()) continue;   // runtime/C declarations stay external
        if (fn.getName() == "main") continue;  // C runtime entry point
        // Skip `used`/address-exposed functions? InternalLinkage still keeps the
        // address valid; only symbol visibility changes. Safe for callbacks.
        fn.setLinkage(llvm::GlobalValue::InternalLinkage);
    }
}

// -- Statements --

// (A a, B b) = tuple;  (declaration)   or  (a, b) = tuple;  (assignment)





// ---- GPU kernel body compilation helpers (Stage 4) ----

void CodeGen::collectExprIdentifiers(const Expr& expr, std::set<std::string>& out,
                                     std::set<std::string>& loop_decls) const {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            auto& id = static_cast<const IdentifierExpr&>(expr);
            if (loop_decls.find(id.name) == loop_decls.end())
                out.insert(id.name);
            break;
        }
        case ExprKind::BinaryOp: {
            auto& b = static_cast<const BinaryOpExpr&>(expr);
            collectExprIdentifiers(*b.lhs, out, loop_decls);
            collectExprIdentifiers(*b.rhs, out, loop_decls);
            break;
        }
        case ExprKind::UnaryOp: {
            auto& u = static_cast<const UnaryOpExpr&>(expr);
            collectExprIdentifiers(*u.operand, out, loop_decls);
            break;
        }
        case ExprKind::Convert: {
            auto& c = static_cast<const ConvertExpr&>(expr);
            collectExprIdentifiers(*c.operand, out, loop_decls);
            break;
        }
        case ExprKind::Call: {
            auto& c = static_cast<const CallExpr&>(expr);
            collectExprIdentifiers(*c.callee, out, loop_decls);
            for (auto& a : c.args)
                collectExprIdentifiers(*a, out, loop_decls);
            break;
        }
        case ExprKind::MemberAccess: {
            auto& m = static_cast<const MemberAccessExpr&>(expr);
            collectExprIdentifiers(*m.object, out, loop_decls);
            break;
        }
        case ExprKind::Subscript: {
            auto& s = static_cast<const SubscriptExpr&>(expr);
            collectExprIdentifiers(*s.array, out, loop_decls);
            collectExprIdentifiers(*s.index, out, loop_decls);
            break;
        }
        case ExprKind::Assignment: {
            auto& a = static_cast<const AssignmentExpr&>(expr);
            collectExprIdentifiers(*a.target, out, loop_decls);
            collectExprIdentifiers(*a.value, out, loop_decls);
            break;
        }
        case ExprKind::Ternary: {
            auto& t = static_cast<const TernaryExpr&>(expr);
            collectExprIdentifiers(*t.condition, out, loop_decls);
            collectExprIdentifiers(*t.true_expr, out, loop_decls);
            collectExprIdentifiers(*t.false_expr, out, loop_decls);
            break;
        }
        case ExprKind::Try:
        case ExprKind::Await:
            break;
        case ExprKind::TupleExpr:
            for (auto& el : static_cast<const TupleExpr&>(expr).elements)
                collectExprIdentifiers(*el, out, loop_decls);
            break;
        case ExprKind::ThisExpr:
        case ExprKind::IntegerLiteral:
        case ExprKind::FloatLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::StringLiteral:
        case ExprKind::NullLiteral:
        case ExprKind::NewExpr:
        case ExprKind::NewArrayExpr:
        case ExprKind::Range:
        case ExprKind::Lambda:
        case ExprKind::EnumVariant:
            break;
    }
}

void CodeGen::collectStmtIdentifiers(const Stmt& stmt, std::set<std::string>& out,
                                     std::set<std::string>& loop_decls) const {
    switch (stmt.kind) {
        case StmtKind::Block: {
            auto& b = static_cast<const BlockStmt&>(stmt);
            for (auto& s : b.statements)
                collectStmtIdentifiers(*s, out, loop_decls);
            break;
        }
        case StmtKind::DestructureStmt: {
            auto& ds = static_cast<const DestructureStmt&>(stmt);
            if (ds.is_decl) {
                std::function<void(const DestructureTarget&)> bind =
                    [&](const DestructureTarget& t) {
                        if (!t.name.empty()) { loop_decls.insert(t.name); return; }
                        for (auto& c : t.elements) bind(c);
                    };
                for (auto& c : ds.target.elements) bind(c);
            } else {
                std::function<void(const DestructureTarget&)> refs =
                    [&](const DestructureTarget& t) {
                        if (!t.name.empty()) {
                            if (loop_decls.find(t.name) == loop_decls.end())
                                out.insert(t.name);
                            return;
                        }
                        for (auto& c : t.elements) refs(c);
                    };
                for (auto& c : ds.target.elements) refs(c);
            }
            if (ds.value) collectExprIdentifiers(*ds.value, out, loop_decls);
            break;
        }
        case StmtKind::VarDeclStmt: {
            auto& vds = static_cast<const VarDeclStmt&>(stmt);
            for (auto& d : vds.decls) {
                loop_decls.insert(d.name);  // declared inside loop — NOT captured
                if (d.init_expr)
                    collectExprIdentifiers(*d.init_expr, out, loop_decls);
            }
            break;
        }
        case StmtKind::ExprStmt: {
            auto& es = static_cast<const ExprStmt&>(stmt);
            if (es.expression)
                collectExprIdentifiers(*es.expression, out, loop_decls);
            break;
        }
        case StmtKind::IfStmt: {
            auto& is = static_cast<const IfStmt&>(stmt);
            collectExprIdentifiers(*is.condition, out, loop_decls);
            collectStmtIdentifiers(*is.then_block, out, loop_decls);
            if (is.else_block)
                collectStmtIdentifiers(*is.else_block, out, loop_decls);
            break;
        }
        case StmtKind::WhileStmt: {
            auto& ws = static_cast<const WhileStmt&>(stmt);
            collectExprIdentifiers(*ws.condition, out, loop_decls);
            collectStmtIdentifiers(*ws.body, out, loop_decls);
            break;
        }
        case StmtKind::ForStmt: {
            auto& fs = static_cast<const ForStmt&>(stmt);
            if (fs.init) collectStmtIdentifiers(*fs.init, out, loop_decls);
            if (fs.condition) collectExprIdentifiers(*fs.condition, out, loop_decls);
            if (fs.step) collectExprIdentifiers(*fs.step, out, loop_decls);
            if (fs.body) collectStmtIdentifiers(*fs.body, out, loop_decls);
            break;
        }
        case StmtKind::ForInStmt: {
            auto& fis = static_cast<const ForInStmt&>(stmt);
            loop_decls.insert(fis.var_name);  // loop var — NOT captured
            collectExprIdentifiers(*fis.iterable, out, loop_decls);
            if (fis.body) collectStmtIdentifiers(*fis.body, out, loop_decls);
            break;
        }
        case StmtKind::ReturnStmt:
        case StmtKind::BreakStmt:
        case StmtKind::ContinueStmt:
        case StmtKind::AwaitStmt:
        case StmtKind::MappingStmt:
        case StmtKind::MatchStmt:
        case StmtKind::TryStmt:
            break;
    }
}

bool CodeGen::regionBodyMayEscape(const Stmt& body) const {
    std::unordered_set<std::string> local_refs;
    std::unordered_set<std::string> local_names;

    auto typeMayUseRegion = [](const TypeNode& type) {
        return type.isArray() || type.class_name == "slice";
    };

    std::function<void(const Stmt&)> collectLocals = [&](const Stmt& stmt) {
        switch (stmt.kind) {
            case StmtKind::Block:
                for (auto& child : static_cast<const BlockStmt&>(stmt).statements)
                    if (child) collectLocals(*child);
                break;
            case StmtKind::VarDeclStmt:
                for (auto& decl : static_cast<const VarDeclStmt&>(stmt).decls) {
                    local_names.insert(decl.name);
                    if (typeMayUseRegion(decl.type)) local_refs.insert(decl.name);
                }
                break;
            case StmtKind::IfStmt: {
                auto& value = static_cast<const IfStmt&>(stmt);
                if (value.then_block) collectLocals(*value.then_block);
                if (value.else_block) collectLocals(*value.else_block);
                break;
            }
            case StmtKind::WhileStmt: {
                auto& value = static_cast<const WhileStmt&>(stmt);
                if (value.body) collectLocals(*value.body);
                break;
            }
            case StmtKind::ForStmt: {
                auto& value = static_cast<const ForStmt&>(stmt);
                if (value.init) collectLocals(*value.init);
                if (value.body) collectLocals(*value.body);
                break;
            }
            case StmtKind::ForInStmt: {
                auto& value = static_cast<const ForInStmt&>(stmt);
                local_names.insert(value.var_name);
                if (typeMayUseRegion(value.var_type)) local_refs.insert(value.var_name);
                if (value.body) collectLocals(*value.body);
                break;
            }
            case StmtKind::TryStmt: {
                auto& value = static_cast<const TryStmt&>(stmt);
                if (value.try_block) collectLocals(*value.try_block);
                for (auto& clause : value.catches)
                    if (clause.block) collectLocals(*clause.block);
                if (value.finally_block) collectLocals(*value.finally_block);
                break;
            }
            default:
                break;
        }
    };
    collectLocals(body);

    std::function<bool(const Expr&)> carriesRegionRef = [&](const Expr& expr) {
        if (expr.type && typeMayUseRegion(*expr.type)) return true;
        switch (expr.kind) {
            case ExprKind::Identifier:
                return local_refs.count(static_cast<const IdentifierExpr&>(expr).name) != 0;
            case ExprKind::NewExpr:
                return static_cast<const NewExpr&>(expr).class_name == "slice";
            case ExprKind::NewArrayExpr:
                return true;
            case ExprKind::Assignment:
                return carriesRegionRef(*static_cast<const AssignmentExpr&>(expr).value);
            case ExprKind::Ternary: {
                auto& value = static_cast<const TernaryExpr&>(expr);
                return carriesRegionRef(*value.true_expr) || carriesRegionRef(*value.false_expr);
            }
            case ExprKind::TupleExpr:
                for (auto& element : static_cast<const TupleExpr&>(expr).elements)
                    if (carriesRegionRef(*element)) return true;
                return false;
            default:
                return false;
        }
    };

    std::function<bool(const Expr&)> exprEscapes = [&](const Expr& expr) {
        switch (expr.kind) {
            case ExprKind::Call: {
                auto& call = static_cast<const CallExpr&>(expr);
                for (auto& arg : call.args)
                    if (carriesRegionRef(*arg) || exprEscapes(*arg)) return true;
                return exprEscapes(*call.callee);
            }
            case ExprKind::Assignment: {
                auto& assignment = static_cast<const AssignmentExpr&>(expr);
                bool external_target = assignment.target->kind == ExprKind::MemberAccess ||
                    assignment.target->kind == ExprKind::Subscript;
                if (assignment.target->kind == ExprKind::Identifier) {
                    auto& id = static_cast<const IdentifierExpr&>(*assignment.target);
                    external_target = local_names.count(id.name) == 0;
                }
                if (external_target && carriesRegionRef(*assignment.value)) return true;
                return exprEscapes(*assignment.target) || exprEscapes(*assignment.value);
            }
            case ExprKind::BinaryOp: {
                auto& value = static_cast<const BinaryOpExpr&>(expr);
                return exprEscapes(*value.lhs) || exprEscapes(*value.rhs);
            }
            case ExprKind::UnaryOp:
                return exprEscapes(*static_cast<const UnaryOpExpr&>(expr).operand);
            case ExprKind::Convert:
                return exprEscapes(*static_cast<const ConvertExpr&>(expr).operand);
            case ExprKind::MemberAccess:
                return exprEscapes(*static_cast<const MemberAccessExpr&>(expr).object);
            case ExprKind::Subscript: {
                auto& value = static_cast<const SubscriptExpr&>(expr);
                return exprEscapes(*value.array) || exprEscapes(*value.index);
            }
            case ExprKind::Ternary: {
                auto& value = static_cast<const TernaryExpr&>(expr);
                return exprEscapes(*value.condition) || exprEscapes(*value.true_expr) ||
                    exprEscapes(*value.false_expr);
            }
            case ExprKind::TupleExpr:
                for (auto& element : static_cast<const TupleExpr&>(expr).elements)
                    if (exprEscapes(*element)) return true;
                return false;
            default:
                return false;
        }
    };

    std::function<bool(const Stmt&)> stmtEscapes = [&](const Stmt& stmt) {
        switch (stmt.kind) {
            case StmtKind::Block:
                for (auto& child : static_cast<const BlockStmt&>(stmt).statements)
                    if (child && stmtEscapes(*child)) return true;
                return false;
            case StmtKind::VarDeclStmt:
                for (auto& decl : static_cast<const VarDeclStmt&>(stmt).decls)
                    if (decl.init_expr && exprEscapes(*decl.init_expr)) return true;
                return false;
            case StmtKind::ExprStmt: {
                auto& value = static_cast<const ExprStmt&>(stmt);
                return value.expression && exprEscapes(*value.expression);
            }
            case StmtKind::IfStmt: {
                auto& value = static_cast<const IfStmt&>(stmt);
                return exprEscapes(*value.condition) ||
                    (value.then_block && stmtEscapes(*value.then_block)) ||
                    (value.else_block && stmtEscapes(*value.else_block));
            }
            case StmtKind::WhileStmt: {
                auto& value = static_cast<const WhileStmt&>(stmt);
                return exprEscapes(*value.condition) ||
                    (value.body && stmtEscapes(*value.body));
            }
            case StmtKind::ForStmt: {
                auto& value = static_cast<const ForStmt&>(stmt);
                return (value.init && stmtEscapes(*value.init)) ||
                    (value.condition && exprEscapes(*value.condition)) ||
                    (value.step && exprEscapes(*value.step)) ||
                    (value.body && stmtEscapes(*value.body));
            }
            case StmtKind::ForInStmt: {
                auto& value = static_cast<const ForInStmt&>(stmt);
                return exprEscapes(*value.iterable) ||
                    (value.body && stmtEscapes(*value.body));
            }
            case StmtKind::ReturnStmt: {
                auto& value = static_cast<const ReturnStmt&>(stmt);
                return value.value && carriesRegionRef(*value.value);
            }
            case StmtKind::ThrowStmt: {
                auto& value = static_cast<const ThrowStmt&>(stmt);
                return value.expr && carriesRegionRef(*value.expr);
            }
            case StmtKind::TryStmt: {
                auto& value = static_cast<const TryStmt&>(stmt);
                if (value.try_block && stmtEscapes(*value.try_block)) return true;
                for (auto& clause : value.catches)
                    if (clause.block && stmtEscapes(*clause.block)) return true;
                return value.finally_block && stmtEscapes(*value.finally_block);
            }
            default:
                return false;
        }
    };

    return stmtEscapes(body);
}






// for (x in coll) — 集合迭代（§四-2）。sema 已解析 iter_kind：
//   0=class(size/get), 1=固定数组, 2=slice, 3=range



// ---------------------------------------------------------------------------
// CUDA libdevice support for @gpu for kernels.
//
// The GPU code path emits calls to CUDA libdevice device functions (__nv_sin,
// __nv_cos, __nv_exp, ...). These are not provided by the CUDA driver itself,
// so we link NVIDIA's libdevice.10.bc bitcode into the kernel module at
// compile time (the same bitcode nvcc links against). The resulting PTX is
// fully self-contained and can be loaded with cuModuleLoadData — no runtime
// JIT-linking (cuLink) is required.
// ---------------------------------------------------------------------------

// Locate the CUDA libdevice bitcode file. Returns empty string if not found.

// Resolve __nvvm_reflect("...") calls to constants. libdevice uses this NVVM
// intrinsic for compile-time configuration (FTZ, arch, ...); we replace all
// calls with 0 (no fast-math/FTZ, generic arch path) which is always safe.

// Link CUDA libdevice into a GPU kernel module so __nv_* device functions
// become self-contained (bodies inlined/emitted in the resulting PTX).
// Returns true on success; leaves the module untouched on failure.




// 修复 #4a：函数返回固定数组栈变量（如 Fs.listDir 返回 string[1024]）时，
// 指向的是本函数栈帧 → 悬垂/共享存储（嵌套调用覆写）。这里堆拷贝一份再返回。

// -- Expressions --





// 逻辑 && / || 短路求值：结果已定时不求值右操作数（修复原 CreateAnd/CreateOr 位运算非短路）。
// && : l 为 false → 结果 false，跳过 rhs；l 为 true → 求值 rhs
// || : l 为 true  → 结果 true，跳过 rhs；l 为 false → 求值 rhs


// 显式类型转换：uint8(x) / long(x) / double(x)。
// 规则：宽→窄截断；窄→宽按源符号（无符号 ZExt / 有符号 SExt）；int↔float 转换。


// Resolve a call's callee to its return TypeNode and report whether it is an
// ARC-counted class / class-array reference (string / interface / scalar = no).






















// Build the global event name→id table once (for runtime-constructed timers,
// e.g. stdlib Timer.init passes a runtime event-name string).




// §五-5 形态3: is `callee` a call target to an @async-annotated function or
// static class method (an await-able async IO operation)?




// -- Enum variant codegen: enum variant ref → i32 constant (variant index) --

// -- Match statement codegen: if/else chain comparing the enum discriminant --




// -- Runtime --
void CodeGen::declareRuntimeFunctions() {
    auto* v = llvm::Type::getVoidTy(ctx_);
    auto* p = llvm::PointerType::get(ctx_, 0);
    auto* i32 = llvm::Type::getInt32Ty(ctx_);
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* d = llvm::Type::getDoubleTy(ctx_);

    runtime_print_ = llvm::Function::Create(llvm::FunctionType::get(v, {p}, false), llvm::Function::ExternalLinkage, "myp_print", module_.get());
    runtime_println_ = llvm::Function::Create(llvm::FunctionType::get(v, {p}, false), llvm::Function::ExternalLinkage, "myp_println", module_.get());
    runtime_print_int_ = llvm::Function::Create(llvm::FunctionType::get(v, {i32}, false), llvm::Function::ExternalLinkage, "myp_print_int", module_.get());
    runtime_print_long_ = llvm::Function::Create(llvm::FunctionType::get(v, {i64}, false), llvm::Function::ExternalLinkage, "myp_print_long", module_.get());
    runtime_print_float_ = llvm::Function::Create(llvm::FunctionType::get(v, {d}, false), llvm::Function::ExternalLinkage, "myp_print_float", module_.get());
    runtime_print_bool_ = llvm::Function::Create(llvm::FunctionType::get(v, {i32}, false), llvm::Function::ExternalLinkage, "myp_print_bool", module_.get());
    runtime_now_ms_ = llvm::Function::Create(llvm::FunctionType::get(i64, {}, false), llvm::Function::ExternalLinkage, "myp_now_ms", module_.get());
    runtime_sleep_ms_ = llvm::Function::Create(llvm::FunctionType::get(v, {i64}, false), llvm::Function::ExternalLinkage, "myp_sleep_ms", module_.get());

    // Math functions
    auto* dd = llvm::Type::getDoubleTy(ctx_);
    runtime_math_sqrt_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_sqrt", module_.get());
    runtime_math_abs_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_abs", module_.get());
    runtime_math_floor_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_floor", module_.get());
    runtime_math_ceil_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_ceil", module_.get());
    runtime_math_sin_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_sin", module_.get());
    runtime_math_cos_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_cos", module_.get());
    runtime_math_tan_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_tan", module_.get());
    runtime_math_asin_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_asin", module_.get());
    runtime_math_acos_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_acos", module_.get());
    runtime_math_atan_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_atan", module_.get());
    runtime_math_atan2_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd, dd}, false), llvm::Function::ExternalLinkage, "myp_math_atan2", module_.get());
    runtime_math_sinh_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_sinh", module_.get());
    runtime_math_cosh_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_cosh", module_.get());
    runtime_math_tanh_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_tanh", module_.get());
    runtime_math_exp_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_exp", module_.get());
    runtime_math_log_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd}, false), llvm::Function::ExternalLinkage, "myp_math_log", module_.get());
    runtime_math_pow_ = llvm::Function::Create(llvm::FunctionType::get(dd, {dd, dd}, false), llvm::Function::ExternalLinkage, "myp_math_pow", module_.get());
    runtime_math_abs_int_ = llvm::Function::Create(llvm::FunctionType::get(i32, {i32}, false), llvm::Function::ExternalLinkage, "myp_math_abs_int", module_.get());

    // File I/O functions
    auto* i8ptr = llvm::PointerType::get(ctx_, 0);
    runtime_io_fopen_ = llvm::Function::Create(llvm::FunctionType::get(i32, {i8ptr, i8ptr}, false), llvm::Function::ExternalLinkage, "myp_io_fopen", module_.get());
    runtime_io_fclose_ = llvm::Function::Create(llvm::FunctionType::get(v, {}, false), llvm::Function::ExternalLinkage, "myp_io_fclose", module_.get());
    runtime_io_read_line_ = llvm::Function::Create(llvm::FunctionType::get(i8ptr, {}, false), llvm::Function::ExternalLinkage, "myp_io_read_line", module_.get());
    runtime_io_write_ = llvm::Function::Create(llvm::FunctionType::get(v, {i8ptr}, false), llvm::Function::ExternalLinkage, "myp_io_write", module_.get());
    runtime_io_write_line_ = llvm::Function::Create(llvm::FunctionType::get(v, {i8ptr}, false), llvm::Function::ExternalLinkage, "myp_io_write_line", module_.get());
    runtime_io_has_next_ = llvm::Function::Create(llvm::FunctionType::get(i32, {}, false), llvm::Function::ExternalLinkage, "myp_io_has_next", module_.get());

    // Binary I/O
    runtime_io_read_byte_ = llvm::Function::Create(llvm::FunctionType::get(i32, {}, false), llvm::Function::ExternalLinkage, "myp_io_read_byte", module_.get());
    runtime_io_read_i32be_ = llvm::Function::Create(llvm::FunctionType::get(i32, {}, false), llvm::Function::ExternalLinkage, "myp_io_read_i32be", module_.get());
    runtime_io_seek_ = llvm::Function::Create(llvm::FunctionType::get(i32, {i32, i32}, false), llvm::Function::ExternalLinkage, "myp_io_seek", module_.get());
    runtime_io_write_byte_ = llvm::Function::Create(llvm::FunctionType::get(i32, {i32}, false), llvm::Function::ExternalLinkage, "myp_io_write_byte", module_.get());
    runtime_io_write_i32be_ = llvm::Function::Create(llvm::FunctionType::get(i32, {i32}, false), llvm::Function::ExternalLinkage, "myp_io_write_i32be", module_.get());
    runtime_io_write_double_ = llvm::Function::Create(llvm::FunctionType::get(i32, {d}, false), llvm::Function::ExternalLinkage, "myp_io_write_double", module_.get());
    runtime_io_read_double_ = llvm::Function::Create(llvm::FunctionType::get(d, {}, false), llvm::Function::ExternalLinkage, "myp_io_read_double", module_.get());
    runtime_io_current_handle_ = llvm::Function::Create(llvm::FunctionType::get(i32, {}, false), llvm::Function::ExternalLinkage, "myp_io_current_handle", module_.get());
    runtime_io_select_ = llvm::Function::Create(llvm::FunctionType::get(v, {i32}, false), llvm::Function::ExternalLinkage, "myp_io_select", module_.get());

    // Read line from stdin
    runtime_read_line_ = llvm::Function::Create(llvm::FunctionType::get(p, {}, false), llvm::Function::ExternalLinkage, "myp_read_line", module_.get());

    // String equality
    runtime_str_eq_ = llvm::Function::Create(llvm::FunctionType::get(i32, {p, p}, false), llvm::Function::ExternalLinkage, "myp_str_eq", module_.get());

    // Non-blocking keyboard
    runtime_kbhit_ = llvm::Function::Create(llvm::FunctionType::get(i32, {}, false), llvm::Function::ExternalLinkage, "myp_kbhit", module_.get());
    runtime_getch_ = llvm::Function::Create(llvm::FunctionType::get(i32, {}, false), llvm::Function::ExternalLinkage, "myp_getch", module_.get());

    // Flush
    runtime_flush_ = llvm::Function::Create(llvm::FunctionType::get(v, {}, false), llvm::Function::ExternalLinkage, "myp_flush", module_.get());

    // Terminal size (TUI)
    runtime_term_width_ = llvm::Function::Create(llvm::FunctionType::get(i32, {}, false), llvm::Function::ExternalLinkage, "myp_term_width", module_.get());
    runtime_term_height_ = llvm::Function::Create(llvm::FunctionType::get(i32, {}, false), llvm::Function::ExternalLinkage, "myp_term_height", module_.get());

    // String length
    runtime_strlen_ = llvm::Function::Create(llvm::FunctionType::get(i32, {p}, false), llvm::Function::ExternalLinkage, "myp_strlen", module_.get());
    // Integer to single-char string
    runtime_chr_ = llvm::Function::Create(llvm::FunctionType::get(p, {i32}, false), llvm::Function::ExternalLinkage, "myp_chr", module_.get());
    runtime_ord_ = llvm::Function::Create(llvm::FunctionType::get(i32, {p}, false), llvm::Function::ExternalLinkage, "myp_ord", module_.get());

    // §五-4 RTTI: type info from the object header (type_id → __myp_type_name_table)
    runtime_type_id_ = llvm::Function::Create(llvm::FunctionType::get(i32, {p}, false), llvm::Function::ExternalLinkage, "myp_obj_type_id", module_.get());
    runtime_type_name_ = llvm::Function::Create(llvm::FunctionType::get(p, {p}, false), llvm::Function::ExternalLinkage, "myp_obj_type_name", module_.get());

    // String to double
    runtime_atof_ = llvm::Function::Create(llvm::FunctionType::get(d, {p}, false), llvm::Function::ExternalLinkage, "myp_atof", module_.get());

    // Event system
    runtime_event_register_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {i32, p, p}, false),
        llvm::Function::ExternalLinkage, "myp_event_register", module_.get());
    runtime_event_fire_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {i32, p, p}, false),
        llvm::Function::ExternalLinkage, "myp_event_fire", module_.get());
    runtime_event_process_all_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {}, false),
        llvm::Function::ExternalLinkage, "myp_event_process_all", module_.get());
    runtime_event_push_scope_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {}, false),
        llvm::Function::ExternalLinkage, "myp_event_push_scope", module_.get());
    runtime_event_pop_scope_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {}, false),
        llvm::Function::ExternalLinkage, "myp_event_pop_scope", module_.get());

    // Thread system: myp_thread_create(startup_fn, startup_arg) -> thread*
    runtime_thread_create_ = llvm::Function::Create(
        llvm::FunctionType::get(p, {p, p}, false),
        llvm::Function::ExternalLinkage, "myp_thread_create", module_.get());
    runtime_thread_run_loop_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_thread_run_loop", module_.get());
    runtime_thread_stop_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_thread_stop", module_.get());
    runtime_thread_destroy_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_thread_destroy", module_.get());
    runtime_thread_assoc_instance_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p, p}, false),
        llvm::Function::ExternalLinkage, "myp_thread_associate_instance", module_.get());

    // Timer system
    runtime_timer_create_ = llvm::Function::Create(
        llvm::FunctionType::get(i32, {i32, p, i64, i64, i64}, false),
        llvm::Function::ExternalLinkage, "myp_timer_create", module_.get());

    // Error handling
    // Use a global jmp_buf (simpler than stack allocation, and longjmp
    // must work from any nested call depth).
    auto* jb_type = llvm::ArrayType::get(i64, 25); // 200 bytes on x86-64
    jmp_buf_type_ = llvm::StructType::create(ctx_, "myp_jmp_buf");
    jmp_buf_type_->setBody({jb_type});

    // Global jmp_buf instance
    global_jmp_buf_ = new llvm::GlobalVariable(*module_, jmp_buf_type_, false,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantAggregateZero::get(jmp_buf_type_), "__myp_jmpbuf");

    // Declare system setjmp as: int setjmp(ptr)
    // MUST carry the returns_twice attribute: the optimizer otherwise assumes
    // setjmp returns exactly once and miscompiles try/catch (longjmp back to a
    // setjmp is a second "return" that optimization passes would not preserve).
    runtime_setjmp_ = llvm::Function::Create(
        llvm::FunctionType::get(i32, {llvm::PointerType::get(ctx_, 0)}, false),
        llvm::Function::ExternalLinkage, "setjmp", module_.get());
    runtime_setjmp_->addFnAttr(llvm::Attribute::ReturnsTwice);

    // Longjmp: void longjmp(ptr, int)  (noreturn).
    // Use __myp_longjmp (runtime wrapper) instead of the raw system longjmp so
    // ASan is told about the non-returning jump (__asan_handle_no_return)
    // before it happens — otherwise ASan reports false-positive frame-mismatch
    // / handle-no-return warnings whenever a coroutine raises an exception.
    runtime_longjmp_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {llvm::PointerType::get(ctx_, 0), i32}, false),
        llvm::Function::ExternalLinkage, "__myp_longjmp", module_.get());
    runtime_longjmp_->addFnAttr(llvm::Attribute::NoReturn);

    // myp_throw(str) — saves the error message (longjmp is generated inline).
    // MUST NOT be marked noreturn: it returns after recording the message; the
    // generated IR then calls __myp_longjmp to reach the handler. Marking it
    // noreturn made the optimizer drop the get_jmpbuf+longjmp calls after it
    // (treated as unreachable), breaking try/catch under -O1/-O2.
    runtime_throw_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_throw", module_.get());

    runtime_get_error_ = llvm::Function::Create(
        llvm::FunctionType::get(p, {}, false),
        llvm::Function::ExternalLinkage, "myp_get_error", module_.get());

    // Exception handler stack (thread-local) — per-try jmp_buf push/pop/get.
    runtime_exception_push_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_exception_push", module_.get());
    runtime_exception_pop_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {}, false),
        llvm::Function::ExternalLinkage, "myp_exception_pop", module_.get());
    runtime_exception_get_jmpbuf_ = llvm::Function::Create(
        llvm::FunctionType::get(p, {}, false),
        llvm::Function::ExternalLinkage, "myp_exception_get_jmpbuf", module_.get());
    runtime_throw_object_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p, i32}, false),
        llvm::Function::ExternalLinkage, "myp_throw_object", module_.get());
    runtime_exception_get_type_ = llvm::Function::Create(
        llvm::FunctionType::get(i32, {}, false),
        llvm::Function::ExternalLinkage, "myp_exception_get_type", module_.get());
    runtime_exception_get_object_ = llvm::Function::Create(
        llvm::FunctionType::get(p, {}, false),
        llvm::Function::ExternalLinkage, "myp_exception_get_object", module_.get());

    // Test framework runtime functions
    runtime_assert_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {i32}, false),
        llvm::Function::ExternalLinkage, "myp_assert", module_.get());
    runtime_assert_eq_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {i32, i32}, false),
        llvm::Function::ExternalLinkage, "myp_assert_eq", module_.get());
    runtime_assert_str_eq_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p, p}, false),
        llvm::Function::ExternalLinkage, "myp_assert_str_eq", module_.get());
    runtime_assert_neq_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {i32, i32}, false),
        llvm::Function::ExternalLinkage, "myp_assert_neq", module_.get());
    runtime_assert_long_eq_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {i64, i64}, false),
        llvm::Function::ExternalLinkage, "myp_assert_long_eq", module_.get());
    runtime_assert_str_neq_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p, p}, false),
        llvm::Function::ExternalLinkage, "myp_assert_str_neq", module_.get());
    runtime_test_report_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p, i32}, false),
        llvm::Function::ExternalLinkage, "myp_test_report", module_.get());
    runtime_test_fail_msg_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_test_fail_msg", module_.get());
    runtime_test_summary_ = llvm::Function::Create(
        llvm::FunctionType::get(i32, {i32}, false),
        llvm::Function::ExternalLinkage, "myp_test_summary", module_.get());
    runtime_assert_long_neq_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {i64, i64}, false),
        llvm::Function::ExternalLinkage, "myp_assert_long_neq", module_.get());
    auto* d_ty = llvm::Type::getDoubleTy(ctx_);
    runtime_assert_float_neq_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {d_ty, d_ty, d_ty}, false),
        llvm::Function::ExternalLinkage, "myp_assert_float_neq", module_.get());
    runtime_assert_null_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_assert_null", module_.get());
    runtime_assert_not_null_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_assert_not_null", module_.get());

    // Memory allocator
    runtime_alloc_ = llvm::Function::Create(
        llvm::FunctionType::get(p, {i64}, false),
        llvm::Function::ExternalLinkage, "myp_alloc", module_.get());
    runtime_free_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_free", module_.get());
    runtime_free_all_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {}, false),
        llvm::Function::ExternalLinkage, "myp_free_all", module_.get());
    // ARC (§五-1): myp_alloc_object(size, type_id) -> data ptr (header before it)
    runtime_alloc_object_ = llvm::Function::Create(
        llvm::FunctionType::get(p, {i64, llvm::Type::getInt32Ty(ctx_)}, false),
        llvm::Function::ExternalLinkage, "myp_alloc_object", module_.get());
    runtime_retain_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_retain", module_.get());
    runtime_release_ = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getInt32Ty(ctx_), {p}, false),
        llvm::Function::ExternalLinkage, "myp_release", module_.get());
    runtime_free_object_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_free_object", module_.get());

    // Thread pool functions
    // myp_pool_ensure_global() -> myp_pool_t*
    runtime_pool_ensure_ = llvm::Function::Create(
        llvm::FunctionType::get(p, {}, false),
        llvm::Function::ExternalLinkage, "myp_pool_ensure_global", module_.get());
    // myp_pool_parallel_for(myp_pool_t*, int start, int end, int step, void (*fn)(int, void*), void* arg)
    auto* fn_ptr_ty = llvm::PointerType::get(ctx_, 0);
    runtime_parallel_for_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p, i32, i32, i32, fn_ptr_ty, p}, false),
        llvm::Function::ExternalLinkage, "myp_pool_parallel_for", module_.get());

    // Build intrinsic name → function map
    intrinsic_map_["__myp_print_int"] = runtime_print_int_;
    intrinsic_map_["__myp_print_long"] = runtime_print_long_;
    intrinsic_map_["__myp_print"] = runtime_print_;
    intrinsic_map_["__myp_println"] = runtime_println_;
    intrinsic_map_["__myp_print_float"] = runtime_print_float_;
    intrinsic_map_["__myp_print_bool"] = runtime_print_bool_;
    intrinsic_map_["__myp_now_ms"] = runtime_now_ms_;
    intrinsic_map_["__myp_sleep_ms"] = runtime_sleep_ms_;
    intrinsic_map_["__myp_math_sqrt"] = runtime_math_sqrt_;
    intrinsic_map_["__myp_math_abs"] = runtime_math_abs_;
    intrinsic_map_["__myp_math_floor"] = runtime_math_floor_;
    intrinsic_map_["__myp_math_ceil"] = runtime_math_ceil_;
    intrinsic_map_["__myp_math_sin"] = runtime_math_sin_;
    intrinsic_map_["__myp_math_cos"] = runtime_math_cos_;
    intrinsic_map_["__myp_math_tan"] = runtime_math_tan_;
    intrinsic_map_["__myp_math_asin"] = runtime_math_asin_;
    intrinsic_map_["__myp_math_acos"] = runtime_math_acos_;
    intrinsic_map_["__myp_math_atan"] = runtime_math_atan_;
    intrinsic_map_["__myp_math_atan2"] = runtime_math_atan2_;
    intrinsic_map_["__myp_math_sinh"] = runtime_math_sinh_;
    intrinsic_map_["__myp_math_cosh"] = runtime_math_cosh_;
    intrinsic_map_["__myp_math_tanh"] = runtime_math_tanh_;
    intrinsic_map_["__myp_math_exp"] = runtime_math_exp_;
    intrinsic_map_["__myp_math_log"] = runtime_math_log_;
    intrinsic_map_["__myp_math_pow"] = runtime_math_pow_;
    intrinsic_map_["__myp_math_abs_int"] = runtime_math_abs_int_;
    intrinsic_map_["__myp_io_fopen"] = runtime_io_fopen_;
    intrinsic_map_["__myp_io_fclose"] = runtime_io_fclose_;
    intrinsic_map_["__myp_io_read_line"] = runtime_io_read_line_;
    intrinsic_map_["__myp_io_write"] = runtime_io_write_;
    intrinsic_map_["__myp_io_write_line"] = runtime_io_write_line_;
    intrinsic_map_["__myp_io_has_next"] = runtime_io_has_next_;
    intrinsic_map_["__myp_io_read_byte"] = runtime_io_read_byte_;
    intrinsic_map_["__myp_io_read_i32be"] = runtime_io_read_i32be_;
    intrinsic_map_["__myp_io_seek"] = runtime_io_seek_;
    intrinsic_map_["__myp_io_write_byte"] = runtime_io_write_byte_;
    intrinsic_map_["__myp_io_write_i32be"] = runtime_io_write_i32be_;
    intrinsic_map_["__myp_io_write_double"] = runtime_io_write_double_;
    intrinsic_map_["__myp_io_read_double"] = runtime_io_read_double_;
    intrinsic_map_["__myp_io_current_handle"] = runtime_io_current_handle_;
    intrinsic_map_["__myp_io_select"] = runtime_io_select_;
    intrinsic_map_["__myp_read_line"] = runtime_read_line_;
    intrinsic_map_["__myp_kbhit"] = runtime_kbhit_;
    intrinsic_map_["__myp_getch"] = runtime_getch_;
    intrinsic_map_["__myp_flush"] = runtime_flush_;
    intrinsic_map_["__myp_term_width"] = runtime_term_width_;
    intrinsic_map_["__myp_term_height"] = runtime_term_height_;
    intrinsic_map_["__myp_strlen"] = runtime_strlen_;
    intrinsic_map_["__myp_chr"] = runtime_chr_;
    intrinsic_map_["__myp_ord"] = runtime_ord_;
    intrinsic_map_["__myp_atof"] = runtime_atof_;
    // §五-4 RTTI intrinsics
    intrinsic_map_["__myp_type_id"] = runtime_type_id_;
    intrinsic_map_["__myp_type_name"] = runtime_type_name_;
    // test intrinsics
    intrinsic_map_["__myp_assert"] = runtime_assert_;
    intrinsic_map_["__myp_assert_eq"] = runtime_assert_eq_;
    intrinsic_map_["__myp_assert_neq"] = runtime_assert_neq_;
    intrinsic_map_["__myp_assert_long_eq"] = runtime_assert_long_eq_;
    intrinsic_map_["__myp_assert_str_eq"] = runtime_assert_str_eq_;
    intrinsic_map_["__myp_assert_str_neq"] = runtime_assert_str_neq_;
    intrinsic_map_["__myp_test_report"] = runtime_test_report_;
    intrinsic_map_["__myp_test_fail_msg"] = runtime_test_fail_msg_;
    intrinsic_map_["__myp_test_summary"] = runtime_test_summary_;
    intrinsic_map_["__myp_assert_long_neq"] = runtime_assert_long_neq_;
    intrinsic_map_["__myp_assert_float_neq"] = runtime_assert_float_neq_;
    intrinsic_map_["__myp_assert_null"] = runtime_assert_null_;
    intrinsic_map_["__myp_assert_not_null"] = runtime_assert_not_null_;
    // __myp_throw is handled specially in generateCall (calls myp_throw + longjmp)
    intrinsic_map_["now"] = runtime_now_ms_;
    intrinsic_map_["sleep"] = runtime_sleep_ms_;

    // GPU / CUDA runtime
    auto* gpu_init_ft = llvm::FunctionType::get(i32, {}, false);
    runtime_gpu_init_ = llvm::Function::Create(gpu_init_ft,
        llvm::Function::ExternalLinkage, "myp_gpu_init", module_.get());
    intrinsic_map_["__myp_cuda_available"] = runtime_gpu_init_;

    auto* gpu_alloc_ft = llvm::FunctionType::get(p, {i64}, false);
    runtime_gpu_alloc_ = llvm::Function::Create(gpu_alloc_ft,
        llvm::Function::ExternalLinkage, "myp_gpu_alloc", module_.get());

    auto* gpu_free_ft = llvm::FunctionType::get(v, {p}, false);
    runtime_gpu_free_ = llvm::Function::Create(gpu_free_ft,
        llvm::Function::ExternalLinkage, "myp_gpu_free", module_.get());

    auto* gpu_to_dev_ft = llvm::FunctionType::get(v, {p, p, i64}, false);
    runtime_gpu_to_device_ = llvm::Function::Create(gpu_to_dev_ft,
        llvm::Function::ExternalLinkage, "myp_gpu_to_device", module_.get());

    auto* gpu_to_host_ft = llvm::FunctionType::get(v, {p, p, i64}, false);
    runtime_gpu_to_host_ = llvm::Function::Create(gpu_to_host_ft,
        llvm::Function::ExternalLinkage, "myp_gpu_to_host", module_.get());

    auto* gpu_load_ft = llvm::FunctionType::get(p, {p, p}, false);
    runtime_gpu_load_kernel_ = llvm::Function::Create(gpu_load_ft,
        llvm::Function::ExternalLinkage, "myp_gpu_load_kernel", module_.get());

    auto* gpu_launch_ft = llvm::FunctionType::get(i32, {p, i32, i32, p, i32}, false);
    runtime_gpu_launch_ = llvm::Function::Create(gpu_launch_ft,
        llvm::Function::ExternalLinkage, "myp_gpu_launch", module_.get());

    auto* gpu_destroy_ft = llvm::FunctionType::get(v, {p}, false);
    runtime_gpu_destroy_kernel_ = llvm::Function::Create(gpu_destroy_ft,
        llvm::Function::ExternalLinkage, "myp_gpu_destroy_kernel", module_.get());

    // CUDA device info intrinsics
    runtime_cuda_count_ = llvm::Function::Create(llvm::FunctionType::get(i32, {}, false),
        llvm::Function::ExternalLinkage, "myp_gpu_device_count", module_.get());
    intrinsic_map_["__myp_cuda_count"] = runtime_cuda_count_;
    runtime_cuda_name_ = llvm::Function::Create(llvm::FunctionType::get(p, {}, false),
        llvm::Function::ExternalLinkage, "myp_gpu_device_name", module_.get());
    intrinsic_map_["__myp_cuda_name"] = runtime_cuda_name_;
    runtime_cuda_memory_ = llvm::Function::Create(llvm::FunctionType::get(i64, {}, false),
        llvm::Function::ExternalLinkage, "myp_gpu_device_memory", module_.get());
    intrinsic_map_["__myp_cuda_memory"] = runtime_cuda_memory_;
    runtime_cuda_capability_ = llvm::Function::Create(llvm::FunctionType::get(i32, {}, false),
        llvm::Function::ExternalLinkage, "myp_gpu_compute_capability", module_.get());
    intrinsic_map_["__myp_cuda_capability"] = runtime_cuda_capability_;
    runtime_cuda_multiprocessors_ = llvm::Function::Create(llvm::FunctionType::get(i32, {}, false),
        llvm::Function::ExternalLinkage, "myp_gpu_multi_processors", module_.get());
    intrinsic_map_["__myp_cuda_multiprocessors"] = runtime_cuda_multiprocessors_;
    runtime_cuda_max_threads_ = llvm::Function::Create(llvm::FunctionType::get(i32, {}, false),
        llvm::Function::ExternalLinkage, "myp_gpu_max_threads_per_block", module_.get());
    intrinsic_map_["__myp_cuda_max_threads"] = runtime_cuda_max_threads_;
    runtime_cuda_warp_ = llvm::Function::Create(llvm::FunctionType::get(i32, {}, false),
        llvm::Function::ExternalLinkage, "myp_gpu_warp_size", module_.get());
    intrinsic_map_["__myp_cuda_warp"] = runtime_cuda_warp_;
}

// -- Output --
bool CodeGen::writeObjectFile(const std::string& p, int opt_level) {
    // Set target triple + datalayout BEFORE the optimization pipeline so that
    // target-dependent passes (LoopVectorize, etc.) get proper TTI / cost model.
    // Reuse the same TargetMachine for optimization and object emission.
    llvm::Triple host_triple(llvm::sys::getDefaultTargetTriple());
    module_->setTargetTriple(host_triple);
    std::string err;
    auto* tgt = llvm::TargetRegistry::lookupTarget(host_triple.getTriple(), err);
    if (!tgt) { diag_.error(SourceRange{}, "target: " + err); return false; }
    std::unique_ptr<llvm::TargetMachine> tm(tgt->createTargetMachine(
        host_triple, "generic", "", llvm::TargetOptions{}, llvm::Reloc::PIC_));
    if (!tm) { diag_.error(SourceRange{}, "cannot create target machine"); return false; }
    module_->setDataLayout(tm->createDataLayout());
    // ---- IR optimization pipeline (-O1/-O2/-O3) ----
    // Mark the pool worker-id accessor readnone/nounwind/willreturn so the
    // optimizer can hoist Parallel.workerId() out of @parallel for chunk
    // loops (thread-local value, constant for the loop's duration). Applied
    // at this single choke point so it covers whichever path declared the
    // function (kernel expr, @extern wrapper, or generic extern call).
    auto markWorkerIdReadNone = [this]() {
        for (const char* name : {"myp_pool_worker_id", "__myp_pool_worker_id"}) {
            if (auto* wf = module_->getFunction(name)) {
                wf->setDoesNotAccessMemory();
                wf->setDoesNotThrow();
                wf->setWillReturn();
            }
        }
    };
    markWorkerIdReadNone();

    // Runs the standard LLVM optimization passes (mem2reg, instcombine, GVN,
    // inlining, loop opts, ...) BEFORE the backend codegen. Without this, -O
    // only affected backend instruction selection and the IR stayed unoptimized
    // (alloca/load/store everywhere). Runs before TSan so instrumentation is
    // applied to the final optimized form.
    if (opt_level > 0) {
        llvm::LoopAnalysisManager LAM;
        llvm::FunctionAnalysisManager FAM;
        llvm::CGSCCAnalysisManager CGAM;
        llvm::ModuleAnalysisManager MAM;
        // Pass the TargetMachine to PassBuilder so TTI (TargetIRAnalysis) is
        // registered for target-dependent passes — without this the
        // LoopVectorizer has no cost model and never vectorizes (historically
        // ALL MYP loops stayed scalar, e.g. matmul ~2x gap vs C++).
        llvm::PassBuilder PB(tm.get());
        PB.registerModuleAnalyses(MAM);
        PB.registerCGSCCAnalyses(CGAM);
        PB.registerFunctionAnalyses(FAM);
        PB.registerLoopAnalyses(LAM);
        PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
        llvm::OptimizationLevel OL =
            opt_level >= 3 ? llvm::OptimizationLevel::O3 :
            opt_level == 2 ? llvm::OptimizationLevel::O2 :
                             llvm::OptimizationLevel::O1;
        llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OL);
        // MYP-specific passes (dead store cleanup etc.) appended after the
        // default pipeline; also registered as -passes="myp-pass".
        registerMypPasses(PB, MPM, OL);
        MPM.addPass(llvm::VerifierPass());
        MPM.run(*module_, MAM);
        if (const char* d = getenv("MYPC_DUMP_OPT_IR"); d && d[0] == '1')
            module_->print(llvm::errs(), nullptr);   // debug: dump post-opt IR
    }

    // ---- Custom MYP pass pipeline (--passes=<name>) ----
    // Runs on top of the -O0 (or -O) IR. Currently supports "myp-pass"
    // (MypRedundantStorePass); dispatched via runMypPasses. Enables -O0
    // users / tests to apply MYP-specific cleanup without -O.
    if (!myp_passes_.empty()) {
        if (!runMypPasses(*module_, myp_passes_)) {
            llvm::errs() << "error: unknown pass pipeline '" << myp_passes_ << "'\n";
        } else if (const char* d = getenv("MYPC_DUMP_OPT_IR"); d && d[0] == '1') {
            module_->print(llvm::errs(), nullptr);   // debug: dump after myp pass
        }
    }

    // ThreadSanitizer instrumentation for generated programs.
    // Enabled via MYP_SANITIZE_TSAN=1 at mypc runtime; instruments the host
    // module so data races in @thread/@parallel user code are detected.
    // (TSan uses the new pass manager; runs before legacy-PM codegen.)
    if (const char* env = getenv("MYP_SANITIZE_TSAN"); env && env[0] == '1') {
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
        llvm::ModulePassManager TSanMPM;
        TSanMPM.addPass(llvm::ModuleThreadSanitizerPass());
        TSanMPM.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::ThreadSanitizerPass()));
        TSanMPM.run(*module_, MAM);
    }
    std::error_code ec;
    llvm::raw_fd_ostream dest(p, ec, llvm::sys::fs::OF_None);
    if (ec) { diag_.error(SourceRange{}, "file: " + ec.message()); return false; }
    llvm::legacy::PassManager pm;
    // Set target machine features
    tm->setOptLevel(opt_level >= 2 ? llvm::CodeGenOptLevel::Aggressive
                    : opt_level >= 1 ? llvm::CodeGenOptLevel::Default
                                     : llvm::CodeGenOptLevel::None);
    if (tm->addPassesToEmitFile(pm, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        diag_.error(SourceRange{}, "cannot emit object"); return false;
    }
    pm.run(*module_);
    dest.flush();
    return true;
}

}

