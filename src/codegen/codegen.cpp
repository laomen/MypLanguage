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

    std::string err_str;
    llvm::raw_string_ostream err_os(err_str);
    if (llvm::verifyModule(*module_, &err_os)) {
        diag_.error(SourceRange{}, "LLVM verify failed: " + err_str);
        return "";
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
            members.push_back(getLLVMType(builtinTypeToInfo(prop.type.basic_type)));
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
            members.push_back(getLLVMType(builtinTypeToInfo(prop.type.basic_type)));
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
        if (p.name == fn) return getLLVMType(builtinTypeToInfo(p.type.basic_type));
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
        if (p.name == pn) return getLLVMType(builtinTypeToInfo(p.type.basic_type));
    return llvm::Type::getInt32Ty(ctx_);
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
        case TypeKind::Array: return llvm::PointerType::get(ctx_, 0);
    }
    return llvm::Type::getVoidTy(ctx_);
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
                pts.push_back(getLLVMType(builtinTypeToInfo(p.type.basic_type)));
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

    // Generate function bodies
    for (auto& f : tu.functions) generateFuncDecl(f);
    for (auto& c : tu.classes) generateClass(c);

    // Generate struct method functions (file-level)
    for (auto& st : tu.structs) {
        generateStructMethods(st);
    }
    // Generate struct method functions (nested in classes)
    for (auto& cls : tu.classes) {
        for (auto& st : cls.structs) {
            generateStructMethods(st);
        }
    }

    // Create global instance pointers for classes used in mappings
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
}
void CodeGen::createClassActionDecl(const ClassDecl& cls, const ActionDecl& action) {
    auto fn = cls.name + "_" + action.name;
    if (module_->getFunction(fn)) return;
    std::vector<llvm::Type*> pts = {llvm::PointerType::get(ctx_, 0)};
    for (auto& p : action.params) {
        TypeInfo pt;
        if (!p.type.class_name.empty() && getClassStruct(p.type.class_name)) {
            pt = TypeInfo(TypeKind::Class); pt.class_name = p.type.class_name;
        } else {
            pt = builtinTypeToInfo(p.type.basic_type);
        }
        pts.push_back(getLLVMType(pt));
    }
    auto* ft = llvm::FunctionType::get(getLLVMType(builtinTypeToInfo(action.return_type.basic_type)), pts, false);
    llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn, module_.get());
    // Track if this is a static action for later use
    is_static_action_[fn] = false;
}

void CodeGen::createStaticActionDecl(const ClassDecl& cls, const ActionDecl& action) {
    auto fn = cls.name + "_" + action.name;
    if (module_->getFunction(fn)) return;
    std::vector<llvm::Type*> pts;
    for (auto& p : action.params) {
        TypeInfo pt;
        if (!p.type.class_name.empty() && getClassStruct(p.type.class_name)) {
            pt = TypeInfo(TypeKind::Class); pt.class_name = p.type.class_name;
        } else {
            pt = builtinTypeToInfo(p.type.basic_type);
        }
        pts.push_back(getLLVMType(pt));
    }
    auto* ft = llvm::FunctionType::get(getLLVMType(builtinTypeToInfo(action.return_type.basic_type)), pts, false);
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
            pts.push_back(getLLVMType(builtinTypeToInfo(p.type.basic_type)));
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
            data_types.push_back(getLLVMType(builtinTypeToInfo(ev.params[i].type.basic_type)));
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
        TypeInfo pt;
        if (!action.params[i].type.class_name.empty() && getClassStruct(action.params[i].type.class_name)) {
            pt = TypeInfo(TypeKind::Class); pt.class_name = action.params[i].type.class_name;
        } else {
            pt = builtinTypeToInfo(action.params[i].type.basic_type);
        }
        auto* a = createEntryBlockAlloca(func, getLLVMType(pt), action.params[i].name);
        builder_.CreateStore(func->getArg(i + 1), a);
        setNamedValue(action.params[i].name, a);
    }

    // Generate action body (stdlib actions use __myp_* intrinsics in their source code)
    if (action.body)
        generateBlock(static_cast<const BlockStmt&>(*action.body));
    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator())
        builder_.CreateRetVoid();
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
        TypeInfo pt;
        if (!action.params[i].type.class_name.empty() && getClassStruct(action.params[i].type.class_name)) {
            pt = TypeInfo(TypeKind::Class); pt.class_name = action.params[i].type.class_name;
        } else {
            pt = builtinTypeToInfo(action.params[i].type.basic_type);
        }
        auto* a = createEntryBlockAlloca(func, getLLVMType(pt), action.params[i].name);
        builder_.CreateStore(func->getArg(i), a);
        setNamedValue(action.params[i].name, a);
    }

    if (action.body)
        generateBlock(static_cast<const BlockStmt&>(*action.body));
    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator())
        builder_.CreateRetVoid();
    popScope();
}

// -- Generate struct methods --
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
        TypeInfo pt;
        if (!p.type.class_name.empty() && getClassStruct(p.type.class_name)) {
            pt = TypeInfo(TypeKind::Class); pt.class_name = p.type.class_name;
        } else {
            pt = builtinTypeToInfo(p.type.basic_type);
        }
        pts.push_back(getLLVMType(pt));
    }

    TypeInfo rt = builtinTypeToInfo(fn_decl.return_type.basic_type);
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
        TypeInfo pt;
        if (!fn_decl.params[i].type.class_name.empty() && getClassStruct(fn_decl.params[i].type.class_name)) {
            pt = TypeInfo(TypeKind::Class); pt.class_name = fn_decl.params[i].type.class_name;
        } else {
            pt = builtinTypeToInfo(fn_decl.params[i].type.basic_type);
        }
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
        TypeInfo pt;
        if (!p.type.class_name.empty() && getClassStruct(p.type.class_name)) {
            pt = TypeInfo(TypeKind::Class); pt.class_name = p.type.class_name;
        } else {
            pt = builtinTypeToInfo(p.type.basic_type);
        }
        pts.push_back(getLLVMType(pt));
    }

    TypeInfo rt = builtinTypeToInfo(decl.return_type.basic_type);
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
            auto* a = createEntryBlockAlloca(func, arg.getType(), decl.params[i].name);
            builder_.CreateStore(&arg, a);
            setNamedValue(decl.params[i].name, a);
        }
        ++i;
    }

    // For main: call init mappings and track for cleanup
    if (decl.name == "main") {
        in_main_ = true;
        if (init_func_) {
            builder_.CreateCall(init_func_, {});
        }
    }

    if (decl.body) generateBlock(*decl.body);

    if (builder_.GetInsertBlock() && !builder_.GetInsertBlock()->getTerminator()) {
        if (rt.kind == TypeKind::Void) builder_.CreateRetVoid();
        else builder_.CreateRet(llvm::ConstantInt::get(getLLVMType(rt), 0));
    }
    popScope();
}

// -- Mapping --
void CodeGen::generateMappingDecl(const MappingDecl& decl, llvm::BasicBlock* insert_bb) {
    if (!runtime_event_register_) return;

    for (auto& chain : decl.chains) {
        if (chain.nodes.size() < 2) continue;
        auto& src = chain.nodes[0];

        // Find source class and event to determine data struct layout
        const ClassDecl* src_cls = nullptr;
        const EventDecl* src_ev = nullptr;
        int event_id = 0;
        if (current_tu_) {
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

        for (size_t i = 1; i < chain.nodes.size(); ++i) {
            auto& tgt = chain.nodes[i];
            std::string tf = tgt.source_name + "_" + tgt.member_name;
            auto* callee = module_->getFunction(tf);

            if (callee) {
                std::vector<llvm::Value*> call_args;

                // Instance pointer: use global if available, otherwise fallback
                auto git = class_instance_globals_.find(tgt.source_name);
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

                // Forward event data or previous return value
                auto* ft = callee->getFunctionType();
                size_t num_expected = ft->getNumParams();

                if (num_expected > call_args.size()) {
                    // Unpack event data from struct if this is the first target
                    if (i == 1 && src_ev && src_ev->params.size() > 0) {
                        // Reconstruct the struct type to GEP into it
                        std::vector<llvm::Type*> param_types;
                        for (auto& p : src_ev->params)
                            param_types.push_back(getLLVMType(builtinTypeToInfo(p.type.basic_type)));
                        auto* data_struct = llvm::StructType::create(ctx_, param_types, "evdata");
                        auto* data_ptr = builder_.CreateBitCast(handler->getArg(1),
                            llvm::PointerType::get(ctx_, 0));

                        for (size_t pi = 0; pi < src_ev->params.size() && call_args.size() < num_expected; pi++) {
                            auto* gep = builder_.CreateStructGEP(data_struct, data_ptr, pi);
                            auto* loaded = builder_.CreateLoad(getLLVMType(builtinTypeToInfo(src_ev->params[pi].type.basic_type)), gep);
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
        default: break;
    }
}

void CodeGen::generateBlock(const BlockStmt& s) {
    if (!builder_.GetInsertBlock()) return;
    pushScope();
    for (auto& st : s.statements) if (st) generateStmt(*st);
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
                auto git = class_instance_globals_.find(ne.class_name);
                if (git != class_instance_globals_.end())
                    builder_.CreateStore(instance_ptr, git->second);

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

    // Detect struct type — allocate inline on stack
    bool is_struct = !d.type.class_name.empty() && getStructType(d.type.class_name) != nullptr;

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
    } else {
        vt = builtinTypeToInfo(d.type.basic_type);
        if (!d.type.class_name.empty() && getClassStruct(d.type.class_name)) {
            vt = TypeInfo(TypeKind::Class); vt.class_name = d.type.class_name;
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
            auto git = class_instance_globals_.find(ne.class_name);
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
    if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(mbb);
    f->insert(f->end(), ebb);
    builder_.SetInsertPoint(ebb);
    if (s.else_block) generateStmt(*s.else_block);
    if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(mbb);
    f->insert(f->end(), mbb);
    builder_.SetInsertPoint(mbb);
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
                          l->getType()->isPointerTy() && r->getType()->isPointerTy();
    if (is_str_concat) {
        // Call runtime myp_strcat(l, r) which returns a new allocated string
        auto* sc = module_->getFunction("myp_strcat");
        if (!sc) {
            auto* ft = llvm::FunctionType::get(llvm::PointerType::get(ctx_, 0),
                {llvm::PointerType::get(ctx_, 0), llvm::PointerType::get(ctx_, 0)}, false);
            sc = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "myp_strcat", module_.get());
        }
        return builder_.CreateCall(sc, {l, r}, "strcat");
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
        // Find the correct class by matching function name AND arg count
        std::string best_class;
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

llvm::Value* CodeGen::generateMemberAccess(const MemberAccessExpr& e) {
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
    // Derive element type from the pointer type
    auto* ptr_ty = llvm::dyn_cast<llvm::PointerType>(a->getType());
    if (!ptr_ty) {
        diag_.error(e.range, "cannot subscript non-pointer type");
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    }
    auto* elem_ty = ptr_ty->getArrayElementType();
    auto* p = builder_.CreateGEP(elem_ty, a, i);
    return builder_.CreateLoad(elem_ty, p);
}

llvm::Value* CodeGen::generateNewExpr(const NewExpr& e) {
    auto* st = getClassStruct(e.class_name);
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
    // arr[i] = value
    if (e.target->kind == ExprKind::Subscript) {
        auto& ss = static_cast<const SubscriptExpr&>(*e.target);
        auto* a = generateExpr(*ss.array);
        auto* i = generateExpr(*ss.index);
        auto* ptr_ty = llvm::dyn_cast<llvm::PointerType>(a->getType());
        if (ptr_ty) {
            auto* elem_ty = ptr_ty->getArrayElementType();
            auto* gep = builder_.CreateGEP(elem_ty, a, i);
            auto* v = generateExpr(*e.value);
            if (v->getType() != elem_ty) {
                if (elem_ty->isIntegerTy() && v->getType()->isIntegerTy())
                    v = builder_.CreateIntCast(v, elem_ty, true);
            }
            builder_.CreateStore(v, gep);
            return v;
        }
    }
    diag_.error(e.range, "not a valid assignment target");
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
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