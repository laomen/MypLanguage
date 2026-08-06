#include "mylang/Sema.h"

#include <algorithm>
#include <unordered_set>

namespace mylang {

// ==============================
// Constructor
// ==============================

Sema::Sema(DiagnosticEngine& diag)
    : diag_(diag) {}

bool Sema::analyze(TranslationUnit& tu) {
    current_tu_ = &tu;
    diag_.reset();

    // Register intrinsic functions (for stdlib use)
    registerIntrinsics();

    // === Pass 0: Validate type aliases (catch unknown/recursive even if unused) ===
    for (auto& ta : tu.type_aliases) {
        typeNodeToTypeInfo(ta.alias_type);
    }
    if (diag_.hasErrors()) return false;

    // === Pass 1: Collect all top-level declarations ===
    visitTranslationUnit(tu);
    if (diag_.hasErrors()) return false;

    // `mypc run`：单类文件无 main 时自动注入合成 main（实例化 @startup 类并触发其
    // 入口 action）。仅 run 模式生效——正常编译仍要求用户显式 main（链接期报错）。
    if (auto_main_) injectAutoMainIfNeeded(tu);

    // === Pass 1.5: Check mapping cycles + type compatibility ===
    for (auto& m : tu.mappings) {
        checkMappingCycles(m);
        checkMappingTypes(m);
        // Process lambda expressions used as mapping chain nodes
        for (auto& chain : m.chains) {
            for (auto& node : chain.nodes) {
                if (node.is_lambda && node.lambda)
                    visitLambda(*node.lambda);
            }
        }
    }
    if (diag_.hasErrors()) return false;

    // === Pass 2: Type-check function bodies ===
    // NOTE: visitFuncBody may monomorphize generic functions (appending to
    // tu.functions → reallocation). Iterate by index, re-fetch per element.
    // Generic templates AND monomorphized instances are skipped: their bodies
    // are shared and type-checked with placeholders would monomorphize generic
    // classes with type-param TypeNodes (e.g. `new Option<R>()`), leaving
    // invalid `R`-named members in the spurious instance. Codegen substitutes
    // type params per-instance, so bodies are correct at runtime.
    for (size_t fi = 0; fi < tu.functions.size(); fi++) {
        if (tu.functions[fi].is_generic_inst) continue;
        if (!tu.functions[fi].type_params.empty()) continue;
        visitFuncBody(tu.functions[fi]);
    }

    // Type-check action bodies inside classes.
    // NOTE: visitStmt() may monomorphize generic classes (e.g. Box<int>), which
    // appends to tu.classes and can REALLOCATE the vector. A range-for would hold
    // a dangling ClassDecl& across that reallocation → use-after-free. Always
    // re-fetch the class by index, and re-fetch per sub-loop / per element.
    for (size_t ci = 0; ci < tu.classes.size(); ci++) {
        // Monomorphized instances share the template's body AST but local generic
        // type params (V in "V x = ...") are not substituted, so their bodies
        // cannot be type-checked here. The template itself is checked below with
        // generic params as placeholders; codegen performs the real substitution.
        if (tu.classes[ci].is_generic_inst)
            continue;
        current_class_name_ = tu.classes[ci].name;
        in_class_method_ = true;
        // Actions (may trigger monomorphization → tu.classes may reallocate)
        size_t nactions = tu.classes[ci].actions.size();
        for (size_t ai = 0; ai < nactions; ai++) {
            auto& action = tu.classes[ci].actions[ai];
            if (action.body) {
                current_return_type_ = typeNodeToTypeInfo(action.return_type);
                in_coro_method_ = action.has_coro;
                symbol_table_.enterScope();
                TypeInfo this_type(TypeKind::Class);
                this_type.class_name = current_class_name_;
                symbol_table_.declare("this", this_type);
                for (auto& param : action.params) {
                    auto param_type = typeNodeToTypeInfo(param.type);
                    symbol_table_.declare(param.name, param_type);
                }
                visitStmt(*action.body);
                symbol_table_.leaveScope();
                in_coro_method_ = false;
            }
        }
        // Type-check function: section bodies (re-fetch: tu.classes may have
        // been reallocated by monomorphization above)
        if (ci >= tu.classes.size()) break;
        size_t nfuncs = tu.classes[ci].functions.size();
        for (size_t fi = 0; fi < nfuncs; fi++) {
            auto& func = tu.classes[ci].functions[fi];
            if (func.body) {
                current_return_type_ = typeNodeToTypeInfo(func.return_type);
                in_coro_method_ = false;
                symbol_table_.enterScope();
                TypeInfo this_type(TypeKind::Class);
                this_type.class_name = current_class_name_;
                symbol_table_.declare("this", this_type);
                for (auto& param : func.params) {
                    symbol_table_.declare(param.name, typeNodeToTypeInfo(param.type));
                }
                visitStmt(*func.body);
                symbol_table_.leaveScope();
                in_coro_method_ = false;
            }
        }
        // Type-check static: section bodies (no 'this')
        if (ci >= tu.classes.size()) break;
        size_t nstatic = tu.classes[ci].static_actions.size();
        for (size_t si = 0; si < nstatic; si++) {
            auto& action = tu.classes[ci].static_actions[si];
            if (!action.body) continue;
            // Generic static method templates are skipped (like generic function
            // templates): their bodies are shared and monomorphized per call.
            if (!action.type_params.empty()) continue;
            current_return_type_ = typeNodeToTypeInfo(action.return_type);
            in_coro_method_ = false;
            symbol_table_.enterScope();
            // No 'this' for static methods
            for (auto& param : action.params) {
                symbol_table_.declare(param.name, typeNodeToTypeInfo(param.type));
            }
            visitStmt(*action.body);
            symbol_table_.leaveScope();
            in_coro_method_ = false;
        }
        in_class_method_ = false;
    }

    // Type-check struct method bodies (file-level and nested)
    for (auto& st : tu.structs) {
        checkStructMethods(st);
    }
    for (auto& cls : tu.classes) {
        for (auto& st : cls.structs) {
            checkStructMethods(st);
        }
    }

    return !diag_.hasErrors();
}

void Sema::checkStructMethods(const StructDecl& decl) {
    std::string type_key = decl.parent_class.empty()
        ? decl.name : decl.parent_class + "::" + decl.name;

    // Pre-compute method type info for sibling method resolution
    struct MethodInfo {
        std::string name;
        TypeInfo type;
    };
    std::vector<MethodInfo> sibling_methods;
    for (auto& fn : decl.functions) {
        // 构造器不注册为兄弟方法：不可裸名调用，且其名==struct 名会遮蔽 struct 类型名
        if (fn.has_constructor) continue;
        TypeInfo ft(TypeKind::Function);
        ft.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(fn.return_type));
        for (auto& p : fn.params)
            ft.param_types.push_back(typeNodeToTypeInfo(p.type));
        sibling_methods.push_back({fn.name, ft});
    }

    for (auto& func : decl.functions) {
        if (func.body) {
            current_return_type_ = typeNodeToTypeInfo(func.return_type);
            symbol_table_.enterScope();

            // 'this' is available in struct methods (type = current struct)
            TypeInfo this_type(TypeKind::Struct);
            this_type.class_name = type_key;
            symbol_table_.declare("this", this_type);

            // Struct fields are accessible by bare name
            for (auto& prop : decl.properties) {
                symbol_table_.declare(prop.name, typeNodeToTypeInfo(prop.type));
            }

            // Sibling methods are callable by bare name (e.g., getNeutronXS calls fillNeutronXS)
            for (auto& mi : sibling_methods) {
                symbol_table_.declare(mi.name, mi.type);
            }

            for (auto& param : func.params) {
                symbol_table_.declare(param.name, typeNodeToTypeInfo(param.type));
            }

            bool saved_in_struct = in_struct_method_;
            std::string saved_struct_key = current_struct_type_key_;
            bool saved_in_main = in_main_function_;
            in_struct_method_ = true;
            current_struct_type_key_ = type_key;
            in_main_function_ = false;  // Allow function calls from struct methods

            visitStmt(*func.body);

            in_struct_method_ = saved_in_struct;
            current_struct_type_key_ = saved_struct_key;
            in_main_function_ = saved_in_main;

            symbol_table_.leaveScope();
        }
    }
}

void Sema::checkInterfaceImpl(const ClassDecl& cls) {
    // Find the interface
    InterfaceDecl* iface = nullptr;
    for (auto& ifd : current_tu_->interfaces) {
        if (ifd.name == cls.interface_class_name) {
            iface = &ifd;
            break;
        }
    }
    if (!iface) {
        error(cls.range, "interface '" + cls.interface_class_name + "' not found");
        return;
    }
    // Check all interface actions exist in the class
    for (auto& ia : iface->actions) {
        // 默认实现（trait 默认方法）：接口方法带 body → 类可省略，虚表回退默认
        if (ia.body) continue;
        // 接口签名含关联类型（Item）时类型由实现类绑定 → 只按名称匹配；
        // 否则按 名称 + 返回类型 basic_type 匹配（既有粗粒度签名比较）。
        bool iface_uses_assoc = (typeNodeToTypeInfo(ia.return_type).kind == TypeKind::Assoc);
        if (!iface_uses_assoc)
            for (auto& p : ia.params)
                if (typeNodeToTypeInfo(p.type).kind == TypeKind::Assoc) { iface_uses_assoc = true; break; }
        auto matches = [&](const ActionDecl& ca) {
            if (ca.name != ia.name) return false;
            if (iface_uses_assoc) return true;
            return ca.return_type.basic_type == ia.return_type.basic_type;
        };
        bool found = false;
        for (auto& ca : cls.actions)
            if (matches(ca)) { found = true; break; }
        if (!found) {
            for (auto& ca : cls.static_actions)
                if (matches(ca)) { found = true; break; }
        }
        if (!found) {
            error(cls.range, "class '" + cls.name + "' does not implement action '" + ia.name + "' from interface '" + iface->name + "'");
        }
    }
    // Check all interface events exist in the class
    for (auto& ie : iface->events) {
        bool found = false;
        for (auto& ce : cls.events) {
            if (ce.name == ie.name) {
                found = true;
                break;
            }
        }
        if (!found) {
            error(cls.range, "class '" + cls.name + "' does not implement event '" + ie.name + "' from interface '" + iface->name + "'");
        }
    }
    // 关联类型（§三-5）：实现类必须绑定接口声明的全部关联类型
    for (auto& at : iface->associated_types) {
        auto bit = cls.associated_type_bindings.find(at);
        if (bit == cls.associated_type_bindings.end()) {
            error(cls.range, "class '" + cls.name + "' does not bind associated type '" +
                at + "' from interface '" + iface->name + "' (add `type " + at + " = ...;`)");
            continue;
        }
        // 校验绑定类型可解析（未知类型/递归别名等在此报错）
        typeNodeToTypeInfo(bit->second);
    }
}

// ==============================
// Pass 1: Collect declarations
// ==============================

void Sema::visitTranslationUnit(TranslationUnit& tu) {
    // stdlib files are already loaded and merged into tu by main.cpp

    // Register all struct types first (so classes can reference them)
    for (auto& st : tu.structs) {
        visitStructDecl(st);
    }
    // Register generic class templates
    for (size_t i = 0; i < tu.classes.size(); i++) {
        if (!tu.classes[i].type_params.empty()) {
            GenericInfo info;
            info.tu_index = i;
            generic_classes_[tu.classes[i].name] = info;
        }
    }
    // Register generic function templates
    for (size_t i = 0; i < tu.functions.size(); i++) {
        if (!tu.functions[i].type_params.empty()) {
            GenericFuncInfo info;
            info.tu_index = i;
            generic_functions_[tu.functions[i].name] = info;
        }
    }
    // Also register nested structs inside classes
    for (auto& cls : tu.classes) {
        for (auto& st : cls.structs) {
            visitStructDecl(st);
        }
    }

    for (auto& ff : tu.ffis) {
        visitFFI(ff);
    }
    for (auto& en : tu.enums) {
        visitEnumDecl(en);
    }
    for (auto& cls : tu.classes) {
        visitClassDecl(cls);
    }
    // Check interface implementations
    for (auto& cls : tu.classes) {
        if (!cls.interface_class_name.empty()) {
            checkInterfaceImpl(cls);
        }
    }
    for (auto& iface : tu.interfaces) {
        visitInterfaceDecl(iface);
    }
    for (auto& func : tu.functions) {
        visitFuncDecl(func);
    }
}

// `mypc run` 自动 main：无用户 main 且恰好一个类带 @startup 时，注入
//   int main() { ClassName c = new ClassName(); c.startupAction(); return 0; }
// 由 analyze() 在 Pass 1 之后调用，注入的 main 会被 Pass 2 完整类型检查（构造器
// 解析/方法解析同普通代码）。
void Sema::injectAutoMainIfNeeded(TranslationUnit& tu) {
    // 用户已定义 main → 直接用，不注入
    for (auto& f : tu.functions)
        if (f.name == "main") return;

    // 收集带 @startup 的类（跳过泛型实例类）
    std::string cls_name, startup_name;
    int count = 0;
    for (auto& cls : tu.classes) {
        if (cls.is_generic_inst) continue;
        for (auto& a : cls.actions) {
            if (a.has_startup) {
                cls_name = cls.name;
                startup_name = a.name;
                count++;
                break;
            }
        }
    }
    if (count == 0) {
        error(SourceRange{}, "run: 无 'main' 函数且无带 '@startup' 注解的类——"
              "请定义 main() 或给类 action 加 '@startup' 注解");
        return;
    }
    if (count > 1) {
        error(SourceRange{}, "run: 无 'main' 函数但有多个类带 '@startup'——"
              "请显式定义 main()");
        return;
    }

    // 构造合成 main 的 AST（与手写等价）
    SourceRange r;
    TypeNode cls_tn;
    cls_tn.class_name = cls_name;

    // ClassName c = new ClassName();
    auto new_expr = std::make_unique<NewExpr>(cls_name, std::vector<TypeNode>{},
                                              std::vector<std::unique_ptr<Expr>>{}, r);
    VarDecl vd;
    vd.name = "c";
    vd.type = cls_tn;
    vd.range = r;
    vd.init_expr = std::move(new_expr);
    auto vds = std::make_unique<VarDeclStmt>(vd);

    // c.startupAction();
    auto ma = std::make_unique<MemberAccessExpr>(
        std::make_unique<IdentifierExpr>("c", r), startup_name, r);
    auto call = std::make_unique<CallExpr>(std::move(ma),
                                           std::vector<std::unique_ptr<Expr>>{}, r);
    auto es = std::make_unique<ExprStmt>(std::move(call), r);

    // return 0;
    auto ret = std::make_unique<ReturnStmt>(std::make_unique<IntegerLiteralExpr>(0, r), r);

    std::vector<std::unique_ptr<Stmt>> stmts;
    stmts.push_back(std::move(vds));
    stmts.push_back(std::move(es));
    stmts.push_back(std::move(ret));

    FuncDecl main_decl;
    main_decl.name = "main";
    main_decl.return_type = TypeNode(); // int（默认）
    main_decl.is_auto_main = true;      // 豁免 main() 直接调用限制（编译器生成）
    main_decl.body = std::make_unique<BlockStmt>(std::move(stmts), r);
    tu.functions.push_back(std::move(main_decl));
}

void Sema::visitEnumDecl(EnumDecl& decl) {
    if (symbol_table_.lookup(decl.name)) {
        error(decl.range, "duplicate enum name '" + decl.name + "'");
        return;
    }
    // Register the enum type
    TypeInfo enum_type(TypeKind::Enum);
    enum_type.class_name = decl.name;
    symbol_table_.declare(decl.name, enum_type);

    // Store enum info for later reference
    EnumInfo info;
    info.variants = decl.variants;
    enum_info_[decl.name] = info;
}

void Sema::visitStructDecl(StructDecl& decl) {
    std::string type_key = decl.parent_class.empty()
        ? decl.name
        : decl.parent_class + "::" + decl.name;
    if (symbol_table_.lookup(type_key)) {
        error(decl.range, "duplicate struct name '" + type_key + "'");
        return;
    }
    TypeInfo struct_type(TypeKind::Struct);
    struct_type.class_name = type_key;
    symbol_table_.declare(type_key, struct_type);

    // Validate struct fields: duplicate names and void-typed fields are rejected.
    for (size_t i = 0; i < decl.properties.size(); ++i) {
        auto& prop = decl.properties[i];
        TypeInfo ft = typeNodeToTypeInfo(prop.type);
        if (ft.kind == TypeKind::Void && prop.type.class_name.empty()) {
            error(prop.range, "cannot declare field of type 'void'");
        }
        for (size_t j = 0; j < i; ++j) {
            if (decl.properties[j].name == prop.name) {
                error(prop.range, "duplicate field '" + prop.name +
                      "' in struct '" + type_key + "'");
                break;
            }
        }
    }

    // Register struct methods
    for (auto& func : decl.functions) {
        // 构造器：不注册为可调用方法（构造器不能直接调用，同名重载合法）；
        // M3 负责函数式构造。body 仍在 checkStructMethods 中带 struct 作用域检查。
        if (func.has_constructor) {
            if (func.name != decl.name) {
                error(func.range, "constructor name '" + func.name +
                      "' must match struct name '" + decl.name + "'");
            }
            if (typeNodeToTypeInfo(func.return_type).kind != TypeKind::Void) {
                error(func.range, "constructor '" + func.name +
                      "' must have void return type");
            }
            continue;
        }
        TypeInfo func_type(TypeKind::Function);
        func_type.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(func.return_type));
        for (auto& param : func.params) {
            TypeInfo pt = typeNodeToTypeInfo(param.type);
            if (pt.kind == TypeKind::Void && param.type.class_name.empty()) {
                error(param.range, "cannot declare parameter of type 'void'");
            }
            func_type.param_types.push_back(pt);
            func_type.param_is_ref.push_back(param.is_ref);
        }
        std::string method_name = type_key + "::" + func.name;
        if (symbol_table_.lookup(method_name)) {
            error(func.range, "duplicate method '" + func.name +
                  "' in struct '" + type_key + "'");
        }
        symbol_table_.declare(method_name, func_type);
    }
}

void Sema::visitClassDecl(ClassDecl& decl) {
    if (symbol_table_.lookup(decl.name)) {
        error(decl.range, "duplicate class name '" + decl.name + "'");
        return;
    }

    TypeInfo class_type(TypeKind::Class);
    class_type.class_name = decl.name;
    symbol_table_.declare(decl.name, class_type);

    // Register generic type parameters as valid types within the class scope
    for (auto& tp : decl.type_params) {
        TypeInfo tp_type(TypeKind::Int);
        // 约束类型参数（where T : I，§三-5）→ 注册为接口类型，使模板体内
        // T 上的方法调用 / T::Item 可静态检查（运行时单态化到具体类）。
        auto cit = decl.type_param_constraints.find(tp);
        if (cit != decl.type_param_constraints.end()) {
            tp_type = TypeInfo(TypeKind::Interface);
            tp_type.class_name = cit->second;
        }
        symbol_table_.declare(tp, tp_type);
    }

    // Set current class name for member type resolution
    current_class_name_ = decl.name;

    // Enter class scope to register members
    symbol_table_.enterScope();

    for (auto& prop : decl.properties) {
        if (!symbol_table_.declare(prop.name, typeNodeToTypeInfo(prop.type))) {
            error(prop.range, "duplicate member '" + prop.name + "' in class '" + decl.name + "'");
        }
    }

    for (auto& action : decl.actions) {
        // 构造器：不注册为可调用 action（构造器不能直接调用，同名重载合法）；
        // M2 负责 new 绑定。body 仍在独立 pass 中带类作用域检查。
        if (action.has_constructor) {
            if (action.has_startup) {
                error(action.range, "cannot be both @constructor and @startup");
            }
            if (action.name != decl.name && !decl.is_generic_inst) {
                error(action.range, "constructor name '" + action.name +
                      "' must match class name '" + decl.name + "'");
            }
            if (typeNodeToTypeInfo(action.return_type).kind != TypeKind::Void) {
                error(action.range, "constructor '" + action.name + "' must have void return type");
            }
            continue;
        }
        TypeInfo func_type(TypeKind::Function);
        func_type.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(action.return_type));
        for (auto& param : action.params) {
            func_type.param_types.push_back(typeNodeToTypeInfo(param.type));
            func_type.param_is_ref.push_back(param.is_ref);
        }
        if (!symbol_table_.declare(action.name, func_type)) {
            error(action.range, "duplicate action '" + action.name + "' in class '" + decl.name + "'");
        }
    }

    // Register function: section methods in class scope (callable by bare name
    // from actions and other function: methods regardless of declaration order).
    // Previously only resolved via the in_class_method_ fallback, which missed
    // methods declared later in the section.
    for (auto& fn : decl.functions) {
        // 构造器：不注册为可调用 function（构造器不能直接调用，同名重载合法）；
        // M2 负责 new 绑定。body 仍在独立 pass 中带类作用域检查。
        if (fn.has_constructor) {
            if (fn.name != decl.name && !decl.is_generic_inst) {
                error(fn.range, "constructor name '" + fn.name +
                      "' must match class name '" + decl.name + "'");
            }
            if (typeNodeToTypeInfo(fn.return_type).kind != TypeKind::Void) {
                error(fn.range, "constructor '" + fn.name + "' must have void return type");
            }
            continue;
        }
        TypeInfo func_type(TypeKind::Function);
        func_type.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(fn.return_type));
        for (auto& param : fn.params) {
            func_type.param_types.push_back(typeNodeToTypeInfo(param.type));
            func_type.param_is_ref.push_back(param.is_ref);
        }
        if (!symbol_table_.declare(fn.name, func_type)) {
            error(fn.range, "duplicate function '" + fn.name + "' in class '" + decl.name + "'");
        }
    }

    // Register static actions in GLOBAL scope (accessible as ClassName.method)
    for (size_t sa_idx = 0; sa_idx < decl.static_actions.size(); sa_idx++) {
        auto& action = decl.static_actions[sa_idx];
        // Generic static method: List.map<T,U>(...) — register for call resolution
        // (monomorphization happens at the call site; the template isn't callable
        // directly since its type params are placeholders).
        if (!action.type_params.empty()) {
            std::string gkey = decl.name + "::" + action.name;
            GenericStaticMethodInfo ginfo;
            // Find the class index (decl may be a generic instance clone; use the
            // original template's index for stable registration).
            for (size_t ci = 0; ci < current_tu_->classes.size(); ci++) {
                if (current_tu_->classes[ci].name == decl.name &&
                    !current_tu_->classes[ci].is_generic_inst) {
                    ginfo.class_index = (int)ci;
                    break;
                }
            }
            ginfo.action_index = (int)sa_idx;
            generic_static_methods_[gkey] = ginfo;
            continue; // template not directly callable
        }
        TypeInfo func_type(TypeKind::Function);
        func_type.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(action.return_type));
        for (auto& param : action.params) {
            func_type.param_types.push_back(typeNodeToTypeInfo(param.type));
            func_type.param_is_ref.push_back(param.is_ref);
        }
        // Register as ClassName.methodName in global scope
        std::string static_name = decl.name + "." + action.name;
        symbol_table_.declare(static_name, func_type);
        // Also register bare name for direct calls from any context
        if (!symbol_table_.lookup(action.name)) {
            symbol_table_.declare(action.name, func_type);
        }
    }

    // Collect fire function declarations (to register in global scope)
    struct FireFunc {
        std::string name;
        TypeInfo type;
    };
    std::vector<FireFunc> fire_funcs;

    for (auto& event : decl.events) {
        if (symbol_table_.lookup(event.name)) {
            error(event.range, "duplicate event '" + event.name + "' in class '" + decl.name + "'");
        } else {
            TypeInfo event_type(TypeKind::Function);
            event_type.return_type = std::make_shared<TypeInfo>(TypeKind::Void);
            for (auto& param : event.params) {
                event_type.param_types.push_back(typeNodeToTypeInfo(param.type));
            }
            symbol_table_.declare(event.name, event_type);

            // Prepare fire_ClassName_EventName registration
            FireFunc ff;
            ff.name = "fire_" + decl.name + "_" + event.name;
            ff.type = TypeInfo(TypeKind::Function);
            ff.type.return_type = std::make_shared<TypeInfo>(TypeKind::Void);
            TypeInfo instance_type(TypeKind::Class);
            instance_type.class_name = decl.name;
            ff.type.param_types.push_back(instance_type);
            for (auto& param : event.params) {
                ff.type.param_types.push_back(typeNodeToTypeInfo(param.type));
            }
            fire_funcs.push_back(ff);
        }
    }

    symbol_table_.leaveScope();

    // Register fire functions in global scope
    for (auto& ff : fire_funcs) {
        if (!symbol_table_.lookup(ff.name)) {
            symbol_table_.declare(ff.name, ff.type);
        }
    }
}

void Sema::visitInterfaceDecl(InterfaceDecl& decl) {
    if (symbol_table_.lookup(decl.name)) {
        error(decl.range, "duplicate interface name '" + decl.name + "'");
        return;
    }
    TypeInfo iface_type(TypeKind::Class);
    iface_type.class_name = decl.name;
    symbol_table_.declare(decl.name, iface_type);
}

void Sema::visitFuncDecl(FuncDecl& decl) {
    if (symbol_table_.lookup(decl.name)) {
        error(decl.range, "duplicate function name '" + decl.name + "'");
        return;
    }
    // Generic function template: type params resolve to Int placeholder here
    // (real substitution happens at monomorphization; template body is checked
    // with placeholders in visitFuncBody, mirroring generic classes).
    auto saved_tp = current_func_type_params_;
    current_func_type_params_ = decl.type_params;
    TypeInfo func_type(TypeKind::Function);
    func_type.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(decl.return_type));
    for (auto& param : decl.params) {
        func_type.param_types.push_back(typeNodeToTypeInfo(param.type));
        func_type.param_is_ref.push_back(param.is_ref);
    }
    current_func_type_params_ = saved_tp;
    symbol_table_.declare(decl.name, func_type);
}

// ==============================
// Pass 2: Type-check function bodies
// ==============================

void Sema::visitFuncBody(FuncDecl& decl) {
    // @macro (M4 proc-macro): the body runs at compile time (interpreter) and
    // is not type-checked here — AST types (StmtList/Stmt/Expr) and quote{...}
    // are only meaningful in the eval environment.
    if (decl.has_proc_macro) return;

    symbol_table_.enterScope();
    // Generic function template body: type params resolve to Int placeholder.
    auto saved_tp = current_func_type_params_;
    current_func_type_params_ = decl.type_params;
    current_return_type_ = typeNodeToTypeInfo(decl.return_type);

    for (auto& param : decl.params) {
        symbol_table_.declare(param.name, typeNodeToTypeInfo(param.type));
    }

    // In main(), only instance creation + mapping is allowed — no direct method calls
    // (编译器注入的合成 main 豁免：它本就是要直接触发 @startup 入口)
    if (decl.name == "main" && !decl.is_auto_main) in_main_function_ = true;

    in_coro_method_ = decl.has_coro;  // top-level @coro function: allow await

    if (decl.body) {
        visitStmt(*decl.body);
    }

    in_coro_method_ = false;
    in_main_function_ = false;
    current_func_type_params_ = saved_tp;
    symbol_table_.leaveScope();
}

// ==============================
// Statement type checking
// ==============================

Sema::StmtResult Sema::visitStmt(Stmt& stmt) {
    switch (stmt.kind) {
        case StmtKind::Block:
            return visitBlock(static_cast<BlockStmt&>(stmt));
        case StmtKind::VarDeclStmt: {
            auto& vds = static_cast<VarDeclStmt&>(stmt);
            for (auto& d : vds.decls) visitVarDecl(d);
            return {};
        }
        case StmtKind::DestructureStmt: {
            auto& ds = static_cast<DestructureStmt&>(stmt);
            TypeInfo rhs = visitExpr(*ds.value);
            if (rhs.kind != TypeKind::Tuple) {
                error(stmt.range, "destructure target requires a tuple value, got '" +
                    typeName(rhs) + "'");
                return {};
            }
            // Walk the target tree against the tuple structure.
            std::function<void(const DestructureTarget&, const TypeInfo&, size_t&)> walk =
                [&](const DestructureTarget& t, const TypeInfo& tup, size_t& idx) {
                    if (!t.name.empty()) {
                        // Leaf: bind/assign element idx
                        if (idx >= tup.tuple_types.size()) {
                            error(t.range, "destructure: not enough elements in tuple '" +
                                typeName(tup) + "'");
                            return;
                        }
                        const TypeInfo& et = tup.tuple_types[idx];
                        idx++;
                        if (ds.is_decl) {
                            // Declare: explicit type or infer from element
                            TypeInfo lt = t.has_type ? typeNodeToTypeInfo(t.type) : et;
                            if (t.has_type && !typesCompatible(lt, et)) {
                                error(t.range, "destructure: variable '" + t.name +
                                    "' declared as '" + typeName(lt) + "' but element is '" +
                                    typeName(et) + "'");
                            }
                            symbol_table_.declare(t.name, lt);
                        } else {
                            auto* existing = symbol_table_.lookup(t.name);
                            if (!existing) {
                                error(t.range, "destructure: undeclared variable '" + t.name + "'");
                                return;
                            }
                            if (!typesCompatible(*existing, et)) {
                                error(t.range, "destructure: cannot assign '" + typeName(et) +
                                    "' to '" + t.name + "' of type '" + typeName(*existing) + "'");
                            }
                        }
                        return;
                    }
                    // Nested tuple: expect element idx to be a tuple of matching arity
                    if (idx >= tup.tuple_types.size() ||
                        tup.tuple_types[idx].kind != TypeKind::Tuple) {
                        error(t.range, "destructure: expected a nested tuple here");
                        return;
                    }
                    if (tup.tuple_types[idx].tuple_types.size() != t.elements.size()) {
                        error(t.range, "destructure: nested tuple arity mismatch");
                        return;
                    }
                    const TypeInfo& sub = tup.tuple_types[idx];
                    size_t sidx = 0;
                    for (auto& c : t.elements) walk(c, sub, sidx);
                    idx++;
                };
            size_t idx = 0;
            for (auto& c : ds.target.elements) walk(c, rhs, idx);
            if (idx != rhs.tuple_types.size()) {
                error(stmt.range, "destructure: tuple has " +
                    std::to_string(rhs.tuple_types.size()) + " elements but target binds " +
                    std::to_string(idx));
            }
            return {};
        }
        case StmtKind::ExprStmt: {
            auto& es = static_cast<ExprStmt&>(stmt);
            if (es.expression) {
                // In main(), reject direct method calls — use mapping instead
                // In main(), block direct class action/event calls (must use mapping)
                // but allow struct method calls
                if (in_main_function_ && !in_struct_method_ && es.expression->kind == ExprKind::Call) {
                    auto& call = static_cast<CallExpr&>(*es.expression);
                    bool is_event_call = call.callee->kind == ExprKind::Identifier;
                    if (is_event_call) {
                        error(es.expression->range,
                            "direct function call not allowed in main() — use mapping() instead");
                        return {};
                    }
                    // For member access calls, allow struct method calls
                    if (call.callee->kind == ExprKind::MemberAccess) {
                        auto& ma = static_cast<MemberAccessExpr&>(*call.callee);
                        auto obj_type = visitExpr(*ma.object);
                        if (obj_type.kind != TypeKind::Struct) {
                            error(es.expression->range,
                                "direct function call not allowed in main() — use mapping() instead");
                            return {};
                        }
                    }
                }
                visitExpr(*es.expression);
            }
            return {};
        }
        case StmtKind::IfStmt:
            return visitIfStmt(static_cast<IfStmt&>(stmt));
        case StmtKind::WhileStmt:
            return visitWhileStmt(static_cast<WhileStmt&>(stmt));
        case StmtKind::ForStmt:
            return visitForStmt(static_cast<ForStmt&>(stmt));
        case StmtKind::ForInStmt:
            return visitForInStmt(static_cast<ForInStmt&>(stmt));
        case StmtKind::ReturnStmt:
            return visitReturnStmt(static_cast<ReturnStmt&>(stmt));
        case StmtKind::BreakStmt:
        case StmtKind::ContinueStmt:
            if (!in_loop_) {
                error(stmt.range, "break/continue outside loop");
            }
            return {};
        case StmtKind::AwaitStmt: {
            if (!in_coro_method_) {
                error(stmt.range, "'await' is only allowed inside an '@coro' method");
            }
            auto& as = static_cast<AwaitStmt&>(stmt);
            if (as.expr) visitExpr(*as.expr);
            if (as.timeout) {
                TypeInfo tt = visitExpr(*as.timeout);
                if (!expectNumeric(tt, as.timeout->range))
                    error(as.timeout->range, "await timeout must be numeric (ms)");
            }
            return {};
        }
        case StmtKind::MappingStmt: {
            auto& ms = static_cast<MappingStmt&>(stmt);
            // Process lambda expressions and where clauses inside mapping chains
            for (auto& chain : ms.decl.chains) {
                for (auto& node : chain.nodes) {
                    if (node.is_lambda && node.lambda)
                        visitLambda(*node.lambda);
                }
                // Visit where expression with event params in scope
                if (chain.where_expr && current_tu_) {
                    auto& ev_node = chain.nodes[0];
                    for (auto& cls : current_tu_->classes) {
                        for (auto& ev : cls.events) {
                            if (ev.name == ev_node.member_name) {
                                symbol_table_.enterScope();
                                for (auto& p : ev.params)
                                    symbol_table_.declare(p.name, typeNodeToTypeInfo(p.type));
                                visitExpr(*chain.where_expr);
                                symbol_table_.leaveScope();
                                goto where_done;
                            }
                        }
                    }
                    where_done:;
                }
            }
            return {};
        }
        case StmtKind::MatchStmt:
            return visitMatchStmt(static_cast<MatchStmt&>(stmt));
        case StmtKind::TryStmt:
            return visitTryStmt(static_cast<TryStmt&>(stmt));
        case StmtKind::ThrowStmt:
            return visitThrowStmt(static_cast<ThrowStmt&>(stmt));
    }
    return {};
}

Sema::StmtResult Sema::visitBlock(BlockStmt& stmt) {
    symbol_table_.enterScope();
    for (auto& s : stmt.statements) {
        if (s) visitStmt(*s);
    }
    symbol_table_.leaveScope();
    return {};
}

Sema::StmtResult Sema::visitVarDecl(VarDecl& decl) {
    auto decl_type = typeNodeToTypeInfo(decl.type);

    // Reject void-typed variables (explicit `void x;` previously crashed codegen)
    // and unknown class types (class_name set but not resolvable → Void kind).
    if (decl_type.kind == TypeKind::Void) {
        if (!decl.type.class_name.empty())
            error(decl.range, "unknown type '" + decl.type.class_name + "'");
        else
            error(decl.range, "cannot declare variable of type 'void'");
        return {};
    }

    // Handle `var` type inference
    if (decl.type.is_inferred) {
        if (!decl.init_expr) {
            error(decl.range, "'var' declaration requires an initializer");
            return {};
        }
        decl_type = visitExpr(*decl.init_expr);
    } else if (decl.init_expr) {
        TypeInfo init_type;
        if (decl.init_expr->kind == ExprKind::Lambda && decl_type.kind == TypeKind::Function) {
            // Contextual typing: lambda assigned to a function-typed var uses
            // the declared function type (return + param types) as its own.
            init_type = visitLambda(static_cast<LambdaExpr&>(*decl.init_expr), &decl_type);
        } else {
            init_type = visitExpr(*decl.init_expr);
        }
        // Skip cascading error when init type is unknown (already reported)
        if (init_type.kind != TypeKind::Void && !typesCompatible(decl_type, init_type)) {
            error(decl.range, "cannot initialize variable '" + decl.name +
                  "' of type '" + typeName(decl_type) +
                  "' with value of type '" + typeName(init_type) + "'");
        }
    }

    if (symbol_table_.lookup(decl.name)) {
        error(decl.range, "duplicate variable '" + decl.name + "'");
        return {};
    }

    symbol_table_.declare(decl.name, decl_type);
    return {};
}

Sema::StmtResult Sema::visitIfStmt(IfStmt& stmt) {
    if (stmt.condition) {
        auto cond_type = visitExpr(*stmt.condition);
        expectBool(cond_type, stmt.condition->range);
    }
    if (stmt.then_block) visitStmt(*stmt.then_block);
    if (stmt.else_block) visitStmt(*stmt.else_block);
    return {};
}

Sema::StmtResult Sema::visitWhileStmt(WhileStmt& stmt) {
    if (stmt.condition) {
        auto cond_type = visitExpr(*stmt.condition);
        expectBool(cond_type, stmt.condition->range);
    }
    bool saved = in_loop_;
    in_loop_ = true;
    if (stmt.body) visitStmt(*stmt.body);
    in_loop_ = saved;
    return {};
}

Sema::StmtResult Sema::visitForStmt(ForStmt& stmt) {
    if (stmt.gpu) {
        // Basic GPU loop validation
        // The loop variable should be integer type
        auto checkGpuBody = [&](Stmt& body) -> bool {
            // Walk the body and check for disallowed constructs
            // For now, just accept and let codegen handle fallback
            return true;
        };
        if (stmt.body && !checkGpuBody(*stmt.body)) {
            // Error already reported
        }
    }
    symbol_table_.enterScope();
    if (stmt.init) visitStmt(*stmt.init);
    if (stmt.condition) {
        auto cond_type = visitExpr(*stmt.condition);
        expectBool(cond_type, stmt.condition->range);
    }
    if (stmt.step) visitExpr(*stmt.step);
    bool saved = in_loop_;
    in_loop_ = true;
    if (stmt.body) visitStmt(*stmt.body);
    in_loop_ = saved;
    symbol_table_.leaveScope();
    return {};
}

// for (x in coll) — 集合迭代（§四-2）。解析迭代方式并注解 ForInStmt：
//   iter_kind 0=class(size/get), 1=固定数组, 2=slice, 3=range
Sema::StmtResult Sema::visitForInStmt(ForInStmt& stmt) {
    auto it_type = visitExpr(*stmt.iterable);
    bool valid = false;

    // 范围 for-in：for (i in start..end) 父括号形式
    if (stmt.iterable->kind == ExprKind::Range) {
        stmt.elem_type = TypeInfo(TypeKind::Int);
        stmt.iter_kind = 3;
        valid = true;
    } else if (it_type.kind == TypeKind::Class) {
        // 集合类：需要 size() + get(int)（迭代器协议，de-facto）
        if (current_tu_) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name != it_type.class_name) continue;
                const ActionDecl* get_a = nullptr;
                const ActionDecl* size_a = nullptr;
                for (auto& a : cls.actions) {
                    if (a.name == "get" && a.params.size() == 1 &&
                        typeNodeToTypeInfo(a.params[0].type).kind == TypeKind::Int)
                        get_a = &a;
                    if (a.name == "size" && a.params.empty())
                        size_a = &a;
                }
                if (!get_a || !size_a) {
                    error(stmt.range, "'" + it_type.class_name + "' is not iterable: " +
                        "requires size() and get(int) methods");
                    break;
                }
                stmt.elem_type = typeNodeToTypeInfo(get_a->return_type);
                if (stmt.elem_type.kind == TypeKind::Array) {
                    error(stmt.range, "cannot iterate a collection whose element is an array '" +
                        typeName(stmt.elem_type) + "'; wrap it in a class or use slice<T>");
                    break;
                }
                stmt.iter_kind = 0;
                stmt.class_name = it_type.class_name;
                stmt.size_fn = it_type.class_name + "_size";
                stmt.get_fn = it_type.class_name + "_get";
                valid = true;
                break;
            }
        }
    } else if (it_type.kind == TypeKind::Array) {
        if (!it_type.element_type) {
            error(stmt.range, "cannot iterate an array with unknown element type");
        } else if (it_type.array_size <= 0) {
            error(stmt.range, "cannot iterate a dynamic array '" + typeName(it_type) +
                "' (no runtime length); use slice<T> or a collection class");
        } else {
            stmt.elem_type = *it_type.element_type;
            stmt.iter_kind = 1;
            stmt.array_size = it_type.array_size;
            valid = true;
        }
    } else if (it_type.kind == TypeKind::Slice) {
        if (!it_type.element_type) {
            error(stmt.range, "cannot iterate a slice with unknown element type");
        } else {
            stmt.elem_type = *it_type.element_type;
            stmt.iter_kind = 2;
            valid = true;
        }
    } else {
        error(stmt.range, "cannot iterate over type '" + typeName(it_type) + "'");
    }

    if (valid) {
        symbol_table_.enterScope();
        TypeInfo var_ti = stmt.has_type ? typeNodeToTypeInfo(stmt.var_type) : stmt.elem_type;
        if (stmt.has_type && !typesCompatible(var_ti, stmt.elem_type)) {
            error(stmt.range, "for-in variable type '" + typeName(var_ti) +
                "' does not match element type '" + typeName(stmt.elem_type) + "'");
        }
        symbol_table_.declare(stmt.var_name, var_ti);
        bool saved = in_loop_;
        in_loop_ = true;
        if (stmt.body) visitStmt(*stmt.body);
        in_loop_ = saved;
        symbol_table_.leaveScope();
    }
    return {};
}

Sema::StmtResult Sema::visitReturnStmt(ReturnStmt& stmt) {
    if (stmt.value) {
        auto val_type = visitExpr(*stmt.value);
        if (!typesCompatible(current_return_type_, val_type)) {
            error(stmt.range, "cannot return value of type '" +
                  typeName(val_type) + "' from function returning '" +
                  typeName(current_return_type_) + "'");
        }
    } else {
        if (current_return_type_.kind != TypeKind::Void) {
            error(stmt.range, "missing return value (function expects '" +
                  typeName(current_return_type_) + "')");
        }
    }
    return {};
}

// ==============================
// Match statement type checking
// ==============================

Sema::StmtResult Sema::visitMatchStmt(MatchStmt& stmt) {
    auto subject_type = visitExpr(*stmt.subject);

    // The subject must be an enum type (represented as int)
    if (subject_type.kind != TypeKind::Int) {
        // For now, just check it can be compared as integer
    }

    // Resolve variant indices and type-check each arm
    for (auto& arm : stmt.arms) {
        // Find the enum declaration
        auto eit = enum_info_.find(arm.enum_name);
        if (eit == enum_info_.end()) {
            error(arm.range, "unknown enum type '" + arm.enum_name + "'");
            continue;
        }
        auto& variants = eit->second.variants;

        // Find variant by name
        int found_idx = -1;
        for (size_t i = 0; i < variants.size(); i++) {
            if (variants[i].name == arm.variant_name) {
                found_idx = (int)i;
                arm.variant_index = (int)i;
                break;
            }
        }
        if (found_idx < 0) {
            error(arm.range, "unknown variant '" + arm.variant_name +
                  "' in enum '" + arm.enum_name + "'");
            continue;
        }

        // Type-check bindings against variant params
        auto& variant = variants[found_idx];
        if (arm.bindings.size() != variant.params.size()) {
            error(arm.range, "variant '" + variant.name + "' expects " +
                  std::to_string(variant.params.size()) + " data fields, got " +
                  std::to_string(arm.bindings.size()));
        }

        // Enter a scope for the bindings
        symbol_table_.enterScope();
        for (size_t i = 0; i < arm.bindings.size() && i < variant.params.size(); i++) {
            symbol_table_.declare(arm.bindings[i], typeNodeToTypeInfo(variant.params[i].type));
        }

        // Type-check the arm body
        if (arm.body) visitStmt(*arm.body);
        symbol_table_.leaveScope();
    }

    return {};
}

// ==============================
// Expression type checking
// ==============================

TypeInfo Sema::visitExpr(Expr& expr) {
    TypeInfo result;
    switch (expr.kind) {
        case ExprKind::IntegerLiteral:
            result = visitIntegerLiteral(static_cast<IntegerLiteralExpr&>(expr));
            break;
        case ExprKind::FloatLiteral:
            result = visitFloatLiteral(static_cast<FloatLiteralExpr&>(expr));
            break;
        case ExprKind::BoolLiteral:
            result = visitBoolLiteral(static_cast<BoolLiteralExpr&>(expr));
            break;
        case ExprKind::StringLiteral:
            result = visitStringLiteral(static_cast<StringLiteralExpr&>(expr));
            break;
        case ExprKind::NullLiteral:
            result = visitNullLiteral(static_cast<NullLiteralExpr&>(expr));
            break;
        case ExprKind::Identifier:
            result = visitIdentifier(static_cast<IdentifierExpr&>(expr));
            break;
        case ExprKind::BinaryOp:
            result = visitBinaryOp(static_cast<BinaryOpExpr&>(expr));
            break;
        case ExprKind::UnaryOp:
            result = visitUnaryOp(static_cast<UnaryOpExpr&>(expr));
            break;
        case ExprKind::Call:
            result = visitCall(static_cast<CallExpr&>(expr));
            break;
        case ExprKind::MemberAccess:
            result = visitMemberAccess(static_cast<MemberAccessExpr&>(expr));
            break;
        case ExprKind::Subscript:
            result = visitSubscript(static_cast<SubscriptExpr&>(expr));
            break;
        case ExprKind::NewExpr:
            result = visitNewExpr(static_cast<NewExpr&>(expr));
            break;
        case ExprKind::NewArrayExpr:
            result = visitNewArrayExpr(static_cast<NewArrayExpr&>(expr));
            break;
        case ExprKind::ThisExpr:
            result = visitThisExpr(static_cast<ThisExpr&>(expr));
            break;
        case ExprKind::Assignment:
            result = visitAssignment(static_cast<AssignmentExpr&>(expr));
            break;
        case ExprKind::Ternary:
            result = visitTernary(static_cast<TernaryExpr&>(expr));
            break;
        case ExprKind::Range:
            result = visitRange(static_cast<RangeExpr&>(expr));
            break;
        case ExprKind::Lambda:
            result = visitLambda(static_cast<LambdaExpr&>(expr));
            break;
        case ExprKind::Pipe:
            result = visitPipe(static_cast<PipeExpr&>(expr));
            break;
        case ExprKind::EnumVariant:
            result = visitEnumVariant(static_cast<EnumVariantExpr&>(expr));
            break;
        case ExprKind::Try:
            result = visitTryExpr(static_cast<TryExpr&>(expr));
            break;
        case ExprKind::TupleExpr: {
            auto& te = static_cast<TupleExpr&>(expr);
            TypeInfo tt(TypeKind::Tuple);
            for (auto& el : te.elements)
                tt.tuple_types.push_back(visitExpr(*el));
            result = tt;
            break;
        }
        case ExprKind::Await: {
            if (!in_coro_method_) {
                error(expr.range, "'await' is only allowed inside an '@coro' method");
            }
            auto& ae = static_cast<AwaitExpr&>(expr);
            if (ae.operand) {
                // await ClassName.eventName — block on an event (C4).
                // Recognized here (not in visitMemberAccess) so a bare
                // `ClassName.eventName` outside await keeps existing semantics.
                bool is_event_ref = false;
                if (ae.operand->kind == ExprKind::MemberAccess) {
                    auto& ma = static_cast<MemberAccessExpr&>(*ae.operand);
                    if (ma.object->kind == ExprKind::Identifier) {
                        auto& oid = static_cast<IdentifierExpr&>(*ma.object);
                        if (current_tu_) {
                            for (auto& cls : current_tu_->classes) {
                                if (cls.name == oid.name) {
                                    for (auto& ev : cls.events)
                                        if (ev.name == ma.member_name) { is_event_ref = true; break; }
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!is_event_ref) visitExpr(*ae.operand);
            }
            if (ae.timeout) {
                TypeInfo tt = visitExpr(*ae.timeout);
                if (!expectNumeric(tt, ae.timeout->range))
                    error(ae.timeout->range, "await timeout must be numeric (ms)");
            }
            // await expr evaluates to the value passed in by resume → long
            result = TypeInfo(TypeKind::Long);
            break;
        }
    }
    return result;
}

TypeInfo Sema::visitTryExpr(TryExpr& expr) {
    // Type-check the try expression (the value on success).
    auto t = visitExpr(*expr.try_expr);
    // Type-check the catch expression (the fallback value on error).
    auto f = visitExpr(*expr.catch_expr);
    if (!typesCompatible(t, f)) {
        error(expr.range, "try/catch expressions have incompatible types: '" +
              typeName(t) + "' and '" + typeName(f) + "'");
        return t;
    }
    return t;
}

TypeInfo Sema::visitIntegerLiteral(IntegerLiteralExpr& expr) {
    auto val = expr.value;
    // L suffix forces long type
    if (expr.is_long) return TypeInfo(TypeKind::Long);
    TypeInfo result;
    if (val >= -128 && val <= 127)           result = TypeInfo(TypeKind::Byte);
    else if (val >= -32768 && val <= 32767)  result = TypeInfo(TypeKind::Short);
    else if (val >= -2147483648LL && val <= 2147483647LL) result = TypeInfo(TypeKind::Int);
    else                                      result = TypeInfo(TypeKind::Long);
    return result;
}

TypeInfo Sema::visitFloatLiteral(FloatLiteralExpr& expr) {
    return TypeInfo(TypeKind::Double);
}

TypeInfo Sema::visitBoolLiteral(BoolLiteralExpr& expr) {
    return TypeInfo(TypeKind::Bool);
}

TypeInfo Sema::visitStringLiteral(StringLiteralExpr& expr) {
    return TypeInfo(TypeKind::String);
}

TypeInfo Sema::visitNullLiteral(NullLiteralExpr& expr) {
    return TypeInfo(TypeKind::Null);
}

TypeInfo Sema::visitIdentifier(IdentifierExpr& expr) {
    auto* type = symbol_table_.lookup(expr.name);
    if (type) return *type;

    // Check for @static class names — they're accessible as identifiers
    if (current_tu_) {
        for (auto& cls : current_tu_->classes) {
            if (cls.is_static && cls.name == expr.name) {
                TypeInfo class_type(TypeKind::Class);
                class_type.class_name = cls.name;
                return class_type;
            }
        }
    }

    // If inside a class method, also search class properties/actions/events
    if (in_class_method_ && current_tu_) {
        for (auto& cls : current_tu_->classes) {
            if (cls.name != current_class_name_) continue;
            // Search properties first
            for (auto& prop : cls.properties) {
                if (prop.name == expr.name)
                    return typeNodeToTypeInfo(prop.type);
            }
            // Then actions
            for (auto& action : cls.actions) {
                if (action.name == expr.name) {
                    TypeInfo func_type(TypeKind::Function);
                    func_type.return_type = std::make_shared<TypeInfo>(
                        typeNodeToTypeInfo(action.return_type));
                    for (auto& p : action.params)
                        func_type.param_types.push_back(typeNodeToTypeInfo(p.type));
                    return func_type;
                }
            }
            // Then function: section
            for (auto& fn : cls.functions) {
                if (fn.name == expr.name) {
                    TypeInfo func_type(TypeKind::Function);
                    func_type.return_type = std::make_shared<TypeInfo>(
                        typeNodeToTypeInfo(fn.return_type));
                    for (auto& p : fn.params)
                        func_type.param_types.push_back(typeNodeToTypeInfo(p.type));
                    return func_type;
                }
            }
            // Then events
            for (auto& ev : cls.events) {
                if (ev.name == expr.name) {
                    TypeInfo event_type(TypeKind::Function);
                    event_type.return_type = std::make_shared<TypeInfo>(TypeKind::Void);
                    for (auto& p : ev.params) {
                        event_type.param_types.push_back(typeNodeToTypeInfo(p.type));
                        event_type.param_is_ref.push_back(false);
                    }
                    return event_type;
                }
            }
        }
    }

    error(expr.range, "undefined symbol '" + expr.name + "'");
    return TypeInfo(TypeKind::Void);
}

TypeInfo Sema::visitBinaryOp(BinaryOpExpr& expr) {
    auto lhs_type = visitExpr(*expr.lhs);
    auto rhs_type = visitExpr(*expr.rhs);

    // Operator overloading: dispatch to a user-defined operator when the
    // operands are not handled by builtin semantics.
    //   Order: 1) struct-internal operator (this = left operand)
    //          2) external @op function matching (lhs_type, rhs_type)
    // Builtin numeric/string/bool is handled first in each case below.
    auto resolveOperator = [&]() -> TypeInfo {
        if (!current_tu_) return TypeInfo(TypeKind::Void);
        std::string sym;
        switch (expr.op) {
            case BinaryOpKind::Add: sym = "+"; break;
            case BinaryOpKind::Sub: sym = "-"; break;
            case BinaryOpKind::Mul: sym = "*"; break;
            case BinaryOpKind::Div: sym = "/"; break;
            case BinaryOpKind::Mod: sym = "%"; break;
            case BinaryOpKind::Eq:  sym = "=="; break;
            case BinaryOpKind::Ne:  sym = "!="; break;
            case BinaryOpKind::Lt:  sym = "<"; break;
            case BinaryOpKind::Gt:  sym = ">"; break;
            case BinaryOpKind::Le:  sym = "<="; break;
            case BinaryOpKind::Ge:  sym = ">="; break;
            default: return TypeInfo(TypeKind::Void);
        }
        // 1) struct-internal operator (this = left operand, 1 param = right)
        if (lhs_type.kind == TypeKind::Struct) {
            for (auto& st : current_tu_->structs) {
                std::string skey = st.parent_class.empty()
                    ? st.name : st.parent_class + "::" + st.name;
                if (skey != lhs_type.class_name) continue;
                for (auto& m : st.functions) {
                    if (m.op_symbol != sym) continue;
                    if (m.params.size() != 1) continue;
                    auto ptype = typeNodeToTypeInfo(m.params[0].type);
                    if (typesCompatible(ptype, rhs_type)) {
                        expr.op_call = std::make_shared<OperatorCall>();
                        expr.op_call->kind = "struct_method";
                        expr.op_call->struct_key = skey;
                        expr.op_call->method = m.name;
                        return typeNodeToTypeInfo(m.return_type);
                    }
                }
            }
        }
        // 2) external @op function matching (lhs_type, rhs_type)
        for (auto& f : current_tu_->functions) {
            if (f.op_symbol != sym) continue;
            if (f.params.size() != 2) continue;
            auto p0 = typeNodeToTypeInfo(f.params[0].type);
            auto p1 = typeNodeToTypeInfo(f.params[1].type);
            if (typesCompatible(p0, lhs_type) && typesCompatible(p1, rhs_type)) {
                expr.op_call = std::make_shared<OperatorCall>();
                expr.op_call->kind = "function";
                expr.op_call->func_name = f.name;
                return typeNodeToTypeInfo(f.return_type);
            }
        }
        return TypeInfo(TypeKind::Void);
    };

    switch (expr.op) {
        case BinaryOpKind::Add: {
            // String concatenation: string + any → string
            if (lhs_type.kind == TypeKind::String || rhs_type.kind == TypeKind::String)
                return TypeInfo(TypeKind::String);
            if (lhs_type.kind == TypeKind::Void || rhs_type.kind == TypeKind::Void)
                return TypeInfo(TypeKind::Int);  // cascading error recovery
            TypeInfo op_ret = resolveOperator();
            if (op_ret.kind != TypeKind::Void) return op_ret;
            if (!expectNumeric(lhs_type, expr.lhs->range) ||
                !expectNumeric(rhs_type, expr.rhs->range)) {
                return TypeInfo(TypeKind::Int);
            }
            goto numeric_common;
        }
        case BinaryOpKind::Sub:
        case BinaryOpKind::Mul: case BinaryOpKind::Div: case BinaryOpKind::Mod: {
            if (lhs_type.kind == TypeKind::Void || rhs_type.kind == TypeKind::Void)
                return TypeInfo(TypeKind::Int);  // cascading error recovery
            TypeInfo op_ret = resolveOperator();
            if (op_ret.kind != TypeKind::Void) return op_ret;
            if (!expectNumeric(lhs_type, expr.lhs->range) ||
                !expectNumeric(rhs_type, expr.rhs->range)) {
                return TypeInfo(TypeKind::Int);
            }
            goto numeric_common;
        }

        case BinaryOpKind::Eq: case BinaryOpKind::Ne: {
            TypeInfo op_ret = resolveOperator();
            if (op_ret.kind != TypeKind::Void) return op_ret;
            return TypeInfo(TypeKind::Bool);
        }

        case BinaryOpKind::Lt: case BinaryOpKind::Gt:
        case BinaryOpKind::Le: case BinaryOpKind::Ge: {
            TypeInfo op_ret = resolveOperator();
            if (op_ret.kind != TypeKind::Void) return op_ret;
            if (!expectNumeric(lhs_type, expr.lhs->range))
                return TypeInfo(TypeKind::Bool);
            return TypeInfo(TypeKind::Bool);
        }

        case BinaryOpKind::And: case BinaryOpKind::Or:
            expectBool(lhs_type, expr.lhs->range);
            expectBool(rhs_type, expr.rhs->range);
            return TypeInfo(TypeKind::Bool);

        case BinaryOpKind::BitAnd: case BinaryOpKind::BitOr:
        case BinaryOpKind::BitXor: case BinaryOpKind::Shl:
        case BinaryOpKind::Shr:
            if (!expectNumeric(lhs_type, expr.lhs->range) ||
                !expectNumeric(rhs_type, expr.rhs->range)) {
                return TypeInfo(TypeKind::Int);
            }
            // Bitwise on integer types: promote to int/long
            if (lhs_type.kind == TypeKind::Long || rhs_type.kind == TypeKind::Long)
                return TypeInfo(TypeKind::Long);
            return TypeInfo(TypeKind::Int);
    }
    return TypeInfo(TypeKind::Void);

numeric_common:
    if (lhs_type.kind == TypeKind::Double || rhs_type.kind == TypeKind::Double)
        return TypeInfo(TypeKind::Double);
    if (lhs_type.kind == TypeKind::Float || rhs_type.kind == TypeKind::Float)
        return TypeInfo(TypeKind::Float);
    if (lhs_type.kind == TypeKind::Long || rhs_type.kind == TypeKind::Long)
        return TypeInfo(TypeKind::Long);
    if (lhs_type.kind == TypeKind::Int || rhs_type.kind == TypeKind::Int)
        return TypeInfo(TypeKind::Int);
    return TypeInfo(TypeKind::Int);
}

TypeInfo Sema::visitUnaryOp(UnaryOpExpr& expr) {
    auto operand_type = visitExpr(*expr.operand);
    switch (expr.op) {
        case UnaryOpKind::Negate:
            if (!expectNumeric(operand_type, expr.operand->range))
                return TypeInfo(TypeKind::Int);
            return operand_type;
        case UnaryOpKind::Not:
            expectBool(operand_type, expr.operand->range);
            return TypeInfo(TypeKind::Bool);
    }
    return TypeInfo(TypeKind::Void);
}

// Monomorphize a generic function call (explicit `foo<int>(...)` or inferred
// `foo(x)`), append the instance to tu.functions, and type-check the call.
TypeInfo Sema::resolveGenericCall(CallExpr& expr, const std::string& name, int tu_index) {
    if (!current_tu_ || tu_index < 0) return TypeInfo(TypeKind::Void);
    FuncDecl& templ = current_tu_->functions[tu_index];

    // 1) Visit args to get their types.
    std::vector<TypeInfo> arg_types;
    for (auto& a : expr.args) arg_types.push_back(visitExpr(*a));

    // 2) Resolve concrete type args: explicit <T1,...> else infer from args.
    std::vector<TypeNode> concrete;
    bool explicit_given = expr.call_type_args.size() > 0;
    if (explicit_given && expr.call_type_args.size() != templ.type_params.size()) {
        error(expr.range, "generic function '" + name + "' expects " +
            std::to_string(templ.type_params.size()) + " type argument(s), got " +
            std::to_string(expr.call_type_args.size()));
        return TypeInfo(TypeKind::Void);
    }
    for (size_t ti = 0; ti < templ.type_params.size(); ti++) {
        if (explicit_given) {
            concrete.push_back(expr.call_type_args[ti]);
            continue;
        }
        // Infer: find the first param whose type is exactly this type param.
        bool found = false;
        for (size_t pi = 0; pi < templ.params.size() && pi < arg_types.size(); pi++) {
            const TypeNode& ptn = templ.params[pi].type;
            if (ptn.isClass() && ptn.class_name == templ.type_params[ti] &&
                ptn.type_args.empty() && !ptn.isArray()) {
                concrete.push_back(TypeNodeFromTypeInfo(arg_types[pi]));
                found = true;
                break;
            }
            // tp[] — infer from array element type
            if (ptn.isArray() && ptn.element_type &&
                ptn.element_type->isClass() &&
                ptn.element_type->class_name == templ.type_params[ti]) {
                if (arg_types[pi].kind == TypeKind::Array && arg_types[pi].element_type) {
                    concrete.push_back(TypeNodeFromTypeInfo(*arg_types[pi].element_type));
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            error(expr.range, "cannot infer type parameter '" + templ.type_params[ti] +
                "' for generic function '" + name + "' (pass explicit args: " +
                name + "<...>(...))");
            return TypeInfo(TypeKind::Void);
        }
    }

    // 3) Mangled instance name; reuse if already instantiated.
    std::string mangled = name;
    for (auto& c : concrete)
        mangled += "_" + typeName(typeNodeToTypeInfo(c));
    mangled += "_inst";

    // 4) Find or clone the instance.
    const FuncDecl* inst_ptr = nullptr;
    for (auto& f : current_tu_->functions)
        if (f.name == mangled) { inst_ptr = &f; break; }
    if (!inst_ptr) {
        FuncDecl inst;
        inst.name = mangled;
        inst.is_generic_inst = true;
        inst.type_params = templ.type_params;
        inst.inst_type_args = concrete;
        inst.range = templ.range;
        inst.has_test = templ.has_test;
        inst.has_region = templ.has_region;
        inst.has_coro = templ.has_coro;
        inst.op_symbol = templ.op_symbol;
        inst.return_type = substituteTypeNode(templ.return_type, templ.type_params, concrete);
        for (auto& p : templ.params) {
            ParamDecl np;
            np.name = p.name;
            np.type = substituteTypeNode(p.type, templ.type_params, concrete);
            np.is_ref = p.is_ref;
            np.range = p.range;
            inst.params.push_back(std::move(np));
        }
        inst.body = templ.body; // shared body (codegen resolves T per-inst)
        current_tu_->functions.push_back(std::move(inst));
        inst_ptr = &current_tu_->functions.back();
    }

    // 5) Type-check args against the instance signature; set the call target.
    TypeInfo rt = typeNodeToTypeInfo(inst_ptr->return_type);
    if (expr.args.size() != inst_ptr->params.size()) {
        error(expr.range, "expected " + std::to_string(inst_ptr->params.size()) +
            " arguments, got " + std::to_string(expr.args.size()));
        return TypeInfo(TypeKind::Void);
    }
    for (size_t i = 0; i < expr.args.size(); i++) {
        TypeInfo pt = typeNodeToTypeInfo(inst_ptr->params[i].type);
        if (!typesCompatible(pt, arg_types[i])) {
            error(expr.args[i]->range, "argument " + std::to_string(i + 1) +
                ": expected '" + typeName(pt) + "', got '" + typeName(arg_types[i]) + "'");
        }
    }
    expr.resolved_call_name = mangled;
    return rt;
}

// Monomorphize a generic static method call (StaticClass.method<T>(...) or
// inferred), append the instance to tu.functions (as a top-level FuncDecl with
// a unique mangled name), and type-check the call. Static methods have no
// `this`, so the instance body is shared and codegen resolves T per-instance
// exactly like a generic function.
TypeInfo Sema::resolveGenericStaticCall(CallExpr& expr, const std::string& cls_name,
                                        const std::string& method, int class_index,
                                        int action_index) {
    if (!current_tu_ || class_index < 0 || action_index < 0) return TypeInfo(TypeKind::Void);
    ActionDecl& templ = current_tu_->classes[class_index].static_actions[action_index];

    // 1) Visit args to get their types.
    std::vector<TypeInfo> arg_types;
    for (auto& a : expr.args) arg_types.push_back(visitExpr(*a));

    // 2) Resolve concrete type args: explicit <T1,...> else infer from args.
    std::vector<TypeNode> concrete;
    bool explicit_given = expr.call_type_args.size() > 0;
    if (explicit_given && expr.call_type_args.size() != templ.type_params.size()) {
        error(expr.range, "generic static method '" + cls_name + "." + method + "' expects " +
            std::to_string(templ.type_params.size()) + " type argument(s), got " +
            std::to_string(expr.call_type_args.size()));
        return TypeInfo(TypeKind::Void);
    }
    for (size_t ti = 0; ti < templ.type_params.size(); ti++) {
        if (explicit_given) { concrete.push_back(expr.call_type_args[ti]); continue; }
        bool found = false;
        for (size_t pi = 0; pi < templ.params.size() && pi < arg_types.size(); pi++) {
            const TypeNode& ptn = templ.params[pi].type;
            if (ptn.isClass() && ptn.class_name == templ.type_params[ti] &&
                ptn.type_args.empty() && !ptn.isArray()) {
                concrete.push_back(TypeNodeFromTypeInfo(arg_types[pi]));
                found = true;
                break;
            }
            if (ptn.isArray() && ptn.element_type &&
                ptn.element_type->isClass() &&
                ptn.element_type->class_name == templ.type_params[ti]) {
                if (arg_types[pi].kind == TypeKind::Array && arg_types[pi].element_type) {
                    concrete.push_back(TypeNodeFromTypeInfo(*arg_types[pi].element_type));
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            error(expr.range, "cannot infer type parameter '" + templ.type_params[ti] +
                "' for generic static method '" + cls_name + "." + method +
                "' (pass explicit args: " + cls_name + "." + method + "<...>(...))");
            return TypeInfo(TypeKind::Void);
        }
    }

    // 3) Mangled instance name (distinct prefix avoids collision with generic
    //    functions named ClassName_method).
    std::string mangled = "__gs_" + cls_name + "_" + method;
    for (auto& c : concrete)
        mangled += "_" + typeName(typeNodeToTypeInfo(c));
    mangled += "_inst";

    // 4) Find or clone the instance.
    const FuncDecl* inst_ptr = nullptr;
    for (auto& f : current_tu_->functions)
        if (f.name == mangled) { inst_ptr = &f; break; }
    if (!inst_ptr) {
        FuncDecl inst;
        inst.name = mangled;
        inst.is_generic_inst = true;
        inst.type_params = templ.type_params;
        inst.inst_type_args = concrete;
        inst.range = templ.range;
        inst.has_region = templ.has_region;
        inst.has_coro = templ.has_coro;
        inst.return_type = substituteTypeNode(templ.return_type, templ.type_params, concrete);
        for (auto& p : templ.params) {
            ParamDecl np;
            np.name = p.name;
            np.type = substituteTypeNode(p.type, templ.type_params, concrete);
            np.is_ref = false;
            np.range = p.range;
            inst.params.push_back(std::move(np));
        }
        inst.body = std::static_pointer_cast<BlockStmt>(templ.body); // shared body (codegen resolves T per-inst)
        current_tu_->functions.push_back(std::move(inst));
        inst_ptr = &current_tu_->functions.back();
    }

    // 5) Type-check args against the instance signature; set the call target.
    TypeInfo rt = typeNodeToTypeInfo(inst_ptr->return_type);
    if (expr.args.size() != inst_ptr->params.size()) {
        error(expr.range, "expected " + std::to_string(inst_ptr->params.size()) +
            " arguments, got " + std::to_string(expr.args.size()));
        return TypeInfo(TypeKind::Void);
    }
    for (size_t i = 0; i < expr.args.size(); i++) {
        TypeInfo pt = typeNodeToTypeInfo(inst_ptr->params[i].type);
        if (!typesCompatible(pt, arg_types[i])) {
            error(expr.args[i]->range, "argument " + std::to_string(i + 1) +
                ": expected '" + typeName(pt) + "', got '" + typeName(arg_types[i]) + "'");
        }
    }
    expr.resolved_call_name = mangled;
    return rt;
}

// Build a TypeNode that typeNodeToTypeInfo would resolve back to `t`.
TypeNode Sema::TypeNodeFromTypeInfo(const TypeInfo& t) {
    TypeNode n;
    switch (t.kind) {
        case TypeKind::Int:    n.basic_type = BuiltinType::Int; break;
        case TypeKind::Float:  n.basic_type = BuiltinType::Float; break;
        case TypeKind::Double: n.basic_type = BuiltinType::Double; break;
        case TypeKind::Bool:   n.basic_type = BuiltinType::Bool; break;
        case TypeKind::Byte:   n.basic_type = BuiltinType::Byte; break;
        case TypeKind::Short:  n.basic_type = BuiltinType::Short; break;
        case TypeKind::Long:   n.basic_type = BuiltinType::Long; break;
        case TypeKind::Char:   n.basic_type = BuiltinType::Char; break;
        case TypeKind::String: n.basic_type = BuiltinType::String; break;
        case TypeKind::UInt:   n.basic_type = BuiltinType::UInt; break;
        case TypeKind::UByte:  n.basic_type = BuiltinType::UByte; break;
        case TypeKind::UShort: n.basic_type = BuiltinType::UShort; break;
        case TypeKind::ULong:  n.basic_type = BuiltinType::ULong; break;
        case TypeKind::Class:  n.class_name = t.class_name; break;
        case TypeKind::Interface: n.class_name = t.class_name; break;
        case TypeKind::Struct: n.class_name = t.class_name; break;
        case TypeKind::Enum:   n.class_name = t.class_name; break;
        case TypeKind::Array:
            if (t.element_type) {
                n.element_type = std::make_shared<TypeNode>(TypeNodeFromTypeInfo(*t.element_type));
                n.array_size = t.array_size;
            }
            break;
        case TypeKind::Slice:
            if (t.element_type) {
                n.class_name = "slice";
                n.type_args.push_back(TypeNodeFromTypeInfo(*t.element_type));
            }
            break;
        case TypeKind::Function:
            n.is_tuple = false;
            for (auto& p : t.param_types)
                n.func_param_types.push_back(TypeNodeFromTypeInfo(p));
            if (t.return_type)
                n.func_return_type = std::make_shared<TypeNode>(TypeNodeFromTypeInfo(*t.return_type));
            break;
        case TypeKind::Tuple:
            n.is_tuple = true;
            for (auto& p : t.tuple_types)
                n.func_param_types.push_back(TypeNodeFromTypeInfo(p));
            break;
        default: n.basic_type = BuiltinType::Int; break;
    }
    return n;
}

TypeInfo Sema::visitCall(CallExpr& expr) {
    // Generic static method call: StaticClass.genericMethod<...>(...) or inferred.
    if (expr.callee->kind == ExprKind::MemberAccess) {
        auto& gma = static_cast<MemberAccessExpr&>(*expr.callee);
        if (gma.object->kind == ExprKind::Identifier) {
            auto& gso = static_cast<IdentifierExpr&>(*gma.object);
            std::string gkey = gso.name + "::" + gma.member_name;
            auto git = generic_static_methods_.find(gkey);
            if (git != generic_static_methods_.end())
                return resolveGenericStaticCall(expr, gso.name, gma.member_name,
                                                git->second.class_index,
                                                git->second.action_index);
        }
    }
    // Generic function call: foo<int>(...) or foo(x) with inference.
    if (expr.callee->kind == ExprKind::Identifier) {
        auto& gid = static_cast<const IdentifierExpr&>(*expr.callee);
        auto gfit = generic_functions_.find(gid.name);
        if (gfit != generic_functions_.end())
            return resolveGenericCall(expr, gid.name, gfit->second.tu_index);
    }
    // Struct 函数式构造：StructName(args) — 用构造器创建 struct 值
    if (expr.callee->kind == ExprKind::Identifier) {
        auto& id = static_cast<const IdentifierExpr&>(*expr.callee);
        if (resolveStructConstruction(expr, id.name)) {
            TypeInfo rt(TypeKind::Struct);
            rt.class_name = expr.resolved_struct_type;
            return rt;
        }
    }

    auto callee_type = visitExpr(*expr.callee);

    if (callee_type.kind != TypeKind::Function) {
        auto* ident = dynamic_cast<IdentifierExpr*>(expr.callee.get());
        std::string name = ident ? ident->name : "expression";
        error(expr.range, "'" + name + "' is not callable");
        return TypeInfo(TypeKind::Void);
    }

    if (expr.args.size() != callee_type.param_types.size()) {
        error(expr.range, "expected " + std::to_string(callee_type.param_types.size()) +
              " arguments, got " + std::to_string(expr.args.size()));
        return TypeInfo(TypeKind::Void);
    }

    for (size_t i = 0; i < expr.args.size(); ++i) {
        TypeInfo arg_type;
        if (expr.args[i]->kind == ExprKind::Lambda &&
            callee_type.param_types[i].kind == TypeKind::Function) {
            arg_type = visitLambda(static_cast<LambdaExpr&>(*expr.args[i]),
                                   &callee_type.param_types[i]);
        } else {
            arg_type = visitExpr(*expr.args[i]);
        }
        if (!typesCompatible(callee_type.param_types[i], arg_type)) {
            error(expr.args[i]->range, "argument " + std::to_string(i + 1) +
                  ": expected '" + typeName(callee_type.param_types[i]) +
                  "', got '" + typeName(arg_type) + "'");
        }
    }

    // A call to a top-level @coro function returns a coroutine handle (long),
    // not the function's declared return type (which is delivered via the
    // coroutine's result slot and read with Coro.result(handle)).
    auto* ident2 = dynamic_cast<IdentifierExpr*>(expr.callee.get());
    if (ident2 && current_tu_) {
        for (auto& f : current_tu_->functions) {
            if (f.has_coro && f.name == ident2->name)
                return TypeInfo(TypeKind::Long);
        }
    }

    if (callee_type.return_type) return *callee_type.return_type;
    return TypeInfo(TypeKind::Void);
}

bool Sema::resolveStructConstruction(CallExpr& expr, const std::string& name) {
    // 查找 struct（文件级或类内嵌套），取类型 key
    const StructDecl* st = nullptr;
    std::string type_key;
    if (current_tu_) {
        for (auto& s : current_tu_->structs) {
            std::string key = s.parent_class.empty()
                ? s.name : s.parent_class + "::" + s.name;
            if (s.name == name) { st = &s; type_key = key; break; }
        }
        if (!st) {
            for (auto& cls : current_tu_->classes) {
                for (auto& s : cls.structs) {
                    if (s.name == name) { st = &s; type_key = cls.name + "::" + s.name; break; }
                }
                if (st) break;
            }
        }
    }
    if (!st) return false;

    // 收集构造器候选（struct 方法中 has_constructor）
    struct Cand { std::string cname; const std::vector<ParamDecl>* params; };
    std::vector<Cand> cands;
    for (auto& f : st->functions) if (f.has_constructor) cands.push_back({f.name, &f.params});
    if (cands.empty()) return false;  // 无构造器 → 不是函数式构造，保持普通调用解析

    // 类型检查并收集实参类型
    std::vector<TypeInfo> arg_types;
    for (auto& a : expr.args) arg_types.push_back(visitExpr(*a));

    int best = -1; int best_score = 1 << 30; bool ambiguous = false;
    for (size_t ci = 0; ci < cands.size(); ci++) {
        auto& params = *cands[ci].params;
        if (params.size() != arg_types.size()) continue;
        int promos = 0; bool ok = true;
        for (size_t i = 0; i < params.size(); i++) {
            TypeInfo pt = typeNodeToTypeInfo(params[i].type);
            if (typesCompatible(pt, arg_types[i])) {
                if (pt.kind != arg_types[i].kind) promos++;
            } else { ok = false; break; }
        }
        if (!ok) continue;
        if (best < 0 || promos < best_score) { best = (int)ci; best_score = promos; ambiguous = false; }
        else if (promos == best_score) ambiguous = true;
    }

    if (best < 0) {
        std::string argstr;
        for (size_t i = 0; i < arg_types.size(); i++) {
            if (i) argstr += ", ";
            argstr += typeName(arg_types[i]);
        }
        error(expr.range, "no matching constructor for '" + name + "(" + argstr + ")'");
        return true;  // 已报错，视为构造调用处理
    }
    if (ambiguous) {
        error(expr.range, "ambiguous constructor call for '" + name + "'");
        return true;
    }
    auto& c = cands[best];
    expr.resolved_struct_type = type_key;
    expr.resolved_struct_ctor = "struct_" +
        constructorMangledName(type_key, c.cname, *c.params);
    return true;
}

TypeInfo Sema::visitMemberAccess(MemberAccessExpr& expr) {
    auto obj_type = visitExpr(*expr.object);

    // Tuple field access: t.0, t.1 — numeric member name on a tuple value.
    if (obj_type.kind == TypeKind::Tuple && !expr.member_name.empty()) {
        bool all_digits = std::all_of(expr.member_name.begin(), expr.member_name.end(),
            [](unsigned char c) { return std::isdigit(c); });
        if (all_digits) {
            unsigned idx = (unsigned)std::stoul(expr.member_name);
            if (idx < obj_type.tuple_types.size())
                return obj_type.tuple_types[idx];
            error(expr.range, "tuple index " + expr.member_name + " out of range (" +
                std::to_string(obj_type.tuple_types.size()) + " elements)");
            return TypeInfo(TypeKind::Void);
        }
    }

    // Slice member access: .size()/.length() → int, .data() → T[]
    if (obj_type.kind == TypeKind::Slice) {
        if (expr.member_name == "size" || expr.member_name == "length") {
            TypeInfo ft(TypeKind::Function);
            ft.return_type = std::make_shared<TypeInfo>(TypeKind::Int);
            return ft;
        }
        if (expr.member_name == "data") {
            TypeInfo ft(TypeKind::Function);
            ft.return_type = std::make_shared<TypeInfo>(TypeKind::Array);
            ft.return_type->element_type = obj_type.element_type;
            return ft;
        }
        error(expr.range, "slice has no member '" + expr.member_name +
            "' (expected size/length/data)");
        return TypeInfo(TypeKind::Void);
    }

    // Interface member access — look up method in interface declaration
    if (obj_type.kind == TypeKind::Interface) {
        if (current_tu_) {
            for (auto& ifd : current_tu_->interfaces) {
                if (ifd.name != obj_type.class_name) continue;
                // Check actions (methods)
                for (auto& act : ifd.actions) {
                    if (act.name == expr.member_name) {
                        TypeInfo ft(TypeKind::Function);
                        ft.return_type = std::make_shared<TypeInfo>(
                            typeNodeToTypeInfo(act.return_type));
                        for (auto& p : act.params)
                            ft.param_types.push_back(typeNodeToTypeInfo(p.type));
                        return ft;
                    }
                }
                // Check events
                for (auto& ev : ifd.events) {
                    if (ev.name == expr.member_name) {
                        TypeInfo ft(TypeKind::Function);
                        ft.return_type = std::make_shared<TypeInfo>(TypeKind::Void);
                        for (auto& p : ev.params)
                            ft.param_types.push_back(typeNodeToTypeInfo(p.type));
                        return ft;
                    }
                }
                error(expr.range, "interface '" + obj_type.class_name +
                    "' has no member '" + expr.member_name + "'");
                return TypeInfo(TypeKind::Void);
            }
        }
        error(expr.range, "unknown interface '" + obj_type.class_name + "'");
        return TypeInfo(TypeKind::Void);
    }

    // Struct member access — fields are PUBLIC
    if (obj_type.kind == TypeKind::Struct) {
        // Search all struct definitions (file-level + nested in classes)
        if (current_tu_) {
            for (auto& st : current_tu_->structs) {
                std::string key = st.parent_class.empty()
                    ? st.name : st.parent_class + "::" + st.name;
                if (key == obj_type.class_name) {
                    // Check fields
                    for (auto& prop : st.properties) {
                        if (prop.name == expr.member_name)
                            return typeNodeToTypeInfo(prop.type);
                    }
                    // Check methods
                    for (auto& func : st.functions) {
                        if (func.name == expr.member_name) {
                            TypeInfo ft(TypeKind::Function);
                            ft.return_type = std::make_shared<TypeInfo>(
                                typeNodeToTypeInfo(func.return_type));
                            for (auto& p : func.params)
                                ft.param_types.push_back(typeNodeToTypeInfo(p.type));
                            return ft;
                        }
                    }
                    error(expr.range, "struct '" + obj_type.class_name +
                          "' has no member '" + expr.member_name + "'");
                    return TypeInfo(TypeKind::Void);
                }
            }
            // Also search nested structs in classes
            for (auto& cls : current_tu_->classes) {
                for (auto& st : cls.structs) {
                    std::string key = st.parent_class.empty()
                        ? st.name : st.parent_class + "::" + st.name;
                    if (key == obj_type.class_name) {
                        for (auto& prop : st.properties) {
                            if (prop.name == expr.member_name)
                                return typeNodeToTypeInfo(prop.type);
                        }
                        for (auto& func : st.functions) {
                            if (func.name == expr.member_name) {
                                TypeInfo ft(TypeKind::Function);
                                ft.return_type = std::make_shared<TypeInfo>(
                                    typeNodeToTypeInfo(func.return_type));
                                for (auto& p : func.params)
                                    ft.param_types.push_back(typeNodeToTypeInfo(p.type));
                                return ft;
                            }
                        }
                        error(expr.range, "struct '" + obj_type.class_name +
                              "' has no member '" + expr.member_name + "'");
                        return TypeInfo(TypeKind::Void);
                    }
                }
            }
        }
        error(expr.range, "unknown struct '" + obj_type.class_name + "'");
        return TypeInfo(TypeKind::Void);
    }

    // Enum variant access — Color.Red
    if (obj_type.kind == TypeKind::Enum) {
        // Find the enum and variant
        auto eit = enum_info_.find(obj_type.class_name);
        if (eit == enum_info_.end()) {
            error(expr.range, "unknown enum '" + obj_type.class_name + "'");
            return TypeInfo(TypeKind::Void);
        }
        for (size_t vi = 0; vi < eit->second.variants.size(); vi++) {
            if (eit->second.variants[vi].name == expr.member_name) {
                // Return function type if variant has data, else just the enum type
                if (eit->second.variants[vi].params.empty()) {
                    return obj_type;
                } else {
                    // Variant with data: return a function type (constructs the variant)
                    TypeInfo ft(TypeKind::Function);
                    ft.return_type = std::make_shared<TypeInfo>(obj_type);
                    for (auto& p : eit->second.variants[vi].params) {
                        ft.param_types.push_back(typeNodeToTypeInfo(p.type));
                    }
                    return ft;
                }
            }
        }
        error(expr.range, "enum '" + obj_type.class_name +
              "' has no variant '" + expr.member_name + "'");
        return TypeInfo(TypeKind::Void);
    }

    if (obj_type.kind == TypeKind::Struct) {
        // Struct member access via 'this' in struct methods
        if (current_tu_) {
            for (auto& st : current_tu_->structs) {
                std::string key = st.parent_class.empty()
                    ? st.name : st.parent_class + "::" + st.name;
                if (key == obj_type.class_name) {
                    for (auto& prop : st.properties) {
                        if (prop.name == expr.member_name)
                            return typeNodeToTypeInfo(prop.type);
                    }
                    for (auto& func : st.functions) {
                        if (func.name == expr.member_name) {
                            TypeInfo ft(TypeKind::Function);
                            ft.return_type = std::make_shared<TypeInfo>(
                                typeNodeToTypeInfo(func.return_type));
                            for (auto& p : func.params)
                                ft.param_types.push_back(typeNodeToTypeInfo(p.type));
                            return ft;
                        }
                    }
                    error(expr.range, "struct '" + obj_type.class_name +
                          "' has no member '" + expr.member_name + "'");
                    return TypeInfo(TypeKind::Void);
                }
            }
            // Also search nested structs
            for (auto& cls : current_tu_->classes) {
                for (auto& st : cls.structs) {
                    std::string key = st.parent_class.empty()
                        ? st.name : st.parent_class + "::" + st.name;
                    if (key == obj_type.class_name) {
                        for (auto& prop : st.properties) {
                            if (prop.name == expr.member_name)
                                return typeNodeToTypeInfo(prop.type);
                        }
                        for (auto& func : st.functions) {
                            if (func.name == expr.member_name) {
                                TypeInfo ft(TypeKind::Function);
                                ft.return_type = std::make_shared<TypeInfo>(
                                    typeNodeToTypeInfo(func.return_type));
                                for (auto& p : func.params)
                                    ft.param_types.push_back(typeNodeToTypeInfo(p.type));
                                return ft;
                            }
                        }
                        error(expr.range, "struct '" + obj_type.class_name +
                              "' has no member '" + expr.member_name + "'");
                        return TypeInfo(TypeKind::Void);
                    }
                }
            }
        }
    }

    if (obj_type.kind != TypeKind::Class) {
        error(expr.range, "cannot access member of non-class type '" +
              typeName(obj_type) + "'");
        return TypeInfo(TypeKind::Void);
    }

    if (current_tu_) {
        for (auto& cls : current_tu_->classes) {
            if (cls.name == obj_type.class_name) {
                // Properties — accessible from anywhere
                for (auto& prop : cls.properties) {
                    if (prop.name == expr.member_name) {
                        return typeNodeToTypeInfo(prop.type);
                    }
                }
                for (auto& action : cls.actions) {
                    if (action.name == expr.member_name) {
                        TypeInfo func_type(TypeKind::Function);
                        if (action.has_coro) {
                            // @coro method call → returns coroutine handle (long)
                            func_type.return_type = std::make_shared<TypeInfo>(TypeKind::Long);
                        } else {
                            func_type.return_type = std::make_shared<TypeInfo>(
                                typeNodeToTypeInfo(action.return_type));
                        }
                        for (auto& p : action.params)
                            func_type.param_types.push_back(typeNodeToTypeInfo(p.type));
                        return func_type;
                    }
                }
                // Static actions: accessible from anywhere (no 'this' needed)
                for (auto& action : cls.static_actions) {
                    if (action.name == expr.member_name) {
                        TypeInfo func_type(TypeKind::Function);
                        func_type.return_type = std::make_shared<TypeInfo>(
                            typeNodeToTypeInfo(action.return_type));
                        for (auto& p : action.params)
                            func_type.param_types.push_back(typeNodeToTypeInfo(p.type));
                        return func_type;
                    }
                }
                // Also check function: section (visible inside class, not from outside)
                for (auto& func : cls.functions) {
                    if (func.name == expr.member_name) {
                        bool is_this = expr.object->kind == ExprKind::ThisExpr;
                        if (!is_this) {
                            error(expr.range, "cannot access function '" + func.name +
                                  "' from outside class '" + cls.name + "'");
                            return TypeInfo(TypeKind::Void);
                        }
                        TypeInfo ft(TypeKind::Function);
                        ft.return_type = std::make_shared<TypeInfo>(
                            typeNodeToTypeInfo(func.return_type));
                        for (auto& p : func.params)
                            ft.param_types.push_back(typeNodeToTypeInfo(p.type));
                        return ft;
                    }
                }
                for (auto& event : cls.events) {
                    if (event.name == expr.member_name) {
                        TypeInfo event_type(TypeKind::Function);
                        event_type.return_type = std::make_shared<TypeInfo>(TypeKind::Void);
                        for (auto& p : event.params) {
                            event_type.param_types.push_back(typeNodeToTypeInfo(p.type));
                            event_type.param_is_ref.push_back(false);
                        }
                        return event_type;
                    }
                }
                error(expr.range, "class '" + obj_type.class_name +
                      "' has no member '" + expr.member_name + "'");
                return TypeInfo(TypeKind::Void);
            }
        }
        error(expr.range, "unknown class '" + obj_type.class_name + "'");
    }
    return TypeInfo(TypeKind::Void);
}

TypeInfo Sema::visitSubscript(SubscriptExpr& expr) {
    auto arr_type = visitExpr(*expr.array);
    auto idx_type = visitExpr(*expr.index);

    if (arr_type.kind != TypeKind::Array && arr_type.kind != TypeKind::Slice) {
        error(expr.range, "cannot index non-array type '" + typeName(arr_type) + "'");
        return TypeInfo(TypeKind::Void);
    }
    if (!expectNumeric(idx_type, expr.index->range))
        return TypeInfo(TypeKind::Void);
    if (arr_type.element_type) return *arr_type.element_type;
    return TypeInfo(TypeKind::Void);
}

TypeInfo Sema::visitNewExpr(NewExpr& expr) {
    // Built-in slice<T>(n): allocate n elements, return { data, len }
    if (expr.class_name == "slice") {
        if (expr.type_args.size() != 1) {
            error(expr.range, "slice requires exactly one type argument: slice<T>");
            return TypeInfo(TypeKind::Void);
        }
        if (expr.args.size() != 1) {
            error(expr.range, "slice requires exactly one size argument: new slice<T>(n)");
            return TypeInfo(TypeKind::Void);
        }
        auto sz = visitExpr(*expr.args[0]);
        if (sz.kind != TypeKind::Int && sz.kind != TypeKind::Long
            && sz.kind != TypeKind::Short && sz.kind != TypeKind::Byte) {
            error(expr.args[0]->range, "slice size must be an integer expression");
        }
        TypeInfo result(TypeKind::Slice);
        result.element_type = std::make_shared<TypeInfo>(
            typeNodeToTypeInfo(expr.type_args[0]));
        return result;
    }
    // Check if this is a generic instantiation
    if (!expr.type_args.empty()) {
        // Construct a temporary TypeNode to trigger monomorphization
        TypeNode tn;
        tn.class_name = expr.class_name;
        tn.type_args = expr.type_args;
        TypeInfo inst_type = typeNodeToTypeInfo(tn);
        // 在实例类上解析构造器（实例名与 codegen 的 mangling 一致：Box_int_inst）
        std::string inst_name = expr.class_name;
        for (auto& ta : expr.type_args) { inst_name += "_"; inst_name += mangleTypeNode(ta); }
        inst_name += "_inst";
        resolveNewConstructor(expr, inst_name);
        return inst_type;
    }

    bool found = false;
    if (current_tu_) {
        for (auto& cls : current_tu_->classes) {
            if (cls.name == expr.class_name) { found = true; break; }
        }
    }
    if (!found)
        error(expr.range, "unknown class '" + expr.class_name + "'");

    resolveNewConstructor(expr, expr.class_name);

    TypeInfo result(TypeKind::Class);
    result.class_name = expr.class_name;
    return result;
}

void Sema::resolveNewConstructor(NewExpr& expr, const std::string& cls_name) {
    const ClassDecl* cls = nullptr;
    if (current_tu_) {
        for (auto& c : current_tu_->classes)
            if (c.name == cls_name) { cls = &c; break; }
    }
    if (!cls) return;

    // 收集构造器候选（action: 与 function: 段）
    struct Cand { std::string name; const std::vector<ParamDecl>* params; };
    std::vector<Cand> cands;
    for (auto& a : cls->actions) if (a.has_constructor) cands.push_back({a.name, &a.params});
    for (auto& f : cls->functions) if (f.has_constructor) cands.push_back({f.name, &f.params});
    if (cands.empty()) return;  // 无构造器 → legacy @startup init / 默认

    // 类型检查并收集实参类型（此时才 visit——避免影响无构造器的 legacy 类）
    std::vector<TypeInfo> arg_types;
    for (auto& a : expr.args) arg_types.push_back(visitExpr(*a));

    int best = -1;
    int best_score = 1 << 30;
    bool ambiguous = false;
    for (size_t ci = 0; ci < cands.size(); ci++) {
        auto& params = *cands[ci].params;
        if (params.size() != arg_types.size()) continue;
        int promos = 0; bool ok = true;
        for (size_t i = 0; i < params.size(); i++) {
            TypeInfo pt = typeNodeToTypeInfo(params[i].type);
            if (typesCompatible(pt, arg_types[i])) {
                if (pt.kind != arg_types[i].kind) promos++;
            } else { ok = false; break; }
        }
        if (!ok) continue;
        if (best < 0 || promos < best_score) { best = (int)ci; best_score = promos; ambiguous = false; }
        else if (promos == best_score) ambiguous = true;
    }

    if (best < 0) {
        std::string argstr;
        for (size_t i = 0; i < arg_types.size(); i++) {
            if (i) argstr += ", ";
            argstr += typeName(arg_types[i]);
        }
        error(expr.range, "no matching constructor for '" + cls_name + "(" + argstr + ")'");
        return;
    }
    if (ambiguous) {
        error(expr.range, "ambiguous constructor call for '" + cls_name + "'");
        return;
    }
    auto& c = cands[best];
    expr.resolved_ctor = constructorMangledName(cls_name, c.name, *c.params);
}

TypeInfo Sema::visitNewArrayExpr(NewArrayExpr& expr) {
    // Type-check dimensions: must be integer expressions
    for (auto& dim : expr.dimensions) {
        auto dt = visitExpr(*dim);
        if (dt.kind != TypeKind::Int && dt.kind != TypeKind::Long
            && dt.kind != TypeKind::Short && dt.kind != TypeKind::Byte) {
            error(dim->range, "array size must be an integer expression");
        }
    }
    // Return array type
    TypeInfo result(TypeKind::Array);
    result.element_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(expr.element_type));
    return result;
}

TypeInfo Sema::visitThisExpr(ThisExpr& expr) {
    if (!in_class_method_ && !in_struct_method_) {
        error(expr.range, "'this' can only be used inside a class action");
        return TypeInfo(TypeKind::Void);
    }
    if (in_struct_method_) {
        TypeInfo result(TypeKind::Struct);
        result.class_name = current_struct_type_key_;
        return result;
    }
    TypeInfo result(TypeKind::Class);
    result.class_name = current_class_name_;
    return result;
}

TypeInfo Sema::visitRange(RangeExpr& expr) {
    auto start_type = visitExpr(*expr.start);
    auto end_type = visitExpr(*expr.end);
    if (!expectNumeric(start_type, expr.start->range) ||
        !expectNumeric(end_type, expr.end->range)) {
        return TypeInfo(TypeKind::Int);
    }
    return TypeInfo(TypeKind::Int); // Range evaluates to int
}

TypeInfo Sema::visitEnumVariant(EnumVariantExpr& expr) {
    // Enum variants are represented as i32 constants
    TypeInfo result(TypeKind::Int);
    return result;
}

TypeInfo Sema::visitTernary(TernaryExpr& expr) {
    auto cond_type = visitExpr(*expr.condition);
    expectBool(cond_type, expr.condition->range);
    auto true_type = visitExpr(*expr.true_expr);
    auto false_type = visitExpr(*expr.false_expr);
    if (!typesCompatible(true_type, false_type)) {
        error(expr.range, "ternary branches have incompatible types: '" +
              typeName(true_type) + "' and '" + typeName(false_type) + "'");
        return true_type;
    }
    return true_type;
}

TypeInfo Sema::visitAssignment(AssignmentExpr& expr) {
    // Target must be an assignable lvalue: variable, field, or array element.
    // (Rejects e.g. `--5`, `5 = 1`, `f() = x` — previously crashed codegen.)
    switch (expr.target->kind) {
        case ExprKind::Identifier:
        case ExprKind::MemberAccess:
        case ExprKind::Subscript:
            break;  // assignable
        default:
            error(expr.range,
                "cannot assign to this expression (target must be a variable, "
                "field, or array element)");
            return TypeInfo(TypeKind::Void);
    }

    // Check if target is a const property
    auto checkConstProperty = [&](const std::string& prop_name) {
        if (!current_class_name_.empty() && current_tu_) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name != current_class_name_) continue;
                for (auto& p : cls.properties) {
                    if (p.name == prop_name && p.is_const) {
                        error(expr.range, "cannot assign to const property '" + prop_name + "'");
                        return true;
                    }
                }
            }
        }
        return false;
    };

    if (expr.target->kind == ExprKind::Identifier) {
        auto& id = static_cast<const IdentifierExpr&>(*expr.target);
        if (checkConstProperty(id.name)) return TypeInfo(TypeKind::Void);
    } else if (expr.target->kind == ExprKind::MemberAccess) {
        auto& ma = static_cast<const MemberAccessExpr&>(*expr.target);
        if (ma.object->kind == ExprKind::ThisExpr) {
            if (checkConstProperty(ma.member_name)) return TypeInfo(TypeKind::Void);
        }
    }
    auto target_type = visitExpr(*expr.target);
    auto value_type = visitExpr(*expr.value);
    // Skip cascading error when target is unknown (already reported)
    if (target_type.kind != TypeKind::Void && !typesCompatible(target_type, value_type)) {
        error(expr.range, "cannot assign value of type '" +
              typeName(value_type) + "' to variable of type '" +
              typeName(target_type) + "'");
    }
    return target_type;
}

// ==============================
// Type utilities
// ==============================

const TypeAliasDecl* Sema::findAlias(const std::string& name) const {
    if (!current_tu_) return nullptr;
    for (auto& a : current_tu_->type_aliases)
        if (a.name == name) return &a;
    return nullptr;
}

TypeInfo Sema::typeNodeToTypeInfo(const TypeNode& node, int alias_depth) {
    // Type alias expansion: `type Name = Type;` — resolve Name to its aliased
    // type (recursively; depth-limited to catch cyclic aliases).
    if (node.isClass() && node.type_args.empty()) {
        if (auto* alias = findAlias(node.class_name)) {
            if (alias_depth > 32) {
                error(node.range, "recursive type alias '" + node.class_name + "'");
                return TypeInfo(TypeKind::Int);
            }
            return typeNodeToTypeInfo(alias->alias_type, alias_depth + 1);
        }
    }
    // Function type: (A, B) -> R
    if (node.isFunction()) {
        TypeInfo ft(TypeKind::Function);
        ft.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(*node.func_return_type));
        for (auto& p : node.func_param_types)
            ft.param_types.push_back(typeNodeToTypeInfo(p));
        return ft;
    }
    // Tuple type: (A, B)
    if (node.isTuple()) {
        TypeInfo tt(TypeKind::Tuple);
        for (auto& p : node.func_param_types)
            tt.tuple_types.push_back(typeNodeToTypeInfo(p));
        return tt;
    }
    if (node.isArray()) {
        TypeInfo arr_type(TypeKind::Array);
        arr_type.array_size = node.array_size;
        if (node.element_type)
            arr_type.element_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(*node.element_type));
        return arr_type;
    }
    if (node.isClass()) {
        // 关联类型：X::Item — X 为实现接口的类（或其类型参数），Item 为绑定类型（§三-5）
        {
            auto pos = node.class_name.find("::");
            if (pos != std::string::npos) {
                std::string owner = node.class_name.substr(0, pos);
                std::string member = node.class_name.substr(pos + 2);
                if (current_tu_) {
                    for (auto& cls : current_tu_->classes) {
                        if (cls.name != owner) continue;
                        auto bit = cls.associated_type_bindings.find(member);
                        if (bit != cls.associated_type_bindings.end())
                            return typeNodeToTypeInfo(bit->second);
                    }
                }
                // owner 是类型参数（T）→ 约束接口的关联类型 → 抽象 Assoc；
                // 无约束 → 占位符 Int（codegen 单态化时经 current_type_params_
                // 解析到具体类绑定）。
                for (auto& tp : current_func_type_params_)
                    if (tp == owner) return TypeInfo(TypeKind::Int);
                if (!current_class_name_.empty())
                    for (auto& cls : current_tu_->classes)
                        if (cls.name == current_class_name_)
                            for (auto& tp : cls.type_params) {
                                if (tp != owner) continue;
                                auto cit = cls.type_param_constraints.find(tp);
                                if (cit != cls.type_param_constraints.end())
                                    return TypeInfo(TypeKind::Assoc);
                                return TypeInfo(TypeKind::Int);
                            }
            }
        }
        // Built-in slice<T>: { T* data; int64 len } fat pointer
        if (node.class_name == "slice" && node.type_args.size() == 1) {
            TypeInfo st(TypeKind::Slice);
            st.element_type = std::make_shared<TypeInfo>(
                typeNodeToTypeInfo(node.type_args[0]));
            return st;
        }
        std::string lookup_name = node.class_name;

        // Check if this is an interface type
        if (current_tu_) {
            for (auto& ifd : current_tu_->interfaces) {
                if (ifd.name == lookup_name) {
                    TypeInfo if_type(TypeKind::Interface);
                    if_type.class_name = lookup_name;
                    return if_type;
                }
            }
        }

        // Check if this is a generic class instantiation
        if (!node.type_args.empty() && generic_classes_.count(node.class_name)) {
            // If any type argument is a generic type-param placeholder (e.g.
            // `new Option<R>()` inside a generic template, or `Box<T>` inside a
            // generic class method), defer monomorphization. The placeholder Int
            // is only for name-mangling; instantiating now would bake the
            // type-param NAME into members (ctor param = `R`), which later
            // resolves to void outside the template scope. The concrete
            // instance (e.g. Option_int_inst) is created at the call site.
            bool has_placeholder = false;
            for (auto& a : node.type_args) {
                std::string nm = a.class_name;
                for (auto& tp : current_func_type_params_)
                    if (tp == nm) { has_placeholder = true; break; }
                if (has_placeholder) break;
                if (!current_class_name_.empty()) {
                    for (auto& cls : current_tu_->classes)
                        if (cls.name == current_class_name_)
                            for (auto& tp : cls.type_params)
                                if (tp == nm) { has_placeholder = true; break; }
                }
                if (has_placeholder) break;
            }
            if (has_placeholder) {
                TypeInfo result(TypeKind::Class);
                result.class_name = node.class_name;
                return result;
            }

            // Monomorphization: create a concrete class with type params substituted
            std::string mangled = node.class_name;
            for (auto& a : node.type_args)
                mangled += "_" + typeName(typeNodeToTypeInfo(a));
            mangled += "_inst";
            // Check if already instantiated
            TypeInfo result(TypeKind::Class);
            result.class_name = mangled;
            if (symbol_table_.lookup(mangled)) return result;

            // Find and clone the generic class
            auto& gen = generic_classes_[node.class_name];
            if (gen.tu_index < 0 || !current_tu_) return result;

            ClassDecl& original = current_tu_->classes[gen.tu_index];

            // ---- M2: enforce generic type-param constraints (where T : I) ----
            // Before monomorphizing, verify each constrained type argument
            // implements the required interface.
            for (auto& [tp, iface] : original.type_param_constraints) {
                size_t ti = original.type_params.size();
                for (size_t k = 0; k < original.type_params.size(); k++)
                    if (original.type_params[k] == tp) { ti = k; break; }
                if (ti >= node.type_args.size()) continue;
                const TypeNode& arg = node.type_args[ti];
                bool ok = false;
                if (!arg.class_name.empty()) {
                    for (auto& c : current_tu_->classes) {
                        if (c.name == arg.class_name) {
                            // The concrete class must declare the interface via
                            // `interface class X;` (interface_class_name).
                            if (c.interface_class_name == iface) { ok = true; }
                            // Also allow if the arg IS the interface itself.
                            for (auto& ifd : current_tu_->interfaces)
                                if (ifd.name == arg.class_name && iface == arg.class_name)
                                    ok = true;
                            break;
                        }
                    }
                }
                if (!ok) {
                    error(node.range, "type argument '" + typeName(typeNodeToTypeInfo(arg)) +
                        "' does not satisfy constraint '" + tp + " : " + iface +
                        "' for generic class '" + node.class_name + "'");
                    return result;
                }
            }

            ClassDecl inst;
            inst.name = mangled;
            inst.is_generic_inst = true;
            inst.type_params = original.type_params;
            inst.inst_type_args = node.type_args; // keep concrete args for codegen
            inst.range = original.range;

            // 关联类型绑定：类型实参代入（如 type Item = T; → Item = int）
            for (auto& [name, bt] : original.associated_type_bindings)
                inst.associated_type_bindings[name] =
                    substituteTypeNode(bt, original.type_params, node.type_args);

            // Substitute properties
            for (auto& prop : original.properties) {
                PropertyDecl p;
                p.name = prop.name;
                p.type = substituteTypeNode(prop.type, original.type_params, node.type_args);
                p.range = prop.range;
                inst.properties.push_back(std::move(p));
            }

            // Substitute actions (share body)
            for (auto& action : original.actions) {
                ActionDecl a;
                a.name = action.name;
                a.return_type = substituteTypeNode(action.return_type, original.type_params, node.type_args);
                for (auto& param : action.params) {
                    ParamDecl p;
                    p.name = param.name;
                    p.type = substituteTypeNode(param.type, original.type_params, node.type_args);
                    p.range = param.range;
                    a.params.push_back(std::move(p));
                }
                a.body = action.body; // share body
                a.has_startup = action.has_startup;
                a.has_test = action.has_test;
                a.has_constructor = action.has_constructor;
                a.range = action.range;
                inst.actions.push_back(std::move(a));
            }

            // Copy static actions
            for (auto& action : original.static_actions) {
                ActionDecl a;
                a.name = action.name;
                a.return_type = substituteTypeNode(action.return_type, original.type_params, node.type_args);
                for (auto& param : action.params) {
                    ParamDecl p;
                    p.name = param.name;
                    p.type = substituteTypeNode(param.type, original.type_params, node.type_args);
                    p.range = param.range;
                    a.params.push_back(std::move(p));
                }
                a.type_params = action.type_params;
                a.body = action.body;
                a.range = action.range;
                inst.static_actions.push_back(std::move(a));
            }

            // Copy events
            for (auto& ev : original.events) {
                EventDecl e;
                e.name = ev.name;
                for (auto& param : ev.params) {
                    ParamDecl p;
                    p.name = param.name;
                    p.type = substituteTypeNode(param.type, original.type_params, node.type_args);
                    p.range = param.range;
                    e.params.push_back(std::move(p));
                }
                e.range = ev.range;
                inst.events.push_back(std::move(e));
            }

            // Copy function: section (share body — type-checking of insts is
            // skipped, but codegen needs the body; generic type params inside
            // the body (e.g. `new T[n]`) are resolved by codegen's type-param
            // map, not by textual substitution).
            for (auto& func : original.functions) {
                FuncDecl f;
                f.name = func.name;
                f.return_type = substituteTypeNode(func.return_type, original.type_params, node.type_args);
                for (auto& param : func.params) {
                    ParamDecl p;
                    p.name = param.name;
                    p.type = substituteTypeNode(param.type, original.type_params, node.type_args);
                    p.range = param.range;
                    f.params.push_back(std::move(p));
                }
                f.body = func.body; // share body (codegen resolves T per-inst)
                f.has_constructor = func.has_constructor;
                f.range = func.range;
                inst.functions.push_back(std::move(f));
            }

            // Register and add to TU — visitClassDecl will declare the type
            current_tu_->classes.push_back(std::move(inst));
            visitClassDecl(current_tu_->classes.back());
            // Now it's registered; look up the result
            auto* lookup_result = symbol_table_.lookup(mangled);
            if (lookup_result) result = *lookup_result;
            return result;
        }

        auto* existing = symbol_table_.lookup(lookup_name);
        if (existing) return *existing;
        if (!current_class_name_.empty()) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name != current_class_name_) continue;
                for (auto& tp : cls.type_params) {
                    if (tp != lookup_name) continue;
                    // 约束类型参数（where T : I，§三-5）→ 解析为接口类型，使
                    // T 上的方法调用 / T::Item 可在模板体中静态检查（运行时单态化
                    // 到具体类）。
                    auto cit = cls.type_param_constraints.find(tp);
                    if (cit != cls.type_param_constraints.end()) {
                        TypeInfo if_type(TypeKind::Interface);
                        if_type.class_name = cit->second;
                        return if_type;
                    }
                    TypeInfo tp_type(TypeKind::Int);
                    return tp_type;
                }
            }
        }
        // Generic function type param placeholder (T → Int, like classes)
        for (auto& tp : current_func_type_params_) {
            if (tp == lookup_name) {
                TypeInfo tp_type(TypeKind::Int);
                return tp_type;
            }
        }

        // 抽象关联类型（§三-5）：标识符匹配某接口的 `type Item;` 声明 → Assoc
        //（静态检查与任意类型兼容；运行时经实现类绑定为具体类型）。
        if (current_tu_) {
            for (auto& ifd : current_tu_->interfaces)
                for (auto& at : ifd.associated_types)
                    if (at == lookup_name) return TypeInfo(TypeKind::Assoc);
        }

        // Check if this is a generic class instantiation
        if (!node.type_args.empty() && generic_classes_.count(node.class_name)) {
            // For now, return a simple class type (actual monomorphization TBD)
            TypeInfo result(TypeKind::Class);
            result.class_name = node.class_name;
            return result;
        }
        // Check if this is an enum type
        if (existing && (existing->kind == TypeKind::Enum)) {
            return *existing;
        }

        // Check if this is a struct type by looking up the symbol table
        if (existing && existing->kind == TypeKind::Struct) {
            TypeInfo st_type(TypeKind::Struct);
            st_type.class_name = lookup_name;
            return st_type;
        }
        // Verify class exists
        bool class_found = false;
        if (current_tu_) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name == node.class_name) { class_found = true; break; }
            }
        }
        if (!class_found) {
            // Return a clear error type — the caller will report it
            TypeInfo err_type(TypeKind::Void);
            return err_type;
        }
        TypeInfo cls_type(TypeKind::Class);
        cls_type.class_name = node.class_name;
        return cls_type;
    }
    switch (node.basic_type) {
        case BuiltinType::Byte:   return TypeInfo(TypeKind::Byte);
        case BuiltinType::Short:  return TypeInfo(TypeKind::Short);
        case BuiltinType::Int:    return TypeInfo(TypeKind::Int);
        case BuiltinType::Long:   return TypeInfo(TypeKind::Long);
        case BuiltinType::UByte:  return TypeInfo(TypeKind::UByte);
        case BuiltinType::UShort: return TypeInfo(TypeKind::UShort);
        case BuiltinType::UInt:   return TypeInfo(TypeKind::UInt);
        case BuiltinType::ULong:  return TypeInfo(TypeKind::ULong);
        case BuiltinType::Char:   return TypeInfo(TypeKind::Char);
        case BuiltinType::Float:  return TypeInfo(TypeKind::Float);
        case BuiltinType::Double: return TypeInfo(TypeKind::Double);
        case BuiltinType::Bool:   return TypeInfo(TypeKind::Bool);
        case BuiltinType::String: return TypeInfo(TypeKind::String);
        case BuiltinType::Void:   return TypeInfo(TypeKind::Void);
    }
    return TypeInfo(TypeKind::Void);
}

std::string Sema::typeName(const TypeInfo& type) const {
    switch (type.kind) {
        case TypeKind::Byte:   return "byte";
        case TypeKind::Short:  return "short";
        case TypeKind::Int:    return "int";
        case TypeKind::Long:   return "long";
        case TypeKind::UByte:  return "ubyte";
        case TypeKind::UShort: return "ushort";
        case TypeKind::UInt:   return "uint";
        case TypeKind::ULong:  return "ulong";
        case TypeKind::Char:   return "char";
        case TypeKind::Float:  return "float";
        case TypeKind::Double: return "double";
        case TypeKind::Bool:   return "bool";
        case TypeKind::String: return "string";
        case TypeKind::Void:   return "void";
        case TypeKind::Null:   return "null";
        case TypeKind::Class:  return type.class_name;
        case TypeKind::Interface: return type.class_name;
        case TypeKind::Array:
            if (type.element_type) return typeName(*type.element_type) + "[]";
            return "array";
        case TypeKind::Slice:
            if (type.element_type) return "slice<" + typeName(*type.element_type) + ">";
            return "slice";
        case TypeKind::Function: {
            std::string s = "(";
            for (size_t i = 0; i < type.param_types.size(); i++) {
                if (i) s += ", ";
                s += typeName(type.param_types[i]);
            }
            s += ") -> ";
            s += type.return_type ? typeName(*type.return_type) : "void";
            return s;
        }
        case TypeKind::Tuple: {
            std::string s = "(";
            for (size_t i = 0; i < type.tuple_types.size(); i++) {
                if (i) s += ", ";
                s += typeName(type.tuple_types[i]);
            }
            s += ")";
            return s;
        }
        case TypeKind::Assoc:
            return "assoc";
    }
    return "unknown";
}

bool Sema::typesCompatible(const TypeInfo& lhs, const TypeInfo& rhs) const {
    if (lhs.kind == rhs.kind) {
        if (lhs.kind == TypeKind::Class) return lhs.class_name == rhs.class_name;
        if (lhs.kind == TypeKind::Array) {
            if (lhs.element_type && rhs.element_type)
                return typesCompatible(*lhs.element_type, *rhs.element_type);
            return !lhs.element_type && !rhs.element_type;
        }
        if (lhs.kind == TypeKind::Slice) {
            if (lhs.element_type && rhs.element_type)
                return typesCompatible(*lhs.element_type, *rhs.element_type);
            return !lhs.element_type && !rhs.element_type;
        }
        if (lhs.kind == TypeKind::Function) {
            // Structural: same param count, each param compatible, return compatible
            if (lhs.param_types.size() != rhs.param_types.size()) return false;
            for (size_t i = 0; i < lhs.param_types.size(); i++)
                if (!typesCompatible(lhs.param_types[i], rhs.param_types[i])) return false;
            if (lhs.return_type && rhs.return_type)
                return typesCompatible(*lhs.return_type, *rhs.return_type);
            return !lhs.return_type && !rhs.return_type;
        }
        if (lhs.kind == TypeKind::Tuple) {
            // Structural: same element count, each element compatible
            if (lhs.tuple_types.size() != rhs.tuple_types.size()) return false;
            for (size_t i = 0; i < lhs.tuple_types.size(); i++)
                if (!typesCompatible(lhs.tuple_types[i], rhs.tuple_types[i])) return false;
            return true;
        }
        return true;
    }
    // 抽象关联类型（§三-5）：Assoc 与任意类型兼容（双向通配）
    if (lhs.kind == TypeKind::Assoc || rhs.kind == TypeKind::Assoc) return true;
    // Implicit numeric promotion: Int ↔ Long are compatible
    if ((lhs.kind == TypeKind::Int && rhs.kind == TypeKind::Long) ||
        (lhs.kind == TypeKind::Long && rhs.kind == TypeKind::Int))
        return true;
    // Interface compatibility: rhs (concrete class) implements lhs (interface)
    if (lhs.kind == TypeKind::Interface && rhs.kind == TypeKind::Class) {
        if (!current_tu_) return false;
        for (auto& cls : current_tu_->classes) {
            if (cls.name != rhs.class_name) continue;
            if (cls.interface_class_name == lhs.class_name) return true;
        }
        return false;
    }
    if (rhs.kind == TypeKind::Null && lhs.kind == TypeKind::Class) return true;

    auto promotes = [](TypeKind from, TypeKind to) -> bool {
        if (from == to) return true;
        // char and byte are interchangeable
        if ((from == TypeKind::Char && to == TypeKind::Byte) ||
            (from == TypeKind::Byte && to == TypeKind::Char))
            return true;
        switch (from) {
            case TypeKind::Byte:
            case TypeKind::Char:
                return to == TypeKind::Short || to == TypeKind::Int || to == TypeKind::Long ||
                       to == TypeKind::Float || to == TypeKind::Double;
            case TypeKind::Short:
                return to == TypeKind::Int || to == TypeKind::Long ||
                       to == TypeKind::Float || to == TypeKind::Double;
            case TypeKind::Int:
                return to == TypeKind::Long || to == TypeKind::Float || to == TypeKind::Double;
            case TypeKind::Long:
                return to == TypeKind::Float || to == TypeKind::Double;
            case TypeKind::Float:
                return to == TypeKind::Double;
            default: return false;
        }
    };
    return promotes(rhs.kind, lhs.kind);
}

// ==============================
// Helper methods
// ==============================

void Sema::error(const SourceRange& range, const std::string& msg) {
    diag_.error(range, msg);
}

bool Sema::expectBool(const TypeInfo& type, const SourceRange& range) {
    if (type.kind != TypeKind::Bool) {
        error(range, "expected boolean expression, got '" + typeName(type) + "'");
        return false;
    }
    return true;
}

bool Sema::expectNumeric(const TypeInfo& type, const SourceRange& range) {
    switch (type.kind) {
        case TypeKind::Byte:  case TypeKind::Short: case TypeKind::Int:
        case TypeKind::Long:  case TypeKind::UByte: case TypeKind::UShort:
        case TypeKind::UInt:  case TypeKind::ULong: case TypeKind::Char:
        case TypeKind::Float: case TypeKind::Double:
            return true;
        default:
            error(range, "expected numeric type, got '" + typeName(type) + "'");
            return false;
    }
}

// ==============================
// Mapping cycle detection
// ==============================

void Sema::checkMappingCycles(const MappingDecl& decl) {
    for (auto& chain : decl.chains) {
        if (chain.nodes.size() < 2) continue;
        // Allow intentional self-triggering (event → action on same instance)
        // Only flag cycles in chains longer than 2 nodes
        if (chain.nodes.size() <= 2) continue;
        std::string event_source = chain.nodes.front().source_name;
        for (size_t i = 1; i < chain.nodes.size(); i++) {
            if (chain.nodes[i].source_name == event_source) {
                error(chain.nodes[i].range,
                    "potential mapping cycle: instance '" + event_source +
                    "' appears as both event source and action target in the same chain");
            }
        }
        std::unordered_set<std::string> seen;
        for (auto& node : chain.nodes) {
            if (!seen.insert(node.source_name).second) {
                error(node.range,
                    "potential mapping cycle: instance '" + node.source_name +
                    "' appears multiple times in the same chain");
            }
        }
    }
}

void Sema::checkMappingTypes(const MappingDecl& decl) {
    if (!current_tu_) return;
    for (auto& chain : decl.chains) {
        if (chain.nodes.size() < 2) continue;
        // First node must be an event — find its parameter types
        auto& ev_node = chain.nodes[0];
        for (auto& cls : current_tu_->classes) {
            for (auto& ev : cls.events) {
                if (ev.name == ev_node.member_name) {
                    // Visit where expression with event params in scope
                    if (chain.where_expr) {
                        symbol_table_.enterScope();
                        for (auto& p : ev.params)
                            symbol_table_.declare(p.name, typeNodeToTypeInfo(p.type));
                        visitExpr(*chain.where_expr);
                        symbol_table_.leaveScope();
                    }
                    // Check subsequent action nodes for parameter compatibility
                    for (size_t i = 1; i < chain.nodes.size(); i++) {
                        auto& act_node = chain.nodes[i];
                        // The first action receives the event params
                        // Subsequent actions receive the previous action's return value
                    }
                    goto next_chain;
                }
            }
        }
        next_chain:;
    }
}

// ==============================
// ==============================
// Type parameter substitution
// ==============================

TypeInfo Sema::substituteTypeParams(const TypeNode& node,
                                    const std::vector<std::string>& type_params,
                                    const std::vector<TypeInfo>& type_args) {
    // Check if this node's class name is a type parameter
    if (node.isClass() && !node.class_name.empty()) {
        for (size_t i = 0; i < type_params.size() && i < type_args.size(); i++) {
            if (node.class_name == type_params[i]) {
                return type_args[i];
            }
        }
    }
    // For array types, substitute element type
    if (node.isArray() && node.element_type) {
        TypeInfo result(TypeKind::Array);
        result.array_size = node.array_size;
        result.element_type = std::make_shared<TypeInfo>(
            substituteTypeParams(*node.element_type, type_params, type_args));
        return result;
    }
    // Default: resolve normally
    return typeNodeToTypeInfo(node);
}

TypeNode Sema::substituteTypeNode(const TypeNode& node,
                                   const std::vector<std::string>& type_params,
                                   const std::vector<TypeNode>& type_args) const {
    // Check if this node's class name is a type parameter
    if (node.isClass() && !node.class_name.empty()) {
        // 关联类型 T::Item — 替换 owner 类型参数（§三-5）
        auto pos = node.class_name.find("::");
        if (pos != std::string::npos) {
            std::string owner = node.class_name.substr(0, pos);
            std::string member = node.class_name.substr(pos + 2);
            for (size_t i = 0; i < type_params.size() && i < type_args.size(); i++) {
                if (owner != type_params[i]) continue;
                TypeNode result = node;
                result.class_name = type_args[i].class_name + "::" + member;
                result.type_args = type_args[i].type_args;
                return result;
            }
        }
        for (size_t i = 0; i < type_params.size() && i < type_args.size(); i++) {
            if (node.class_name == type_params[i]) {
                return type_args[i];
            }
        }
        // Recursively substitute type arguments (e.g. HashMap<T, bool> where T is a type param)
        if (!node.type_args.empty()) {
            TypeNode result = node;
            result.type_args.clear();
            for (auto& arg : node.type_args) {
                result.type_args.push_back(substituteTypeNode(arg, type_params, type_args));
            }
            return result;
        }
    }
    // Function type: (A,B) -> R — substitute param types and return type
    if (node.isFunction()) {
        TypeNode result;
        result.range = node.range;
        for (auto& p : node.func_param_types)
            result.func_param_types.push_back(substituteTypeNode(p, type_params, type_args));
        if (node.func_return_type)
            result.func_return_type = std::make_shared<TypeNode>(
                substituteTypeNode(*node.func_return_type, type_params, type_args));
        return result;
    }
    // Tuple type: (A,B) — substitute element types
    if (node.isTuple()) {
        TypeNode result;
        result.range = node.range;
        result.is_tuple = true;
        for (auto& p : node.func_param_types)
            result.func_param_types.push_back(substituteTypeNode(p, type_params, type_args));
        return result;
    }
    // For array types, substitute element type
    if (node.isArray() && node.element_type) {
        TypeNode result;
        result.basic_type = node.basic_type;
        result.array_size = node.array_size;
        result.element_type = std::make_shared<TypeNode>(
            substituteTypeNode(*node.element_type, type_params, type_args));
        return result;
    }
    return node;
}

// Built-in modules
// ==============================

void Sema::visitFFI(FFIDecl& decl) {
    // Register FFI function in symbol table as a function type
    TypeInfo ft(TypeKind::Function);
    ft.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(decl.return_type));
    for (auto& p : decl.params)
        ft.param_types.push_back(typeNodeToTypeInfo(p.type));
    symbol_table_.declare(decl.name, ft);
}

void Sema::collectExprLocals(Expr& e, std::set<std::string>& locals) {
    switch (e.kind) {
        case ExprKind::BinaryOp: {
            auto& b = static_cast<BinaryOpExpr&>(e);
            collectExprLocals(*b.lhs, locals); collectExprLocals(*b.rhs, locals); break;
        }
        case ExprKind::UnaryOp:
            collectExprLocals(*static_cast<UnaryOpExpr&>(e).operand, locals); break;
        case ExprKind::Call: {
            auto& c = static_cast<CallExpr&>(e);
            collectExprLocals(*c.callee, locals);
            for (auto& a : c.args) collectExprLocals(*a, locals);
            break;
        }
        case ExprKind::MemberAccess:
            collectExprLocals(*static_cast<MemberAccessExpr&>(e).object, locals); break;
        case ExprKind::Subscript: {
            auto& s = static_cast<SubscriptExpr&>(e);
            collectExprLocals(*s.array, locals); collectExprLocals(*s.index, locals); break;
        }
        case ExprKind::NewExpr:
            for (auto& a : static_cast<NewExpr&>(e).args) collectExprLocals(*a, locals);
            break;
        case ExprKind::NewArrayExpr:
            for (auto& d : static_cast<NewArrayExpr&>(e).dimensions) collectExprLocals(*d, locals);
            break;
        case ExprKind::Assignment: {
            auto& as = static_cast<AssignmentExpr&>(e);
            collectExprLocals(*as.target, locals); collectExprLocals(*as.value, locals); break;
        }
        case ExprKind::Ternary: {
            auto& t = static_cast<TernaryExpr&>(e);
            collectExprLocals(*t.condition, locals); collectExprLocals(*t.true_expr, locals);
            collectExprLocals(*t.false_expr, locals); break;
        }
        case ExprKind::Range: {
            auto& r = static_cast<RangeExpr&>(e);
            collectExprLocals(*r.start, locals); collectExprLocals(*r.end, locals); break;
        }
        case ExprKind::Lambda: {
            // Nested lambda: its params/locals belong to the inner scope — skip.
            break;
        }
        case ExprKind::Pipe: {
            auto& p = static_cast<PipeExpr&>(e);
            if (p.lhs) collectExprLocals(*p.lhs, locals);
            if (p.rhs) collectExprLocals(*p.rhs, locals);
            break;
        }
        case ExprKind::Try: {
            auto& t = static_cast<TryExpr&>(e);
            if (t.try_expr) collectExprLocals(*t.try_expr, locals);
            if (t.catch_expr) collectExprLocals(*t.catch_expr, locals);
            break;
        }
        case ExprKind::TupleExpr:
            for (auto& el : static_cast<TupleExpr&>(e).elements) collectExprLocals(*el, locals);
            break;
        case ExprKind::Await:
            if (static_cast<AwaitExpr&>(e).operand) collectExprLocals(*static_cast<AwaitExpr&>(e).operand, locals);
            break;
        default: break;
    }
}

void Sema::collectLambdaLocals(Stmt& stmt, std::set<std::string>& locals) {
    switch (stmt.kind) {
        case StmtKind::Block:
            for (auto& s : static_cast<BlockStmt&>(stmt).statements)
                if (s) collectLambdaLocals(*s, locals);
            break;
        case StmtKind::VarDeclStmt:
            for (auto& d : static_cast<VarDeclStmt&>(stmt).decls) {
                locals.insert(d.name);
                if (d.init_expr) collectExprLocals(*d.init_expr, locals);
            }
            break;
        case StmtKind::ExprStmt:
            if (static_cast<ExprStmt&>(stmt).expression)
                collectExprLocals(*static_cast<ExprStmt&>(stmt).expression, locals);
            break;
        case StmtKind::IfStmt: {
            auto& i = static_cast<IfStmt&>(stmt);
            if (i.condition) collectExprLocals(*i.condition, locals);
            if (i.then_block) collectLambdaLocals(*i.then_block, locals);
            if (i.else_block) collectLambdaLocals(*i.else_block, locals);
            break;
        }
        case StmtKind::WhileStmt: {
            auto& w = static_cast<WhileStmt&>(stmt);
            if (w.condition) collectExprLocals(*w.condition, locals);
            if (w.body) collectLambdaLocals(*w.body, locals);
            break;
        }
        case StmtKind::ForStmt: {
            auto& f = static_cast<ForStmt&>(stmt);
            if (f.init) collectLambdaLocals(*f.init, locals);
            if (f.condition) collectExprLocals(*f.condition, locals);
            if (f.step) collectExprLocals(*f.step, locals);
            if (f.body) collectLambdaLocals(*f.body, locals);
            break;
        }
        case StmtKind::ForInStmt: {
            auto& f = static_cast<ForInStmt&>(stmt);
            locals.insert(f.var_name);  // loop var — lambda-local
            if (f.body) collectLambdaLocals(*f.body, locals);
            break;
        }
        case StmtKind::ReturnStmt:
            if (static_cast<ReturnStmt&>(stmt).value)
                collectExprLocals(*static_cast<ReturnStmt&>(stmt).value, locals);
            break;
        case StmtKind::DestructureStmt: {
            auto& ds = static_cast<DestructureStmt&>(stmt);
            // Declaration leaves are locals (shadowed by the tuple element types).
            std::function<void(const DestructureTarget&)> bind =
                [&](const DestructureTarget& t) {
                    if (!t.name.empty()) { locals.insert(t.name); return; }
                    for (auto& c : t.elements) bind(c);
                };
            for (auto& c : ds.target.elements) bind(c);
            if (ds.value) collectExprLocals(*ds.value, locals);
            break;
        }
        case StmtKind::MatchStmt:
            for (auto& a : static_cast<MatchStmt&>(stmt).arms) {
                for (auto& b : a.bindings) locals.insert(b);
                if (a.body) collectLambdaLocals(*a.body, locals);
            }
            break;
        default: break;
    }
}

void Sema::collectExprCaptures(Expr& e, const std::set<std::string>& locals,
                               const std::vector<std::string>& params,
                               std::vector<std::string>& out) {
    switch (e.kind) {
        case ExprKind::Identifier: {
            auto& id = static_cast<IdentifierExpr&>(e);
            if (id.name == "this" || id.name == "true" || id.name == "false") break;
            if (std::find(params.begin(), params.end(), id.name) != params.end()) break;
            if (locals.count(id.name)) break;
            if (symbol_table_.lookup(id.name))
                if (std::find(out.begin(), out.end(), id.name) == out.end())
                    out.push_back(id.name);
            break;
        }
        case ExprKind::BinaryOp: {
            auto& b = static_cast<BinaryOpExpr&>(e);
            collectExprCaptures(*b.lhs, locals, params, out);
            collectExprCaptures(*b.rhs, locals, params, out); break;
        }
        case ExprKind::UnaryOp:
            collectExprCaptures(*static_cast<UnaryOpExpr&>(e).operand, locals, params, out); break;
        case ExprKind::Call: {
            auto& c = static_cast<CallExpr&>(e);
            collectExprCaptures(*c.callee, locals, params, out);
            for (auto& a : c.args) collectExprCaptures(*a, locals, params, out);
            break;
        }
        case ExprKind::MemberAccess:
            collectExprCaptures(*static_cast<MemberAccessExpr&>(e).object, locals, params, out); break;
        case ExprKind::Subscript: {
            auto& s = static_cast<SubscriptExpr&>(e);
            collectExprCaptures(*s.array, locals, params, out);
            collectExprCaptures(*s.index, locals, params, out); break;
        }
        case ExprKind::NewExpr:
            for (auto& a : static_cast<NewExpr&>(e).args) collectExprCaptures(*a, locals, params, out);
            break;
        case ExprKind::NewArrayExpr:
            for (auto& d : static_cast<NewArrayExpr&>(e).dimensions) collectExprCaptures(*d, locals, params, out);
            break;
        case ExprKind::Assignment: {
            auto& as = static_cast<AssignmentExpr&>(e);
            collectExprCaptures(*as.target, locals, params, out);
            collectExprCaptures(*as.value, locals, params, out); break;
        }
        case ExprKind::Ternary: {
            auto& t = static_cast<TernaryExpr&>(e);
            collectExprCaptures(*t.condition, locals, params, out);
            collectExprCaptures(*t.true_expr, locals, params, out);
            collectExprCaptures(*t.false_expr, locals, params, out); break;
        }
        case ExprKind::Range: {
            auto& r = static_cast<RangeExpr&>(e);
            collectExprCaptures(*r.start, locals, params, out);
            collectExprCaptures(*r.end, locals, params, out); break;
        }
        case ExprKind::Lambda: {
            auto& l = static_cast<LambdaExpr&>(e);
            if (l.body) collectLambdaCaptures(*l.body, locals, params, out);
            break;
        }
        case ExprKind::Pipe: {
            auto& p = static_cast<PipeExpr&>(e);
            if (p.lhs) collectExprCaptures(*p.lhs, locals, params, out);
            if (p.rhs) collectExprCaptures(*p.rhs, locals, params, out);
            break;
        }
        case ExprKind::Try: {
            auto& t = static_cast<TryExpr&>(e);
            if (t.try_expr) collectExprCaptures(*t.try_expr, locals, params, out);
            if (t.catch_expr) collectExprCaptures(*t.catch_expr, locals, params, out);
            break;
        }
        case ExprKind::TupleExpr:
            for (auto& el : static_cast<TupleExpr&>(e).elements) collectExprCaptures(*el, locals, params, out);
            break;
        case ExprKind::Await:
            if (static_cast<AwaitExpr&>(e).operand) collectExprCaptures(*static_cast<AwaitExpr&>(e).operand, locals, params, out);
            break;
        default: break;
    }
}

void Sema::collectLambdaCaptures(Stmt& stmt, const std::set<std::string>& locals,
                                 const std::vector<std::string>& params,
                                 std::vector<std::string>& out) {
    switch (stmt.kind) {
        case StmtKind::Block:
            for (auto& s : static_cast<BlockStmt&>(stmt).statements)
                if (s) collectLambdaCaptures(*s, locals, params, out);
            break;
        case StmtKind::VarDeclStmt:
            for (auto& d : static_cast<VarDeclStmt&>(stmt).decls)
                if (d.init_expr) collectExprCaptures(*d.init_expr, locals, params, out);
            break;
        case StmtKind::ExprStmt:
            if (static_cast<ExprStmt&>(stmt).expression)
                collectExprCaptures(*static_cast<ExprStmt&>(stmt).expression, locals, params, out);
            break;
        case StmtKind::IfStmt: {
            auto& i = static_cast<IfStmt&>(stmt);
            if (i.condition) collectExprCaptures(*i.condition, locals, params, out);
            if (i.then_block) collectLambdaCaptures(*i.then_block, locals, params, out);
            if (i.else_block) collectLambdaCaptures(*i.else_block, locals, params, out);
            break;
        }
        case StmtKind::WhileStmt: {
            auto& w = static_cast<WhileStmt&>(stmt);
            if (w.condition) collectExprCaptures(*w.condition, locals, params, out);
            if (w.body) collectLambdaCaptures(*w.body, locals, params, out);
            break;
        }
        case StmtKind::ForStmt: {
            auto& f = static_cast<ForStmt&>(stmt);
            if (f.init) collectLambdaCaptures(*f.init, locals, params, out);
            if (f.condition) collectExprCaptures(*f.condition, locals, params, out);
            if (f.step) collectExprCaptures(*f.step, locals, params, out);
            if (f.body) collectLambdaCaptures(*f.body, locals, params, out);
            break;
        }
        case StmtKind::ForInStmt: {
            auto& f = static_cast<ForInStmt&>(stmt);
            if (f.iterable) collectExprCaptures(*f.iterable, locals, params, out);
            if (f.body) collectLambdaCaptures(*f.body, locals, params, out);
            break;
        }
        case StmtKind::ReturnStmt:
            if (static_cast<ReturnStmt&>(stmt).value)
                collectExprCaptures(*static_cast<ReturnStmt&>(stmt).value, locals, params, out);
            break;
        case StmtKind::DestructureStmt: {
            auto& ds = static_cast<DestructureStmt&>(stmt);
            if (ds.value) collectExprCaptures(*ds.value, locals, params, out);
            break;
        }
        case StmtKind::MatchStmt:
            for (auto& a : static_cast<MatchStmt&>(stmt).arms)
                if (a.body) collectLambdaCaptures(*a.body, locals, params, out);
            break;
        default: break;
    }
}

TypeInfo Sema::visitLambda(LambdaExpr& expr, const TypeInfo* expected_fn) {
    // Create a hidden class: __lambda_N
    std::string cls_name = "__lambda_" + std::to_string(lambda_counter_++);
    expr.hidden_class_name = cls_name;

    // ---- M-FN-2: capture analysis (outer locals referenced in the body) ----
    std::vector<std::string> params;
    for (auto& p : expr.params) params.push_back(p.name);
    std::set<std::string> locals;
    for (auto& p : expr.params) locals.insert(p.name);
    if (expr.body) collectLambdaLocals(*expr.body, locals);
    std::vector<std::string> captures;
    if (expr.body) collectLambdaCaptures(*expr.body, locals, params, captures);

    ClassDecl cls;
    cls.name = cls_name;
    cls.range = expr.range;

    // Capture slots as hidden-class properties: cap_0, cap_1, ...
    std::vector<TypeNode> cap_types;
    for (size_t i = 0; i < captures.size(); i++) {
        auto* sym = symbol_table_.lookup(captures[i]);
        PropertyDecl prop;
        std::string slot = "cap_" + std::to_string(i);
        TypeNode cty = sym ? TypeNodeFromTypeInfo(*sym) : TypeNode();
        prop.name = slot;
        prop.type = cty;
        prop.range = expr.range;
        cls.properties.push_back(std::move(prop));
        expr.capture_names.push_back(captures[i]);
        expr.capture_slots.push_back(slot);
        cap_types.push_back(cty);
    }

    ActionDecl action;
    action.name = "__call";
    action.return_type = TypeNode(); // default Int (will be inferred from body)
    if (!expr.params.empty()) {
        // Use the last param's type as return type (default void)
        action.params = expr.params;
    }
    action.body = expr.body;
    action.range = expr.range;

    // Prepend capture copies to the __call body: `T name = this.cap_i;` so the
    // original body's references to `name` resolve to the local copy.
    if (!captures.empty()) {
        if (auto* block = dynamic_cast<BlockStmt*>(action.body.get())) {
            std::vector<std::unique_ptr<Stmt>> pre;
            for (size_t i = 0; i < captures.size(); i++) {
                VarDecl d;
                d.name = captures[i];
                d.type = cap_types[i];
                d.range = expr.range;
                auto ma = std::make_unique<MemberAccessExpr>(
                    std::make_unique<ThisExpr>(expr.range),
                    "cap_" + std::to_string(i), expr.range);
                d.init_expr = std::move(ma);
                std::vector<VarDecl> decls;
                decls.push_back(std::move(d));
                auto st = std::make_unique<VarDeclStmt>(std::move(decls), expr.range);
                pre.push_back(std::move(st));
            }
            block->statements.insert(block->statements.begin(),
                std::make_move_iterator(pre.begin()), std::make_move_iterator(pre.end()));
        }
    }

    // Determine __call return type: contextual expected type (M-FN-2, e.g.
    // `(int) -> int f = (x) => ...` / passed to a `(T)->R` param) takes
    // priority — this avoids inferring from body expressions that reference
    // body-local variables (not yet in scope during inference). Otherwise fall
    // back to inferring from the first `return expr;` (bare return / none ⇒ void).
    TypeNode ret_ty;
    if (expected_fn && expected_fn->kind == TypeKind::Function && expected_fn->return_type) {
        ret_ty = TypeNodeFromTypeInfo(*expected_fn->return_type);
    } else {
        symbol_table_.enterScope();
        for (auto& p : expr.params)
            symbol_table_.declare(p.name, typeNodeToTypeInfo(p.type));
        bool found = false;
        inferLambdaReturn(*expr.body, ret_ty, found);
        symbol_table_.leaveScope();
    }
    action.return_type = ret_ty;
    cls.actions.push_back(std::move(action));

    // Register class and add to TU
    if (current_tu_) {
        current_tu_->classes.push_back(std::move(cls));
        visitClassDecl(current_tu_->classes.back());
    }

    // First-class function value: the lambda's type is its function type.
    TypeInfo ft(TypeKind::Function);
    ft.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(action.return_type));
    for (auto& p : expr.params) ft.param_types.push_back(typeNodeToTypeInfo(p.type));
    return ft;
}

void Sema::inferLambdaReturn(Stmt& stmt, TypeNode& out, bool& found) {
    if (found) return;
    switch (stmt.kind) {
        case StmtKind::Block: {
            auto& b = static_cast<BlockStmt&>(stmt);
            for (auto& s : b.statements) {
                if (s) { inferLambdaReturn(*s, out, found); if (found) return; }
            }
            break;
        }
        case StmtKind::IfStmt: {
            auto& i = static_cast<IfStmt&>(stmt);
            if (i.then_block) { inferLambdaReturn(*i.then_block, out, found); if (found) return; }
            if (i.else_block) { inferLambdaReturn(*i.else_block, out, found); if (found) return; }
            break;
        }
        case StmtKind::WhileStmt: {
            auto& w = static_cast<WhileStmt&>(stmt);
            if (w.body) { inferLambdaReturn(*w.body, out, found); if (found) return; }
            break;
        }
        case StmtKind::ForStmt: {
            auto& f = static_cast<ForStmt&>(stmt);
            if (f.body) { inferLambdaReturn(*f.body, out, found); if (found) return; }
            break;
        }
        case StmtKind::ForInStmt: {
            auto& f = static_cast<ForInStmt&>(stmt);
            if (f.body) { inferLambdaReturn(*f.body, out, found); if (found) return; }
            break;
        }
        case StmtKind::ReturnStmt: {
            auto& r = static_cast<ReturnStmt&>(stmt);
            if (r.value) {
                TypeInfo t = visitExpr(*r.value);
                out = TypeNodeFromTypeInfo(t);
            } else {
                TypeNode vt;
                vt.basic_type = BuiltinType::Void;
                out = vt;
            }
            found = true;
            break;
        }
        default: break;
    }
}

// Pipe: lhs |> op — apply an operator component (class with a `transform`
// method) to lhs. target_kind="class" instantiates a fresh component;
// "instance" reuses an operator variable. Left-assoc via chained PipeExpr.
TypeInfo Sema::visitPipe(PipeExpr& expr) {
    auto lhs_type = visitExpr(*expr.lhs);

    // Resolve the operator on the RHS: class name or instance variable.
    auto findTransform = [&](const ClassDecl& cls) -> const ActionDecl* {
        for (auto& a : cls.actions)
            if (a.name == "transform" && a.params.size() == 1) return &a;
        for (auto& f : cls.functions)
            if (f.name == "transform" && f.params.size() == 1) return nullptr;  // functions not supported as pipe target v1
        return nullptr;
    };

    if (expr.rhs->kind == ExprKind::Identifier) {
        auto& id = static_cast<IdentifierExpr&>(*expr.rhs);
        // 1) Class name → instantiate
        if (current_tu_) {
            for (auto& cls : current_tu_->classes) {
                if (cls.name != id.name) continue;
                if (auto* a = findTransform(cls)) {
                    auto ptype = typeNodeToTypeInfo(a->params[0].type);
                    if (!typesCompatible(ptype, lhs_type)) {
                        error(expr.range, "pipe: cannot apply '" + id.name +
                              ".transform' to operand of type '" + typeName(lhs_type) + "'");
                        return TypeInfo(TypeKind::Void);
                    }
                    expr.target_kind = "class";
                    expr.class_name = cls.name;
                    expr.method = "transform";
                    return typeNodeToTypeInfo(a->return_type);
                }
            }
        }
        // 2) Instance variable → reuse
        if (auto* sym = symbol_table_.lookup(id.name)) {
            if (sym->kind == TypeKind::Class && current_tu_) {
                for (auto& cls : current_tu_->classes) {
                    if (cls.name != sym->class_name) continue;
                    if (auto* a = findTransform(cls)) {
                        auto ptype = typeNodeToTypeInfo(a->params[0].type);
                        if (!typesCompatible(ptype, lhs_type)) {
                            error(expr.range, "pipe: cannot apply '" + id.name +
                                  ".transform' to operand of type '" + typeName(lhs_type) + "'");
                            return TypeInfo(TypeKind::Void);
                        }
                        expr.target_kind = "instance";
                        expr.class_name = cls.name;
                        expr.method = "transform";
                        return typeNodeToTypeInfo(a->return_type);
                    }
                }
            }
        }
    }

    error(expr.range, "pipe '|>' requires an operator component with a single-argument 'transform' method");
    return TypeInfo(TypeKind::Void);
}

void Sema::registerIntrinsics() {
    // Intrinsic functions for stdlib use (__myp_ prefix = internal)
    auto add_intrinsic = [&](const std::string& name, TypeKind ret, std::vector<TypeKind> params) {
        TypeInfo t(TypeKind::Function);
        t.return_type = std::make_shared<TypeInfo>(ret);
        for (auto p : params)
            t.param_types.push_back(TypeInfo(p));
        symbol_table_.declare(name, t);
    };

    add_intrinsic("__myp_print_int", TypeKind::Void, {TypeKind::Int});
    add_intrinsic("__myp_print_long", TypeKind::Void, {TypeKind::Long});
    add_intrinsic("__myp_print", TypeKind::Void, {TypeKind::String});
    add_intrinsic("__myp_println", TypeKind::Void, {TypeKind::String});
    add_intrinsic("__myp_print_float", TypeKind::Void, {TypeKind::Double});
    add_intrinsic("__myp_print_bool", TypeKind::Void, {TypeKind::Bool});
    add_intrinsic("__myp_now_ms", TypeKind::Long, {});
    add_intrinsic("__myp_sleep_ms", TypeKind::Void, {TypeKind::Long});

    // Terminal size intrinsics (TUI)
    add_intrinsic("__myp_term_width", TypeKind::Int, {});
    add_intrinsic("__myp_term_height", TypeKind::Int, {});
    // String utilities (for TUI)
    add_intrinsic("__myp_strlen", TypeKind::Int, {TypeKind::String});
    add_intrinsic("__myp_chr", TypeKind::String, {TypeKind::Int});
    add_intrinsic("__myp_ord", TypeKind::Int, {TypeKind::String});

    // Timer intrinsics
    // __myp_timer_create(event_name, delay_ms, interval_ms)
    // Codegen resolves event_name to event_id using the current class context.
    add_intrinsic("__myp_timer_create", TypeKind::Int,
                  {TypeKind::String, TypeKind::Long, TypeKind::Long});

    // Math intrinsics
    add_intrinsic("__myp_math_sqrt", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_abs", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_floor", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_ceil", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_sin", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_cos", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_tan", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_asin", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_acos", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_atan", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_atan2", TypeKind::Double, {TypeKind::Double, TypeKind::Double});
    add_intrinsic("__myp_math_sinh", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_cosh", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_tanh", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_exp", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_log", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_pow", TypeKind::Double, {TypeKind::Double, TypeKind::Double});
    add_intrinsic("__myp_math_abs_int", TypeKind::Int, {TypeKind::Int});

    // CUDA GPU availability (returns 1 if GPU offload usable, 0 otherwise)
    add_intrinsic("__myp_cuda_available", TypeKind::Int, {});

    // CUDA device info (return 0/empty if GPU unavailable)
    add_intrinsic("__myp_cuda_count", TypeKind::Int, {});
    add_intrinsic("__myp_cuda_name", TypeKind::String, {});
    add_intrinsic("__myp_cuda_memory", TypeKind::Long, {});
    add_intrinsic("__myp_cuda_capability", TypeKind::Int, {});
    add_intrinsic("__myp_cuda_multiprocessors", TypeKind::Int, {});
    add_intrinsic("__myp_cuda_max_threads", TypeKind::Int, {});
    add_intrinsic("__myp_cuda_warp", TypeKind::Int, {});

    // File I/O intrinsics
    add_intrinsic("__myp_io_fopen", TypeKind::Int, {TypeKind::String, TypeKind::String});
    add_intrinsic("__myp_io_fclose", TypeKind::Void, {});
    add_intrinsic("__myp_io_current_handle", TypeKind::Int, {});
    add_intrinsic("__myp_io_select", TypeKind::Void, {TypeKind::Int});
    add_intrinsic("__myp_io_read_line", TypeKind::String, {});
    add_intrinsic("__myp_io_write", TypeKind::Void, {TypeKind::String});
    add_intrinsic("__myp_io_write_line", TypeKind::Void, {TypeKind::String});
    add_intrinsic("__myp_io_has_next", TypeKind::Int, {});
    add_intrinsic("__myp_io_read_byte", TypeKind::Int, {});
    add_intrinsic("__myp_io_read_i32be", TypeKind::Int, {});
    add_intrinsic("__myp_io_seek", TypeKind::Int, {TypeKind::Int, TypeKind::Int});
    add_intrinsic("__myp_io_write_byte", TypeKind::Int, {TypeKind::Int});
    add_intrinsic("__myp_io_write_i32be", TypeKind::Int, {TypeKind::Int});
    add_intrinsic("__myp_io_write_double", TypeKind::Int, {TypeKind::Double});
    add_intrinsic("__myp_io_read_double", TypeKind::Double, {});

    // stdin read line
    add_intrinsic("__myp_read_line", TypeKind::String, {});
    // stdout flush
    add_intrinsic("__myp_flush", TypeKind::Void, {});
    // string to double
    add_intrinsic("__myp_atof", TypeKind::Double, {TypeKind::String});
    // non-blocking keyboard
    add_intrinsic("__myp_kbhit", TypeKind::Int, {});
    add_intrinsic("__myp_getch", TypeKind::Int, {});
    // error handling
    add_intrinsic("__myp_throw", TypeKind::Void, {TypeKind::String});
    // testing
    add_intrinsic("__myp_assert", TypeKind::Void, {TypeKind::Bool});
    add_intrinsic("__myp_assert_eq", TypeKind::Void, {TypeKind::Int, TypeKind::Int});
    add_intrinsic("__myp_assert_neq", TypeKind::Void, {TypeKind::Int, TypeKind::Int});
    add_intrinsic("__myp_assert_long_eq", TypeKind::Void, {TypeKind::Long, TypeKind::Long});
    add_intrinsic("__myp_assert_str_eq", TypeKind::Void, {TypeKind::String, TypeKind::String});
    add_intrinsic("__myp_assert_str_neq", TypeKind::Void, {TypeKind::String, TypeKind::String});
    add_intrinsic("__myp_test_report", TypeKind::Void, {TypeKind::String, TypeKind::Bool});

    // NOTE: __myp_coro_* are NOT registered here. The stdlib `Coro` class is a
    // compiler built-in (codegen generates the runtime call); keeping these out
    // of the symbol table makes them invisible to user code (calls → undefined).

    // Atomic intrinsics (codegen generates LLVM atomic instructions directly)
    // These take (int[] array, int index, int value) and return the OLD value.
    // Register as raw function types bypassing the add_intrinsic helper for array params.
    auto add_atomic = [&](const std::string& name, TypeKind ret, std::vector<TypeKind> params, std::vector<TypeKind> elem_types) {
        TypeInfo t(TypeKind::Function);
        t.return_type = std::make_shared<TypeInfo>(ret);
        size_t ei = 0;
        for (auto p : params) {
            if (p == TypeKind::Array) {
                TypeInfo arr(TypeKind::Array);
                arr.element_type = std::make_shared<TypeInfo>(
                    ei < elem_types.size() ? elem_types[ei] : TypeKind::Int);
                t.param_types.push_back(arr);
                ei++;
            } else {
                t.param_types.push_back(TypeInfo(p));
            }
        }
        symbol_table_.declare(name, t);
    };
    add_atomic("__myp_atomic_add_i32", TypeKind::Int, {TypeKind::Array, TypeKind::Int, TypeKind::Int}, {TypeKind::Int});
    add_atomic("__myp_atomic_sub_i32", TypeKind::Int, {TypeKind::Array, TypeKind::Int, TypeKind::Int}, {TypeKind::Int});
    add_atomic("__myp_atomic_xchg_i32", TypeKind::Int, {TypeKind::Array, TypeKind::Int, TypeKind::Int}, {TypeKind::Int});
    add_atomic("__myp_atomic_add_f64", TypeKind::Double, {TypeKind::Array, TypeKind::Int, TypeKind::Double}, {TypeKind::Double});
    add_atomic("__myp_atomic_load_i32", TypeKind::Int, {TypeKind::Array, TypeKind::Int}, {TypeKind::Int});
    add_atomic("__myp_atomic_store_i32", TypeKind::Void, {TypeKind::Array, TypeKind::Int, TypeKind::Int}, {TypeKind::Int});

    // Thread pool (v6)
    add_intrinsic("__myp_pool_thread_count", TypeKind::Int, {});

    // Float-to-int truncation
    add_intrinsic("__myp_trunc", TypeKind::Int, {TypeKind::Double});
}

Sema::StmtResult Sema::visitThrowStmt(ThrowStmt& stmt) {
    // throw; — bare rethrow of the current exception (only valid inside a catch)
    if (!stmt.expr) {
        if (in_catch_depth_ == 0) {
            error(stmt.range, "'throw;' rethrow is only valid inside a catch block");
        }
        stmt.throw_type = "rethrow";
        return {};
    }
    auto t = visitExpr(*stmt.expr);
    if (t.kind == TypeKind::String) {
        stmt.throw_type = "string";
    } else if (t.kind == TypeKind::Class) {
        stmt.throw_type = t.class_name;
    } else if (t.kind == TypeKind::Void) {
        // cascading error recovery
    } else {
        error(stmt.range, "throw requires a string or class instance, got '" +
              typeName(t) + "'");
    }
    return {};
}

Sema::StmtResult Sema::visitTryStmt(TryStmt& stmt) {
    // Save and temporarily clear main function flag (try blocks allow calls)
    bool saved_main = in_main_function_;
    in_main_function_ = false;

    // Type-check the try block
    if (stmt.try_block) visitBlock(*stmt.try_block);

    // Restore main flag for catch/finally blocks
    in_main_function_ = saved_main;

    // Type-check each catch clause: declare its variable with its declared type.
    // catch-all ("") and catch (string e) → the variable is a string message;
    // catch (ClassName e) → the variable is a class instance.
    for (auto& cc : stmt.catches) {
        symbol_table_.enterScope();
        TypeInfo ct(TypeKind::String);
        if (!cc.var_type.empty() && cc.var_type != "string") {
            bool found = false;
            bool is_iface = false;
            if (current_tu_) {
                for (auto& cls : current_tu_->classes)
                    if (cls.name == cc.var_type) { found = true; break; }
                if (!found) {
                    // Interface catch, e.g. catch (Error e): matches any class
                    // that implements the interface (dispatch happens at runtime).
                    for (auto& ifd : current_tu_->interfaces)
                        if (ifd.name == cc.var_type) {
                            found = true;
                            is_iface = true;
                            break;
                        }
                }
            }
            if (!found) {
                error(stmt.range, "catch type '" + cc.var_type +
                    "' is not a class or interface");
            } else if (is_iface) {
                ct = TypeInfo(TypeKind::Interface);
                ct.class_name = cc.var_type;
            } else {
                ct = TypeInfo(TypeKind::Class);
                ct.class_name = cc.var_type;
            }
        }
        symbol_table_.declare(cc.var_name, ct);
        in_main_function_ = false; // catch also allows calls
        ++in_catch_depth_;
        if (cc.block) visitBlock(*cc.block);
        --in_catch_depth_;
        symbol_table_.leaveScope();
    }

    // Type-check the finally block
    if (stmt.finally_block) {
        in_main_function_ = false;
        visitBlock(*stmt.finally_block);
    }

    in_main_function_ = saved_main;
    return {};
}

} // namespace mylang
