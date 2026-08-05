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

    finalizeDebugInfo();

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
    for (auto& cls : tu.classes) {
        std::vector<llvm::Type*> members;
        unsigned idx = 0;
        for (auto& prop : cls.properties) {
            members.push_back(typeNodeToLLVMType(prop.type));
            property_indices_[cls.name][prop.name] = idx++;
        }
        class_structs_[cls.name] = llvm::StructType::create(ctx_, members, cls.name);
    }
}

// -- Struct type builder --
void CodeGen::buildStructTypes(TranslationUnit& tu) {
    // File-level structs
    for (auto& st : tu.structs) {
        std::string key = st.parent_class.empty()
            ? st.name : st.parent_class + "::" + st.name;
        std::vector<llvm::Type*> members;
        unsigned idx = 0;
        for (auto& prop : st.properties) {
            members.push_back(typeNodeToLLVMType(prop.type));
            struct_field_indices_[key][prop.name] = idx++;
        }
        struct_types_[key] = llvm::StructType::create(ctx_, members, key);
    }
    // Nested structs in classes
    for (auto& cls : tu.classes) {
        for (auto& st : cls.structs) {
            std::string key = st.parent_class.empty()
                ? st.name : st.parent_class + "::" + st.name;
            std::vector<llvm::Type*> members;
            unsigned idx = 0;
            for (auto& prop : st.properties) {
                members.push_back(getLLVMType(builtinTypeToInfo(prop.type.basic_type)));
                struct_field_indices_[key][prop.name] = idx++;
            }
            struct_types_[key] = llvm::StructType::create(ctx_, members, key);
        }
    }
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
    for (auto& p : cls.properties)
        if (p.name == pn) return typeNodeToLLVMType(p.type);
    return llvm::Type::getInt32Ty(ctx_);
}

llvm::Type* CodeGen::typeNodeToLLVMType(const TypeNode& tn) {
    // Check for array type
    if (tn.isArray() && tn.element_type) {
        auto* elem = typeNodeToLLVMType(*tn.element_type);
        if (tn.array_size > 0)
            return llvm::ArrayType::get(elem, tn.array_size);
        return llvm::PointerType::get(ctx_, 0);
    }
    // Check for class type
    if (!tn.class_name.empty()) {
        if (getClassStruct(tn.class_name))
            return llvm::PointerType::get(ctx_, 0);
        if (auto* st = getStructType(tn.class_name))
            return st;
    }
    return getLLVMType(builtinTypeToInfo(tn.basic_type));
}

const ClassDecl* CodeGen::findClass(const std::string& n) {
    if (!current_tu_) return nullptr;
    for (auto& c : current_tu_->classes) if (c.name == n) return &c;
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
        case TypeKind::Function:  return llvm::PointerType::get(ctx_, 0);
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
            return llvm::Type::getInt32Ty(ctx_);
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

/// Convert a TypeNode (from AST) to TypeInfo for codegen use.
/// Properly handles array types (both sized and unsized).
TypeInfo CodeGen::typeNodeToCodegenType(const TypeNode& node) {
    if (node.isArray() && node.element_type) {
        TypeInfo result(TypeKind::Array);
        result.array_size = node.array_size;
        result.element_type = std::make_shared<TypeInfo>(
            typeNodeToCodegenType(*node.element_type));
        return result;
    }
    if (!node.class_name.empty()) {
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

// -- Symbol table --
void CodeGen::pushScope() { named_values_.emplace_back(); }
void CodeGen::popScope() {
    if (debug_mode_) emitScopeLocalsDebug();
    if (!named_values_.empty()) named_values_.pop_back();
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

    // Generate function bodies
    for (auto& f : tu.functions) generateFuncDecl(f);
    for (auto& c : tu.classes) generateClass(c);

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
void CodeGen::createClassActionDecl(const ClassDecl& cls, const ActionDecl& action) {
    auto fn = cls.name + "_" + action.name;
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


void CodeGen::generateClass(const ClassDecl& cls) {
    // Generate function: section bodies FIRST so actions can call them
    for (auto& fn : cls.functions) {
        generateClassFunction(cls, fn);
    }

    // Generate static action bodies (no 'this' pointer needed)
    for (auto& a : cls.static_actions) {
        generateStaticAction(cls, a);
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
        builder_.CreateCall(runtime_event_fire_, {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), event_id),
            func->getArg(0),
            builder_.CreateBitCast(data_alloca, llvm::PointerType::get(ctx_, 0))
        });
    } else {
        builder_.CreateCall(runtime_event_fire_, {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), event_id),
            func->getArg(0),
            llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0))
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
    std::string fn = cls.name + "_" + action.name;
    auto* func = module_->getFunction(fn);
    if (!func) return; // declaration not found (shouldn't happen)

    current_function_ = func;
    current_class_name_ = cls.name;
    current_is_coro_ = action.has_coro;
    finally_ret_slot_ = nullptr;
    finally_ctx_stack_.clear();
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);
    pushScope();
    if (debug_mode_) beginFunctionDebug(func, fn, action.range);

    auto* this_a = createEntryBlockAlloca(func, llvm::PointerType::get(ctx_, 0), "this");
    builder_.CreateStore(func->getArg(0), this_a);
    setNamedValue("this", this_a);
    if (debug_mode_)
        emitParamDebug(this_a, "this", llvm::PointerType::get(ctx_, 0),
                       action.range.begin.line ? action.range.begin.line : 1, 0);

    for (size_t i = 0; i < action.params.size(); ++i) {
        TypeInfo pt = typeNodeToCodegenType(action.params[i].type);
        auto* a = createEntryBlockAlloca(func, getLLVMType(pt), action.params[i].name);
        builder_.CreateStore(func->getArg(i + 1), a);
        setNamedValue(action.params[i].name, a);
        if (debug_mode_)
            emitParamDebug(a, action.params[i].name, getLLVMType(pt),
                           action.range.begin.line ? action.range.begin.line : 1, (unsigned)(i + 1));
        // Record array element type for subscript access
        if (action.params[i].type.isArray() && action.params[i].type.element_type) {
            array_elem_types_[action.params[i].name] = getLLVMType(typeNodeToCodegenType(*action.params[i].type.element_type));
        }
    }

    // @region: enter arena (skipped if return type is a reference — it escapes)
    bool fn_region = action.has_region &&
        !typeIsReference(typeNodeToCodegenType(action.return_type));
    if (fn_region) { in_region_function_ = true; emitRegionEnter(); }

    // Generate action body (stdlib actions use __myp_* intrinsics in their source code)
    if (action.body)
        generateBlock(static_cast<const BlockStmt&>(*action.body));
    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator()) {
        if (scope_functions_.count(func))
            builder_.CreateCall(runtime_event_pop_scope_, {});
        if (fn_region) emitRegionExit();
        builder_.CreateRetVoid();
    }
    in_region_function_ = false;
    current_region_mark_ = nullptr;
    current_is_coro_ = false;
    popScope();
    if (debug_mode_) endFunctionDebug();
}

// -- Generate a built-in Coro static method (direct runtime call) --
// The stdlib `Coro` class declares these methods with empty bodies; codegen
// emits the runtime call here so the __myp_coro_* intrinsics are never exposed
// to user code (sema does not register them either).
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

    const char* rt = nullptr;
    llvm::Type* ret = v;
    std::vector<llvm::Type*> pts;
    if (action.name == "scheduler") { rt = "__myp_coro_scheduler"; }
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

// -- Generate static action (no 'this' pointer) --
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
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);
    pushScope();
    if (debug_mode_) beginFunctionDebug(func, fn, action.range);

    for (size_t i = 0; i < action.params.size(); ++i) {
        TypeInfo pt = typeNodeToCodegenType(action.params[i].type);
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
    }

    // @region (static action)
    bool fn_region = action.has_region &&
        !typeIsReference(typeNodeToCodegenType(action.return_type));
    if (fn_region) { in_region_function_ = true; emitRegionEnter(); }

    if (action.body)
        generateBlock(static_cast<const BlockStmt&>(*action.body));
    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator()) {
        if (scope_functions_.count(func))
            builder_.CreateCall(runtime_event_pop_scope_, {});
        if (fn_region) emitRegionExit();
        builder_.CreateRetVoid();
    }
    in_region_function_ = false;
    current_region_mark_ = nullptr;
    popScope();
    if (debug_mode_) endFunctionDebug();
}

// -- Generate coroutine entry wrapper for an @coro method --
// void __myp_coro_entry_<Class>_<method>(void)
// Reads 'this' (slot 0) and params (slots 1..N) from the thread-local entry
// arg table and calls Class_method(this, params...). ucontext trampolines call
// this with no arguments.
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

// -- Entry wrapper for a top-level @coro function (no 'this' slot) --
// void __myp_coro_entry_name() {
//     args = __myp_coro_get_entry_arg(1..N);
//     name(args...);
// }
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
// h = __myp_coro_create();
// __myp_coro_set_entry_arg(0, this);
// __myp_coro_set_entry_arg(1+i, arg_i);
// __myp_coro_set_entry(h, ptrtoint(__myp_coro_entry_Class_method));
// __myp_coro_resume(h);   // first start
// return h;               // handle
llvm::Value* CodeGen::generateCoroSpawn(llvm::Function* target, const CallExpr& e,
                                        llvm::Value* mthis, bool is_method) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);
    auto* void_ty = llvm::Type::getVoidTy(ctx_);

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

    auto create_fn = module_->getOrInsertFunction("__myp_coro_create",
        llvm::FunctionType::get(i64, {i64}, false));
    auto* handle = builder_.CreateCall(create_fn,
        {llvm::ConstantInt::get(i64, stack_bytes)}, "coro_handle");

    auto set_arg = module_->getOrInsertFunction("__myp_coro_set_entry_arg",
        llvm::FunctionType::get(void_ty, {i64, i64}, false));
    auto idx = [&](uint64_t v) { return llvm::ConstantInt::get(i64, v); };

    // Slot 0: 'this'
    llvm::Value* this_i = idx(0);
    if (is_method && mthis)
        this_i = builder_.CreatePtrToInt(mthis, i64);
    builder_.CreateCall(set_arg, {idx(0), this_i});

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
        builder_.CreateCall(set_arg, {idx((uint64_t)(i + 1)), slot});
    }

    // __myp_coro_set_entry(handle, ptrtoint(entry_wrapper))
    auto set_entry = module_->getOrInsertFunction("__myp_coro_set_entry",
        llvm::FunctionType::get(void_ty, {i64, i64}, false));
    std::string entry_name = "__myp_coro_entry_" + target->getName().str();
    llvm::Value* entry_i = idx(0);
    if (auto* entry = module_->getFunction(entry_name))
        entry_i = builder_.CreatePtrToInt(entry, i64);
    builder_.CreateCall(set_entry, {handle, entry_i});

    // First start
    auto resume_fn = module_->getOrInsertFunction("__myp_coro_resume",
        llvm::FunctionType::get(i64, {i64, i64}, false));
    builder_.CreateCall(resume_fn, {handle, idx(0)});

    return handle;
}

// -- Generate struct methods --
void CodeGen::declareStructMethods(const StructDecl& st) {
    std::string type_key = st.parent_class.empty()
        ? st.name : st.parent_class + "::" + st.name;
    auto* st_type = getStructType(type_key);
    if (!st_type) return;

    for (auto& method : st.functions) {
        if (!method.body) continue;
        std::string fn = "struct_" + type_key + "_" + method.name;
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
        std::string fn = "struct_" + type_key + "_" + method.name;
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

// -- Generate class function: section --
void CodeGen::generateClassFunction(const ClassDecl& cls, const FuncDecl& fn_decl) {
    std::string fn = cls.name + "_" + fn_decl.name;
    auto* existing = module_->getFunction(fn);
    if (existing) existing->deleteBody();

    std::vector<llvm::Type*> pts = {llvm::PointerType::get(ctx_, 0)};
    for (auto& p : fn_decl.params) {
        pts.push_back(getLLVMType(typeNodeToCodegenType(p.type)));
    }

    TypeInfo rt = typeNodeToCodegenType(fn_decl.return_type);
    auto* ft = llvm::FunctionType::get(getLLVMType(rt), pts, false);
    auto* func = llvm::Function::Create(ft, llvm::Function::InternalLinkage, fn, module_.get());

    current_function_ = func;
    current_class_name_ = cls.name;
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);
    pushScope();
    if (debug_mode_) beginFunctionDebug(func, fn, fn_decl.range);

    auto* this_a = createEntryBlockAlloca(func, llvm::PointerType::get(ctx_, 0), "this");
    builder_.CreateStore(func->getArg(0), this_a);
    setNamedValue("this", this_a);
    if (debug_mode_)
        emitParamDebug(this_a, "this", llvm::PointerType::get(ctx_, 0),
                       fn_decl.range.begin.line ? fn_decl.range.begin.line : 1, 0);

    for (size_t i = 0; i < fn_decl.params.size(); ++i) {
        TypeInfo pt = typeNodeToCodegenType(fn_decl.params[i].type);
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
        !typeIsReference(typeNodeToCodegenType(fn_decl.return_type));
    if (fn_region) { in_region_function_ = true; emitRegionEnter(); }

    if (fn_decl.body)
        generateBlock(static_cast<const BlockStmt&>(*fn_decl.body));
    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator()) {
        if (fn_region) emitRegionExit();
        builder_.CreateRetVoid();
    }
    in_region_function_ = false;
    current_region_mark_ = nullptr;
    popScope();
    if (debug_mode_) endFunctionDebug();
}

void CodeGen::generateFuncDecl(const FuncDecl& decl) {
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
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);
    pushScope();
    if (debug_mode_) beginFunctionDebug(func, decl.name, decl.range);

    i = 0;
    for (auto& arg : func->args()) {
        if (i < decl.params.size()) {
            TypeInfo pt = typeNodeToCodegenType(decl.params[i].type);
            auto* a = createEntryBlockAlloca(func, getLLVMType(pt), decl.params[i].name);
            builder_.CreateStore(&arg, a);
            if (pt.kind == TypeKind::Slice)
                var_slice_types_[decl.params[i].name] = pt;
            setNamedValue(decl.params[i].name, a);
            if (debug_mode_)
                emitParamDebug(a, decl.params[i].name, getLLVMType(pt),
                               decl.range.begin.line ? decl.range.begin.line : 1, (unsigned)i);
            // Record array element type for subscript access
            if (decl.params[i].type.isArray() && decl.params[i].type.element_type) {
                array_elem_types_[decl.params[i].name] = getLLVMType(typeNodeToCodegenType(*decl.params[i].type.element_type));
            }
        }
        ++i;
    }

    // For main: call init mappings and track for cleanup
    if (decl.name == "main") {
        // In test mode, skip user's main - test runner main will be generated
        if (test_mode_) {
            popScope();
            current_function_ = nullptr;
            return;
        }
        in_main_ = true;
        if (init_func_) {
            builder_.CreateCall(init_func_, {});
        }
    }

    // @region (top-level function)
    bool fn_region = decl.has_region &&
        !typeIsReference(rt);
    if (fn_region) { in_region_function_ = true; emitRegionEnter(); }

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
    if (decl.name == "main") {
        in_main_ = false;
    }
    if (debug_mode_) endFunctionDebug();
}

void CodeGen::generateTestRunner() {
    if (!current_tu_) return;
    auto& tu = *current_tu_;

    auto* i32t = llvm::Type::getInt32Ty(ctx_);
    auto* i8t = llvm::Type::getInt8Ty(ctx_);
    auto* pt = llvm::PointerType::get(ctx_, 0);
    auto* vt = llvm::Type::getVoidTy(ctx_);

    // Collect @test functions and @test actions
    struct TestAction {
        std::string class_name;
        std::string action_name;
        bool is_static;
    };
    std::vector<TestAction> test_actions;
    std::vector<std::string> test_functions;

    for (auto& cls : tu.classes) {
        for (auto& action : cls.actions) {
            if (action.has_test) {
                test_actions.push_back({cls.name, action.name, false});
            }
        }
        for (auto& action : cls.static_actions) {
            if (action.has_test) {
                test_actions.push_back({cls.name, action.name, true});
            }
        }
    }
    for (auto& f : tu.functions) {
        if (f.has_test) {
            test_functions.push_back(f.name);
        }
    }

    // If nothing to test, generate a minimal main
    auto* ft = llvm::FunctionType::get(i32t, {}, false);
    auto* main_func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "main", module_.get());
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", main_func);
    builder_.SetInsertPoint(bb);
    current_function_ = main_func;

    // Call init mappings first
    if (init_func_) {
        builder_.CreateCall(init_func_, {});
    }

    llvm::Function* printf_fn = module_->getFunction("printf");
    if (!printf_fn) {
        auto* printf_ft = llvm::FunctionType::get(i32t, {pt}, true);
        printf_fn = llvm::Function::Create(printf_ft, llvm::Function::ExternalLinkage, "printf", module_.get());
    }

    // Print header
    auto* fmt_header = builder_.CreateGlobalStringPtr("=== MYP Test Runner ===\n");
    builder_.CreateCall(printf_fn, {fmt_header});

    // Call each @test function
    for (auto& fname : test_functions) {
        auto* func = module_->getFunction(fname);
        if (!func) continue;
        auto* fmt = builder_.CreateGlobalStringPtr(("  RUN: " + fname + "\n").c_str());
        builder_.CreateCall(printf_fn, {fmt});
        builder_.CreateCall(func, {});
    }

    // For each @test action, create instance and call
    for (auto& ta : test_actions) {
        std::string fn_name = ta.class_name + "_" + ta.action_name;
        auto* func = module_->getFunction(fn_name);
        if (!func) continue;

        auto* fmt = builder_.CreateGlobalStringPtr(("  RUN: " + ta.class_name + "." + ta.action_name + "\n").c_str());
        builder_.CreateCall(printf_fn, {fmt});

        if (ta.is_static) {
            builder_.CreateCall(func, {llvm::ConstantPointerNull::get(pt)});
        } else {
            // Allocate instance
            auto* cls_struct = class_structs_[ta.class_name];
            if (!cls_struct) continue;
            auto* instance = builder_.CreateCall(runtime_alloc_, {
                llvm::ConstantExpr::getSizeOf(cls_struct)
            });
            // Zero-init
            builder_.CreateMemSet(instance, llvm::ConstantInt::get(i8t, 0),
                llvm::ConstantExpr::getSizeOf(cls_struct), llvm::MaybeAlign(1));
            // Call init mapping to register event handlers
            auto* alloc_init = module_->getFunction("__myp_alloc_init_" + ta.class_name);
            if (alloc_init) {
                builder_.CreateCall(alloc_init, {instance});
            }
            builder_.CreateCall(func, {instance});
        }
    }

    // Print summary
    auto* fmt_done = builder_.CreateGlobalStringPtr("=== MYP Tests Complete ===\n");
    builder_.CreateCall(printf_fn, {fmt_done});

    builder_.CreateRet(llvm::ConstantInt::get(i32t, 0));
    current_function_ = nullptr;
}

// -- Mapping --
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

        builder_.CreateCall(runtime_event_register_, {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), event_id),
            llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)),
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

// -- Statements --
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
        case StmtKind::ReturnStmt: generateReturnStmt(static_cast<const ReturnStmt&>(s)); break;
        case StmtKind::BreakStmt:   generateBreakStmt(static_cast<const BreakStmt&>(s)); break;
        case StmtKind::ContinueStmt: generateContinueStmt(static_cast<const ContinueStmt&>(s)); break;
        case StmtKind::AwaitStmt: generateAwaitStmt(static_cast<const AwaitStmt&>(s)); break;
        case StmtKind::MappingStmt: generateMappingDecl(static_cast<const MappingStmt&>(s).decl, builder_.GetInsertBlock()); break;
        case StmtKind::MatchStmt: generateMatchStmt(static_cast<const MatchStmt&>(s)); break;
        case StmtKind::TryStmt: generateTryStmt(static_cast<const TryStmt&>(s)); break;
        case StmtKind::ThrowStmt: generateThrowStmt(static_cast<const ThrowStmt&>(s)); break;
        default: break;
    }
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
            // Create instance via myp_alloc
            auto* st = getClassStruct(elem_class);
            if (!st) continue;
            const auto& lay = module_->getDataLayout();
            auto sz = lay.getTypeAllocSize(st);
            auto* alloc_fn = runtime_alloc_;
            if (!alloc_fn) {
                auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0), {llvm::Type::getInt64Ty(ctx_)}, false);
                alloc_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_alloc", module_.get());
            }
            auto* obj = builder_.CreateCall(alloc_fn, {llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), sz > 0 ? sz : 1)});
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

    TypeInfo vt = builtinTypeToInfo(d.type.basic_type);
    // Detect class type from parser's type node
    if (!d.type.class_name.empty() && getClassStruct(d.type.class_name)) {
        vt = TypeInfo(TypeKind::Class); vt.class_name = d.type.class_name;
    }
    // Detect interface type
    bool is_interface = false;
    if (!d.type.class_name.empty() && current_tu_) {
        for (auto& ifd : current_tu_->interfaces) {
            if (ifd.name == d.type.class_name) {
                vt = TypeInfo(TypeKind::Interface); vt.class_name = d.type.class_name;
                is_interface = true;
                break;
            }
        }
    }

    // Detect struct type — allocate inline on stack
    bool is_struct = !d.type.class_name.empty() && getStructType(d.type.class_name) != nullptr;

    // --- Interface variable: fat pointer {ptr data, ptr vtable} ---
    if (is_interface) {
        auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
        auto* fat_ty = llvm::StructType::get(ctx_, {ptr_ty, ptr_ty});
        auto* a = createEntryBlockAlloca(current_function_, fat_ty, d.name);
        setNamedValue(d.name, a);

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
            if (iface_git == class_instance_globals_.end()) {
                auto* gv = new llvm::GlobalVariable(*module_,
                    llvm::PointerType::get(ctx_, 0), false,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)),
                    "__myp_inst_" + d.name);
                class_instance_globals_[d.name] = gv;
                iface_git = class_instance_globals_.find(d.name);
            }
            if (iface_git != class_instance_globals_.end())
                builder_.CreateStore(inst, iface_git->second);

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
            // Assign from an existing concrete variable: IFoo f = impl;
            auto* inst = generateExpr(*d.init_expr);
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
        return;
    }

    // slice<T> — value type { T* data; int64 len }, allocated on stack
    if (d.type.class_name == "slice" && d.type.type_args.size() == 1) {
        TypeInfo st(TypeKind::Slice);
        st.element_type = std::make_shared<TypeInfo>(
            typeNodeToCodegenType(d.type.type_args[0]));
        auto* slt = getLLVMType(st);
        auto* a = createEntryBlockAlloca(current_function_, slt, d.name);
        setNamedValue(d.name, a);
        if (d.init_expr) {
            auto* v = generateExpr(*d.init_expr);
            if (v && v->getType() == slt)
                builder_.CreateStore(v, a);
        } else {
            builder_.CreateStore(llvm::ConstantAggregateZero::get(slt), a);
        }
        var_slice_types_[d.name] = st;
        return;
    }

    llvm::Type* lt;
    if (is_struct) {
        lt = getStructType(d.type.class_name);
    } else if (d.type.isArray() && d.type.element_type) {
        // Array type
        if (d.type.array_size > 0) {
            TypeInfo arr_ti = typeNodeToCodegenType(d.type);
            lt = getLLVMType(arr_ti);
            auto* arr_a = createEntryBlockAlloca(current_function_, lt, d.name + "_arr");
            auto arr_sz = module_->getDataLayout().getTypeAllocSize(lt);
            if (arr_sz > 0)
                builder_.CreateMemSet(arr_a, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0), arr_sz, llvm::Align(8));
            auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
            auto* elem_ptr = builder_.CreateGEP(lt, arr_a, {zero, zero});
            auto* ptr_a = createEntryBlockAlloca(current_function_, llvm::PointerType::get(ctx_, 0), d.name);
            builder_.CreateStore(elem_ptr, ptr_a);
            setNamedValue(d.name, ptr_a);
        } else {
            auto* ptr_a = createEntryBlockAlloca(current_function_, llvm::PointerType::get(ctx_, 0), d.name);
            // Handle initializer: double[] buf = new double[n]
            if (d.init_expr) {
                auto* init_val = generateExpr(*d.init_expr);
                builder_.CreateStore(init_val, ptr_a);
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
        }
        // Record element type for subscript access
        array_elem_types_[d.name] = getLLVMType(typeNodeToCodegenType(*d.type.element_type));
        return;
    } else {
        vt = builtinTypeToInfo(d.type.basic_type);
        if (!d.type.class_name.empty() && getClassStruct(d.type.class_name)) {
            std::string cls_name = d.type.class_name;
            // Mangle name for generic classes: Box<int> → Box_int_inst
            if (!d.type.type_args.empty()) {
                cls_name = d.type.class_name;
                for (auto& ta : d.type.type_args) {
                    cls_name += "_";
                    switch (ta.basic_type) {
                        case BuiltinType::Byte: cls_name += "byte"; break;
                        case BuiltinType::Short: cls_name += "short"; break;
                        case BuiltinType::Int: cls_name += "int"; break;
                        case BuiltinType::Long: cls_name += "long"; break;
                        case BuiltinType::Double: cls_name += "double"; break;
                        case BuiltinType::Float: cls_name += "float"; break;
                        case BuiltinType::Bool: cls_name += "bool"; break;
                        case BuiltinType::String: cls_name += "string"; break;
                        default: cls_name += "unknown"; break;
                    }
                }
                cls_name += "_inst";
            }
            vt = TypeInfo(TypeKind::Class); vt.class_name = cls_name;
            // Track variable → class mapping for method resolution
            var_class_map_[d.name] = cls_name;
        }
        lt = getLLVMType(vt);
    }

    auto* a = createEntryBlockAlloca(current_function_, lt, d.name);
    setNamedValue(d.name, a);

    if (is_struct) {
        // Zero-initialize struct
        auto sz = module_->getDataLayout().getTypeAllocSize(lt);
        builder_.CreateMemSet(a, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0),
                              llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), sz), llvm::Align(8));
    } else if (lt->isPointerTy()) {
        builder_.CreateStore(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(lt)), a);
    } else {
        builder_.CreateStore(llvm::ConstantInt::get(lt, 0), a);
    }

    if (d.init_expr) {
        auto* v = generateExpr(*d.init_expr);
        if (v->getType() != lt) {
            if (lt->isIntegerTy() && v->getType()->isIntegerTy()) v = builder_.CreateIntCast(v, lt, true);
            else if (lt->isFloatingPointTy() && v->getType()->isIntegerTy()) v = builder_.CreateSIToFP(v, lt);
            else if (lt->isIntegerTy() && v->getType()->isFloatingPointTy()) v = builder_.CreateFPToSI(v, lt);
        }
        builder_.CreateStore(v, a);

        // Auto-call @startup actions for newly created class instances
        if (d.init_expr->kind == ExprKind::NewExpr && !d.type.class_name.empty() && current_tu_) {
            auto& ne = static_cast<const NewExpr&>(*d.init_expr);
            // Store instance in global for mapping handler access
            // Check both by class name and variable name
            auto git = class_instance_globals_.find(ne.class_name);
            if (git == class_instance_globals_.end())
                git = class_instance_globals_.find(d.name);
            if (git == class_instance_globals_.end()) {
                // Create global on-the-fly for function-level mapping access
                auto* gv = new llvm::GlobalVariable(*module_,
                    llvm::PointerType::get(ctx_, 0), false,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)),
                    "__myp_inst_" + d.name);
                class_instance_globals_[d.name] = gv;
                git = class_instance_globals_.find(d.name);
            }
            if (git != class_instance_globals_.end()) {
                auto* loaded = builder_.CreateLoad(lt, a, d.name);
                builder_.CreateStore(loaded, git->second);
            }
            for (auto& cls : current_tu_->classes) {
                if (cls.name != ne.class_name) continue;
                for (auto& act : cls.actions) {
                    if (act.has_startup) {
                        auto fn = cls.name + "_" + act.name;
                        auto* start_func = module_->getFunction(fn);
                        if (start_func) {
                            auto* loaded = builder_.CreateLoad(lt, a, d.name);
                            std::vector<llvm::Value*> init_args;
                            init_args.push_back(loaded);
                            // Pass constructor arguments from NewExpr
                            for (auto& arg : ne.args) {
                                init_args.push_back(generateExpr(*arg));
                            }
                            builder_.CreateCall(start_func, init_args);
                        }
                    }
                }
            }
        }
    }
}

void CodeGen::generateIfStmt(const IfStmt& s) {
    if (!s.condition) return;
    auto* c = generateExpr(*s.condition);
    if (!c->getType()->isIntegerTy(1)) c = builder_.CreateICmpNE(c, llvm::ConstantInt::get(c->getType(), 0));
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
        auto* c = generateExpr(*s.condition);
        if (!c->getType()->isIntegerTy(1)) c = builder_.CreateICmpNE(c, llvm::ConstantInt::get(c->getType(), 0));
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

llvm::Value* CodeGen::emitKernelExpr(const Expr& expr, llvm::IRBuilder<>& kb,
    std::map<std::string, llvm::Value*>& kernel_vars,
    const std::vector<llvm::Value*>& kernel_arg_values,
    const std::string& loop_var_name, llvm::Value* tid_val) {

    auto* i32_ty = llvm::Type::getInt32Ty(ctx_);
    auto* i64_ty = llvm::Type::getInt64Ty(ctx_);
    auto* double_ty = llvm::Type::getDoubleTy(ctx_);
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);

    switch (expr.kind) {
        case ExprKind::IntegerLiteral: {
            auto& e = static_cast<const IntegerLiteralExpr&>(expr);
            return llvm::ConstantInt::get(e.is_long ? i64_ty : i32_ty, e.value, true);
        }
        case ExprKind::FloatLiteral: {
            auto& e = static_cast<const FloatLiteralExpr&>(expr);
            return llvm::ConstantFP::get(double_ty, e.value);
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
            auto* l = emitKernelExpr(*e.lhs, kb, kernel_vars, kernel_arg_values,
                                      loop_var_name, tid_val);
            auto* r = emitKernelExpr(*e.rhs, kb, kernel_vars, kernel_arg_values,
                                      loop_var_name, tid_val);
            if (!l || !r) return llvm::ConstantInt::get(i64_ty, 0);

            // Type promotion
            if (l->getType() != r->getType()) {
                if (l->getType()->isDoubleTy() || r->getType()->isDoubleTy()) {
                    if (!l->getType()->isDoubleTy() && l->getType()->isIntegerTy())
                        l = kb.CreateSIToFP(l, double_ty);
                    if (!r->getType()->isDoubleTy() && r->getType()->isIntegerTy())
                        r = kb.CreateSIToFP(r, double_ty);
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
        case ExprKind::Call: {
            auto& e = static_cast<const CallExpr&>(expr);
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
                    if (!a->getType()->isDoubleTy() && a->getType()->isIntegerTy())
                        a = kb.CreateSIToFP(a, double_ty);
                    if (a->getType()->isFloatTy())
                        a = kb.CreateFPExt(a, double_ty);
                    llvm::Module* cur_mod = kb.GetInsertBlock()->getParent()->getParent();
                    auto* fn = cur_mod->getFunction(nv);
                    if (!fn)
                        fn = llvm::Function::Create(
                            llvm::FunctionType::get(double_ty, {double_ty}, false),
                            llvm::Function::ExternalLinkage, nv, cur_mod);
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
                    if (!a->getType()->isDoubleTy() && a->getType()->isIntegerTy()) a = kb.CreateSIToFP(a, double_ty);
                    if (!b->getType()->isDoubleTy() && b->getType()->isIntegerTy()) b = kb.CreateSIToFP(b, double_ty);
                    llvm::Module* cur_mod = kb.GetInsertBlock()->getParent()->getParent();
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
                    if (!a->getType()->isDoubleTy() && a->getType()->isIntegerTy()) a = kb.CreateSIToFP(a, double_ty);
                    if (!b->getType()->isDoubleTy() && b->getType()->isIntegerTy()) b = kb.CreateSIToFP(b, double_ty);
                    llvm::Module* cur_mod = kb.GetInsertBlock()->getParent()->getParent();
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
                            auto* elem_ptr = kb.CreateGEP(elem_ty, arr_ptr, idx, "atomic_ptr");
                            if (is_double) {
                                if (val->getType()->isIntegerTy())
                                    val = kb.CreateSIToFP(val, double_ty);
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
            return llvm::ConstantInt::get(i64_ty, 0);
        }
        case ExprKind::Subscript: {
            auto& e = static_cast<const SubscriptExpr&>(expr);
            auto* arr = emitKernelExpr(*e.array, kb, kernel_vars, kernel_arg_values,
                                        loop_var_name, tid_val);
            auto* idx = emitKernelExpr(*e.index, kb, kernel_vars, kernel_arg_values,
                                        loop_var_name, tid_val);
            if (!arr || !idx) return llvm::ConstantInt::get(i64_ty, 0);

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
                auto* gep = kb.CreateGEP(elem_ty, arr, idx);
                return kb.CreateLoad(elem_ty, gep);
            }
            auto* gep = kb.CreateGEP(elem_ty, arr, idx);
            return kb.CreateLoad(elem_ty, gep);
        }
        case ExprKind::Assignment: {
            auto& e = static_cast<const AssignmentExpr&>(expr);
            if (e.target->kind == ExprKind::Subscript) {
                auto& ss = static_cast<const SubscriptExpr&>(*e.target);
                auto* arr = emitKernelExpr(*ss.array, kb, kernel_vars, kernel_arg_values,
                                            loop_var_name, tid_val);
                auto* idx = emitKernelExpr(*ss.index, kb, kernel_vars, kernel_arg_values,
                                            loop_var_name, tid_val);
                auto* val = emitKernelExpr(*e.value, kb, kernel_vars, kernel_arg_values,
                                            loop_var_name, tid_val);
                if (!arr || !idx || !val) return llvm::ConstantInt::get(i64_ty, 0);

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
                }
                auto* gep = kb.CreateGEP(elem_ty, arr, idx);
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
        case ExprKind::StringLiteral:
        case ExprKind::NewExpr:
        case ExprKind::NewArrayExpr:
        case ExprKind::Range:
        case ExprKind::Lambda:
        case ExprKind::EnumVariant:
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
                // Evaluate init expr ONCE to get value and type
                llvm::Value* init_val = nullptr;
                llvm::Type* var_ty = i64_ty;
                if (d.init_expr) {
                    init_val = emitKernelExpr(*d.init_expr, kb, kernel_vars,
                                               kernel_arg_values, loop_var_name, tid_val);
                    if (init_val) var_ty = init_val->getType();
                }
                // Create alloca for mutable local variable
                llvm::BasicBlock& entry_b = kernel_func->getEntryBlock();
                llvm::IRBuilder<> entry_kb(&entry_b, entry_b.getFirstInsertionPt());
                auto* alloca_p = entry_kb.CreateAlloca(var_ty, nullptr, d.name);
                kernel_vars[d.name] = alloca_p;
                
                if (init_val) {
                    if (init_val->getType() != var_ty) {
                        if (var_ty->isIntegerTy() && init_val->getType()->isIntegerTy())
                            init_val = kb.CreateIntCast(init_val, var_ty, true);
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
        auto* c = generateExpr(*s.condition);
        if (!c->getType()->isIntegerTy(1)) c = builder_.CreateICmpNE(c, llvm::ConstantInt::get(c->getType(), 0));
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

    // ===== Create parallel body function =====
    auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {i32_ty, ptr_ty}, false);
    auto* body_fn = llvm::Function::Create(ft, llvm::Function::InternalLinkage,
        "parallel_body_" + loop_var_name, module_.get());
    auto* entry_bb = llvm::BasicBlock::Create(ctx_, "entry", body_fn);
    llvm::IRBuilder<> pb(entry_bb);
    
    auto* i_arg = body_fn->getArg(0); i_arg->setName("i");
    auto* void_arg = body_fn->getArg(1); void_arg->setName("arg");
    
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
    
    // Create local alloca for loop variable
    auto* i_alloca = pb.CreateAlloca(i32_ty, nullptr, loop_var_name);
    pb.CreateStore(i_arg, i_alloca);
    kernel_vars[loop_var_name] = i_alloca;
    
    // Generate the loop body in the body function
    pushScope();
    if (s.body) {
        emitKernelStmt(*s.body, pb, kernel_vars, empty_args, loop_var_name, i_arg);
        if (!pb.GetInsertBlock()->getTerminator())
            pb.CreateRetVoid();
    } else {
        pb.CreateRetVoid();
    }
    popScope();
    
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

// Resolve __nvvm_reflect("...") calls to constants. libdevice uses this NVVM
// intrinsic for compile-time configuration (FTZ, arch, ...); we replace all
// calls with 0 (no fast-math/FTZ, generic arch path) which is always safe.
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

// Link CUDA libdevice into a GPU kernel module so __nv_* device functions
// become self-contained (bodies inlined/emitted in the resulting PTX).
// Returns true on success; leaves the module untouched on failure.
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
    if (s.condition && s.condition->kind == ExprKind::BinaryOp) {
        auto& cond = static_cast<const BinaryOpExpr&>(*s.condition);
        if (cond.lhs->kind == ExprKind::Identifier &&
            static_cast<const IdentifierExpr&>(*cond.lhs).name != loop_var) {
            bound_var_names.insert(static_cast<const IdentifierExpr&>(*cond.lhs).name);
        } else if (cond.rhs->kind == ExprKind::Identifier &&
                   static_cast<const IdentifierExpr&>(*cond.rhs).name != loop_var) {
            bound_var_names.insert(static_cast<const IdentifierExpr&>(*cond.rhs).name);
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

    // Emit PTX
    llvm::legacy::PassManager pm;
    llvm::SmallString<16384> ptx_buf;
    llvm::raw_svector_ostream ptx_os(ptx_buf);

    tm->setOptLevel(llvm::CodeGenOptLevel::None);
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

    for (auto& ka : kernel_args_) {
        if (!ka.is_array) continue;

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
                auto* bs_val = bsit->second;
                if (llvm::isa<llvm::Constant>(bs_val))
                    bs_valid = true;
                else if (auto* bi = llvm::dyn_cast<llvm::Instruction>(bs_val))
                    bs_valid = (bi->getFunction() == func);
            }
            if (bs_valid) {
                byte_size = bsit->second;
            } else {
                auto eit = array_elem_types_.find(ka.name);
                if (eit != array_elem_types_.end()) {
                    auto* elem_size = llvm::ConstantInt::get(i64_ty,
                        eit->second->isDoubleTy() ? 8 :
                        eit->second->isIntegerTy(32) ? 4 : 8);
                    // Transfer exactly the elements the kernel touches (0..n-1).
                    // NOTE: do NOT pad to a large minimum size here — the host
                    // arrays may be exactly n elements, and a padded copy-back
                    // would write past the end of the host array (heap overflow).
                    byte_size = builder_.CreateMul(n_val, elem_size, ka.name + "_sz");
                } else {
                    byte_size = builder_.CreateMul(n_val, llvm::ConstantInt::get(i64_ty, 8),
                                                   ka.name + "_sz");
                }
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
            auto gpit = gpu_ptr_map.find(ka.name);
            if (gpit != gpu_ptr_map.end()) {
                auto* tmp = builder_.CreateAlloca(ptr_ty);
                builder_.CreateStore(gpit->second, tmp);
                store_val = tmp;
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
    generateForStmt(s);
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
}

void CodeGen::emitFunctionReturn(llvm::Value* ret_val) {
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
    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator()) {
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
            }
            builder_.CreateRet(ret_val);
        } else {
            builder_.CreateRetVoid();
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
    emitFunctionReturn(v);
}

// -- Expressions --
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
    }
    return nullptr;
}

llvm::Value* CodeGen::generateIntegerLiteral(const IntegerLiteralExpr& e) {
    auto val = e.value;
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
    return llvm::ConstantFP::get(llvm::Type::getDoubleTy(ctx_), e.value);
}
llvm::Value* CodeGen::generateBoolLiteral(const BoolLiteralExpr& e) {
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx_), e.value ? 1 : 0);
}
llvm::Value* CodeGen::generateStringLiteral(const StringLiteralExpr& e) {
    return builder_.CreateGlobalString(e.value, "str");
}
llvm::Value* CodeGen::generateNullLiteral(const NullLiteralExpr&) {
    return llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0));
}

llvm::Value* CodeGen::generateIdentifier(const IdentifierExpr& e) {
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
        if (runtime_read_line_ && e.name == "__myp_read_line") return runtime_read_line_;
        if (runtime_kbhit_ && e.name == "__myp_kbhit") return runtime_kbhit_;
        if (runtime_getch_ && e.name == "__myp_getch") return runtime_getch_;
        if (runtime_flush_ && e.name == "__myp_flush") return runtime_flush_;
        if (runtime_term_width_ && e.name == "__myp_term_width") return runtime_term_width_;
        if (runtime_term_height_ && e.name == "__myp_term_height") return runtime_term_height_;
        if (runtime_strlen_ && e.name == "__myp_strlen") return runtime_strlen_;
        if (runtime_chr_ && e.name == "__myp_chr") return runtime_chr_;
        if (runtime_atof_ && e.name == "__myp_atof") return runtime_atof_;
        // Try class property via 'this'
        if (!current_class_name_.empty() && current_tu_) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name != current_class_name_) continue;
                unsigned pi;
                if (getPropertyIndex(cls.name, e.name, pi)) {
                    auto* ta = getNamedValue("this");
                    if (ta) {
                        auto* tp = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ta);
                        auto* st = getClassStruct(cls.name);
                        if (st) {
                            auto* gep = builder_.CreateStructGEP(st, tp, pi);
                            auto* pt = getPropertyType(cls, e.name);
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

    auto* l = generateExpr(*e.lhs);
    auto* r = generateExpr(*e.rhs);
    if (l->getType() != r->getType()) {
        if (l->getType()->isDoubleTy() || r->getType()->isDoubleTy()) {
            if (!l->getType()->isDoubleTy() && l->getType()->isIntegerTy()) l = builder_.CreateSIToFP(l, llvm::Type::getDoubleTy(ctx_));
            if (!r->getType()->isDoubleTy() && r->getType()->isIntegerTy()) r = builder_.CreateSIToFP(r, llvm::Type::getDoubleTy(ctx_));
        } else if (l->getType()->isFloatTy() || r->getType()->isFloatTy()) {
            if (!l->getType()->isFloatTy() && l->getType()->isIntegerTy()) l = builder_.CreateSIToFP(l, llvm::Type::getFloatTy(ctx_));
            if (!r->getType()->isFloatTy() && r->getType()->isIntegerTy()) r = builder_.CreateSIToFP(r, llvm::Type::getFloatTy(ctx_));
        } else if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy()) {
            auto lw = l->getType()->getIntegerBitWidth(), rw = r->getType()->getIntegerBitWidth();
            if (lw < rw) l = builder_.CreateSExt(l, r->getType());
            else if (rw < lw) r = builder_.CreateSExt(r, l->getType());
        }
    }

    auto fp = l->getType()->isFloatingPointTy();
    // String concatenation with +
    bool is_str_concat = (e.op == BinaryOpKind::Add) &&
                          (l->getType()->isPointerTy() || r->getType()->isPointerTy());
    if (is_str_concat) {
        // Ensure both operands are strings
        auto* ptr_type = llvm::PointerType::get(ctx_, 0);
        if (!l->getType()->isPointerTy()) {
            // Convert non-string to string
            auto fn_name = std::string("myp_to_string_") +
                (l->getType()->isIntegerTy(32) ? "i32" :
                 l->getType()->isIntegerTy(64) ? "i64" :
                 l->getType()->isDoubleTy() ? "double" : "i32");
            auto* conv_fn = module_->getFunction(fn_name);
            if (!conv_fn) {
                auto* ft = llvm::FunctionType::get(ptr_type,
                    {l->getType()}, false);
                conv_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn_name, module_.get());
            }
            l = builder_.CreateCall(conv_fn, {l});
        }
        if (!r->getType()->isPointerTy()) {
            auto fn_name = std::string("myp_to_string_") +
                (r->getType()->isIntegerTy(32) ? "i32" :
                 r->getType()->isIntegerTy(64) ? "i64" :
                 r->getType()->isDoubleTy() ? "double" : "i32");
            auto* conv_fn = module_->getFunction(fn_name);
            if (!conv_fn) {
                auto* ft = llvm::FunctionType::get(ptr_type,
                    {r->getType()}, false);
                conv_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn_name, module_.get());
            }
            r = builder_.CreateCall(conv_fn, {r});
        }
        // Call runtime myp_strcat(l, r)
        auto* sc = module_->getFunction("myp_strcat");
        if (!sc) {
            auto* ft = llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type}, false);
            sc = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_strcat", module_.get());
        }
        return builder_.CreateCall(sc, {l, r}, "strcat");
    }

    // For string equality (== / !=), use myp_str_eq for content comparison
    if (runtime_str_eq_ && (e.op == BinaryOpKind::Eq || e.op == BinaryOpKind::Ne) &&
        l->getType()->isPointerTy() && r->getType()->isPointerTy() && !fp) {
        auto* result = builder_.CreateCall(runtime_str_eq_, {l, r}, "streq");
        if (e.op == BinaryOpKind::Ne) {
            return builder_.CreateICmpEQ(result, llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0));
        } else {
            return builder_.CreateICmpNE(result, llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0));
        }
    }

    switch (e.op) {
        case BinaryOpKind::Add: return fp ? builder_.CreateFAdd(l, r) : builder_.CreateAdd(l, r);
        case BinaryOpKind::Sub: return fp ? builder_.CreateFSub(l, r) : builder_.CreateSub(l, r);
        case BinaryOpKind::Mul: return fp ? builder_.CreateFMul(l, r) : builder_.CreateMul(l, r);
        case BinaryOpKind::Div: return fp ? builder_.CreateFDiv(l, r) : builder_.CreateSDiv(l, r);
        case BinaryOpKind::Mod: return fp ? builder_.CreateFRem(l, r) : builder_.CreateSRem(l, r);
        case BinaryOpKind::Eq:
        case BinaryOpKind::Ne:
        case BinaryOpKind::Lt:
        case BinaryOpKind::Gt:
        case BinaryOpKind::Le:
        case BinaryOpKind::Ge: {
            // Ensure both operands have same type for comparison
            if (!fp && l->getType() != r->getType()) {
                auto* i32 = llvm::Type::getInt32Ty(ctx_);
                auto* i64 = llvm::Type::getInt64Ty(ctx_);
                if (l->getType() == i64 && r->getType() == i32)
                    r = builder_.CreateSExt(r, i64);
                else if (l->getType() == i32 && r->getType() == i64)
                    l = builder_.CreateSExt(l, i64);
                else if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy()) {
                    auto lw = l->getType()->getIntegerBitWidth();
                    auto rw = r->getType()->getIntegerBitWidth();
                    if (lw < rw) l = builder_.CreateSExt(l, r->getType());
                    else if (rw < lw) r = builder_.CreateSExt(r, l->getType());
                    else r = builder_.CreateIntCast(r, l->getType(), true);
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
                return fp ? builder_.CreateFCmpOLT(l, r) : builder_.CreateICmpSLT(l, r);
            if (e.op == BinaryOpKind::Gt)
                return fp ? builder_.CreateFCmpOGT(l, r) : builder_.CreateICmpSGT(l, r);
            if (e.op == BinaryOpKind::Le)
                return fp ? builder_.CreateFCmpOLE(l, r) : builder_.CreateICmpSLE(l, r);
            if (e.op == BinaryOpKind::Ge)
                return fp ? builder_.CreateFCmpOGE(l, r) : builder_.CreateICmpSGE(l, r);
        }
        case BinaryOpKind::And: return builder_.CreateAnd(l, r);
        case BinaryOpKind::Or:  return builder_.CreateOr(l, r);
        case BinaryOpKind::BitAnd: return builder_.CreateAnd(l, r);
        case BinaryOpKind::BitOr:  return builder_.CreateOr(l, r);
        case BinaryOpKind::BitXor: return builder_.CreateXor(l, r);
        case BinaryOpKind::Shl: {
            // Shift amount must match value type
            if (r->getType() != l->getType())
                r = builder_.CreateZExt(r, l->getType());
            return builder_.CreateShl(l, r);
        }
        case BinaryOpKind::Shr: {
            if (r->getType() != l->getType())
                r = builder_.CreateZExt(r, l->getType());
            return builder_.CreateAShr(l, r);
        }
    }
    return nullptr;
}

llvm::Value* CodeGen::generateUnaryOp(const UnaryOpExpr& e) {
    auto* o = generateExpr(*e.operand);
    switch (e.op) {
        case UnaryOpKind::Negate: return o->getType()->isFloatingPointTy() ? builder_.CreateFNeg(o) : builder_.CreateNeg(o);
        case UnaryOpKind::Not:    return builder_.CreateNot(o);
    }
    return nullptr;
}

llvm::Value* CodeGen::generateCall(const CallExpr& e) {
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

        // Enum variant construction: Option.Some(42) → return variant index
        if (ma.object->kind == ExprKind::Identifier) {
            auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
            if (current_tu_) {
                for (auto& en : current_tu_->enums) {
                    if (en.name == oi.name) {
                        for (size_t vi = 0; vi < en.variants.size(); vi++) {
                            if (en.variants[vi].name == ma.member_name) {
                                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), vi);
                            }
                        }
                    }
                }
            }
        }

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
                        int method_idx = -1;
                        for (auto& ifd : current_tu_->interfaces) {
                            for (size_t mi = 0; mi < ifd.actions.size(); mi++) {
                                if (ifd.actions[mi].name == ma.member_name) {
                                    method_idx = (int)mi;
                                    goto iface_method_found;
                                }
                            }
                        }
                        iface_method_found:

                        if (method_idx >= 0) {
                            // Vtable is [N x ptr], load function pointer by index
                            auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
                            auto* func_gep = builder_.CreateGEP(ptr_ty, vt,
                                {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), method_idx)},
                                "iface_method");
                            auto* func_ptr = builder_.CreateLoad(ptr_ty, func_gep, "iface_fn");

                            // Determine return type from interface method declaration
                            llvm::Type* ret_ty = llvm::Type::getVoidTy(ctx_);
                            for (auto& ifd : current_tu_->interfaces) {
                                if ((size_t)method_idx < ifd.actions.size() &&
                                    ifd.actions[method_idx].name == ma.member_name) {
                                    ret_ty = getLLVMType(typeNodeToCodegenType(
                                        ifd.actions[method_idx].return_type));
                                    break;
                                }
                            }

                            // Build call args: instance + explicit args
                            std::vector<llvm::Value*> call_args;
                            call_args.push_back(data); // 'this' pointer
                            for (auto& arg : e.args)
                                call_args.push_back(generateExpr(*arg));

                            // Build parameter types
                            std::vector<llvm::Type*> param_types;
                            param_types.push_back(ptr_ty);
                            for (auto& arg : e.args)
                                param_types.push_back(generateExpr(*arg)->getType());

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
                if (callee) {
                    mthis = generateExpr(*ma.object);
                    is_method = true;
                    goto call_ready;
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
            // If the object is a known class name (static call like Vectors.min()),
            // restrict the search to that class so method names that collide across
            // classes (e.g. min/max in Math and Vectors) resolve to the right one.
            std::string obj_cls;
            if (ma.object->kind == ExprKind::Identifier) {
                auto& oid = static_cast<const IdentifierExpr&>(*ma.object);
                for (auto& cls : current_tu_->classes) {
                    if (cls.name == oid.name) { obj_cls = cls.name; break; }
                }
            }
            for (auto& cls : current_tu_->classes) {
                if (!obj_cls.empty() && cls.name != obj_cls) continue;
                for (auto& a : cls.actions) {
                    if (a.name == ma.member_name && a.params.size() == num_args) {
                        auto fn = cls.name + "_" + a.name;
                        if (module_->getFunction(fn)) {
                            best_class = cls.name;
                            goto found_method;
                        }
                    }
                }
                // Also check static actions
                for (auto& a : cls.static_actions) {
                    if (a.name == ma.member_name && a.params.size() == num_args) {
                        auto fn = cls.name + "_" + a.name;
                        if (module_->getFunction(fn)) {
                            best_class = cls.name;
                            goto found_method;
                        }
                    }
                }
                // Also check function: section
                for (auto& fn : cls.functions) {
                    if (fn.name == ma.member_name && fn.params.size() == num_args) {
                        auto fn_name = cls.name + "_" + fn.name;
                        if (module_->getFunction(fn_name)) {
                            best_class = cls.name;
                            goto found_method;
                        }
                    }
                }
            }
        }

        // Fallback: match by name only (only if no struct method was found)
        if (!callee) {
            // First, if the object is a KNOWN class name (static method call like
            // Vectors.min()), only search that class. Otherwise a method name that
            // collides across classes (e.g. min/max in Math AND Vectors) could
            // resolve to the wrong class.
            if (ma.object->kind == ExprKind::Identifier) {
                auto& oid = static_cast<const IdentifierExpr&>(*ma.object);
                for (auto& cls : current_tu_->classes) {
                    if (cls.name == oid.name) {
                        // static actions
                        for (auto& a : cls.static_actions) {
                            if (a.name == ma.member_name) {
                                auto fn = cls.name + "_" + a.name;
                                if (module_->getFunction(fn)) {
                                    best_class = cls.name;
                                    goto found_method;
                                }
                            }
                        }
                        // function: section
                        for (auto& fn : cls.functions) {
                            if (fn.name == ma.member_name) {
                                auto fn_name = cls.name + "_" + fn.name;
                                if (module_->getFunction(fn_name)) {
                                    best_class = cls.name;
                                    goto found_method;
                                }
                            }
                        }
                        break;
                    }
                }
            }
            for (auto& cls : current_tu_->classes) {
                for (auto& a : cls.actions) {
                    if (a.name == ma.member_name) {
                        auto fn = cls.name + "_" + a.name;
                        if (module_->getFunction(fn)) {
                            best_class = cls.name;
                            goto found_method;
                        }
                    }
                }
                // Also check static actions
                for (auto& a : cls.static_actions) {
                    if (a.name == ma.member_name) {
                        auto fn = cls.name + "_" + a.name;
                        if (module_->getFunction(fn)) {
                            best_class = cls.name;
                            goto found_method;
                        }
                    }
                }
                // function: section fallback
                for (auto& fn : cls.functions) {
                    if (fn.name == ma.member_name) {
                        auto fn_name = cls.name + "_" + fn.name;
                        if (module_->getFunction(fn_name)) {
                            best_class = cls.name;
                            goto found_method;
                        }
                    }
                }
            }
        }
        found_method:

        if (!best_class.empty()) {
            callee = module_->getFunction(best_class + "_" + ma.member_name);
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
                    auto* loaded = builder_.CreateLoad(elem_ty, elem_ptr);
                    return loaded;
                }

                if (e.args.size() < 3) {
                    diag_.error(e.range, "atomic store/add/sub/xchg requires 3 arguments");
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
                }
                auto* val = generateExpr(*e.args[2]);

                if (id.name == "__myp_atomic_store_i32") {
                    builder_.CreateStore(val, elem_ptr);
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
        if (!callee && !current_class_name_.empty() && current_tu_) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name != current_class_name_) continue;
                // Check actions
                for (auto& a : cls.actions) {
                    if (a.name == id.name) {
                        auto fn = cls.name + "_" + a.name;
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
                for (auto& fn : cls.functions) {
                    if (fn.name == id.name) {
                        auto fn_name = cls.name + "_" + fn.name;
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
                for (auto& ev : cls.events) {
                    if (ev.name == id.name) {
                        auto fn = "fire_" + cls.name + "_" + ev.name;
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
                // For chained access a.b.c.method(), evaluate the object
                // expression to determine its struct type by walking the chain
                if (ma.object->kind == ExprKind::MemberAccess) {
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
            if (args[i]->getType()->isIntegerTy() && expected->isIntegerTy()) {
                args[i] = builder_.CreateIntCast(args[i], expected, true);
            } else if (args[i]->getType()->isIntegerTy() && expected->isFloatingPointTy()) {
                args[i] = builder_.CreateSIToFP(args[i], expected);
            } else if (args[i]->getType()->isFloatingPointTy() && expected->isIntegerTy()) {
                args[i] = builder_.CreateFPToSI(args[i], expected);
            } else if (args[i]->getType()->isPointerTy() && expected->isPointerTy()) {
                args[i] = builder_.CreateBitCast(args[i], expected);
            }
        }
    }
    bool isv = cf->getReturnType()->isVoidTy();
    return builder_.CreateCall(cf->getFunctionType(), cf, args, isv ? "" : "calltmp");
}

void CodeGen::generateFFIDecl(const FFIDecl& decl) {
    std::vector<llvm::Type*> pts;
    for (auto& p : decl.params) {
        if (p.type.isArray()) {
            pts.push_back(llvm::PointerType::get(ctx_, 0));
        } else {
            pts.push_back(getLLVMType(builtinTypeToInfo(p.type.basic_type)));
        }
    }
    auto* rt = getLLVMType(builtinTypeToInfo(decl.return_type.basic_type));
    auto* ft = llvm::FunctionType::get(rt, pts, false);
    llvm::Function::Create(ft, llvm::Function::ExternalLinkage, decl.name, module_.get());
}

llvm::Value* CodeGen::generateLambda(const LambdaExpr& e) {
    // Create instance of hidden class: new __lambda_N()
    NewExpr ne(e.hidden_class_name, {}, {}, e.range);
    auto* obj = generateNewExpr(ne);
    // Also call @startup if any
    return obj;
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

llvm::Value* CodeGen::generateMemberAccess(const MemberAccessExpr& e) {
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
                            auto* pt = getPropertyType(cls, e.member_name);
                            if (pt->isArrayTy()) return gep;
                            return builder_.CreateLoad(pt, gep);
                        }
                    }
                }
            }
        }
    }

    // Enum variant access: Color.Red → i32 constant
    if (e.object->kind == ExprKind::Identifier) {
        auto& oi = static_cast<const IdentifierExpr&>(*e.object);
        if (current_tu_) {
            for (auto& en : current_tu_->enums) {
                if (en.name == oi.name) {
                    for (size_t vi = 0; vi < en.variants.size(); vi++) {
                        if (en.variants[vi].name == e.member_name) {
                            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), vi);
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
                // Class property access
                auto* tp = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), ta);
                if (tp && current_tu_) {
                    for (auto& cls : current_tu_->classes) {
                        unsigned pi;
                        if (getPropertyIndex(cls.name, e.member_name, pi)) {
                            auto* st = getClassStruct(cls.name);
                            if (st) {
                                auto* gep = builder_.CreateStructGEP(st, tp, pi);
                                auto* pt = getPropertyType(cls, e.member_name);
                                return builder_.CreateLoad(pt, gep);
                            }
                        }
                    }
                }
            }
        }
    }
    return generateExpr(*e.object);
}

llvm::Value* CodeGen::generateSubscript(const SubscriptExpr& e) {
    // slice<T>[i] — unpack data, bounds-check, GEP
    if (e.array->kind == ExprKind::Identifier) {
        auto& id = static_cast<const IdentifierExpr&>(*e.array);
        auto sit = var_slice_types_.find(id.name);
        if (sit != var_slice_types_.end()) {
            auto* va = getNamedValue(id.name);
            if (va && sit->second.element_type) {
                auto* sval = builder_.CreateLoad(getLLVMType(sit->second), va, id.name);
                auto* data = builder_.CreateExtractValue(sval, 0);
                auto* len = builder_.CreateExtractValue(sval, 1);
                auto* idx = generateExpr(*e.index);
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
                    be_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_bounds_error", module_.get());
                }
                auto* cur_fn = builder_.GetInsertBlock()->getParent();
                auto* err_bb = llvm::BasicBlock::Create(ctx_, "slice_oob", cur_fn);
                auto* cont_bb = llvm::BasicBlock::Create(ctx_, "slice_ok", cur_fn);
                builder_.CreateCondBr(ok, cont_bb, err_bb);
                builder_.SetInsertPoint(err_bb);
                builder_.CreateCall(be_fn, {idx64, len});
                builder_.CreateBr(cont_bb);
                builder_.SetInsertPoint(cont_bb);
                auto* elem_ty = getLLVMType(*sit->second.element_type);
                auto* p = builder_.CreateGEP(elem_ty, data, idx64);
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
    auto* p = builder_.CreateGEP(elem_ty, a, i);
    return builder_.CreateLoad(elem_ty, p);
}

llvm::Value* CodeGen::generateNewExpr(const NewExpr& e) {
    // Built-in slice<T>(n): allocate n*elem bytes, return { data, len }
    if (e.class_name == "slice") {
        auto* elem_ty = typeNodeToLLVMType(e.type_args[0]);
        uint64_t es = module_->getDataLayout().getTypeAllocSize(elem_ty);
        auto* len_val = generateExpr(*e.args[0]);
        if (len_val->getType()->isIntegerTy(32) || len_val->getType()->isIntegerTy(8)
            || len_val->getType()->isIntegerTy(16))
            len_val = builder_.CreateZExt(len_val, llvm::Type::getInt64Ty(ctx_));
        auto* byte_size = builder_.CreateMul(len_val,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), es));
        // Always allocate via the region-aware allocator: when called inside an
        // @region's dynamic scope it lands in the region list (reclaimed on exit),
        // otherwise it behaves exactly like the process-level allocator.
        llvm::Function* alloc_fn = module_->getFunction("myp_region_alloc");
        if (!alloc_fn) {
            auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0), {llvm::Type::getInt64Ty(ctx_)}, false);
            alloc_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_region_alloc", module_.get());
        }
        auto* ptr = builder_.CreateCall(alloc_fn, {byte_size}, "slice_data");
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
    if (!e.type_args.empty()) {
        cls_name = e.class_name;
        for (auto& ta : e.type_args) {
            cls_name += "_";
            switch (ta.basic_type) {
                case BuiltinType::Byte: cls_name += "byte"; break;
                case BuiltinType::Short: cls_name += "short"; break;
                case BuiltinType::Int: cls_name += "int"; break;
                case BuiltinType::Long: cls_name += "long"; break;
                case BuiltinType::Double: cls_name += "double"; break;
                case BuiltinType::Float: cls_name += "float"; break;
                case BuiltinType::Bool: cls_name += "bool"; break;
                case BuiltinType::String: cls_name += "string"; break;
                default: cls_name += "unknown"; break;
            }
        }
        cls_name += "_inst";
    }
    auto* st = getClassStruct(cls_name);
    if (!st) {
        // Struct type not found — allocate 1 byte (minimum valid pointer)
        // The struct has no properties, so no real storage is needed
        auto* alloc_fn = runtime_alloc_;
        if (!alloc_fn) {
            auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0), {llvm::Type::getInt64Ty(ctx_)}, false);
            alloc_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_alloc", module_.get());
        }
        return builder_.CreateCall(alloc_fn, {llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 1)});
    }
    const auto& lay = module_->getDataLayout();
    auto sz = lay.getTypeAllocSize(st);
    auto* alloc_fn = runtime_alloc_;
    if (!alloc_fn) {
        auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0), {llvm::Type::getInt64Ty(ctx_)}, false);
        alloc_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_alloc", module_.get());
    }
    auto* obj = builder_.CreateCall(alloc_fn, {llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), sz > 0 ? sz : 1)});
    if (sz > 0)
        builder_.CreateMemSet(obj, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0), sz, llvm::Align(8));

    // Apply declared property default values (`int x = 5;`, class-level
    // `const double T = 0.0253;`). The allocator zero-inits; non-default
    // initializers are stored here on every new. Generic instances are skipped
    // (their template's defaults keep the zero value).
    if (current_tu_) {
        for (auto& cls : current_tu_->classes) {
            if (cls.name != cls_name) continue;
            auto* st2 = getClassStruct(cls.name);
            if (!st2) continue;
            for (size_t pi = 0; pi < cls.properties.size(); pi++) {
                auto& prop = cls.properties[pi];
                if (!prop.init_expr) continue;
                llvm::Value* v = generateExpr(*prop.init_expr);
                auto* gep = builder_.CreateStructGEP(st2, obj, pi);
                builder_.CreateStore(v, gep);
            }
            break;
        }
    }
    return obj;
}


llvm::Value* CodeGen::generateNewArrayExpr(const NewArrayExpr& e) {
    llvm::Value* total = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 1);
    for (auto& dim : e.dimensions) {
        auto* dim_val = generateExpr(*dim);
        if (dim_val->getType()->isIntegerTy(32))
            dim_val = builder_.CreateZExt(dim_val, llvm::Type::getInt64Ty(ctx_));
        total = builder_.CreateMul(total, dim_val);
    }
    auto* elem_ty = typeNodeToLLVMType(e.element_type);
    uint64_t elem_size = module_->getDataLayout().getTypeAllocSize(elem_ty);
    auto* byte_size = builder_.CreateMul(total,
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), elem_size));
    // Always region-aware allocator (see generateNewExpr): dynamic extent — inside
    // an @region's call scope it is reclaimed; otherwise acts as process-level.
    llvm::Function* alloc_fn = module_->getFunction("myp_region_alloc");
    if (!alloc_fn) {
        auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0), {llvm::Type::getInt64Ty(ctx_)}, false);
        alloc_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_region_alloc", module_.get());
    }
    auto* ptr = builder_.CreateCall(alloc_fn, {byte_size}, "new_arr");
    if (elem_size > 0)
        builder_.CreateMemSet(ptr, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0), byte_size, llvm::Align(8));
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

llvm::Value* CodeGen::generateThisExpr(const ThisExpr&) {
    return getNamedValue("this");
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

// Build the global event name→id table once (for runtime-constructed timers,
// e.g. stdlib Timer.init passes a runtime event-name string).
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

void CodeGen::generateAwaitStmt(const AwaitStmt& s) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);

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

llvm::Value* CodeGen::generateAwaitExpr(const AwaitExpr& e) {
    auto* i64 = llvm::Type::getInt64Ty(ctx_);

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
    auto* err_var = createEntryBlockAlloca(func, llvm::PointerType::get(ctx_, 0), e.catch_var_name);
    builder_.CreateStore(err_ptr, err_var);
    setNamedValue(e.catch_var_name, err_var);
    auto* v2 = generateExpr(*e.catch_expr);
    auto* last_catch = builder_.GetInsertBlock();
    if (!last_catch->getTerminator()) builder_.CreateBr(merge_bb);

    // Merge via PHI (cast fallback to the success type if needed)
    func->insert(func->end(), merge_bb);
    builder_.SetInsertPoint(merge_bb);
    if (v2->getType() != v1->getType()) {
        if (v1->getType()->isIntegerTy() && v2->getType()->isIntegerTy())
            v2 = builder_.CreateIntCast(v2, v1->getType(), true);
        else if (v1->getType()->isFloatingPointTy() && v2->getType()->isIntegerTy())
            v2 = builder_.CreateSIToFP(v2, v1->getType());
        else if (v1->getType()->isIntegerTy() && v2->getType()->isFloatingPointTy())
            v2 = builder_.CreateFPToSI(v2, v1->getType());
    }
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
    auto* true_val = generateExpr(*e.true_expr);
    if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(merge_bb);
    auto* last_true = builder_.GetInsertBlock();
    // False branch
    func->insert(func->end(), false_bb);
    builder_.SetInsertPoint(false_bb);
    auto* false_val = generateExpr(*e.false_expr);
    if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(merge_bb);
    auto* last_false = builder_.GetInsertBlock();
    // Merge
    func->insert(func->end(), merge_bb);
    builder_.SetInsertPoint(merge_bb);
    auto* phi = builder_.CreatePHI(true_val->getType(), 2, "ternary");
    phi->addIncoming(true_val, last_true);
    phi->addIncoming(false_val, last_false);
    return phi;
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
        builder_.CreateStore(v, a);
        return v;
    }
    // arr[i] = value
    if (e.target->kind == ExprKind::Subscript) {
        auto& ss = static_cast<const SubscriptExpr&>(*e.target);
        // slice<T>[i] = value — unpack data, bounds-check, GEP, store
        if (ss.array->kind == ExprKind::Identifier) {
            auto& id = static_cast<const IdentifierExpr&>(*ss.array);
            auto sit = var_slice_types_.find(id.name);
            if (sit != var_slice_types_.end()) {
                auto* va = getNamedValue(id.name);
                if (va && sit->second.element_type) {
                    auto* sval = builder_.CreateLoad(getLLVMType(sit->second), va, id.name);
                    auto* data = builder_.CreateExtractValue(sval, 0);
                    auto* len = builder_.CreateExtractValue(sval, 1);
                    auto* idx = generateExpr(*ss.index);
                    auto* idx64 = idx;
                    if (idx64->getType()->isIntegerTy(32) || idx64->getType()->isIntegerTy(8)
                        || idx64->getType()->isIntegerTy(16))
                        idx64 = builder_.CreateZExt(idx64, llvm::Type::getInt64Ty(ctx_));
                    auto* i64ty = llvm::Type::getInt64Ty(ctx_);
                    auto* nonneg = builder_.CreateICmpSGE(idx64, llvm::ConstantInt::get(i64ty, 0));
                    auto* inb = builder_.CreateICmpULT(idx64, len);
                    auto* ok = builder_.CreateAnd(nonneg, inb);
                    auto* be_fn = module_->getFunction("myp_bounds_error");
                    if (!be_fn) {
                        auto* ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                            {i64ty, i64ty}, false);
                        be_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_bounds_error", module_.get());
                    }
                    auto* cur_fn = builder_.GetInsertBlock()->getParent();
                    auto* err_bb = llvm::BasicBlock::Create(ctx_, "slice_oob", cur_fn);
                    auto* cont_bb = llvm::BasicBlock::Create(ctx_, "slice_ok", cur_fn);
                    builder_.CreateCondBr(ok, cont_bb, err_bb);
                    builder_.SetInsertPoint(err_bb);
                    builder_.CreateCall(be_fn, {idx64, len});
                    builder_.CreateBr(cont_bb);
                    builder_.SetInsertPoint(cont_bb);
                    auto* elem_ty = getLLVMType(*sit->second.element_type);
                    auto* p = builder_.CreateGEP(elem_ty, data, idx64);
                    auto* v = generateExpr(*e.value);
                    if (v->getType() != elem_ty) {
                        if (elem_ty->isFloatingPointTy() && v->getType()->isIntegerTy())
                            v = builder_.CreateSIToFP(v, elem_ty);
                        else if (elem_ty->isIntegerTy() && v->getType()->isFloatingPointTy())
                            v = builder_.CreateFPToSI(v, elem_ty);
                        else if (elem_ty->isIntegerTy() && v->getType()->isIntegerTy())
                            v = builder_.CreateIntCast(v, elem_ty, true);
                    }
                    builder_.CreateStore(v, p);
                    return v;
                }
            }
        }
        auto* a = generateExpr(*ss.array);
        auto* i = generateExpr(*ss.index);
        llvm::Type* elem_ty = llvm::Type::getInt32Ty(ctx_);

        // Determine element type from the array expression (see generateSubscript)
        if (ss.array->kind == ExprKind::Identifier) {
            auto& id = static_cast<const IdentifierExpr&>(*ss.array);
            if (!current_class_name_.empty() && current_tu_) {
                for (auto& cls : current_tu_->classes) {
                    if (cls.name != current_class_name_) continue;
                    for (auto& p : cls.properties) {
                        if (p.name == id.name && p.type.isArray()) {
                            elem_ty = typeNodeToLLVMType(*p.type.element_type);
                            goto assign_gep;
                        }
                    }
                }
            }
            // Check local array variables
            auto eit = array_elem_types_.find(id.name);
            if (eit != array_elem_types_.end()) {
                elem_ty = eit->second;
                goto assign_gep;
            }
        } else if (ss.array->kind == ExprKind::MemberAccess) {
            auto& ma = static_cast<const MemberAccessExpr&>(*ss.array);
            if (ma.object->kind == ExprKind::Identifier) {
                auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
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
                                goto assign_gep;
                            }
                        }
                    }
                }
            }
        }

assign_gep:
        auto* p = builder_.CreateGEP(elem_ty, a, i);
        auto* v = generateExpr(*e.value);
        if (v->getType() != elem_ty) {
            if (elem_ty->isIntegerTy() && v->getType()->isIntegerTy())
                v = builder_.CreateIntCast(v, elem_ty, true);
            else if (elem_ty->isFloatingPointTy() && v->getType()->isIntegerTy())
                v = builder_.CreateSIToFP(v, elem_ty);
            else if (elem_ty->isIntegerTy() && v->getType()->isFloatingPointTy())
                v = builder_.CreateFPToSI(v, elem_ty);
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

// -- Enum variant codegen: enum variant ref → i32 constant (variant index) --
llvm::Value* CodeGen::generateEnumVariant(const EnumVariantExpr& e) {
    // Enum variants are represented as i32 constants holding the variant index
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), e.variant_index, false);
}

// -- Match statement codegen: if/else chain comparing i32 --
void CodeGen::generateMatchStmt(const MatchStmt& s) {
    auto* subject_val = generateExpr(*s.subject);
    auto* func = builder_.GetInsertBlock()->getParent();

    // Ensure subject is i32 for comparison
    if (!subject_val->getType()->isIntegerTy(32)) {
        if (subject_val->getType()->isIntegerTy())
            subject_val = builder_.CreateIntCast(subject_val, llvm::Type::getInt32Ty(ctx_), false);
        else
            subject_val = builder_.CreatePtrToInt(subject_val, llvm::Type::getInt32Ty(ctx_));
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
        auto* cmp = builder_.CreateICmpEQ(subject_val, variant_const, "match_cmp");

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

        // If arm has data bindings, introduce variables for them in a scope
        pushScope();
        if (!arm.bindings.empty()) {
            // For enum variants with data — allocate local variables for bindings
            // The data is the subject value itself (which is just the i32 discriminant).
            // Data-carrying enum variants would require heap-allocated data,
            // but for now we just create the bindings as variables.
            for (auto& bname : arm.bindings) {
                auto* a = createEntryBlockAlloca(func,
                    llvm::Type::getInt32Ty(ctx_), bname);
                builder_.CreateStore(
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0), a);
                setNamedValue(bname, a);
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
            builder_.CreateBr(merge_bb);
        }
    }

    // === Dispatch: match the exception type against each catch clause ===
    if (dispatch_bb) {
        builder_.SetInsertPoint(dispatch_bb);
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
            if (isErrorInterface(cc.var_type)) {
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
            } else {
                llvm::Value* bound = nullptr;
                if (!cc.var_type.empty() && cc.var_type != "string") {
                    bound = builder_.CreateCall(runtime_exception_get_object_->getFunctionType(),
                        runtime_exception_get_object_, {}, "exc_obj");
                } else {
                    bound = builder_.CreateCall(runtime_get_error_->getFunctionType(),
                        runtime_get_error_, {}, "err_msg");
                }
                auto* ev = createEntryBlockAlloca(func, ptr_ty, cc.var_name);
                builder_.CreateStore(bound, ev);
                setNamedValue(cc.var_name, ev);
                if (!cc.var_type.empty() && cc.var_type != "string")
                    var_class_map_[cc.var_name] = cc.var_type;
            }
            if (cc.block) generateBlock(*cc.block);
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
        builder_.CreateStore(llvm::ConstantInt::get(i8_ty, 1), finally_flag);
        builder_.CreateBr(finally_bb);
    }

    // === Generate finally block ===
    if (finally_bb) {
        finally_ctx_stack_.back().in_finally = true;
        builder_.SetInsertPoint(finally_bb);
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
                llvm::Value* rv = nullptr;
                if (finally_ret_slot_ && !current_function_->getReturnType()->isVoidTy())
                    rv = builder_.CreateLoad(current_function_->getReturnType(), finally_ret_slot_);
                emitFunctionReturn(rv);
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
        builder_.CreateCall(runtime_exception_pop_->getFunctionType(),
            runtime_exception_pop_, {});
        auto* jb2 = builder_.CreateCall(runtime_exception_get_jmpbuf_->getFunctionType(),
            runtime_exception_get_jmpbuf_, {}, "outer_handler");
        auto* one = llvm::ConstantInt::get(i32_ty, 1);
        builder_.CreateCall(runtime_longjmp_->getFunctionType(), runtime_longjmp_, {jb2, one});
        builder_.CreateUnreachable();
    }

    builder_.SetInsertPoint(merge_bb);
    // Pop this try's handler (all paths converge here).
    builder_.CreateCall(runtime_exception_pop_->getFunctionType(),
        runtime_exception_pop_, {});
}

void CodeGen::emitExceptionRethrow() {
    auto* ptr_ty = llvm::PointerType::get(ctx_, 0);
    auto* i32_ty = llvm::Type::getInt32Ty(ctx_);
    builder_.CreateCall(runtime_exception_pop_->getFunctionType(),
        runtime_exception_pop_, {});
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
        builder_.CreateCall(runtime_throw_object_->getFunctionType(),
            runtime_throw_object_, {obj, llvm::ConstantInt::get(i32_ty, tid)});
    }
    // longjmp to the innermost active handler (stack top).
    auto* jb = builder_.CreateCall(runtime_exception_get_jmpbuf_->getFunctionType(),
        runtime_exception_get_jmpbuf_, {}, "cur_handler");
    auto* one = llvm::ConstantInt::get(i32_ty, 1);
    builder_.CreateCall(runtime_longjmp_->getFunctionType(), runtime_longjmp_, {jb, one});
    builder_.CreateUnreachable();
}

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
    intrinsic_map_["__myp_read_line"] = runtime_read_line_;
    intrinsic_map_["__myp_kbhit"] = runtime_kbhit_;
    intrinsic_map_["__myp_getch"] = runtime_getch_;
    intrinsic_map_["__myp_flush"] = runtime_flush_;
    intrinsic_map_["__myp_term_width"] = runtime_term_width_;
    intrinsic_map_["__myp_term_height"] = runtime_term_height_;
    intrinsic_map_["__myp_strlen"] = runtime_strlen_;
    intrinsic_map_["__myp_chr"] = runtime_chr_;
    intrinsic_map_["__myp_atof"] = runtime_atof_;
    // test intrinsics
    intrinsic_map_["__myp_assert"] = runtime_assert_;
    intrinsic_map_["__myp_assert_eq"] = runtime_assert_eq_;
    intrinsic_map_["__myp_assert_neq"] = runtime_assert_neq_;
    intrinsic_map_["__myp_assert_long_eq"] = runtime_assert_long_eq_;
    intrinsic_map_["__myp_assert_str_eq"] = runtime_assert_str_eq_;
    intrinsic_map_["__myp_assert_str_neq"] = runtime_assert_str_neq_;
    intrinsic_map_["__myp_test_report"] = runtime_test_report_;
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
    // ---- IR optimization pipeline (-O1/-O2/-O3) ----
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
        llvm::PassBuilder PB;
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
    auto host_triple = llvm::Triple(llvm::sys::getDefaultTargetTriple());
    module_->setTargetTriple(host_triple);
    std::string err;
    std::string ts = host_triple.getTriple();
    auto* tgt = llvm::TargetRegistry::lookupTarget(ts, err);
    if (!tgt) { diag_.error(SourceRange{}, "target: " + err); return false; }
    auto* tm = tgt->createTargetMachine(host_triple, "generic", "", llvm::TargetOptions{}, llvm::Reloc::PIC_);
    module_->setDataLayout(tm->createDataLayout());
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
