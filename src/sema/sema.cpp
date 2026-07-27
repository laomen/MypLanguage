#include "mylang/Sema.h"

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

    // === Pass 1: Collect all top-level declarations ===
    visitTranslationUnit(tu);
    if (diag_.hasErrors()) return false;

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
    for (auto& func : tu.functions) {
        visitFuncBody(func);
    }

    // Type-check action bodies inside classes
    for (auto& cls : tu.classes) {
        current_class_name_ = cls.name;
        in_class_method_ = true;
        for (auto& action : cls.actions) {
            if (action.body) {
                current_return_type_ = typeNodeToTypeInfo(action.return_type);
                symbol_table_.enterScope();
                TypeInfo this_type(TypeKind::Class);
                this_type.class_name = cls.name;
                symbol_table_.declare("this", this_type);
                for (auto& param : action.params) {
                    auto param_type = typeNodeToTypeInfo(param.type);
                    symbol_table_.declare(param.name, param_type);
                }
                visitStmt(*action.body);
                symbol_table_.leaveScope();
            }
        }
        // Type-check function: section bodies
        for (auto& func : cls.functions) {
            if (func.body) {
                current_return_type_ = typeNodeToTypeInfo(func.return_type);
                symbol_table_.enterScope();
                TypeInfo this_type(TypeKind::Class);
                this_type.class_name = cls.name;
                symbol_table_.declare("this", this_type);
                for (auto& param : func.params) {
                    symbol_table_.declare(param.name, typeNodeToTypeInfo(param.type));
                }
                visitStmt(*func.body);
                symbol_table_.leaveScope();
            }
        }
        // Type-check static: section bodies (no 'this')
        for (auto& action : cls.static_actions) {
            if (action.body) {
                current_return_type_ = typeNodeToTypeInfo(action.return_type);
                symbol_table_.enterScope();
                // No 'this' for static methods
                for (auto& param : action.params) {
                    symbol_table_.declare(param.name, typeNodeToTypeInfo(param.type));
                }
                visitStmt(*action.body);
                symbol_table_.leaveScope();
            }
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
    for (auto& func : decl.functions) {
        if (func.body) {
            current_return_type_ = typeNodeToTypeInfo(func.return_type);
            symbol_table_.enterScope();
            // Struct methods can access struct fields (no 'this', fields are local)
            for (auto& prop : decl.properties) {
                symbol_table_.declare(prop.name, typeNodeToTypeInfo(prop.type));
            }
            for (auto& param : func.params) {
                symbol_table_.declare(param.name, typeNodeToTypeInfo(param.type));
            }
            visitStmt(*func.body);
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
        bool found = false;
        for (auto& ca : cls.actions) {
            if (ca.name == ia.name && ca.return_type.basic_type == ia.return_type.basic_type) {
                found = true;
                break;
            }
        }
        if (!found) {
            for (auto& ca : cls.static_actions) {
                if (ca.name == ia.name && ca.return_type.basic_type == ia.return_type.basic_type) {
                    found = true;
                    break;
                }
            }
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

    // Register struct methods
    for (auto& func : decl.functions) {
        TypeInfo func_type(TypeKind::Function);
        func_type.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(func.return_type));
        for (auto& param : func.params) {
            func_type.param_types.push_back(typeNodeToTypeInfo(param.type));
        }
        std::string method_name = type_key + "::" + func.name;
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
        TypeInfo func_type(TypeKind::Function);
        func_type.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(action.return_type));
        for (auto& param : action.params) {
            func_type.param_types.push_back(typeNodeToTypeInfo(param.type));
        }
        if (!symbol_table_.declare(action.name, func_type)) {
            error(action.range, "duplicate action '" + action.name + "' in class '" + decl.name + "'");
        }
    }

    // Register static actions in GLOBAL scope (accessible as ClassName.method)
    for (auto& action : decl.static_actions) {
        TypeInfo func_type(TypeKind::Function);
        func_type.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(action.return_type));
        for (auto& param : action.params) {
            func_type.param_types.push_back(typeNodeToTypeInfo(param.type));
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
    TypeInfo func_type(TypeKind::Function);
    func_type.return_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(decl.return_type));
    for (auto& param : decl.params) {
        func_type.param_types.push_back(typeNodeToTypeInfo(param.type));
    }
    symbol_table_.declare(decl.name, func_type);
}

// ==============================
// Pass 2: Type-check function bodies
// ==============================

void Sema::visitFuncBody(FuncDecl& decl) {
    symbol_table_.enterScope();
    current_return_type_ = typeNodeToTypeInfo(decl.return_type);

    for (auto& param : decl.params) {
        symbol_table_.declare(param.name, typeNodeToTypeInfo(param.type));
    }

    // In main(), only instance creation + mapping is allowed — no direct method calls
    if (decl.name == "main") in_main_function_ = true;

    if (decl.body) {
        visitStmt(*decl.body);
    }

    in_main_function_ = false;
    symbol_table_.leaveScope();
}

// ==============================
// Statement type checking
// ==============================

Sema::StmtResult Sema::visitStmt(Stmt& stmt) {
    switch (stmt.kind) {
        case StmtKind::Block:
            return visitBlock(static_cast<BlockStmt&>(stmt));
        case StmtKind::VarDeclStmt:
            return visitVarDecl(static_cast<VarDeclStmt&>(stmt).decl);
        case StmtKind::ExprStmt: {
            auto& es = static_cast<ExprStmt&>(stmt);
            if (es.expression) {
                // In main(), reject direct method calls — use mapping instead
                if (in_main_function_ && es.expression->kind == ExprKind::Call) {
                    auto& call = static_cast<CallExpr&>(*es.expression);
                    bool is_method_call = call.callee->kind == ExprKind::MemberAccess;
                    bool is_event_call = call.callee->kind == ExprKind::Identifier;
                    if (is_method_call || is_event_call) {
                        error(es.expression->range,
                            "direct function call not allowed in main() — use mapping() instead");
                        return {};
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
        case StmtKind::ReturnStmt:
            return visitReturnStmt(static_cast<ReturnStmt&>(stmt));
        case StmtKind::BreakStmt:
        case StmtKind::ContinueStmt:
            if (!in_loop_) {
                error(stmt.range, "break/continue outside loop");
            }
            return {};
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

    // Check for unknown class/type
    if (decl_type.kind == TypeKind::Void && !decl.type.class_name.empty()) {
        error(decl.range, "unknown type '" + decl.type.class_name + "'");
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
        auto init_type = visitExpr(*decl.init_expr);
        if (!typesCompatible(decl_type, init_type)) {
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
        case ExprKind::EnumVariant:
            result = visitEnumVariant(static_cast<EnumVariantExpr&>(expr));
            break;
    }
    return result;
}

TypeInfo Sema::visitIntegerLiteral(IntegerLiteralExpr& expr) {
    auto val = expr.value;
    TypeInfo result;
    if (val >= -128 && val <= 127)           result = TypeInfo(TypeKind::Byte);
    else if (val >= -32768 && val <= 32767)  result = TypeInfo(TypeKind::Short);
    else if (val >= -2147483648LL && val <= 2147483647LL) result = TypeInfo(TypeKind::Int);
    else                                      result = TypeInfo(TypeKind::Long);
    // Store resolved type on the expression for codegen to use
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
                    for (auto& p : ev.params)
                        event_type.param_types.push_back(typeNodeToTypeInfo(p.type));
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

    switch (expr.op) {
        case BinaryOpKind::Add: {
            // String concatenation: string + any → string
            if (lhs_type.kind == TypeKind::String || rhs_type.kind == TypeKind::String)
                return TypeInfo(TypeKind::String);
            // Numeric addition
            if (!expectNumeric(lhs_type, expr.lhs->range) ||
                !expectNumeric(rhs_type, expr.rhs->range)) {
                return TypeInfo(TypeKind::Int);
            }
            goto numeric_common;
        }
        case BinaryOpKind::Sub:
        case BinaryOpKind::Mul: case BinaryOpKind::Div: case BinaryOpKind::Mod:
            if (!expectNumeric(lhs_type, expr.lhs->range) ||
                !expectNumeric(rhs_type, expr.rhs->range)) {
                return TypeInfo(TypeKind::Int);
            }
            goto numeric_common;

        case BinaryOpKind::Eq: case BinaryOpKind::Ne:
            return TypeInfo(TypeKind::Bool);

        case BinaryOpKind::Lt: case BinaryOpKind::Gt:
        case BinaryOpKind::Le: case BinaryOpKind::Ge:
            if (!expectNumeric(lhs_type, expr.lhs->range))
                return TypeInfo(TypeKind::Bool);
            return TypeInfo(TypeKind::Bool);

        case BinaryOpKind::And: case BinaryOpKind::Or:
            expectBool(lhs_type, expr.lhs->range);
            expectBool(rhs_type, expr.rhs->range);
            return TypeInfo(TypeKind::Bool);
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

TypeInfo Sema::visitCall(CallExpr& expr) {
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
        auto arg_type = visitExpr(*expr.args[i]);
        if (!typesCompatible(callee_type.param_types[i], arg_type)) {
            error(expr.args[i]->range, "argument " + std::to_string(i + 1) +
                  ": expected '" + typeName(callee_type.param_types[i]) +
                  "', got '" + typeName(arg_type) + "'");
        }
    }

    if (callee_type.return_type) return *callee_type.return_type;
    return TypeInfo(TypeKind::Void);
}

TypeInfo Sema::visitMemberAccess(MemberAccessExpr& expr) {
    auto obj_type = visitExpr(*expr.object);

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
                        func_type.return_type = std::make_shared<TypeInfo>(
                            typeNodeToTypeInfo(action.return_type));
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
                        for (auto& p : event.params)
                            event_type.param_types.push_back(typeNodeToTypeInfo(p.type));
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

    if (arr_type.kind != TypeKind::Array) {
        error(expr.range, "cannot index non-array type '" + typeName(arr_type) + "'");
        return TypeInfo(TypeKind::Void);
    }
    if (!expectNumeric(idx_type, expr.index->range))
        return TypeInfo(TypeKind::Void);
    if (arr_type.element_type) return *arr_type.element_type;
    return TypeInfo(TypeKind::Void);
}

TypeInfo Sema::visitNewExpr(NewExpr& expr) {
    // Check if this is a generic instantiation
    if (!expr.type_args.empty()) {
        // Construct a temporary TypeNode to trigger monomorphization
        TypeNode tn;
        tn.class_name = expr.class_name;
        tn.type_args = expr.type_args;
        return typeNodeToTypeInfo(tn);
    }

    bool found = false;
    if (current_tu_) {
        for (auto& cls : current_tu_->classes) {
            if (cls.name == expr.class_name) { found = true; break; }
        }
    }
    if (!found)
        error(expr.range, "unknown class '" + expr.class_name + "'");

    TypeInfo result(TypeKind::Class);
    result.class_name = expr.class_name;
    return result;
}

TypeInfo Sema::visitThisExpr(ThisExpr& expr) {
    if (!in_class_method_) {
        error(expr.range, "'this' can only be used inside a class action");
        return TypeInfo(TypeKind::Void);
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
    auto target_type = visitExpr(*expr.target);
    auto value_type = visitExpr(*expr.value);
    if (!typesCompatible(target_type, value_type)) {
        error(expr.range, "cannot assign value of type '" +
              typeName(value_type) + "' to variable of type '" +
              typeName(target_type) + "'");
    }
    return target_type;
}

// ==============================
// Type utilities
// ==============================

TypeInfo Sema::typeNodeToTypeInfo(const TypeNode& node) {
    if (node.isArray()) {
        TypeInfo arr_type(TypeKind::Array);
        arr_type.array_size = node.array_size;
        if (node.element_type)
            arr_type.element_type = std::make_shared<TypeInfo>(typeNodeToTypeInfo(*node.element_type));
        return arr_type;
    }
    if (node.isClass()) {
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
            ClassDecl inst;
            inst.name = mangled;
            inst.range = original.range;

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

            // Copy function: section (skip body — not needed for type checking)
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
                // function: section bodies are not needed for instantiation
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
                for (auto& tp : cls.type_params) {
                    if (tp == lookup_name) {
                        TypeInfo tp_type(TypeKind::Int);
                        return tp_type;
                    }
                }
            }
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
        case TypeKind::Function: return "function";
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
        return true;
    }
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

TypeInfo Sema::visitLambda(LambdaExpr& expr) {
    // Create a hidden class: __lambda_N
    std::string cls_name = "__lambda_" + std::to_string(lambda_counter_++);
    expr.hidden_class_name = cls_name;

    ClassDecl cls;
    cls.name = cls_name;
    cls.range = expr.range;

    ActionDecl action;
    action.name = "__call";
    action.return_type = TypeNode(); // default Int (will be inferred from body)
    if (!expr.params.empty()) {
        // Use the last param's type as return type (default void)
        action.params = expr.params;
    }
    action.body = expr.body;
    action.range = expr.range;
    cls.actions.push_back(std::move(action));

    // Register class and add to TU
    if (current_tu_) {
        current_tu_->classes.push_back(std::move(cls));
        visitClassDecl(current_tu_->classes.back());
    }

    TypeInfo result(TypeKind::Class);
    result.class_name = cls_name;
    return result;
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
    add_intrinsic("__myp_math_exp", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_log", TypeKind::Double, {TypeKind::Double});
    add_intrinsic("__myp_math_pow", TypeKind::Double, {TypeKind::Double, TypeKind::Double});
    add_intrinsic("__myp_math_abs_int", TypeKind::Int, {TypeKind::Int});

    // File I/O intrinsics
    add_intrinsic("__myp_io_fopen", TypeKind::Int, {TypeKind::String, TypeKind::String});
    add_intrinsic("__myp_io_fclose", TypeKind::Void, {});
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
    add_intrinsic("__myp_assert_str_eq", TypeKind::Void, {TypeKind::String, TypeKind::String});
    add_intrinsic("__myp_test_report", TypeKind::Void, {TypeKind::String, TypeKind::Bool});
}

Sema::StmtResult Sema::visitTryStmt(TryStmt& stmt) {
    // Save and temporarily clear main function flag (try blocks allow calls)
    bool saved_main = in_main_function_;
    in_main_function_ = false;

    // Type-check the try block
    if (stmt.try_block) visitBlock(*stmt.try_block);

    // Restore main flag for catch/finally blocks
    in_main_function_ = saved_main;

    // Type-check the catch block with the variable declared
    if (stmt.catch_block) {
        symbol_table_.enterScope();
        TypeInfo catch_type(TypeKind::String);
        symbol_table_.declare(stmt.catch_var_name, catch_type);
        in_main_function_ = false; // catch also allows calls
        visitBlock(*stmt.catch_block);
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
