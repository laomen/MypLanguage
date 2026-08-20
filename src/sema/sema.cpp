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

ClassDecl* Sema::findClassDecl(const std::string& name) {
    if (!current_tu_) return nullptr;
    auto it = class_indices_.find(name);
    if (it == class_indices_.end() || it->second >= current_tu_->classes.size())
        return nullptr;
    return &current_tu_->classes[it->second];
}

void Sema::buildCurrentClassMemberTypes(TranslationUnit& tu, size_t ci) {
    // ⚠ 悬垂引用（UAF）：解析泛型属性/返回类型时，typeNodeToTypeInfo 可能触发
    // monomorphization（current_tu_->classes.push_back → vector 重分配），使外部
    // 传入的 tu.classes[ci] 引用失效。故改为按下标循环，每轮重取 tu.classes[ci]，
    // 且 name/type 先拷到局部——绝不跨 typeNodeToTypeInfo 调用持有类内引用。
    current_class_member_types_.clear();

    size_t nprops = tu.classes[ci].properties.size();
    for (size_t pi = 0; pi < nprops; pi++) {
        std::string pname = tu.classes[ci].properties[pi].name;
        TypeNode ptype = tu.classes[ci].properties[pi].type;
        current_class_member_types_.emplace(std::move(pname), typeNodeToTypeInfo(ptype));
    }

    size_t nactions = tu.classes[ci].actions.size();
    for (size_t ai = 0; ai < nactions; ai++) {
        TypeInfo func_type(TypeKind::Function);
        TypeNode rtype = tu.classes[ci].actions[ai].return_type;
        func_type.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(rtype));
        size_t nparams = tu.classes[ci].actions[ai].params.size();
        for (size_t pi = 0; pi < nparams; pi++) {
            TypeNode ptype = tu.classes[ci].actions[ai].params[pi].type;
            func_type.param_types.push_back(typeNodeToTypeInfo(ptype));
        }
        populateFuncTypeMeta(func_type, tu.classes[ci].actions[ai].params);
        current_class_member_types_.emplace(tu.classes[ci].actions[ai].name, std::move(func_type));
    }

    size_t nfuncs = tu.classes[ci].functions.size();
    for (size_t fi = 0; fi < nfuncs; fi++) {
        TypeInfo func_type(TypeKind::Function);
        TypeNode rtype = tu.classes[ci].functions[fi].return_type;
        func_type.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(rtype));
        size_t nparams = tu.classes[ci].functions[fi].params.size();
        for (size_t pi = 0; pi < nparams; pi++) {
            TypeNode ptype = tu.classes[ci].functions[fi].params[pi].type;
            func_type.param_types.push_back(typeNodeToTypeInfo(ptype));
        }
        populateFuncTypeMeta(func_type, tu.classes[ci].functions[fi].params);
        current_class_member_types_.emplace(tu.classes[ci].functions[fi].name, std::move(func_type));
    }

    size_t nevts = tu.classes[ci].events.size();
    for (size_t ei = 0; ei < nevts; ei++) {
        TypeInfo event_type(TypeKind::Function);
        event_type.return_type = std::make_shared<TypeInfo>(TypeKind::Void);
        size_t nparams = tu.classes[ci].events[ei].params.size();
        for (size_t pi = 0; pi < nparams; pi++) {
            TypeNode ptype = tu.classes[ci].events[ei].params[pi].type;
            event_type.param_types.push_back(typeNodeToTypeInfo(ptype));
            event_type.param_is_ref.push_back(false);
        }
        current_class_member_types_.emplace(tu.classes[ci].events[ei].name, std::move(event_type));
    }
}

bool Sema::analyze(TranslationUnit& tu) {
    current_tu_ = &tu;
    current_class_member_types_.clear();
    class_indices_.clear();
    struct_member_types_.clear();
    for (size_t i = 0; i < tu.classes.size(); i++)
        class_indices_.emplace(tu.classes[i].name, i);
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
        // M-FN-2 nonlocal：按索引重取（visitFuncBody 内单态化可能重分配），赋捕获集。
        if (fi < tu.functions.size()) {
            tu.functions[fi].nonlocal_captures = last_func_nonlocal_vars_;
            tu.functions[fi].nonlocal_cell_class = last_func_nonlocal_cell_class_;
        }
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
        buildCurrentClassMemberTypes(tu, ci);
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
                    // M-FN-2 nonlocal：把 nonlocal 变量按其 cell 属性类型声明进
                    // __call 作用域（body 引用解析；实际存储由 codegen 注入别名）。
                    for (auto& ns : tu.classes[ci].nonlocal_slots) {
                        for (auto& c : tu.classes) {
                            if (c.name != ns.cell_class) continue;
                            if (!c.properties.empty())
                                symbol_table_.declare(ns.var, typeNodeToTypeInfo(c.properties[0].type));
                            break;
                        }
                    }
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
                current_func_nonlocal_vars_.clear();   // M-FN-2 nonlocal accumulator
                current_func_nonlocal_cell_class_.clear();
                if (named_lambda) in_lambda_body_++;
                visitStmt(*abody);
                if (named_lambda) in_lambda_body_--;
                // `action` 引用在 visitStmt 后可能悬垂（单态化重分配 tu.classes）→
                // 先拷成员，再按索引重取赋值。
                last_func_nonlocal_vars_ = current_func_nonlocal_vars_;
                last_func_nonlocal_cell_class_ = current_func_nonlocal_cell_class_;
                if (ci < tu.classes.size() && ai < tu.classes[ci].actions.size()) {
                    tu.classes[ci].actions[ai].nonlocal_captures = last_func_nonlocal_vars_;
                    tu.classes[ci].actions[ai].nonlocal_cell_class = last_func_nonlocal_cell_class_;
                }
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
                current_func_nonlocal_vars_.clear();
                current_func_nonlocal_cell_class_.clear();
                visitStmt(*fbody);
                last_func_nonlocal_vars_ = current_func_nonlocal_vars_;
                last_func_nonlocal_cell_class_ = current_func_nonlocal_cell_class_;
                if (ci < tu.classes.size() && fi < tu.classes[ci].functions.size()) {
                    tu.classes[ci].functions[fi].nonlocal_captures = last_func_nonlocal_vars_;
                    tu.classes[ci].functions[fi].nonlocal_cell_class = last_func_nonlocal_cell_class_;
                }
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
            current_func_nonlocal_vars_.clear();
            current_func_nonlocal_cell_class_.clear();
            visitStmt(*sabody);
            last_func_nonlocal_vars_ = current_func_nonlocal_vars_;
            last_func_nonlocal_cell_class_ = current_func_nonlocal_cell_class_;
            if (ci < tu.classes.size() && si < tu.classes[ci].static_actions.size()) {
                tu.classes[ci].static_actions[si].nonlocal_captures = last_func_nonlocal_vars_;
                tu.classes[ci].static_actions[si].nonlocal_cell_class = last_func_nonlocal_cell_class_;
            }
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
            current_func_nonlocal_vars_.clear();
            visitStmt(*sbody);
            // 非 local 不支持 struct 方法内 lambda（visitLambda 已报错）；清空防泄漏。
            current_func_nonlocal_vars_.clear();
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
        // BUG-008: 参数列表精确匹配（个数 + 类型）。此前只按名称 + 返回类型 basic_type
        // 核对，`double area(int)` 实现 `double area(int,int)` 也编译通过。关联类型
        // 场景仍只按名称（绑定类型在实现类）。
        auto paramsMatch = [&](const ActionDecl& ca, const ActionDecl& ia) {
            if (ca.params.size() != ia.params.size()) return false;
            for (size_t pi = 0; pi < ca.params.size(); pi++) {
                const TypeNode& cpt = ca.params[pi].type;
                const TypeNode& ipt = ia.params[pi].type;
                if (cpt.basic_type != ipt.basic_type) return false;
                if (cpt.isArray() != ipt.isArray()) return false;
                if (!cpt.class_name.empty() || !ipt.class_name.empty())
                    if (cpt.class_name != ipt.class_name) return false;
            }
            return true;
        };
        auto matches = [&](const ActionDecl& ca) {
            if (ca.name != ia.name) return false;
            if (iface_uses_assoc) return true;
            if (ca.return_type.basic_type != ia.return_type.basic_type) return false;
            return paramsMatch(ca, ia);
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
    // Register generic class templates BEFORE struct field validation (BUG-004):
    // a struct field typed as a generic instantiation (`Option<Node> next`) must
    // monomorphize while the field type is resolved; otherwise it resolves to the
    // un-instantiated template name and later assignments/member access fail
    // type checks ("Option_Node_inst vs Option").
    for (size_t i = 0; i < tu.classes.size(); i++) {
        if (!tu.classes[i].type_params.empty()) {
            GenericInfo info;
            info.tu_index = i;
            generic_classes_[tu.classes[i].name] = info;
        }
    }
    // Register generic function templates (same reason: struct fields may carry
    // generic function types).
    for (size_t i = 0; i < tu.functions.size(); i++) {
        if (!tu.functions[i].type_params.empty()) {
            GenericFuncInfo info;
            info.tu_index = i;
            generic_functions_[tu.functions[i].name] = info;
        }
    }
    // Phase B: validate fields + register methods (order-independent now).
    // ⚠ 解析字段类型可能触发 monomorphization（tu.classes push_back 重分配）——
    // 此处 range-for 遍历的是 tu.structs（不受影响），实例追加只动 tu.classes。
    for (auto& st : tu.structs) {
        visitStructDecl(st);
    }
    // §5.1 bitfield：先声明名字（可互相引用/被引用），再登记字段布局。
    for (auto& bf : tu.bitfields) {
        declareBitfieldName(bf);
    }
    for (auto& bf : tu.bitfields) {
        visitBitfieldDecl(bf);
    }
    // Also register nested structs inside classes
    // ⚠ visitStructDecl 解析字段类型可能触发 monomorphization（tu.classes 重分配），
    // range-for 缓存迭代器会悬垂（UAF）→ 按下标循环每轮重取，只访问起始时的类。
    size_t nc_structs = tu.classes.size();
    for (size_t i = 0; i < nc_structs; i++) {
        size_t ns = tu.classes[i].structs.size();
        for (size_t j = 0; j < ns; j++) {
            visitStructDecl(tu.classes[i].structs[j]);
        }
    }

    // All structs registered → detect by-value recursion (infinite size) and
    // report clean diagnostics instead of a cryptic codegen failure.
    detectStructRecursion();

    for (auto& ff : tu.ffis) {
        visitFFI(ff);
    }
    // ⚠ visitClassDecl 解析泛型成员类型会触发 monomorphization（tu.classes
    // push_back → 重分配），range-for 缓存迭代器悬垂 → 读已释放内存（UAF，
    // 表现为误报 duplicate class name ''）。按下标循环每轮重取 tu.classes[i]，
    // 且只访问循环开始时的类（新增泛型实例由 analyze 的索引循环另行处理）。
    // 泛型实例在单态化时已 visitClassDecl（声明名字+注册成员），此处跳过——
    // 否则 struct 字段触发的实例会在此被二次 visitClassDecl → duplicate class。
    size_t nc = tu.classes.size();
    for (size_t i = 0; i < nc; i++) {
        if (tu.classes[i].is_generic_inst) continue;
        visitClassDecl(tu, i);
    }
    // Check interface implementations
    size_t nc_if = tu.classes.size();
    for (size_t i = 0; i < nc_if; i++) {
        if (!tu.classes[i].interface_class_name.empty()) {
            checkInterfaceImpl(tu.classes[i]);
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

// §5.1 bitfield：声明类型名（如 Flags），供变量/参数/字段引用。
void Sema::declareBitfieldName(BitfieldDecl& decl) {
    if (symbol_table_.lookup(decl.name)) {
        error(decl.range, "duplicate bitfield name '" + decl.name + "'");
        return;
    }
    TypeInfo bf_type(TypeKind::Bitfield);
    bf_type.class_name = decl.name;
    bf_type.bitfield_bits = decl.total_bits;
    symbol_table_.declare(decl.name, bf_type);
}

// §5.1 bitfield：登记字段布局（offset/width）与成员类型（bit→Bit，bit[N]→UInt）。
void Sema::visitBitfieldDecl(BitfieldDecl& decl) {
    bitfield_bits_[decl.name] = decl.total_bits;
    auto& layout = bitfield_layout_[decl.name];
    auto& mtypes = bitfield_member_types_[decl.name];
    layout.clear();
    mtypes.clear();
    for (auto& f : decl.fields) {
        if (layout.count(f.name)) {
            error(f.range, "duplicate bitfield field '" + f.name + "' in '" +
                  decl.name + "'");
            continue;
        }
        BitfieldFieldInfo info;
        info.offset = f.offset;
        info.width = f.bit_width;
        layout.emplace(f.name, info);
        mtypes.emplace(f.name,
            TypeInfo(f.bit_width == 1 ? TypeKind::Bit : TypeKind::UInt));
    }
}

void Sema::visitStructDecl(StructDecl& decl) {
    declareStructName(decl);
    std::string type_key = decl.parent_class.empty()
        ? decl.name
        : decl.parent_class + "::" + decl.name;
    auto& member_types = struct_member_types_[type_key];
    member_types.clear();

    // Validate struct fields: duplicate names and void-typed fields are rejected.
    for (size_t i = 0; i < decl.properties.size(); ++i) {
        auto& prop = decl.properties[i];
        TypeInfo ft = typeNodeToTypeInfo(prop.type);
        member_types.emplace(prop.name, ft);
        if (prop.weak)
            error(prop.range, "@weak is not supported on struct fields yet");
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
        TypeInfo member_type(TypeKind::Function);
        member_type.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(func.return_type));
        for (auto& param : func.params)
            member_type.param_types.push_back(typeNodeToTypeInfo(param.type));
        member_types.emplace(func.name, member_type);

        TypeInfo func_type = member_type;
        for (auto& param : func.params)
            func_type.param_is_ref.push_back(param.is_ref);
        populateFuncTypeMeta(func_type, func.params);

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
        for (size_t i = 0; i < func.params.size(); i++) {
            auto& param = func.params[i];
            const TypeInfo& pt = func_type.param_types[i];
            if (pt.kind == TypeKind::Void && param.type.class_name.empty()) {
                error(param.range, "cannot declare parameter of type 'void'");
            }
        }
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

void Sema::visitClassDecl(TranslationUnit& tu, size_t ci) {
    // ⚠ typeNodeToTypeInfo 解析成员类型可能触发 monomorphization（tu.classes
    // push_back → 重分配），持有 tu.classes[ci] 成员引用跨该调用会悬垂（UAF）。
    // 故先快照本类全部成员（properties 字段级拷贝——PropertyDecl move-only；
    // actions/functions/events 整段拷贝），再在快照上处理。
    const std::string cls_name = tu.classes[ci].name;
    const bool cls_is_inst = tu.classes[ci].is_generic_inst;
    const SourceRange cls_range = tu.classes[ci].range;

    struct PropSnap {
        std::string name;
        TypeNode type;
        SourceRange range;
        bool weak = false;
        bool is_const = false;
    };
    std::vector<PropSnap> props;
    props.reserve(tu.classes[ci].properties.size());
    for (auto& p : tu.classes[ci].properties)
        props.push_back(PropSnap{p.name, p.type, p.range, p.weak, p.is_const});
    std::vector<ActionDecl> actions = tu.classes[ci].actions;
    std::vector<ActionDecl> static_actions = tu.classes[ci].static_actions;
    std::vector<FuncDecl> functions = tu.classes[ci].functions;
    std::vector<EventDecl> events = tu.classes[ci].events;

    // BUG-021: 本函数（含泛型实例化触发的 visitClassDecl）会在下方把
    // current_class_name_ 设为 cls_name。若调用方正处于外层类的方法体检查中
    // （如 H 的 action 内访问 this.v，而 H 含 `Option<int> o` 泛型属性 → 成员类型
    // 解析触发 Option<int> 实例化 → 进入本函数），退出后 current_class_name_ 残留为
    // 实例类名 → 外层 this 解析到 Option_int_inst → `class 'Option_int_inst' has no
    // member 'v'`。保存并恢复，杜绝类上下文污染。
    const std::string saved_current_class = current_class_name_;

    if (symbol_table_.lookup(cls_name)) {
        error(cls_range, "duplicate class name '" + cls_name + "'");
        return;
    }

    TypeInfo class_type(TypeKind::Class);
    class_type.class_name = cls_name;
    symbol_table_.declare(cls_name, class_type);

    // Set current class name for member type resolution
    current_class_name_ = cls_name;

    // Enter class scope to register members
    symbol_table_.enterScope();

    // Register generic type parameters as valid types within the CLASS scope.
    // ⚠ BUG-018: 必须在 enterScope 之后声明——此前在类作用域之外（全局）声明，类
    // 作用域弹出后 T 仍留在全局符号表；后续另一个用同名类型参数（如 Set<T> 后
    // Processor<T>）的泛型类会把全局 T 覆盖成自己的绑定（如 Container 接口）。随后
    // 检查 Set<T> 模板体时 T 解析为 Container → `val % cap_`/`data_[i] < x` 报
    // `expected numeric type, got 'Container'`（8 个伪错误，行号落在 stdlib）。
    for (auto& tp : tu.classes[ci].type_params) {
        TypeInfo tp_type(TypeKind::Int);
        // 约束类型参数（where T : I，§三-5）→ 注册为接口类型，使模板体内
        // T 上的方法调用 / T::Item 可静态检查（运行时单态化到具体类）。
        auto cit = tu.classes[ci].type_param_constraints.find(tp);
        if (cit != tu.classes[ci].type_param_constraints.end()) {
            tp_type = TypeInfo(TypeKind::Interface);
            tp_type.class_name = cit->second;
        }
        symbol_table_.declare(tp, tp_type);
    }

    for (auto& prop : props) {
        // M7: @weak only on class/interface reference fields (not string/
        // slice/numeric/struct), and not const.
        if (prop.weak) {
            TypeInfo pti = typeNodeToTypeInfo(prop.type);
            bool ref_ok = (pti.kind == TypeKind::Class) ||
                          (pti.kind == TypeKind::Interface);
            if (!ref_ok)
                error(prop.range, "@weak property '" + prop.name +
                      "' must be a class or interface reference type");
            if (prop.is_const)
                error(prop.range, "@weak property '" + prop.name +
                      "' cannot also be const");
        }
        if (!symbol_table_.declare(prop.name, typeNodeToTypeInfo(prop.type))) {
            error(prop.range, "duplicate member '" + prop.name + "' in class '" + cls_name + "'");
        }
    }

    // BUG-009: 一个类最多一个 @startup——@thread 线程入口取**最后一个**、`mypc run`
    // 合成 main 取**第一个**，多个 @startup 行为不一致（手写 main+@thread 输出 SECOND、
    // mypc run 输出 FIRST）。诊断「一类一个 @startup」（design §6.5 建议）。
    {
        std::string first_startup;
        for (auto& action : actions) {
            if (!action.has_startup) continue;
            if (first_startup.empty()) {
                first_startup = action.name;
            } else {
                error(action.range, "class '" + cls_name + "' declares multiple '@startup' "
                      "actions ('" + first_startup + "' and '" + action.name +
                      "') — at most one @startup per class");
            }
        }
    }

    for (auto& action : actions) {
        // 构造器：不注册为可调用 action（构造器不能直接调用，同名重载合法）；
        // M2 负责 new 绑定。body 仍在独立 pass 中带类作用域检查。
        if (action.has_constructor) {
            if (action.has_startup) {
                error(action.range, "cannot be both @constructor and @startup");
            }
            if (action.name != cls_name && !cls_is_inst) {
                error(action.range, "constructor name '" + action.name +
                      "' must match class name '" + cls_name + "'");
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
            error(action.range, "duplicate action '" + action.name + "' in class '" + cls_name + "'");
        }
    }

    // Register function: section methods in class scope (callable by bare name
    // from actions and other function: methods regardless of declaration order).
    // Previously only resolved via the in_class_method_ fallback, which missed
    // methods declared later in the section.
    for (auto& fn : functions) {
        // 构造器：不注册为可调用 function（构造器不能直接调用，同名重载合法）；
        // M2 负责 new 绑定。body 仍在独立 pass 中带类作用域检查。
        if (fn.has_constructor) {
            if (fn.name != cls_name && !cls_is_inst) {
                error(fn.range, "constructor name '" + fn.name +
                      "' must match class name '" + cls_name + "'");
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
            error(fn.range, "duplicate function '" + fn.name + "' in class '" + cls_name + "'");
        }
    }

    // Register static actions in GLOBAL scope (accessible as ClassName.method)
    for (size_t sa_idx = 0; sa_idx < static_actions.size(); sa_idx++) {
        auto& action = static_actions[sa_idx];
        // Generic static method: List.map<T,U>(...) — register for call resolution
        // (monomorphization happens at the call site; the template isn't callable
        // directly since its type params are placeholders).
        if (!action.type_params.empty()) {
            std::string gkey = cls_name + "::" + action.name;
            GenericStaticMethodInfo ginfo;
            // Find the class index (may be a generic instance clone; use the
            // original template's index for stable registration).
            for (size_t cj = 0; cj < current_tu_->classes.size(); cj++) {
                if (current_tu_->classes[cj].name == cls_name &&
                    !current_tu_->classes[cj].is_generic_inst) {
                    ginfo.class_index = (int)cj;
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
        std::string static_name = cls_name + "." + action.name;
        // BUG-046：同名 static 方法——**签名不同**才报错（此前无条件忽略 declare
        // 返回值 → 同名不同签名静默注册 → codegen 用同一 LLVM 函数生成不同签名 body
        // → func->getArg() out of range 崩溃）。签名相同（如 cuda.myp Vectors 的
        // CPU/GPU 版 max/min）保持历史静默合并行为（不重复注册）。
        if (const TypeInfo* existing = symbol_table_.lookup(static_name)) {
            if (!(*existing == func_type)) {
                error(action.range, "duplicate static action '" + action.name +
                      "' in class '" + cls_name + "' (different signature)");
            }
        } else {
            symbol_table_.declare(static_name, func_type);
        }
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

    for (auto& event : events) {
        if (symbol_table_.lookup(event.name)) {
            error(event.range, "duplicate event '" + event.name + "' in class '" + cls_name + "'");
        } else {
            TypeInfo event_type(TypeKind::Function);
            event_type.return_type = std::make_shared<TypeInfo>(TypeKind::Void);
            for (auto& param : event.params) {
                event_type.param_types.push_back(typeNodeToTypeInfo(param.type));
            }
            symbol_table_.declare(event.name, event_type);

            // Prepare fire_ClassName_EventName registration
            FireFunc ff;
            ff.name = "fire_" + cls_name + "_" + event.name;
            ff.type = TypeInfo(TypeKind::Function);
            ff.type.return_type = std::make_shared<TypeInfo>(TypeKind::Void);
            TypeInfo instance_type(TypeKind::Class);
            instance_type.class_name = cls_name;
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

    // BUG-021: 恢复外层类上下文（见函数头注释）。
    current_class_name_ = saved_current_class;
}

void Sema::visitInterfaceDecl(InterfaceDecl& decl) {
    if (symbol_table_.lookup(decl.name)) {
        error(decl.range, "duplicate interface name '" + decl.name + "'");
        return;
    }
    auto& members = interface_member_types_[decl.name];
    for (auto& act : decl.actions) {
        TypeInfo ft(TypeKind::Function);
        ft.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(act.return_type));
        for (auto& param : act.params)
            ft.param_types.push_back(typeNodeToTypeInfo(param.type));
        populateFuncTypeMeta(ft, act.params);
        members.emplace(act.name, std::move(ft));
    }
    for (auto& ev : decl.events) {
        TypeInfo ft(TypeKind::Function);
        ft.return_type = std::make_shared<TypeInfo>(TypeKind::Void);
        for (auto& param : ev.params)
            ft.param_types.push_back(typeNodeToTypeInfo(param.type));
        members.emplace(ev.name, std::move(ft));
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
        current_func_nonlocal_vars_.clear();   // M-FN-2 nonlocal: 本函数 accumulator
        current_func_nonlocal_cell_class_.clear();
        visitStmt(*body);
        // decl 引用在 visitStmt 后可能悬垂（单态化重分配 tu.functions）→ 先拷到
        // 成员，由调用方按索引重取 decl 后赋值。
        last_func_nonlocal_vars_ = current_func_nonlocal_vars_;
        last_func_nonlocal_cell_class_ = current_func_nonlocal_cell_class_;
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
                            // Declare: explicit type or infer from element.
                            // redeclare (not declare) so a same-scope name shadow
                            // takes the destructured element type (MYP last-wins);
                            // declare() would silently keep the stale first type.
                            TypeInfo lt = t.has_type ? typeNodeToTypeInfo(t.type) : et;
                            if (t.has_type && !typesCompatible(lt, et)) {
                                error(t.range, "destructure: variable '" + t.name +
                                    "' declared as '" + typeName(lt) + "' but element is '" +
                                    typeName(et) + "'");
                            }
                            symbol_table_.redeclare(t.name, lt);
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
        case StmtKind::GpuTileStmt:
            return visitGpuTileStmt(static_cast<GpuTileStmt&>(stmt));
        case StmtKind::GpuReduceStmt:
            return visitGpuReduceStmt(static_cast<GpuReduceStmt&>(stmt));
        case StmtKind::GpuScanStmt:
            return visitGpuScanStmt(static_cast<GpuScanStmt&>(stmt));
        case StmtKind::GpuScatterStmt:
            return visitGpuScatterStmt(static_cast<GpuScatterStmt&>(stmt));
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
            // BUG-011: 函数内 mapping 若用函数局部实例变量名作节点（`s.e -> t.a`），
            // codegen 会在 handler 函数里 load 外层函数的局部 alloca → 跨函数指令
            // 引用 → LLVM verify "Referring to an instruction in another function!"。
            // 实例级映射暂不支持（注册是全局的，handler 无法捕获函数局部实例）——
            // 编译期诊断，指引改用类名节点（`S.e -> T.a`）。
            for (auto& chain : ms.decl.chains) {
                for (auto& node : chain.nodes) {
                    if (node.is_function || node.is_lambda || node.is_transformer)
                        continue;
                    bool is_class_node = false;
                    if (current_tu_) {
                        for (auto& cls : current_tu_->classes) {
                            if (cls.name == node.source_name) { is_class_node = true; break; }
                        }
                    }
                    // 不是类名但符号表里有该名（函数局部变量/参数）→ 实例级节点
                    if (!is_class_node && symbol_table_.lookup(node.source_name)) {
                        std::string cn = node.source_name;
                        if (auto* st = symbol_table_.lookup(node.source_name))
                            if (!st->class_name.empty()) cn = st->class_name;
                        error(node.range, "mapping 节点 '" + node.source_name +
                            "' 是函数内局部变量名；实例级映射暂不支持，请改用类名节点"
                            "（如 '" + cn + "." + node.member_name + "'）");
                    }
                }
            }
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
        case StmtKind::NonlocalStmt:
            return visitNonlocalStmt(static_cast<NonlocalStmt&>(stmt));
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
        uint32_t err_before = diag_.errorCount();
        decl_type = visitExpr(*decl.init_expr);
        // BUG-016: `var r = voidCall();` 推断为 void 此前放行 → codegen 用 Int(i32)
        // alloca 存 void 值 → LLVM getPrefTypeAlign(void) 无限递归 → 段错误。void 值
        // 不能赋给变量，应报错。仅当 visitExpr 未级联报错（init 确为已知 void 调用，
        // 而非未解析表达式）时才补报。
        if (diag_.errorCount() == err_before && decl_type.kind == TypeKind::Void) {
            error(decl.range, "cannot infer type of 'var' from a void expression");
            return {};
        }
    } else if (decl.init_expr) {
        TypeInfo init_type;
        uint32_t err_before = diag_.errorCount();
        if (decl.init_expr->kind == ExprKind::Lambda && decl_type.kind == TypeKind::Function) {
            // Contextual typing: lambda assigned to a function-typed var uses
            // the declared function type (return + param types) as its own.
            // Recursive closures: treat the variable name as a NAMED lambda
            // (M-FN-2 __self) so a self-reference `name(...)` inside the body
            // routes to the lambda's own tramp — otherwise the self-reference
            // is seen as capturing an as-yet-uninitialized variable (typed
            // void → "cannot return value of type 'void'"; value null at call).
            static_cast<LambdaExpr&>(*decl.init_expr).name = decl.name;
            init_type = visitLambda(static_cast<LambdaExpr&>(*decl.init_expr), &decl_type);
        } else {
            init_type = visitExpr(*decl.init_expr);
        }
        bool init_reported = diag_.errorCount() > err_before;
        // BUG-016: `int x = voidCall();` 已知 void 值赋给非 void 变量——此前被
        // `init_type.kind != Void` 守卫误跳过（该守卫只该在 visitExpr 已级联报错、
        // init 类型未解析时跳过），导致 codegen 崩溃。补报类型不匹配。
        if (!init_reported && init_type.kind == TypeKind::Void) {
            error(decl.range, "cannot initialize variable '" + decl.name +
                  "' of type '" + typeName(decl_type) +
                  "' with value of type 'void'");
        } else if (init_type.kind != TypeKind::Void && !typesCompatible(decl_type, init_type)) {
            // Skip cascading error when init type is unknown (already reported)
            error(decl.range, "cannot initialize variable '" + decl.name +
                  "' of type '" + typeName(decl_type) +
                  "' with value of type '" + typeName(init_type) + "'");
        }
    }

    // BUG-022: `@thread` 只能用于 class 实例变量——struct 是值类型，@thread 会
    // 启动工作线程托管实例，对 struct 静默无效果（此前接受、编译运行通过但无意义）。
    // 放在 decl_type 完整解析之后（var 推断路径须先解析 init）。
    if (decl.has_thread_annotation && decl_type.kind != TypeKind::Class) {
        error(decl.range, "'@thread' can only be applied to a class instance variable");
        return {};
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
    // §3.3 kernel 上下文内 if 分支 = 发散控制流（aligned barrier 需 uniform）
    bool div = false;
    if (in_gpu_for_) { in_gpu_divergent_++; div = true; }
    if (stmt.then_block) visitStmt(*stmt.then_block);
    if (stmt.else_block) visitStmt(*stmt.else_block);
    if (div) in_gpu_divergent_--;
    return {};
}

Sema::StmtResult Sema::visitWhileStmt(WhileStmt& stmt) {
    if (stmt.condition) {
        auto cond_type = visitExpr(*stmt.condition);
        expectBool(cond_type, stmt.condition->range);
    }
    bool saved = in_loop_;
    in_loop_ = true;
    // §3.3 kernel 上下文内循环 = 发散控制流
    bool div = false;
    if (in_gpu_for_) { in_gpu_divergent_++; div = true; }
    if (stmt.body) visitStmt(*stmt.body);
    if (div) in_gpu_divergent_--;
    in_loop_ = saved;
    return {};
}

Sema::StmtResult Sema::visitGpuTileStmt(GpuTileStmt& stmt) {
    // §3.2 共享内存声明校验：维度编译期常量、48KB 上限、grid 常量、body 校验。
    // 提取维度（shared_type 嵌套 element_type，外层→内层）
    std::vector<int64_t> dims;
    const TypeNode* t = &stmt.shared_type;
    while (t && t->element_type) {
        if (t->array_size <= 0) {
            error(stmt.shared_type.range,
                "@gpu tile shared array dimension must be a compile-time "
                "constant (e.g. float[32][32])");
            return {};
        }
        dims.push_back(t->array_size);
        t = t->element_type.get();
    }
    if (dims.empty()) {
        error(stmt.shared_type.range,
            "@gpu tile requires an array type (e.g. float[32][32])");
        return {};
    }
    std::reverse(dims.begin(), dims.end());
    stmt.dim_vals = dims;

    // 元素类型 + 字节数（48KB 上限）
    TypeInfo elem = typeNodeToTypeInfo(*t);
    stmt.elem_type_info = elem;
    auto elemBytes = [](const TypeInfo& e) -> int64_t {
        switch (e.kind) {
            case TypeKind::Double: case TypeKind::Long: case TypeKind::ULong: return 8;
            case TypeKind::Float:  case TypeKind::Int:  case TypeKind::UInt:  return 4;
            case TypeKind::Short:  case TypeKind::UShort: case TypeKind::Char: return 2;
            default: return 1; // byte/ubyte/bool 等
        }
    };
    int64_t total = 1;
    for (auto d : dims) total *= d;
    if (total * elemBytes(elem) > 48 * 1024) {
        error(stmt.range,
            "@gpu tile shared array size (" + std::to_string(total * elemBytes(elem)) +
            " bytes) exceeds the 48KB limit (NV sm_75)");
    }

    // grid(nb) 子句：块数须为正整数。字面量 → grid_val（编译期）；否则为运行时
    // 表达式（host 求值，如 conv3d 的 nTiles），grid_val 置 -1 标记。
    if (stmt.has_grid) {
        TypeInfo gt = visitExpr(*stmt.grid_expr);
        if (!isNumericKind(gt.kind)) {
            error(stmt.grid_expr->range, "grid must be an integer (block count)");
            stmt.grid_val = -1;
        } else if (stmt.grid_expr->kind == ExprKind::IntegerLiteral) {
            stmt.grid_val = static_cast<const IntegerLiteralExpr&>(*stmt.grid_expr).value;
            if (stmt.grid_val <= 0)
                error(stmt.grid_expr->range, "grid must be a positive block count");
        } else {
            stmt.grid_val = -1;  // 运行时表达式（codegen 在 launch 点求值）
        }
    } else {
        stmt.grid_val = 1;
    }

    // M3 设备驻留子句 resident(arr = dev) 校验（同 @gpu for）：
    // arr 须为作用域内数组变量；dev 须为 long 变量。
    for (auto& [arr, dev] : stmt.resident) {
        auto* at = symbol_table_.lookup(arr);
        if (!at) {
            error(stmt.range, "resident array '" + arr + "' not found in scope");
        } else if (at->kind != TypeKind::Array) {
            error(stmt.range, "resident '" + arr + "' is not an array (got '" +
                  typeName(*at) + "')");
        }
        auto* dt = symbol_table_.lookup(dev);
        if (!dt) {
            error(stmt.range, "resident device-pointer variable '" + dev +
                  "' not found in scope");
        } else if (dt->kind != TypeKind::Long) {
            error(stmt.range, "resident device-pointer variable '" + dev +
                  "' must be 'long' (got '" + typeName(*dt) + "')");
        }
    }

    // §4.1 @gpu tile stream(s)：s 须为 GpuStream 实例。
    if (stmt.has_stream) {
        TypeInfo st = visitExpr(*stmt.stream_expr);
        bool is_stream = (st.kind == TypeKind::Class && st.class_name == "GpuStream");
        if (!is_stream) {
            error(stmt.range, "'stream(...)' argument must be a 'GpuStream' (got '" +
                  typeName(st) + "')");
        }
    }

    // §3.7 @gpu tile block(n)：块大小须为 32 的倍数，≤ 1024。
    if (stmt.block_val > 0) {
        if (stmt.block_val % 32 != 0) {
            error(stmt.range, "'block(...)' size must be a multiple of 32 (warp size)");
        } else if (stmt.block_val > 1024) {
            error(stmt.range, "'block(...)' size exceeds maxThreadsPerBlock (1024)");
        }
        // §5.3 静态检查：块大小小于最大共享维度 → 协作覆盖不完（如
        // smem[kernel.tx] 时 tx ∈ [0, block_val) 无法写满 dim），sync 后读垃圾。
        int64_t max_dim = 0;
        for (auto d : dims) if (d > max_dim) max_dim = d;
        if (max_dim > 0 && stmt.block_val < max_dim) {
            diag_.warn(stmt.range, "'block(...)' size (" + std::to_string(stmt.block_val) +
                 ") is smaller than shared array dimension (" +
                 std::to_string(max_dim) + "); threads may not cover the array");
        }
    }

    // 进入 kernel 上下文（kernel.bx/tx/gid/... 隐式可见），声明共享数组局部可见
    bool saved_gpu = in_gpu_for_;
    in_gpu_for_ = true;
    symbol_table_.enterScope();
    if (symbol_table_.lookup(stmt.name)) {
        error(stmt.range, "shared array name '" + stmt.name + "' already declared");
    } else {
        symbol_table_.declare(stmt.name, typeNodeToTypeInfo(stmt.shared_type));
    }
    if (stmt.body) visitStmt(*stmt.body);
    symbol_table_.leaveScope();
    in_gpu_for_ = saved_gpu;
    return {};
}

// §8.2 @gpu reduce (acc, x) => { return <op>; } init V over a[lo..hi) -> out;
// 声明式归约 out = fold(init, a[lo..hi))。校验：
//   · 数组元素类型 = init = op 返回 = out 类型（支持 float/double/int）；
//   · begin/end 为整型；block(n) 32 倍数 ≤1024；
// 把 op 的 return 表达式提取到 stmt.op_expr（codegen 用它）。
Sema::StmtResult Sema::visitGpuReduceStmt(GpuReduceStmt& stmt) {
    // 数组：须为 T[] 动态数组
    auto* at = symbol_table_.lookup(stmt.array_name);
    if (!at || at->kind != TypeKind::Array || !at->element_type) {
        error(stmt.range, "'@gpu reduce' array '" + stmt.array_name +
              "' must be a 'T[]' dynamic array (got '" +
              (at ? typeName(*at) : std::string("(not found)")) + "')");
        return {};
    }
    TypeKind et = at->element_type->kind;
    if (et != TypeKind::Float && et != TypeKind::Double && et != TypeKind::Int) {
        error(stmt.range, "'@gpu reduce' element type must be float/double/int "
              "(got '" + typeName(*at->element_type) + "')");
        return {};
    }
    // out：host 标量变量（float/double/int）
    auto* ot = symbol_table_.lookup(stmt.out_name);
    if (!ot || ot->kind != et) {
        error(stmt.range, "'@gpu reduce' output '" + stmt.out_name +
              "' must be a '" + typeName(*at->element_type) + "' scalar");
        return {};
    }
    // init：与元素同类型
    TypeInfo init_t = visitExpr(*stmt.init_expr);
    if (init_t.kind != et) {
        error(stmt.init_expr->range, "'@gpu reduce' init must be '" +
              typeName(*at->element_type) + "' (got '" + typeName(init_t) + "')");
        return {};
    }
    // range：begin/end 整型
    auto bt = visitExpr(*stmt.begin_expr);
    if (!isNumericKind(bt.kind)) {
        error(stmt.begin_expr->range, "'@gpu reduce' range bound must be an integer");
        return {};
    }
    auto ht = visitExpr(*stmt.end_expr);
    if (!isNumericKind(ht.kind)) {
        error(stmt.end_expr->range, "'@gpu reduce' range bound must be an integer");
        return {};
    }
    // op body：声明 acc/x 为元素类型后访问，提取 return 表达式
    // （current_return_type_ 临时设为元素类型，使 `return acc + x;` 通过类型检查）
    auto saved_ret = current_return_type_;
    current_return_type_ = *at->element_type;
    symbol_table_.enterScope();
    symbol_table_.declare(stmt.op_acc, *at->element_type);
    symbol_table_.declare(stmt.op_x, *at->element_type);
    if (stmt.op_body) visitStmt(*stmt.op_body);
    symbol_table_.leaveScope();
    current_return_type_ = saved_ret;
    if (stmt.op_body && stmt.op_body->kind == StmtKind::Block) {
        auto& blk = static_cast<BlockStmt&>(*stmt.op_body);
        for (auto& s : blk.statements) {
            if (s->kind == StmtKind::ReturnStmt) {
                auto& r = static_cast<ReturnStmt&>(*s);
                if (r.value) {
                    stmt.op_expr = std::move(r.value);
                    break;
                }
            }
        }
    }
    if (!stmt.op_expr) {
        error(stmt.range, "'@gpu reduce' op body must contain a 'return <expr>;'");
        return {};
    }
    // §3.7 block(n) 约束
    if (stmt.block_val > 0) {
        if (stmt.block_val % 32 != 0)
            error(stmt.range, "'block(...)' size must be a multiple of 32 (warp size)");
        else if (stmt.block_val > 1024)
            error(stmt.range, "'block(...)' size exceeds maxThreadsPerBlock (1024)");
    }
    return {};
}

// §8.2 @gpu reduce 表达式形式（GpuReduceExpr）：无 `-> out`，结果作为表达式值。
// 合成临时输出标量（元素类型）声明进符号表后，复用 visitGpuReduceStmt 全量校验；
// 表达式结果类型 = 数组元素类型。
TypeInfo Sema::visitGpuReduceExpr(GpuReduceExpr& expr) {
    auto& stmt = *expr.stmt;
    auto* at = symbol_table_.lookup(stmt.array_name);
    if (!at || at->kind != TypeKind::Array || !at->element_type) {
        error(stmt.range, "'@gpu reduce' array '" + stmt.array_name +
              "' must be a 'T[]' dynamic array");
        return TypeInfo(TypeKind::Int);
    }
    TypeKind et = at->element_type->kind;
    if (et != TypeKind::Float && et != TypeKind::Double && et != TypeKind::Int) {
        error(stmt.range, "'@gpu reduce' element type must be float/double/int "
              "(got '" + typeName(*at->element_type) + "')");
        return TypeInfo(TypeKind::Int);
    }
    // 声明合成临时输出标量（元素类型），使 visitGpuReduceStmt 的 out 校验通过
    symbol_table_.declare(stmt.out_name, *at->element_type);
    visitGpuReduceStmt(stmt);
    expr.result_kind = et;
    return TypeInfo(et);
}

// §8.3 @gpu scan：前缀和 b[lo+i] = init∘a[lo]∘…∘a[lo+i]。in/out 均为 T[]，
// init/op 同 reduce 校验。提取 return 表达式到 stmt.op_expr。
Sema::StmtResult Sema::visitGpuScanStmt(GpuScanStmt& stmt) {
    auto* at = symbol_table_.lookup(stmt.in_name);
    if (!at || at->kind != TypeKind::Array || !at->element_type) {
        error(stmt.range, "'@gpu scan' input '" + stmt.in_name +
              "' must be a 'T[]' dynamic array");
        return {};
    }
    TypeKind et = at->element_type->kind;
    if (et != TypeKind::Float && et != TypeKind::Double && et != TypeKind::Int) {
        error(stmt.range, "'@gpu scan' element type must be float/double/int "
              "(got '" + typeName(*at->element_type) + "')");
        return {};
    }
    auto* ot = symbol_table_.lookup(stmt.out_name);
    if (!ot || ot->kind != TypeKind::Array || !ot->element_type ||
        ot->element_type->kind != et) {
        error(stmt.range, "'@gpu scan' output '" + stmt.out_name +
              "' must be a '" + typeName(*at->element_type) + "[]' array");
        return {};
    }
    TypeInfo init_t = visitExpr(*stmt.init_expr);
    if (init_t.kind != et) {
        error(stmt.init_expr->range, "'@gpu scan' init must be '" +
              typeName(*at->element_type) + "' (got '" + typeName(init_t) + "')");
        return {};
    }
    auto bt = visitExpr(*stmt.begin_expr);
    if (!isNumericKind(bt.kind)) {
        error(stmt.begin_expr->range, "'@gpu scan' range bound must be an integer");
        return {};
    }
    auto ht = visitExpr(*stmt.end_expr);
    if (!isNumericKind(ht.kind)) {
        error(stmt.end_expr->range, "'@gpu scan' range bound must be an integer");
        return {};
    }
    auto saved_ret = current_return_type_;
    current_return_type_ = *at->element_type;
    symbol_table_.enterScope();
    symbol_table_.declare(stmt.op_acc, *at->element_type);
    symbol_table_.declare(stmt.op_x, *at->element_type);
    if (stmt.op_body) visitStmt(*stmt.op_body);
    symbol_table_.leaveScope();
    current_return_type_ = saved_ret;
    if (stmt.op_body && stmt.op_body->kind == StmtKind::Block) {
        auto& blk = static_cast<BlockStmt&>(*stmt.op_body);
        for (auto& s : blk.statements) {
            if (s->kind == StmtKind::ReturnStmt) {
                auto& r = static_cast<ReturnStmt&>(*s);
                if (r.value) { stmt.op_expr = std::move(r.value); break; }
            }
        }
    }
    if (!stmt.op_expr) {
        error(stmt.range, "'@gpu scan' op body must contain a 'return <expr>;'");
        return {};
    }
    if (stmt.block_val > 0) {
        if (stmt.block_val % 32 != 0)
            error(stmt.range, "'block(...)' size must be a multiple of 32 (warp size)");
        else if (stmt.block_val > 1024)
            error(stmt.range, "'block(...)' size exceeds maxThreadsPerBlock (1024)");
    }
    return {};
}

// §8.4 @gpu scatter：b[idx[lo_i+p]] = a[lo_a+p]（冲突语义显式）。
// a/b 须同元素类型 T[]（float/double/int）；idx 须整型[]（int/long）；
// 两区间界须整型；mode ∈ {0=any, 1=unique, 2=atomic_add}。
Sema::StmtResult Sema::visitGpuScatterStmt(GpuScatterStmt& stmt) {
    auto* at = symbol_table_.lookup(stmt.a_name);
    if (!at || at->kind != TypeKind::Array || !at->element_type) {
        error(stmt.range, "'@gpu scatter' input '" + stmt.a_name +
              "' must be a 'T[]' dynamic array");
        return {};
    }
    TypeKind et = at->element_type->kind;
    if (et != TypeKind::Float && et != TypeKind::Double && et != TypeKind::Int) {
        error(stmt.range, "'@gpu scatter' element type must be float/double/int "
              "(got '" + typeName(*at->element_type) + "')");
        return {};
    }
    auto* bt = symbol_table_.lookup(stmt.b_name);
    if (!bt || bt->kind != TypeKind::Array || !bt->element_type ||
        bt->element_type->kind != et) {
        error(stmt.range, "'@gpu scatter' output '" + stmt.b_name +
              "' must be a '" + typeName(*at->element_type) + "[]' array");
        return {};
    }
    auto* it = symbol_table_.lookup(stmt.idx_name);
    if (!it || it->kind != TypeKind::Array || !it->element_type ||
        it->element_type->kind != TypeKind::Int) {
        error(stmt.range, "'@gpu scatter' index '" + stmt.idx_name +
              "' must be an 'int[]' array");
        return {};
    }
    // 冲突模式：any/unique/atomic_add 均合法（parser 已限定）
    if (stmt.mode < 0 || stmt.mode > 2) {
        error(stmt.range, "'@gpu scatter' conflict mode must be any/unique/atomic_add");
        return {};
    }
    // 两区间界：整型
    auto ab = visitExpr(*stmt.a_begin);
    if (!isNumericKind(ab.kind)) {
        error(stmt.a_begin->range, "'@gpu scatter' range bound must be an integer");
        return {};
    }
    auto ae = visitExpr(*stmt.a_end);
    if (!isNumericKind(ae.kind)) {
        error(stmt.a_end->range, "'@gpu scatter' range bound must be an integer");
        return {};
    }
    auto ib = visitExpr(*stmt.idx_begin);
    if (!isNumericKind(ib.kind)) {
        error(stmt.idx_begin->range, "'@gpu scatter' index range bound must be an integer");
        return {};
    }
    auto ie = visitExpr(*stmt.idx_end);
    if (!isNumericKind(ie.kind)) {
        error(stmt.idx_end->range, "'@gpu scatter' index range bound must be an integer");
        return {};
    }
    if (stmt.block_val > 0) {
        if (stmt.block_val % 32 != 0)
            error(stmt.range, "'block(...)' size must be a multiple of 32 (warp size)");
        else if (stmt.block_val > 1024)
            error(stmt.range, "'block(...)' size exceeds maxThreadsPerBlock (1024)");
    }
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

    // M3 设备驻留子句 resident(arr = dev) 校验：
    //   · 仅 @gpu for 可用
    //   · arr 须为作用域内数组变量；dev 须为 long 变量
    if (!stmt.resident.empty()) {
        if (!stmt.gpu) {
            error(stmt.range,
                "'resident(...)' is only valid on '@gpu for'");
        } else {
            for (auto& [arr, dev] : stmt.resident) {
                auto* at = symbol_table_.lookup(arr);
                if (!at) {
                    error(stmt.range, "resident array '" + arr + "' not found in scope");
                } else if (at->kind != TypeKind::Array) {
                    error(stmt.range, "resident '" + arr + "' is not an array (got '" +
                          typeName(*at) + "')");
                }
                auto* dt = symbol_table_.lookup(dev);
                if (!dt) {
                    error(stmt.range, "resident device-pointer variable '" + dev +
                          "' not found in scope");
                } else if (dt->kind != TypeKind::Long) {
                    error(stmt.range, "resident device-pointer variable '" + dev +
                          "' must be 'long' (got '" + typeName(*dt) + "')");
                }
            }
        }
    }

    // §4.1 @gpu stream(s)：仅 @gpu for 可用，s 须为 GpuStream 实例。
    if (stmt.has_stream) {
        if (!stmt.gpu) {
            error(stmt.range, "'stream(...)' is only valid on '@gpu for'");
        } else {
            TypeInfo st = visitExpr(*stmt.stream_expr);
            bool is_stream = (st.kind == TypeKind::Class && st.class_name == "GpuStream");
            if (!is_stream) {
                error(stmt.range, "'stream(...)' argument must be a 'GpuStream' (got '" +
                      typeName(st) + "')");
            }
        }
    }

    // §3.7 @gpu block(n)：块大小须为 32 的倍数，≤ maxThreads(1024)。
    if (stmt.block_val > 0) {
        if (!stmt.gpu) {
            error(stmt.range, "'block(...)' is only valid on '@gpu for'");
        } else if (stmt.block_val % 32 != 0) {
            error(stmt.range, "'block(...)' size must be a multiple of 32 (warp size)");
        } else if (stmt.block_val > 1024) {
            error(stmt.range, "'block(...)' size exceeds maxThreadsPerBlock (1024)");
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
    // §3.1 kernel 执行上下文：@gpu for body 内 `kernel` 保留标识符隐式可见。
    bool saved_gpu = in_gpu_for_;
    if (stmt.gpu) in_gpu_for_ = true;
    if (stmt.body) visitStmt(*stmt.body);
    in_gpu_for_ = saved_gpu;
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

// §9 内置数值 trait：类型 t 是否满足约束（Numeric/Integer/Float/Ordered；
// 非内建 trait 名按接口实现检查，与泛型类 where T : I 一致）。
bool Sema::satisfiesTraitConstraint(const TypeInfo& t, const std::string& trait) const {
    auto is_int_family = [](TypeKind k) {
        return k == TypeKind::Byte || k == TypeKind::Short ||
               k == TypeKind::Int || k == TypeKind::Long ||
               k == TypeKind::UByte || k == TypeKind::UShort ||
               k == TypeKind::UInt || k == TypeKind::ULong ||
               k == TypeKind::Char;
    };
    if (trait == "Numeric")
        return is_int_family(t.kind) || t.kind == TypeKind::Float ||
               t.kind == TypeKind::Double;
    if (trait == "Integer")
        return is_int_family(t.kind);
    if (trait == "Float")
        return t.kind == TypeKind::Float || t.kind == TypeKind::Double;
    if (trait == "Ordered")
        return is_int_family(t.kind) || t.kind == TypeKind::Float ||
               t.kind == TypeKind::Double || t.kind == TypeKind::String;
    // 接口约束：类实现检查
    if (t.kind == TypeKind::Class) {
        if (!current_tu_) return false;
        for (auto& c : current_tu_->classes) {
            if (c.name == t.class_name && c.interface_class_name == trait)
                return true;
        }
    }
    if (t.kind == TypeKind::Interface && t.class_name == trait)
        return true;
    return false;
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
    if (type.kind != TypeKind::Bool && type.kind != TypeKind::Bit) {
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
    add_intrinsic("__myp_charcode", TypeKind::Int, {TypeKind::String, TypeKind::Int});

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

    // §7.4 厂商探测 + 能力查询（vendor-neutral __myp_gpu_* 前缀）
    add_intrinsic("__myp_gpu_vendor", TypeKind::String, {});
    add_intrinsic("__myp_gpu_gfx_arch", TypeKind::String, {});
    add_intrinsic("__myp_gpu_shared_per_block", TypeKind::Int, {});
    add_intrinsic("__myp_gpu_regs_per_block", TypeKind::Int, {});
    add_intrinsic("__myp_gpu_max_grid_dim", TypeKind::Int, {});
    add_intrinsic("__myp_gpu_max_block_dim", TypeKind::Int, {});
    add_intrinsic("__myp_gpu_clock_mhz", TypeKind::Int, {});
    add_intrinsic("__myp_gpu_concurrent_kernels", TypeKind::Int, {});
    add_intrinsic("__myp_gpu_mem_alignment", TypeKind::Int, {});
    add_intrinsic("__myp_gpu_double_precision", TypeKind::Int, {});
    add_intrinsic("__myp_gpu_atomics64", TypeKind::Int, {});

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
    add_intrinsic("__myp_assert_msg", TypeKind::Void, {TypeKind::Bool, TypeKind::String});
    add_intrinsic("__myp_test_set_msg", TypeKind::Void, {TypeKind::String});
    add_intrinsic("__myp_assert_eq", TypeKind::Void, {TypeKind::Int, TypeKind::Int});
    add_intrinsic("__myp_assert_neq", TypeKind::Void, {TypeKind::Int, TypeKind::Int});
    add_intrinsic("__myp_assert_long_eq", TypeKind::Void, {TypeKind::Long, TypeKind::Long});
    add_intrinsic("__myp_assert_str_eq", TypeKind::Void, {TypeKind::String, TypeKind::String});
    add_intrinsic("__myp_assert_str_neq", TypeKind::Void, {TypeKind::String, TypeKind::String});
    add_intrinsic("__myp_test_report", TypeKind::Void, {TypeKind::String, TypeKind::Bool});
    add_intrinsic("__myp_test_fail_msg", TypeKind::Void, {TypeKind::String});
    add_intrinsic("__myp_test_summary", TypeKind::Int, {TypeKind::Int});
    add_intrinsic("__myp_assert_long_neq", TypeKind::Void, {TypeKind::Long, TypeKind::Long});
    add_intrinsic("__myp_assert_float_neq", TypeKind::Void, {TypeKind::Double, TypeKind::Double, TypeKind::Double});
    add_intrinsic("__myp_assert_null", TypeKind::Void, {TypeKind::Class});
    add_intrinsic("__myp_assert_not_null", TypeKind::Void, {TypeKind::Class});
    // @test 输出捕获（阶段 1）
    add_intrinsic("__myp_test_capture_start", TypeKind::Void, {});
    add_intrinsic("__myp_test_capture_stop", TypeKind::Void, {});
    add_intrinsic("__myp_test_capture_get", TypeKind::String, {});
    add_intrinsic("__myp_test_capture_contains", TypeKind::Int, {TypeKind::String});
    add_intrinsic("__myp_test_capture_eq", TypeKind::Int, {TypeKind::String});

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

    // GPU 显式显存 / 流 FFI（M1 范式库 stdlib/gpu 使用）
    // 句柄统一用 long；带数组参数的用 add_gpu_arr 注册（元素类型显式指定）。
    add_intrinsic("__myp_gpu_alloc", TypeKind::Long, {TypeKind::Long});
    add_intrinsic("__myp_gpu_free", TypeKind::Int, {TypeKind::Long});
    add_intrinsic("__myp_gpu_copy_d2d", TypeKind::Int,
                  {TypeKind::Long, TypeKind::Long, TypeKind::Long, TypeKind::Long, TypeKind::Long});
    add_intrinsic("__myp_gpu_sync", TypeKind::Int, {});
    add_intrinsic("__myp_gpu_stream_create", TypeKind::Long, {});
    add_intrinsic("__myp_gpu_stream_sync", TypeKind::Int, {TypeKind::Long});
    add_intrinsic("__myp_gpu_stream_destroy", TypeKind::Int, {TypeKind::Long});

    // M2：异步拷贝 / 事件（流句柄为 long）
    add_intrinsic("__myp_gpu_copy_d2d_async", TypeKind::Int,
                  {TypeKind::Long, TypeKind::Long, TypeKind::Long, TypeKind::Long, TypeKind::Long, TypeKind::Long});
    add_intrinsic("__myp_gpu_event_create", TypeKind::Long, {});
    add_intrinsic("__myp_gpu_event_record", TypeKind::Int, {TypeKind::Long, TypeKind::Long});
    add_intrinsic("__myp_gpu_event_wait", TypeKind::Int, {TypeKind::Long, TypeKind::Long});
    add_intrinsic("__myp_gpu_event_sync", TypeKind::Int, {TypeKind::Long});
    add_intrinsic("__myp_gpu_event_elapsed", TypeKind::Double, {TypeKind::Long, TypeKind::Long});
    add_intrinsic("__myp_gpu_event_destroy", TypeKind::Int, {TypeKind::Long});

    auto add_gpu_arr = [&](const std::string& name, TypeKind ret, std::vector<TypeKind> params, std::vector<TypeKind> elem_types) {
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
    // __myp_gpu_copy_h2d(dev, double[] host, srcOff, dstOff, len) -> int
    add_gpu_arr("__myp_gpu_copy_h2d", TypeKind::Int,
                {TypeKind::Long, TypeKind::Array, TypeKind::Int, TypeKind::Int, TypeKind::Int},
                {TypeKind::Double});
    // __myp_gpu_copy_d2h(double[] host, dev, srcOff, dstOff, len) -> int
    add_gpu_arr("__myp_gpu_copy_d2h", TypeKind::Int,
                {TypeKind::Array, TypeKind::Long, TypeKind::Int, TypeKind::Int, TypeKind::Int},
                {TypeKind::Double});
    // float 变体
    add_gpu_arr("__myp_gpu_copy_h2d_f", TypeKind::Int,
                {TypeKind::Long, TypeKind::Array, TypeKind::Int, TypeKind::Int, TypeKind::Int},
                {TypeKind::Float});
    add_gpu_arr("__myp_gpu_copy_d2h_f", TypeKind::Int,
                {TypeKind::Array, TypeKind::Long, TypeKind::Int, TypeKind::Int, TypeKind::Int},
                {TypeKind::Float});
    // M2 异步拷贝（带流句柄）
    add_gpu_arr("__myp_gpu_copy_h2d_async", TypeKind::Int,
                {TypeKind::Long, TypeKind::Array, TypeKind::Int, TypeKind::Int, TypeKind::Int, TypeKind::Long},
                {TypeKind::Double});
    add_gpu_arr("__myp_gpu_copy_d2h_async", TypeKind::Int,
                {TypeKind::Array, TypeKind::Long, TypeKind::Int, TypeKind::Int, TypeKind::Int, TypeKind::Long},
                {TypeKind::Double});
    add_gpu_arr("__myp_gpu_copy_h2d_async_f", TypeKind::Int,
                {TypeKind::Long, TypeKind::Array, TypeKind::Int, TypeKind::Int, TypeKind::Int, TypeKind::Long},
                {TypeKind::Float});
    add_gpu_arr("__myp_gpu_copy_d2h_async_f", TypeKind::Int,
                {TypeKind::Array, TypeKind::Long, TypeKind::Int, TypeKind::Int, TypeKind::Int, TypeKind::Long},
                {TypeKind::Float});

    // §P6 ② CUDA Graph（图内存）：流捕获 → 图 → 实例化 → 重放
    add_intrinsic("__myp_gpu_graph_capture_begin", TypeKind::Int, {TypeKind::Long});
    add_intrinsic("__myp_gpu_graph_capture_end", TypeKind::Long, {TypeKind::Long});
    add_intrinsic("__myp_gpu_graph_instantiate", TypeKind::Long, {TypeKind::Long});
    add_intrinsic("__myp_gpu_graph_launch", TypeKind::Int, {TypeKind::Long, TypeKind::Long});
    add_intrinsic("__myp_gpu_graph_destroy", TypeKind::Void, {TypeKind::Long});
    add_intrinsic("__myp_gpu_graph_exec_destroy", TypeKind::Void, {TypeKind::Long});

    // §P6 ③ BYOC：自定义 PTX 内核加载/启动（args 为 long[]）
    add_intrinsic("__myp_gpu_byoc_load", TypeKind::Long,
                  {TypeKind::String, TypeKind::String});
    add_gpu_arr("__myp_gpu_byoc_launch", TypeKind::Int,
                {TypeKind::Long, TypeKind::Int, TypeKind::Int, TypeKind::Array, TypeKind::Int, TypeKind::Long},
                {TypeKind::Long});
    // §P6 ③ BYOC：cuBLAS SGEMM 厂商库 hook
    add_intrinsic("__myp_cublas_available", TypeKind::Int, {});
    add_intrinsic("__myp_cublas_sgemm", TypeKind::Int,
                  {TypeKind::Long, TypeKind::Long, TypeKind::Long,
                   TypeKind::Int, TypeKind::Int, TypeKind::Int,
                   TypeKind::Double, TypeKind::Double});

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

// nonlocal k, m; — 仅 lambda body 内合法（visitLambda 已做主要校验/捕获设置；
// 此处兜底：非 lambda 上下文报错，且名字须可解析）。
Sema::StmtResult Sema::visitNonlocalStmt(NonlocalStmt& stmt) {
    if (in_lambda_body_ == 0) {
        error(stmt.range, "'nonlocal' is only allowed inside a lambda body");
        return {};
    }
    for (auto& n : stmt.names) {
        if (!symbol_table_.lookup(n)) {
            error(stmt.range, "nonlocal: undeclared variable '" + n + "'");
        }
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

