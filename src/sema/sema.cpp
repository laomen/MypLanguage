#include "mylang/Sema.h"

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace mylang {

// ==============================
// Constructor
// ==============================

Sema::Sema(DiagnosticEngine& diag)
    : diag_(diag) {}

void Sema::buildCurrentClassMemberTypes(const ClassDecl& decl) {
    current_class_member_types_.clear();
    for (auto& prop : decl.properties)
        current_class_member_types_.emplace(prop.name, typeNodeToTypeInfo(prop.type));
    for (auto& action : decl.actions) {
        TypeInfo func_type(TypeKind::Function);
        func_type.return_type = std::make_shared<TypeInfo>(
            typeNodeToTypeInfo(action.return_type));
        for (auto& param : action.params)
            func_type.param_types.push_back(typeNodeToTypeInfo(param.type));
        populateFuncTypeMeta(func_type, action.params);
        current_class_member_types_.emplace(action.name, std::move(func_type));
    }
    for (auto& func : decl.functions) {
        TypeInfo func_type(TypeKind::Function);
        func_type.return_type = std::make_shared<TypeInfo>(
            typeNodeToTypeInfo(func.return_type));
        for (auto& param : func.params)
            func_type.param_types.push_back(typeNodeToTypeInfo(param.type));
        populateFuncTypeMeta(func_type, func.params);
        current_class_member_types_.emplace(func.name, std::move(func_type));
    }
    for (auto& event : decl.events) {
        TypeInfo event_type(TypeKind::Function);
        event_type.return_type = std::make_shared<TypeInfo>(TypeKind::Void);
        for (auto& param : event.params) {
            event_type.param_types.push_back(typeNodeToTypeInfo(param.type));
            event_type.param_is_ref.push_back(false);
        }
        current_class_member_types_.emplace(event.name, std::move(event_type));
    }
}

bool Sema::analyze(TranslationUnit& tu) {
    current_tu_ = &tu;
    current_class_member_types_.clear();
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
        buildCurrentClassMemberTypes(tu.classes[ci]);
        // Actions (may trigger monomorphization → tu.classes may reallocate)
        size_t nactions = tu.classes[ci].actions.size();
        for (size_t ai = 0; ai < nactions; ai++) {
            auto& action = tu.classes[ci].actions[ai];
            if (action.body) {
                current_return_type_ = typeNodeToTypeInfo(action.return_type);
                in_coro_method_ = action.has_coro;
                current_method_name_ = action.name;
                symbol_table_.enterScope();
                TypeInfo this_type(TypeKind::Class);
                this_type.class_name = current_class_name_;
                symbol_table_.declare("this", this_type);
                // M-FN-2 named lambda：__call body 内把自名声明为自身函数类型（走
                // 自身 tramp 递归），并置 lambda_self_* 标记供调用解析识别。
                bool named_lambda =
                    current_class_name_.rfind("__lambda_", 0) == 0 &&
                    !tu.classes[ci].lambda_name.empty() &&
                    action.name == "__call";
                if (named_lambda) {
                    lambda_self_name_ = tu.classes[ci].lambda_name;
                    lambda_self_class_ = current_class_name_;
                    TypeInfo self_ft(TypeKind::Function);
                    self_ft.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(action.return_type));
                    for (auto& p : action.params)
                        self_ft.param_types.push_back(typeNodeToTypeInfo(p.type));
                    symbol_table_.declare(tu.classes[ci].lambda_name, self_ft);
                }
                for (auto& param : action.params) {
                    auto param_type = typeNodeToTypeInfo(param.type);
                    symbol_table_.declare(param.name, param_type);
                }
                checkParamDefaults(action.params);
                // Capture before visitStmt (monomorphization may reallocate
                // tu.classes → dangling `action` reference).
                SourceRange ar = action.range;
                std::shared_ptr<Stmt> abody = action.body;
                visitStmt(*abody);
                checkMissingReturn(ar, *abody);
                symbol_table_.leaveScope();
                if (named_lambda) {
                    lambda_self_name_.clear();
                    lambda_self_class_.clear();
                }
                in_coro_method_ = false;
                current_method_name_.clear();
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
                checkParamDefaults(func.params);
                SourceRange fr = func.range;
                std::shared_ptr<BlockStmt> fbody = func.body;
                visitStmt(*fbody);
                checkMissingReturn(fr, *fbody);
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
            checkParamDefaults(action.params);
            SourceRange sar = action.range;
            std::shared_ptr<Stmt> sabody = action.body;
            visitStmt(*sabody);
            checkMissingReturn(sar, *sabody);
            symbol_table_.leaveScope();
            in_coro_method_ = false;
        }
        in_class_method_ = false;
        current_class_member_types_.clear();
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
        populateFuncTypeMeta(ft, fn.params);
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

            SourceRange sr = func.range;
            std::shared_ptr<BlockStmt> sbody = func.body;
            visitStmt(*sbody);
            checkMissingReturn(sr, *sbody);

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

    // Register all ENUM types FIRST: visitStructDecl validates field types, so
    // a struct field referencing an enum (`struct Box { Color c; }`) previously
    // failed with "unknown type 'Color'" because structs were registered before
    // enums. Enums reference no other types, so ordering them first is safe.
    for (auto& en : tu.enums) {
        visitEnumDecl(en);
    }
    // Phase A: declare ALL struct names (top-level + nested) before any field
    // validation, so `struct Holder { Item it; }` resolving `Item` (declared
    // later in the merged TU) works regardless of declaration order.
    for (auto& st : tu.structs) {
        declareStructName(st);
    }
    for (auto& cls : tu.classes) {
        for (auto& st : cls.structs) {
            declareStructName(st);
        }
    }
    // Phase B: validate fields + register methods (order-independent now)
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

    // All structs registered → detect by-value recursion (infinite size) and
    // report clean diagnostics instead of a cryptic codegen failure.
    detectStructRecursion();

    for (auto& ff : tu.ffis) {
        visitFFI(ff);
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

void Sema::declareStructName(StructDecl& decl) {
    std::string type_key = decl.parent_class.empty()
        ? decl.name
        : decl.parent_class + "::" + decl.name;
    // Phase A already declared this exact struct → idempotent return. A genuine
    // duplicate (a *different* decl with the same name) still hits the symbol
    // table lookup below and reports an error.
    if (declared_struct_names_.count(type_key)) return;
    if (symbol_table_.lookup(type_key)) {
        error(decl.range, "duplicate struct name '" + type_key + "'");
        return;
    }
    TypeInfo struct_type(TypeKind::Struct);
    struct_type.class_name = type_key;
    symbol_table_.declare(type_key, struct_type);
    declared_struct_names_.insert(type_key);
}

void Sema::visitStructDecl(StructDecl& decl) {
    declareStructName(decl);
    std::string type_key = decl.parent_class.empty()
        ? decl.name
        : decl.parent_class + "::" + decl.name;

    // Validate struct fields: duplicate names and void-typed fields are rejected.
    for (size_t i = 0; i < decl.properties.size(); ++i) {
        auto& prop = decl.properties[i];
        TypeInfo ft = typeNodeToTypeInfo(prop.type);
        if (ft.kind == TypeKind::Void && prop.type.class_name.empty()) {
            error(prop.range, "cannot declare field of type 'void'");
        }
        // Record by-value struct embedding (struct fields are always by value
        // in MYP) for recursive/infinite-size detection. Self-embedding
        // (S { S next; }) and mutual cycles (A { B b; } B { A a; }) are caught
        // later in detectStructRecursion(). Fixed-size array fields (T[N]) are
        // also inline by value — S { S[3] arr; } is equally infinite and would
        // otherwise silently produce a broken struct layout.
        if (ft.kind == TypeKind::Struct && !ft.class_name.empty()) {
            struct_byval_edges_[type_key].push_back(ft.class_name);
            struct_decl_ranges_[type_key] = decl.range;
        } else if (ft.kind == TypeKind::Array && ft.array_size > 0) {
            const TypeInfo* et = ft.element_type.get();
            while (et && et->kind == TypeKind::Array) et = et->element_type.get();
            if (et && et->kind == TypeKind::Struct && !et->class_name.empty()) {
                struct_byval_edges_[type_key].push_back(et->class_name);
                struct_decl_ranges_[type_key] = decl.range;
            }
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
        populateFuncTypeMeta(func_type, func.params);
        std::string method_name = type_key + "::" + func.name;
        if (symbol_table_.lookup(method_name)) {
            error(func.range, "duplicate method '" + func.name +
                  "' in struct '" + type_key + "'");
        }
        symbol_table_.declare(method_name, func_type);
    }
}

void Sema::detectStructRecursion() {
    // Detect by-value struct cycles → infinitely-sized structs. Any cycle in
    // the by-value embedding graph is an error (C would say "field has
    // incomplete type"). Without this, `struct S { S next; }` passes sema and
    // fails later in codegen with a cryptic "Code generation failed".
    std::unordered_map<std::string, int> color;   // 0=white 1=gray 2=black
    std::function<void(const std::string&)> dfs =
        [&](const std::string& n) {
            color[n] = 1;
            for (auto& f : struct_byval_edges_[n]) {
                auto it = color.find(f);
                if (it == color.end() || it->second == 0) {
                    dfs(f);
                } else if (it->second == 1) {
                    // back-edge → cycle; report once on the member the edge
                    // points back to
                    auto r = struct_decl_ranges_.find(f);
                    if (r != struct_decl_ranges_.end())
                        error(r->second,
                              "recursive struct definition (infinite size): '" +
                              f + "' is embedded by value in a cycle");
                }
            }
            color[n] = 2;
        };
    for (auto& e : struct_byval_edges_)
        if (color[e.first] == 0)
            dfs(e.first);
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
        populateFuncTypeMeta(func_type, action.params);
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
        populateFuncTypeMeta(func_type, fn.params);
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
        populateFuncTypeMeta(func_type, action.params);
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
    populateFuncTypeMeta(func_type, decl.params);
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
    checkParamDefaults(decl.params);

    // In main(), only instance creation + mapping is allowed — no direct method calls
    // (编译器注入的合成 main 豁免：它本就是要直接触发 @startup 入口)
    if (decl.name == "main" && !decl.is_auto_main) in_main_function_ = true;

    in_coro_method_ = decl.has_coro;  // top-level @coro function: allow await

    if (decl.body) {
        // Capture BEFORE visitStmt: monomorphization may reallocate
        // tu.functions (dangling the decl reference).
        SourceRange dr = decl.range;
        std::shared_ptr<BlockStmt> body = decl.body;
        visitStmt(*body);
        // @coro/@async functions: non-void ones must still return a value (the
        // source return stores into the coroutine result slot).
        checkMissingReturn(dr, *body);
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
                        // `_` 忽略符：跳过该元素——不声明/不赋值、不校验类型、仍消耗一个槽位
                        if (t.name == "_") { idx++; return; }
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
                // Mark a statement-level CALL as having its result discarded
                // (e.g. `deep(n-1);`) — @coro self-calls in this form spawn a
                // chain and are allowed (tests/coro_stack). Nested value uses
                // (args/assignment/arithmetic) are NOT discarded and still get
                // the recursive-@coro diagnostic.
                bool was_discard = in_discarded_stmt_expr_;
                if (es.expression->kind == ExprKind::Call)
                    in_discarded_stmt_expr_ = true;
                visitExpr(*es.expression);
                in_discarded_stmt_expr_ = was_discard;
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

    // Track @thread-annotated instances so manual calls to their @startup
    // method are rejected at compile time (auto-invoked in worker thread).
    if (decl.has_thread_annotation)
        thread_annotated_vars_.insert(decl.name);
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
// 注解逻辑抽成独立函数：visitForInStmt（正常访问）与泛型单态化重注解共用
// （§四-2 × 泛型：泛型模板体在 sema 被跳过，ForInStmt 注解从未计算 → 单态化时
// 用具体参数类型重注解共享 body，否则 codegen 读到默认值 → void 循环变量崩溃）。
bool Sema::annotateForInStmt(ForInStmt& stmt, const TypeInfo& it_type) {
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
    return valid;
}

// 递归遍历共享泛型 body，为每个 ForInStmt 用具体参数类型重注解（iterable 为
// 参数标识符的常见情形）。非参数 iterable（字段/方法调用）暂不处理（codegen 有守卫）。
void Sema::annotateForInsInStmt(Stmt& s, const std::vector<ParamDecl>& params) {
    switch (s.kind) {
        case StmtKind::ForInStmt: {
            auto& fis = static_cast<ForInStmt&>(s);
            if (fis.iterable && fis.iterable->kind == ExprKind::Identifier) {
                auto& id = static_cast<IdentifierExpr&>(*fis.iterable);
                for (auto& p : params) {
                    if (p.name == id.name) {
                        TypeInfo it = typeNodeToTypeInfo(p.type);
                        annotateForInStmt(fis, it);
                        break;
                    }
                }
            }
            break;
        }
        case StmtKind::Block: {
            auto& b = static_cast<BlockStmt&>(s);
            for (auto& st : b.statements) annotateForInsInStmt(*st, params);
            break;
        }
        case StmtKind::IfStmt: {
            auto& is = static_cast<IfStmt&>(s);
            if (is.then_block) annotateForInsInStmt(*is.then_block, params);
            if (is.else_block) annotateForInsInStmt(*is.else_block, params);
            break;
        }
        case StmtKind::WhileStmt: {
            auto& w = static_cast<WhileStmt&>(s);
            if (w.body) annotateForInsInStmt(*w.body, params);
            break;
        }
        case StmtKind::ForStmt: {
            auto& f = static_cast<ForStmt&>(s);
            if (f.body) annotateForInsInStmt(*f.body, params);
            break;
        }
        case StmtKind::TryStmt: {
            auto& t = static_cast<TryStmt&>(s);
            if (t.try_block) annotateForInsInStmt(*t.try_block, params);
            for (auto& c : t.catches)
                if (c.block) annotateForInsInStmt(*c.block, params);
            if (t.finally_block) annotateForInsInStmt(*t.finally_block, params);
            break;
        }
        case StmtKind::MatchStmt: {
            auto& m = static_cast<MatchStmt&>(s);
            for (auto& arm : m.arms)
                if (arm.body) annotateForInsInStmt(*arm.body, params);
            break;
        }
        default: break;
    }
}

Sema::StmtResult Sema::visitForInStmt(ForInStmt& stmt) {
    auto it_type = visitExpr(*stmt.iterable);
    bool valid = annotateForInStmt(stmt, it_type);

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

// Conservative "control never falls off the end" analysis. Used to flag non-void
// functions missing a return at the end (previously codegen emitted `ret void`
// in an i32 function → LLVM verify failure).
bool Sema::stmtGuaranteesTermination(const Stmt& s) const {
    switch (s.kind) {
        case StmtKind::ReturnStmt:
        case StmtKind::ThrowStmt:
            return true;
        case StmtKind::Block: {
            auto& b = static_cast<const BlockStmt&>(s);
            if (b.statements.empty()) return false;
            return stmtGuaranteesTermination(*b.statements.back());
        }
        case StmtKind::IfStmt: {
            auto& i = static_cast<const IfStmt&>(s);
            if (!i.else_block) return false;
            bool t = i.then_block && stmtGuaranteesTermination(*i.then_block);
            bool f = stmtGuaranteesTermination(*i.else_block);
            return t && f;
        }
        case StmtKind::MatchStmt: {
            auto& m = static_cast<const MatchStmt&>(s);
            if (m.arms.empty()) return false;
            for (auto& a : m.arms)
                if (!(a.body && stmtGuaranteesTermination(*a.body))) return false;
            return true;
        }
        case StmtKind::TryStmt: {
            auto& t = static_cast<const TryStmt&>(s);
            // If the try block itself returns, control never falls off (the
            // finally only runs on the return path). If the try falls through,
            // a terminating finally prevents fall-off (its return covers the
            // fall-through path). A terminating catch alone is not enough: the
            // non-throw path (try falls through) would still reach the end.
            if (t.try_block && stmtGuaranteesTermination(*t.try_block)) return true;
            if (t.finally_block && stmtGuaranteesTermination(*t.finally_block))
                return true;
            return false;
        }
        case StmtKind::WhileStmt: {
            // `while (true)` / `while (1)` never falls through.
            auto& w = static_cast<const WhileStmt&>(s);
            if (w.condition) {
                if (w.condition->kind == ExprKind::BoolLiteral)
                    return static_cast<const BoolLiteralExpr&>(*w.condition).value;
                if (w.condition->kind == ExprKind::IntegerLiteral)
                    return static_cast<const IntegerLiteralExpr&>(*w.condition).value != 0;
            }
            return false;
        }
        case StmtKind::ForStmt: {
            // `for (;;)` (no condition) never falls through.
            auto& f = static_cast<const ForStmt&>(s);
            return f.condition == nullptr;
        }
        default:
            return false;
    }
}

void Sema::checkMissingReturn(const SourceRange& range, const Stmt& body) {
    if (current_return_type_.kind != TypeKind::Void &&
        !stmtGuaranteesTermination(body)) {
        // FFI/extern stubs: `long f(...) {}` with an empty body (e.g. the Coro.*
        // / runtime-backed functions) legitimately have no return statement.
        if (body.kind == StmtKind::Block &&
            static_cast<const BlockStmt&>(body).statements.empty())
            return;
        error(range, "missing return statement (function expects '" +
                     typeName(current_return_type_) + "')");
    }
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











// 显式类型转换：uint8(x) / long(x) / double(x)。要求操作数是数值类型；
// 转换规则（宽→窄截断、窄→宽按源符号、int↔float）在 codegen 实现。

// Monomorphize a generic function call (explicit `foo<int>(...)` or inferred
// `foo(x)`), append the instance to tu.functions, and type-check the call.

// Monomorphize a generic static method call (StaticClass.method<T>(...) or
// inferred), append the instance to tu.functions (as a top-level FuncDecl with
// a unique mangled name), and type-check the call. Static methods have no
// `this`, so the instance body is shared and codegen resolves T per-instance
// exactly like a generic function.

// Build a TypeNode that typeNodeToTypeInfo would resolve back to `t`.

// ==============================
// §四-1 默认参数 / 命名实参
// ==============================

// 深克隆表达式（默认值按调用点复制；不支持克隆的复杂表达式返回 nullptr）。


// 实参是否为命名实参形态：`AssignmentExpr(Identifier name = value)` 且 name 是形参名。
// parser 把 `f(name = value)` 按赋值表达式解析（为兼容宏的赋值实参 `$n/$body` 形态）；
// 此处按「目标标识符匹配形参名」重解释为命名实参 → 与宏实参无歧义。

// 计算「形参 i ← 实参 j」映射（-1 = 缺省）。不修改实参；不匹配返回 nullopt。
// 供构造器候选按形参类型打分发。（0 参候选返回空向量——合法，勿当失败。）

// 校验实参是否适配形参（不修改、不发错）：位置 ≤ 总数、命名名合法且不重复、
// 位置/命名不重叠、必填形参全提供。供构造器候选筛选用。

// 把实参规范化为与形参一一对应的有序列表（见 fitsArgsToParamDecls 的校验规则）。
// 成功后 args 重建为完整有序列表（NamedArgExpr 已被替换为值）；失败已发错。

// 从 Function TypeInfo 的参数元数据构造 ParamDecl 列表并委托核心规范化。

// 声明期校验每个带默认值的形参：默认表达式类型须与形参类型兼容。
// （调用点克隆默认值到实参后也会被 visitCall 类型检查；此处保证从未被省略
//  调用的默认值也被检查。）












// True for Byte/Short/Int/Long/Float/Double (and unsigned/char variants).
bool Sema::isNumericKind(TypeKind k) const {
    switch (k) {
        case TypeKind::Byte: case TypeKind::Short: case TypeKind::Int:
        case TypeKind::Long: case TypeKind::UByte: case TypeKind::UShort:
        case TypeKind::UInt: case TypeKind::ULong: case TypeKind::Char:
        case TypeKind::Float: case TypeKind::Double:
            return true;
        default: return false;
    }
}

// uint32 族：源类型是否为无符号整数（UByte/UShort/UInt/ULong）。
bool Sema::isUnsignedKind(TypeKind k) const {
    return k == TypeKind::UByte || k == TypeKind::UShort ||
           k == TypeKind::UInt || k == TypeKind::ULong;
}

// Wider of two numeric kinds; Void if not promotable either way.
TypeKind Sema::commonNumericKind(TypeKind a, TypeKind b) const {
    if (a == b) return a;
    if (typesCompatible(TypeInfo(a), TypeInfo(b))) return a;  // b promotes to a
    if (typesCompatible(TypeInfo(b), TypeInfo(a))) return b;  // a promotes to b
    return TypeKind::Void;
}


// ==============================
// Type utilities
// ==============================





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



// 标识符是否为全局函数/泛型函数/类/接口/struct/枚举名。lambda 捕获分析据此跳过
// 这些名字（它们不是外层局部变量，运行时始终可解析）。§五-3

// §五-5: is `callee` an @async-annotated top-level function or class
// (action:/static:) method? Used to reject @async calls outside @coro contexts.





// Pipe: lhs |> op — apply an operator component (class with a `transform`
// method) to lhs. target_kind="class" instantiates a fresh component;
// "instance" reuses an operator variable. Left-assoc via chained PipeExpr.

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

    // §五-4 RTTI intrinsics (stdlib Rtti wraps them; param = any class object)
    add_intrinsic("__myp_type_id", TypeKind::Int, {TypeKind::Class});
    add_intrinsic("__myp_type_name", TypeKind::String, {TypeKind::Class});

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
        // cascading error recovery: only when a prior error already occurred
        // (e.g. the thrown expr itself failed). A genuinely void expression
        // (`throw Console.writeString(...)`) must be rejected — codegen would
        // otherwise emit myp_throw_object(void <badref>) → LLVM verify.
        if (!diag_.hasErrors())
            error(stmt.range, "throw requires a string or class instance, got 'void'");
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

