#include "mylang/CodeGen.h"

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
    // Basic or class type
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
void CodeGen::popScope() { if (!named_values_.empty()) named_values_.pop_back(); }
void CodeGen::setNamedValue(const std::string& n, llvm::Value* a) {
    if (named_values_.empty()) named_values_.emplace_back();
    named_values_.back()[n] = a;
}
llvm::Value* CodeGen::getNamedValue(const std::string& n) {
    for (auto it = named_values_.rbegin(); it != named_values_.rend(); ++it) {
        auto f = it->find(n);
        if (f != it->end()) return f->second;
    }
    return nullptr;
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
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);
    pushScope();

    auto* this_a = createEntryBlockAlloca(func, llvm::PointerType::get(ctx_, 0), "this");
    builder_.CreateStore(func->getArg(0), this_a);
    setNamedValue("this", this_a);

    for (size_t i = 0; i < action.params.size(); ++i) {
        TypeInfo pt = typeNodeToCodegenType(action.params[i].type);
        auto* a = createEntryBlockAlloca(func, getLLVMType(pt), action.params[i].name);
        builder_.CreateStore(func->getArg(i + 1), a);
        setNamedValue(action.params[i].name, a);
        // Record array element type for subscript access
        if (action.params[i].type.isArray() && action.params[i].type.element_type) {
            array_elem_types_[action.params[i].name] = getLLVMType(typeNodeToCodegenType(*action.params[i].type.element_type));
        }
    }

    // Generate action body (stdlib actions use __myp_* intrinsics in their source code)
    if (action.body)
        generateBlock(static_cast<const BlockStmt&>(*action.body));
    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator()) {
        if (scope_functions_.count(func))
            builder_.CreateCall(runtime_event_pop_scope_, {});
        builder_.CreateRetVoid();
    }
    popScope();
}

// -- Generate static action (no 'this' pointer) --
void CodeGen::generateStaticAction(const ClassDecl& cls, const ActionDecl& action) {
    std::string fn = cls.name + "_" + action.name;
    auto* func = module_->getFunction(fn);
    if (!func) return;

    current_function_ = func;
    current_class_name_ = cls.name;
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);
    pushScope();

    for (size_t i = 0; i < action.params.size(); ++i) {
        TypeInfo pt = typeNodeToCodegenType(action.params[i].type);
        auto* a = createEntryBlockAlloca(func, getLLVMType(pt), action.params[i].name);
        builder_.CreateStore(func->getArg(i), a);
        setNamedValue(action.params[i].name, a);
        // Record array element type for subscript access
        if (action.params[i].type.isArray() && action.params[i].type.element_type) {
            array_elem_types_[action.params[i].name] = getLLVMType(typeNodeToCodegenType(*action.params[i].type.element_type));
        }
    }

    if (action.body)
        generateBlock(static_cast<const BlockStmt&>(*action.body));
    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator()) {
        if (scope_functions_.count(func))
            builder_.CreateCall(runtime_event_pop_scope_, {});
        builder_.CreateRetVoid();
    }
    popScope();
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
            pts.push_back(getLLVMType(builtinTypeToInfo(p.type.basic_type)));

        TypeInfo rt = builtinTypeToInfo(method.return_type.basic_type);
        auto* ft = llvm::FunctionType::get(getLLVMType(rt), pts, false);
        llvm::Function::Create(ft, llvm::Function::InternalLinkage, fn, module_.get());
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
        auto* existing = module_->getFunction(fn);
        if (existing) existing->deleteBody();

        std::vector<llvm::Type*> pts;
        // First param: pointer to the struct
        pts.push_back(llvm::PointerType::get(ctx_, 0));
        for (auto& p : method.params) {
            pts.push_back(getLLVMType(builtinTypeToInfo(p.type.basic_type)));
        }

        TypeInfo rt = builtinTypeToInfo(method.return_type.basic_type);
        auto* ft = llvm::FunctionType::get(getLLVMType(rt), pts, false);
        auto* func = llvm::Function::Create(ft, llvm::Function::InternalLinkage, fn, module_.get());

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
                setNamedValue(prop.name, gep);
            }
        }

        // Method parameters
        for (size_t i = 0; i < method.params.size(); ++i) {
            auto* a = createEntryBlockAlloca(func,
                getLLVMType(builtinTypeToInfo(method.params[i].type.basic_type)),
                method.params[i].name);
            builder_.CreateStore(func->getArg(i + 1), a);
            setNamedValue(method.params[i].name, a);
        }

        if (method.body)
            generateBlock(static_cast<const BlockStmt&>(*method.body));
        if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator())
            builder_.CreateRetVoid();
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

    auto* this_a = createEntryBlockAlloca(func, llvm::PointerType::get(ctx_, 0), "this");
    builder_.CreateStore(func->getArg(0), this_a);
    setNamedValue("this", this_a);

    for (size_t i = 0; i < fn_decl.params.size(); ++i) {
        TypeInfo pt = typeNodeToCodegenType(fn_decl.params[i].type);
        auto* a = createEntryBlockAlloca(func, getLLVMType(pt), fn_decl.params[i].name);
        builder_.CreateStore(func->getArg(i + 1), a);
        setNamedValue(fn_decl.params[i].name, a);
    }

    if (fn_decl.body)
        generateBlock(static_cast<const BlockStmt&>(*fn_decl.body));
    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator())
        builder_.CreateRetVoid();
    popScope();
}

void CodeGen::generateFuncDecl(const FuncDecl& decl) {
    std::vector<llvm::Type*> pts;
    for (auto& p : decl.params) {
        TypeInfo pt = typeNodeToCodegenType(p.type);
        pts.push_back(getLLVMType(pt));
    }

    TypeInfo rt = typeNodeToCodegenType(decl.return_type);
    auto* ft = llvm::FunctionType::get(getLLVMType(rt), pts, false);
    auto* func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, decl.name, module_.get());

    size_t i = 0;
    for (auto& arg : func->args()) { if (i < decl.params.size()) arg.setName(decl.params[i].name); ++i; }

    current_function_ = func;
    auto* bb = llvm::BasicBlock::Create(ctx_, "entry", func);
    builder_.SetInsertPoint(bb);
    pushScope();

    i = 0;
    for (auto& arg : func->args()) {
        if (i < decl.params.size()) {
            TypeInfo pt = typeNodeToCodegenType(decl.params[i].type);
            auto* a = createEntryBlockAlloca(func, getLLVMType(pt), decl.params[i].name);
            builder_.CreateStore(&arg, a);
            setNamedValue(decl.params[i].name, a);
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

    if (decl.body) generateBlock(*decl.body);

    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator()) {
        if (scope_functions_.count(func))
            builder_.CreateCall(runtime_event_pop_scope_, {});
        if (rt.kind == TypeKind::Void) builder_.CreateRetVoid();
        else builder_.CreateRet(llvm::ConstantInt::get(getLLVMType(rt), 0));
    }
    popScope();
    if (decl.name == "main") {
        in_main_ = false;
    }
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
        case StmtKind::VarDeclStmt: generateVarDecl(static_cast<const VarDeclStmt&>(s).decl); break;
        case StmtKind::ExprStmt:
            if (static_cast<const ExprStmt&>(s).expression) generateExpr(*static_cast<const ExprStmt&>(s).expression);
            break;
        case StmtKind::IfStmt: generateIfStmt(static_cast<const IfStmt&>(s)); break;
        case StmtKind::WhileStmt: generateWhileStmt(static_cast<const WhileStmt&>(s)); break;
        case StmtKind::ForStmt: generateForStmt(static_cast<const ForStmt&>(s)); break;
        case StmtKind::ReturnStmt: generateReturnStmt(static_cast<const ReturnStmt&>(s)); break;
        case StmtKind::BreakStmt:   generateBreakStmt(static_cast<const BreakStmt&>(s)); break;
        case StmtKind::ContinueStmt: generateContinueStmt(static_cast<const ContinueStmt&>(s)); break;
        case StmtKind::MappingStmt: generateMappingDecl(static_cast<const MappingStmt&>(s).decl, builder_.GetInsertBlock()); break;
        case StmtKind::MatchStmt: generateMatchStmt(static_cast<const MatchStmt&>(s)); break;
        case StmtKind::TryStmt: generateTryStmt(static_cast<const TryStmt&>(s)); break;
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

    llvm::Type* lt;
    if (is_struct) {
        lt = getStructType(d.type.class_name);
    } else if (d.type.isArray() && d.type.element_type) {
        // Array type: allocate as array, store pointer to element 0
        TypeInfo arr_ti = typeNodeToCodegenType(d.type);
        lt = getLLVMType(arr_ti);
        auto* arr_a = createEntryBlockAlloca(current_function_, lt, d.name + "_arr");
        // Zero-init the array
        auto arr_sz = module_->getDataLayout().getTypeAllocSize(lt);
        if (arr_sz > 0)
            builder_.CreateMemSet(arr_a, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0),
                                  arr_sz, llvm::Align(8));
        // Store pointer to first element as the variable's value
        auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
        auto* elem_ptr = builder_.CreateGEP(lt, arr_a, {zero, zero});
        auto* ptr_a = createEntryBlockAlloca(current_function_, llvm::PointerType::get(ctx_, 0), d.name);
        builder_.CreateStore(elem_ptr, ptr_a);
        setNamedValue(d.name, ptr_a);
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
                            builder_.CreateCall(start_func, {loaded});
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

void CodeGen::generateForStmt(const ForStmt& s) {
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
    if (s.body) generateStmt(*s.body);
    if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(sbb);
    f->insert(f->end(), sbb);
    builder_.SetInsertPoint(sbb);
    if (s.step) generateExpr(*s.step);
    builder_.CreateBr(cbb);
    f->insert(f->end(), abb);
    builder_.SetInsertPoint(abb);
    popScope();
}

void CodeGen::generateReturnStmt(const ReturnStmt& s) {
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
    if (s.value) {
        auto* v = generateExpr(*s.value);
        if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator())
            builder_.CreateRet(v);
    } else {
        if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator())
            builder_.CreateRetVoid();
    }
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
        case ExprKind::ThisExpr:       return generateThisExpr(static_cast<const ThisExpr&>(e));
        case ExprKind::Assignment:     return generateAssignment(static_cast<const AssignmentExpr&>(e));
        case ExprKind::Ternary:        return generateTernary(static_cast<const TernaryExpr&>(e));
        case ExprKind::Range:          return generateRange(static_cast<const RangeExpr&>(e));
        case ExprKind::Lambda:         return generateLambda(static_cast<const LambdaExpr&>(e));
        case ExprKind::EnumVariant:    return generateEnumVariant(static_cast<const EnumVariantExpr&>(e));
    }
    return nullptr;
}

llvm::Value* CodeGen::generateIntegerLiteral(const IntegerLiteralExpr& e) {
    // Use the type determined by Sema if available, else default to i32
    if (e.type) {
        auto* lt = getLLVMType(builtinTypeToInfo(e.type->basic_type));
        if (lt) return llvm::ConstantInt::get(lt, e.value, true);
    }
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), e.value, true);
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
        if (runtime_math_exp_ && e.name == "__myp_math_exp") return runtime_math_exp_;
        if (runtime_math_log_ && e.name == "__myp_math_log") return runtime_math_log_;
        if (runtime_math_pow_ && e.name == "__myp_math_pow") return runtime_math_pow_;
        if (runtime_math_abs_int_ && e.name == "__myp_math_abs_int") return runtime_math_abs_int_;
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
    // For non-alloca (GEP pointers), load using the pointer's element type
    return builder_.CreateLoad(a->getType(), a, e.name);
}

llvm::Value* CodeGen::generateBinaryOp(const BinaryOpExpr& e) {
    auto* l = generateExpr(*e.lhs);
    auto* r = generateExpr(*e.rhs);
    if (l->getType() != r->getType()) {
        if (l->getType()->isDoubleTy() || r->getType()->isDoubleTy()) {
            if (!l->getType()->isDoubleTy()) l = builder_.CreateSIToFP(l, llvm::Type::getDoubleTy(ctx_));
            if (!r->getType()->isDoubleTy()) r = builder_.CreateSIToFP(r, llvm::Type::getDoubleTy(ctx_));
        } else if (l->getType()->isFloatTy() || r->getType()->isFloatTy()) {
            if (!l->getType()->isFloatTy()) l = builder_.CreateSIToFP(l, llvm::Type::getFloatTy(ctx_));
            if (!r->getType()->isFloatTy()) r = builder_.CreateSIToFP(r, llvm::Type::getFloatTy(ctx_));
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
        case BinaryOpKind::Eq:  return fp ? builder_.CreateFCmpOEQ(l, r) : builder_.CreateICmpEQ(l, r);
        case BinaryOpKind::Ne:  return fp ? builder_.CreateFCmpONE(l, r) : builder_.CreateICmpNE(l, r);
        case BinaryOpKind::Lt:  return fp ? builder_.CreateFCmpOLT(l, r) : builder_.CreateICmpSLT(l, r);
        case BinaryOpKind::Gt:  return fp ? builder_.CreateFCmpOGT(l, r) : builder_.CreateICmpSGT(l, r);
        case BinaryOpKind::Le:  return fp ? builder_.CreateFCmpOLE(l, r) : builder_.CreateICmpSLE(l, r);
        case BinaryOpKind::Ge:  return fp ? builder_.CreateFCmpOGE(l, r) : builder_.CreateICmpSGE(l, r);
        case BinaryOpKind::And: return builder_.CreateAnd(l, r);
        case BinaryOpKind::Or:  return builder_.CreateOr(l, r);
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
                auto* allocated_ty = llvm::cast<llvm::AllocaInst>(obj_val)->getAllocatedType();
                if (allocated_ty->isStructTy()) {
                    auto* st = llvm::cast<llvm::StructType>(allocated_ty);
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

        size_t num_args = e.args.size();
        if (current_tu_) {
            for (auto& cls : current_tu_->classes) {
                for (auto& a : cls.actions) {
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
            // Fallback: match by name only
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
                }
                is_method = true;
            }
        }

        // Struct method call: v.method()
        if (!callee && ma.object->kind == ExprKind::Identifier) {
            auto& oi = static_cast<const IdentifierExpr&>(*ma.object);
            auto* oa = getNamedValue(oi.name);
            if (oa && llvm::cast<llvm::AllocaInst>(oa)->getAllocatedType()->isStructTy()) {
                std::string st_name = llvm::cast<llvm::AllocaInst>(oa)->getAllocatedType()->getStructName().str();
                std::string fn = "struct_" + st_name + "_" + ma.member_name;
                callee = module_->getFunction(fn);
                if (callee) {
                    mthis = oa;
                    is_method = true;
                }
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
                // First arg must be a string literal (event name)
                std::string event_name;
                if (e.args[0]->kind == ExprKind::StringLiteral) {
                    event_name = static_cast<const StringLiteralExpr&>(*e.args[0]).value;
                } else {
                    diag_.error(e.range, "first argument to __myp_timer_create must be a string literal");
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
                }
                // Look up event ID
                std::string ekey = current_class_name_ + "::" + event_name;
                int event_id = -1;
                auto eit = event_id_map_.find(ekey);
                if (eit != event_id_map_.end()) event_id = eit->second;
                if (event_id < 0) {
                    diag_.error(e.range, "unknown event '" + event_name +
                                "' in class '" + current_class_name_ + "'");
                    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
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
                auto* eid_val = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), event_id);
                auto* param = builder_.CreateSExtOrTrunc(delay, llvm::Type::getInt64Ty(ctx_));
                auto* delay64 = builder_.CreateSExtOrTrunc(delay, llvm::Type::getInt64Ty(ctx_));
                auto* interval64 = builder_.CreateSExtOrTrunc(interval, llvm::Type::getInt64Ty(ctx_));
                auto* result = builder_.CreateCall(runtime_timer_create_->getFunctionType(),
                    runtime_timer_create_,
                    {eid_val, this_ptr, delay64, param, interval64});
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
                // longjmp(&__myp_jmpbuf, 1) — noreturn
                auto* one = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 1);
                builder_.CreateCall(runtime_longjmp_->getFunctionType(),
                    runtime_longjmp_, {global_jmp_buf_, one});
                // Unreachable (longjmp is noreturn, but LLVM needs terminator)
                builder_.CreateUnreachable();
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
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
            if (ma.object->kind == ExprKind::Identifier) {
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
        }
        callee = generateExpr(*e.callee);
    }
    call_ready:

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
    for (auto& p : decl.params)
        pts.push_back(getLLVMType(builtinTypeToInfo(p.type.basic_type)));
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

llvm::Value* CodeGen::generateMemberAccess(const MemberAccessExpr& e) {
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
    if (e.object->kind == ExprKind::Identifier) {
        auto& oi = static_cast<const IdentifierExpr&>(*e.object);
        auto* oa = getNamedValue(oi.name);
        if (oa) {
            auto* allocated_type = llvm::cast<llvm::AllocaInst>(oa)->getAllocatedType();
            // Check if the variable is a struct type
            if (allocated_type->isStructTy()) {
                auto* st = llvm::cast<llvm::StructType>(allocated_type);
                // Find the field index by name
                std::string st_name = st->getName().str();
                unsigned fi = 0;
                if (getStructFieldIndex(st_name, e.member_name, fi)) {
                    auto* gep = builder_.CreateStructGEP(st, oa, fi);
                    auto* field_type = st->getElementType(fi);
                    if (field_type->isArrayTy()) return gep;
                    return builder_.CreateLoad(field_type, gep);
                }
            } else {
                // Class pointer — check for action/event resolution
                auto* op = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0), oa);
                if (op && current_tu_) {
                    // Only allow action/event resolution (for mapping calls etc.)
                }
            }
        }
    }
    // this.prop — internal property access (allowed)
    if (e.object->kind == ExprKind::ThisExpr) {
        auto* ta = getNamedValue("this");
        if (ta) {
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
    return generateExpr(*e.object);
}

llvm::Value* CodeGen::generateSubscript(const SubscriptExpr& e) {
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
            auto* at = llvm::cast<llvm::AllocaInst>(va)->getAllocatedType();
            if (at->isArrayTy()) {
                elem_ty = at->getArrayElementType();
            } else if (at->isPointerTy()) {
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
    return obj;
}

llvm::Value* CodeGen::generateThisExpr(const ThisExpr&) {
    return getNamedValue("this");
}

void CodeGen::generateBreakStmt(const BreakStmt&) {
    if (loop_context_.empty()) return;
    builder_.CreateBr(loop_context_.back().break_bb);
}

void CodeGen::generateContinueStmt(const ContinueStmt&) {
    if (loop_context_.empty()) return;
    builder_.CreateBr(loop_context_.back().continue_bb);
}

llvm::Value* CodeGen::generateRange(const RangeExpr& e) {
    // Range evaluation: returns the start value (for use in for loops mainly)
    return generateExpr(*e.start);
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
        auto* at = llvm::cast<llvm::AllocaInst>(a)->getAllocatedType();
        if (v->getType() != at) {
            if (at->isIntegerTy() && v->getType()->isIntegerTy()) v = builder_.CreateIntCast(v, at, true);
            else if (at->isFloatingPointTy() && v->getType()->isIntegerTy()) v = builder_.CreateSIToFP(v, at);
            else if (at->isIntegerTy() && v->getType()->isFloatingPointTy()) v = builder_.CreateFPToSI(v, at);
        }
        builder_.CreateStore(v, a);
        return v;
    }
    // arr[i] = value
    if (e.target->kind == ExprKind::Subscript) {
        auto& ss = static_cast<const SubscriptExpr&>(*e.target);
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
                if (oa && llvm::cast<llvm::AllocaInst>(oa)->getAllocatedType()->isStructTy()) {
                    auto* st = llvm::cast<llvm::StructType>(llvm::cast<llvm::AllocaInst>(oa)->getAllocatedType());
                    std::string st_name = st->getName().str();
                    unsigned fi = 0;
                    if (getStructFieldIndex(st_name, ma.member_name, fi)) {
                        auto* gep = builder_.CreateStructGEP(st, oa, fi);
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

    // Call setjmp on the global jmp_buf: int setjmp(&__myp_jmpbuf)
    auto* result = builder_.CreateCall(runtime_setjmp_->getFunctionType(),
        runtime_setjmp_, {global_jmp_buf_}, "setjmp_result");

    // Compare result == 0 (normal path) vs != 0 (error/catch path)
    auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    auto* is_error = builder_.CreateICmpNE(result, zero, "is_error");

    auto* try_bb = llvm::BasicBlock::Create(ctx_, "try_block", func);
    auto* catch_bb = s.catch_block
        ? llvm::BasicBlock::Create(ctx_, "catch_block", func) : nullptr;
    auto* finally_bb = s.finally_block
        ? llvm::BasicBlock::Create(ctx_, "finally_block", func) : nullptr;
    auto* merge_bb = llvm::BasicBlock::Create(ctx_, "try_end", func);

    if (catch_bb) {
        builder_.CreateCondBr(is_error, catch_bb, try_bb);
    } else {
        builder_.CreateCondBr(is_error, merge_bb, try_bb);
    }

    // === Generate try block ===
    builder_.SetInsertPoint(try_bb);
    if (s.try_block) generateBlock(*s.try_block);
    if (!builder_.GetInsertBlock()->getTerminator()) {
        if (finally_bb) {
            builder_.CreateBr(finally_bb);
        } else {
            builder_.CreateBr(merge_bb);
        }
    }

    // === Generate catch block ===
    if (catch_bb) {
        builder_.SetInsertPoint(catch_bb);
        // Get the error message via myp_get_error()
        auto* err_ptr = builder_.CreateCall(runtime_get_error_->getFunctionType(),
            runtime_get_error_, {}, "err_msg");
        // Store it as the catch variable
        auto* err_var = createEntryBlockAlloca(func,
            llvm::PointerType::get(ctx_, 0), s.catch_var_name);
        builder_.CreateStore(err_ptr, err_var);
        setNamedValue(s.catch_var_name, err_var);

        if (s.catch_block) generateBlock(*s.catch_block);
        if (!builder_.GetInsertBlock()->getTerminator()) {
            if (finally_bb) {
                builder_.CreateBr(finally_bb);
            } else {
                builder_.CreateBr(merge_bb);
            }
        }
    }

    // === Generate finally block ===
    if (finally_bb) {
        builder_.SetInsertPoint(finally_bb);
        if (s.finally_block) generateBlock(*s.finally_block);
        if (!builder_.GetInsertBlock()->getTerminator()) {
            builder_.CreateBr(merge_bb);
        }
    }

    builder_.SetInsertPoint(merge_bb);
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
    runtime_setjmp_ = llvm::Function::Create(
        llvm::FunctionType::get(i32, {llvm::PointerType::get(ctx_, 0)}, false),
        llvm::Function::ExternalLinkage, "setjmp", module_.get());

    // Longjmp: void longjmp(ptr, int)  (noreturn)
    runtime_longjmp_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {llvm::PointerType::get(ctx_, 0), i32}, false),
        llvm::Function::ExternalLinkage, "longjmp", module_.get());
    runtime_longjmp_->addFnAttr(llvm::Attribute::NoReturn);

    // myp_throw(str) — saves the error message (longjmp is generated inline)
    runtime_throw_ = llvm::Function::Create(
        llvm::FunctionType::get(v, {p}, false),
        llvm::Function::ExternalLinkage, "myp_throw", module_.get());
    runtime_throw_->addFnAttr(llvm::Attribute::NoReturn);

    runtime_get_error_ = llvm::Function::Create(
        llvm::FunctionType::get(p, {}, false),
        llvm::Function::ExternalLinkage, "myp_get_error", module_.get());

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
    intrinsic_map_["__myp_atof"] = runtime_atof_;
    // test intrinsics
    intrinsic_map_["__myp_assert"] = runtime_assert_;
    intrinsic_map_["__myp_assert_eq"] = runtime_assert_eq_;
    intrinsic_map_["__myp_assert_str_eq"] = runtime_assert_str_eq_;
    intrinsic_map_["__myp_test_report"] = runtime_test_report_;
    // __myp_throw is handled specially in generateCall (calls myp_throw + longjmp)
    intrinsic_map_["now"] = runtime_now_ms_;
    intrinsic_map_["sleep"] = runtime_sleep_ms_;
}

// -- Output --
bool CodeGen::writeObjectFile(const std::string& p, int opt_level) {
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

} // namespace mylang