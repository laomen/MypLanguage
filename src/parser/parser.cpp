#include "mylang/Parser.h"

namespace mylang {

// 类型关键字 → TypeKind（显式转换 uint8(x) 的目标类型）

// ==============================
// Constructor & main entry
// ==============================

Parser::Parser(const std::vector<Token>& tokens, DiagnosticEngine& diag)
    : tokens_(tokens), diag_(diag) {}

std::unique_ptr<TranslationUnit> Parser::parse() {
    return parseProgram();
}

// ==============================
// Top-level: Program
// ==============================

std::unique_ptr<TranslationUnit> Parser::parseProgram() {
    auto tu = std::make_unique<TranslationUnit>();

    while (!isAtEnd()) {
        if (match(TokenKind::Keyword_import)) {
            tu->imports.push_back(parseImport());
        } else if (match(TokenKind::Keyword_class)) {
            auto cls = parseClass();
            if (cls) tu->classes.push_back(std::move(*cls));
        } else if (match(TokenKind::Keyword_struct)) {
            // parseStruct handles both file-level (`struct Name { }`) and
            // qualified (`struct ClassName::StructName { }`) definitions —
            // current_ is already past `struct` and points at the struct name.
            // (No cursor rewind here: rewinding to `struct` made parseStruct's
            // internal qualified check see the `struct` keyword instead of the
            // name, so `struct A::B { }` always failed with "expected struct
            // name".)
            auto st = parseStruct();
            if (st) tu->structs.push_back(std::move(*st));
        } else if (match(TokenKind::Keyword_bitfield)) {
            // bitfield Flags { bit a; bit[6] r; } —— 结构体位域（§5.1）
            auto bf = parseBitfieldDecl();
            if (bf) tu->bitfields.push_back(std::move(*bf));
        } else if (match(TokenKind::Keyword_interface)) {
            auto iface = parseInterface();
            if (iface) tu->interfaces.push_back(std::move(*iface));
        } else if (match(TokenKind::Keyword_mapping)) {
            auto mp = parseMapping();
            if (mp) tu->mappings.push_back(std::move(*mp));
        } else if (match(TokenKind::Keyword_ffi)) {
            auto ff = parseFFIDecl();
            if (ff) tu->ffis.push_back(std::move(*ff));
        } else if (match(TokenKind::Keyword_enum)) {
            auto en = parseEnumDecl();
            if (en) tu->enums.push_back(std::move(*en));
        } else if (match(TokenKind::Keyword_macro)) {
            auto m = parseMacroDecl();
            if (m) tu->macros.push_back(std::move(*m));
        } else if (peek().kind == TokenKind::At) {
            // Check for @static class (static is tokenized as Keyword_static)
            auto n1 = peekNext();
            auto n2 = peekNext2();
            if (n1.kind == TokenKind::Keyword_static &&
                n2.kind == TokenKind::Keyword_class) {
                // @static class — consume @static class and parse
                advance(); advance(); advance(); // skip @static class
                auto cls = parseClass();
                if (cls) { cls->is_static = true; tu->classes.push_back(std::move(*cls)); }
            } else if (n1.kind == TokenKind::Identifier && n1.value == "derive") {
                // @derive(X) class — 类级派生注解（@derive(Json) → 自动生成 toJson/fromJson）
                advance(); advance(); // @ derive
                consume(TokenKind::LeftParen, "expected '(' after @derive");
                std::string macro_name = parseIdentifier("expected derive macro name");
                consume(TokenKind::RightParen, "expected ')' after @derive");
                if (!check(TokenKind::Keyword_class)) {
                    diag_.error(peek().range, "expected 'class' after @derive(...)");
                    synchronize();
                } else {
                    advance(); // class
                    auto cls = parseClass();
                    if (cls) {
                        cls->derive = macro_name;
                        tu->classes.push_back(std::move(*cls));
                    }
                }
            } else {
                // @ annotation at top level → parse as function (@test, @startup)
                auto func = parseFunction();
                if (func) tu->functions.push_back(std::move(*func));
            }
        } else if (match(TokenKind::Keyword_const)) {
            // File-level const: const Type name = value;
            // Treat as function returning the value so sema/codegen handles it
            TypeNode ct = parseType();
            std::string cn = parseIdentifier("expected constant name");
            consume(TokenKind::Equal, "expected '=' in const declaration");
            auto ie = parseExpr();
            consume(TokenKind::Semicolon, "expected ';' after const declaration");
            FuncDecl fd;
            fd.name = cn;
            fd.return_type = ct;
            fd.is_const_decl = true;   // top-level const: compile-time value
            std::vector<std::unique_ptr<Stmt>> fb;
            fb.push_back(std::make_unique<ReturnStmt>(std::move(ie), SourceRange{}));
            fd.body = std::make_unique<BlockStmt>(std::move(fb), SourceRange{});
            tu->functions.push_back(std::move(fd));
        } else if (check(TokenKind::Identifier) && peek().value == "type" &&
                   tokens_[current_ + 1].kind == TokenKind::Identifier &&
                   tokens_[current_ + 2].kind == TokenKind::Equal) {
            // Type alias (contextual keyword): `type Name = Type;`
            // Only recognized in this exact top-level shape, so `type` remains
            // usable as an ordinary identifier elsewhere (additive, non-breaking).
            auto ta = parseTypeAlias();
            if (ta) tu->type_aliases.push_back(std::move(*ta));
        } else if (checkType() || check(TokenKind::Keyword_void) ||
                   (check(TokenKind::LeftParen) && scanFunctionType()) ||
                   (check(TokenKind::LeftParen) && scanTupleType())) {
            auto func = parseFunction();
            if (func) tu->functions.push_back(std::move(*func));
        } else {
            diag_.error(peek().range,
                std::string("unexpected token '") + Token::kindName(peek().kind) + "'");
            synchronize();
        }
    }

    return tu;
}

// ==============================
// Import: "import" identifier ";"
// ==============================

ImportDecl Parser::parseImport() {
    ImportDecl decl;
    decl.range = previous().range;

    // Support three forms:
    //   import ModuleName;        -- stdlib module (stdlib/ModuleName.myp)
    //   import gpu.backend;       -- stdlib 子模块（点分名 → stdlib/gpu/backend.myp）
    //   import "path/file.myp";   -- user file path (resolved relative to source file)
    if (check(TokenKind::StringLiteral)) {
        auto tok = advance();
        decl.is_path = true;
        decl.file_path = tok.value;
        decl.range = tok.range;
    } else {
        // 点分模块名：gpu / gpu.backend / gpu.backend_cuda / ...
        decl.module_name = parseIdentifier("expected module name after 'import'");
        while (match(TokenKind::Dot)) {
            std::string part = parseIdentifier("expected module name after '.'");
            decl.module_name = decl.module_name + "." + part;
        }
    }
    consume(TokenKind::Semicolon, "expected ';' after import");
    return decl;
}

// ==============================
// Class: "class" id "{" { section } [interface_class] "}"
// ==============================

std::unique_ptr<ClassDecl> Parser::parseClass() {
    auto cls = std::make_unique<ClassDecl>();
    cls->range = previous().range;
    cls->name = parseIdentifier("expected class name");

    if (match(TokenKind::Less)) {
        cls->type_params = parseTypeParamList();
        // Optional `where T : Interface` constraint clause (M2) — inside <...>
        parseTypeParamConstraints(*cls);
        consumeGenericClose("expected '>' after generic parameters");
    }

    consume(TokenKind::LeftBrace, "expected '{' after class name");

    while (!check(TokenKind::RightBrace) && !isAtEnd()) {
        if (check(TokenKind::Keyword_action) ||
            check(TokenKind::Keyword_event) ||
            check(TokenKind::Keyword_property) ||
            check(TokenKind::Keyword_function) ||
            check(TokenKind::Keyword_struct) ||
            check(TokenKind::Keyword_static)) {
            parseClassSection(*cls);
        } else if (check(TokenKind::Keyword_interface)) {
            parseInterfaceClassDecl(*cls);
        } else if (check(TokenKind::Keyword_const)) {
            // Class-level const: `const double THERMAL = 0.0253;` directly in the
            // class body. Stored as a const property (semantics identical to the
            // property: section; useful for per-instance physical constants).
            cls->properties.push_back(parsePropertyDecl());
        } else if (check(TokenKind::Identifier) && peek().value == "type" &&
                   tokens_[current_ + 1].kind == TokenKind::Identifier &&
                   tokens_[current_ + 2].kind == TokenKind::Equal) {
            // 关联类型绑定：type Item = int;  （§三-5，实现接口的关联类型）
            advance(); // 'type'
            std::string name = parseIdentifier("expected associated type name");
            consume(TokenKind::Equal, "expected '=' in associated type binding");
            TypeNode bt = parseType();
            consume(TokenKind::Semicolon, "expected ';' after associated type binding");
            cls->associated_type_bindings[name] = std::move(bt);
        } else {
            diag_.error(peek().range,
                "expected 'action:', 'event:', 'property:', 'function:', 'struct:', or 'interface class'");
            synchronize();
        }
    }

    consume(TokenKind::RightBrace, "expected '}' after class body");
    return cls;
}

void Parser::parseClassSection(ClassDecl& cls) {
    if (match(TokenKind::Keyword_action)) {
        consume(TokenKind::Colon, "expected ':' after 'action'");
        while (!check(TokenKind::RightBrace) &&
               !check(TokenKind::Keyword_event) &&
               !check(TokenKind::Keyword_property) &&
               !check(TokenKind::Keyword_function) &&
               !check(TokenKind::Keyword_struct) &&
               !check(TokenKind::Keyword_interface) &&
               !isAtEnd()) {
            if (checkType() || check(TokenKind::Keyword_void) || check(TokenKind::At)) {
                ActionDecl ad = parseActionDecl();
                // 函数名==类名 → 隐式构造器（可省略 @constructor）
                if (ad.name == cls.name) ad.has_constructor = true;
                cls.actions.push_back(std::move(ad));
            } else {
                break;
            }
        }
    } else if (match(TokenKind::Keyword_event)) {
        consume(TokenKind::Colon, "expected ':' after 'event'");
        while (!check(TokenKind::RightBrace) &&
               !check(TokenKind::Keyword_action) &&
               !check(TokenKind::Keyword_property) &&
               !check(TokenKind::Keyword_function) &&
               !check(TokenKind::Keyword_struct) &&
               !check(TokenKind::Keyword_interface) &&
               !isAtEnd()) {
            if (check(TokenKind::Identifier)) {
                cls.events.push_back(parseEventDecl());
            } else {
                break;
            }
        }
    } else if (match(TokenKind::Keyword_property)) {
        consume(TokenKind::Colon, "expected ':' after 'property'");
        while (!check(TokenKind::RightBrace) &&
               !check(TokenKind::Keyword_action) &&
               !check(TokenKind::Keyword_event) &&
               !check(TokenKind::Keyword_function) &&
               !check(TokenKind::Keyword_struct) &&
               !check(TokenKind::Keyword_interface) &&
               !isAtEnd()) {
            if (checkType() || check(TokenKind::Keyword_const) ||
                (check(TokenKind::At) && peekNext().value == "weak")) {
                cls.properties.push_back(parsePropertyDecl());
            } else {
                break;
            }
        }
    } else if (match(TokenKind::Keyword_function)) {
        consume(TokenKind::Colon, "expected ':' after 'function'");
        while (!check(TokenKind::RightBrace) &&
               !check(TokenKind::Keyword_action) &&
               !check(TokenKind::Keyword_event) &&
               !check(TokenKind::Keyword_property) &&
               !check(TokenKind::Keyword_struct) &&
               !check(TokenKind::Keyword_interface) &&
               !isAtEnd()) {
            if (checkType() || check(TokenKind::Keyword_void)) {
                auto func = parseFunction();
                if (func) {
                    // 函数名==类名 → 隐式构造器（可省略 @constructor）
                    if (func->name == cls.name) func->has_constructor = true;
                    cls.functions.push_back(std::move(*func));
                }
            } else {
                break;
            }
        }
    } else if (match(TokenKind::Keyword_struct)) {
        // Nested struct: struct Name { fields }
        auto st = parseStruct();
        if (st) {
            st->parent_class = cls.name;
            cls.structs.push_back(std::move(*st));
        }
    } else if (match(TokenKind::Keyword_static)) {
        consume(TokenKind::Colon, "expected ':' after 'static'");
        cls.is_static = true;
        while (!check(TokenKind::RightBrace) &&
               !check(TokenKind::Keyword_action) &&
               !check(TokenKind::Keyword_event) &&
               !check(TokenKind::Keyword_property) &&
               !check(TokenKind::Keyword_function) &&
               !check(TokenKind::Keyword_struct) &&
               !check(TokenKind::Keyword_interface) &&
               !isAtEnd()) {
            if (checkType() || check(TokenKind::Keyword_void) || check(TokenKind::At)) {
                cls.static_actions.push_back(parseActionDecl());
            } else {
                break;
            }
        }
    }
}

ActionDecl Parser::parseActionDecl() {
    ActionDecl decl;
    decl.range = previous().range;

    // Check for @ annotation
    if (peek().kind == TokenKind::At) {
        advance(); // consume @
        std::string annot = parseIdentifier("expected annotation name");
        if (annot == "startup") decl.has_startup = true;
        if (annot == "constructor") decl.has_constructor = true;
        if (annot == "test") decl.has_test = true;
        if (annot == "async") decl.has_async = true;
        if (annot == "coro") {
            decl.has_coro = true;
            // Optional: @coro(stack=N) — N = coroutine stack size in KB (default 128)
            if (check(TokenKind::LeftParen)) {
                consume(TokenKind::LeftParen, "expected '(' after '@coro'");
                std::string kw = parseIdentifier("expected 'stack' in @coro(stack=N)");
                if (kw != "stack") {
                    diag_.error(previous().range, "expected 'stack' in @coro(stack=N)");
                }
                consume(TokenKind::Equal, "expected '=' in @coro(stack=N)");
                if (check(TokenKind::IntegerLiteral)) {
                    try {
                        decl.coro_stack_kb = (int)std::stoll(advance().value);
                    } catch (...) {
                        diag_.error(previous().range, "invalid stack size in @coro(stack=N)");
                    }
                } else {
                    diag_.error(peek().range, "expected integer stack size (KB) in @coro(stack=N)");
                }
                consume(TokenKind::RightParen, "expected ')' after @coro(stack=N)");
            }
        }
        if (annot == "region") decl.has_region = true;
    }

    if (decl.has_constructor) {
        // 构造器：无返回类型（隐含 void），名称必须==类名（sema 校验）
        TypeNode vt;
        vt.basic_type = BuiltinType::Void;
        decl.return_type = vt;
        decl.name = parseIdentifier("expected constructor name");
    } else {
        decl.return_type = parseType();
        decl.name = parseIdentifier("expected action name");
        // Generic static method type params: List.map<T, U>(...)
        if (match(TokenKind::Less)) {
            if (check(TokenKind::Identifier)) {
                decl.type_params.push_back(parseIdentifier("expected type parameter name"));
                while (match(TokenKind::Comma)) {
                    decl.type_params.push_back(parseIdentifier("expected type parameter name"));
                }
            }
            // §9 泛型静态方法 where 约束：foo<T where T : Numeric>(...)，在 <...> 内
            parseGenericWhereClause(decl.type_params, decl.type_param_constraints);
            consumeGenericClose("expected '>' after generic type parameters");
        }
    }

    consume(TokenKind::LeftParen, "expected '(' after action name");
    if (!check(TokenKind::RightParen)) {
        decl.params = parseParamList(true);  // action/method/constructor: 允许默认值
    }
    consume(TokenKind::RightParen, "expected ')' after parameters");

    if (match(TokenKind::Semicolon)) {
        // Declaration only
    } else if (match(TokenKind::LeftBrace)) {
        auto block = parseBlock();
        decl.body = std::move(block);
    } else {
        diag_.error(peek().range, "expected ';' or '{' after action signature");
    }

    return decl;
}

EventDecl Parser::parseEventDecl() {
    EventDecl decl;
    decl.range = previous().range;
    decl.name = parseIdentifier("expected event name");

    consume(TokenKind::LeftParen, "expected '(' after event name");
    if (!check(TokenKind::RightParen)) {
        decl.params = parseParamList(false);  // event：无默认值
    }
    consume(TokenKind::RightParen, "expected ')' after parameters");
    consume(TokenKind::Semicolon, "expected ';' after event declaration");

    return decl;
}

// Parse param list where names are optional (for events); allow_default 控制
// 是否允许 `= expr` 默认值（仅函数/action/构造器；事件/枚举/lambda 不允许）。
std::vector<ParamDecl> Parser::parseParamList(bool allow_default) {
    std::vector<ParamDecl> params;
    params.push_back(parseParam(allow_default));
    while (match(TokenKind::Comma)) {
        params.push_back(parseParam(allow_default));
    }
    return params;
}

ParamDecl Parser::parseParam(bool allow_default) {
    ParamDecl param;
    param.range = peek().range;
    if (match(TokenKind::Keyword_ref)) {
        param.is_ref = true;
    }
    param.type = parseType();
    // Parameter name is optional for event declarations
    if (check(TokenKind::Identifier) || check(TokenKind::Comma) || check(TokenKind::RightParen)) {
        if (check(TokenKind::Identifier)) {
            param.name = parseIdentifier("expected parameter name");
            // §四-1 默认参数：`int b = 10`（仅命名参数、允许默认时）
            if (allow_default && match(TokenKind::Equal)) {
                param.default_expr = parseExpr();
            }
        }
    }
    return param;
}

// 解析调用实参 `( a, b, ... )`（含空括号）。调用方已把当前 token 定位到 '('；
// 本函数消费 '(' … ')' 并返回实参列表。
// 注意：`f(x = y)` 在此按普通赋值表达式解析（宏把赋值 AST 作实参，如
// `repeat(3, v = v + 10)`）；sema 在 normalizeCallArgs 中按「目标标识符是否匹配
// 形参名」把赋值实参重解释为命名实参（§四-1），从而与宏实参无歧义。
std::vector<std::unique_ptr<Expr>> Parser::parseCallArgs() {
    std::vector<std::unique_ptr<Expr>> args;
    consume(TokenKind::LeftParen, "expected '(' after arguments");
    if (!check(TokenKind::RightParen)) {
        args.push_back(parseExpr());
        while (match(TokenKind::Comma)) {
            args.push_back(parseExpr());
        }
    }
    consume(TokenKind::RightParen, "expected ')' after arguments");
    return args;
}

PropertyDecl Parser::parsePropertyDecl() {
    PropertyDecl decl;
    decl.range = previous().range;
    // M7: optional @weak annotation before the type (weak reference field).
    if (check(TokenKind::At)) {
        advance(); // consume @
        std::string annot = parseIdentifier("expected annotation name");
        if (annot == "weak") decl.weak = true;
        else diag_.error(previous().range, "unknown property annotation '@" + annot + "'");
    }
    // Check for 'const' before the type
    if (check(TokenKind::Keyword_const)) {
        advance(); // consume 'const'
        decl.is_const = true;
    }
    decl.type = parseType();
    decl.name = parseIdentifier("expected property name");

    if (match(TokenKind::Equal)) {
        decl.init_expr = parseExpr();
    }

    consume(TokenKind::Semicolon, "expected ';' after property declaration");
    return decl;
}

void Parser::parseInterfaceClassDecl(ClassDecl& cls) {
    match(TokenKind::Keyword_interface);
    consume(TokenKind::Keyword_class, "expected 'class' after 'interface'");
    cls.interface_class_name = parseIdentifier("expected interface class name");
    consume(TokenKind::Semicolon, "expected ';' after interface class declaration");
}

// ==============================
// Interface: "interface" id "{" { action ";" | event } "}"
// ==============================

std::unique_ptr<InterfaceDecl> Parser::parseInterface() {
    auto decl = std::make_unique<InterfaceDecl>();
    decl->range = previous().range;
    decl->name = parseIdentifier("expected interface name");

    consume(TokenKind::LeftBrace, "expected '{' after interface name");

    while (!check(TokenKind::RightBrace) && !isAtEnd()) {
        // Look ahead: if peek+1 is '(' it's an event; otherwise it's an action
        if (peek().kind == TokenKind::Identifier) {
            // 关联类型声明：type Item;  （§三-5）
            if (peek().value == "type" && peekNext().kind == TokenKind::Identifier) {
                advance(); // 'type'
                decl->associated_types.push_back(
                    parseIdentifier("expected associated type name"));
                consume(TokenKind::Semicolon, "expected ';' after associated type declaration");
                continue;
            }
            // Could be event name directly, or class-type return type for an action
            if (peekNext().kind == TokenKind::LeftParen) {
                decl->events.push_back(parseEventDecl());
            } else {
                decl->actions.push_back(parseActionDecl());
            }
        } else if (checkType() || check(TokenKind::Keyword_void)) {
            decl->actions.push_back(parseActionDecl());
        } else {
            diag_.error(peek().range, "expected action or event declaration");
            advance();
        }
    }

    consume(TokenKind::RightBrace, "expected '}' after interface body");
    return decl;
}

// ==============================
// Struct: "struct" id "{" { property | function } "}"
// or "struct" ClassName "::" StructName "{" { property | function } "}"
// ==============================

// 前瞻：当前 token 是 struct 方法声明（type [<...>] name '('）还是属性
// （type name ';'）。泛型返回类型（ArrayList<AstType> name）须跳过 <...> 才能
// 定位方法名——旧前瞻只跳一个类型 token，泛型返回方法被误判为属性。
bool Parser::aheadIsStructMethod() const {
    size_t ahead = current_;
    if (ahead >= tokens_.size()) return false;
    TokenKind k = tokens_[ahead].kind;
    if (!((k >= TokenKind::Type_byte && k <= TokenKind::Type_bitvector) ||
          k == TokenKind::Keyword_void || k == TokenKind::Identifier))
        return false;
    ahead++;
    // 跳过泛型实参 <...>（平衡，>> 作两个 '>' 计）。
    if (ahead < tokens_.size() && tokens_[ahead].kind == TokenKind::Less) {
        int depth = 0;
        while (ahead < tokens_.size()) {
            TokenKind g = tokens_[ahead].kind;
            if (g == TokenKind::Less) { depth++; }
            else if (g == TokenKind::Greater) {
                depth--;
                if (depth == 0) { ahead++; break; }
            } else if (g == TokenKind::GreaterGreater) {
                depth -= 2;
                if (depth <= 0) { ahead++; break; }
            }
            ahead++;
        }
    }
    // 方法名 identifier
    if (ahead < tokens_.size() && tokens_[ahead].kind == TokenKind::Identifier)
        ahead++;
    return ahead < tokens_.size() && tokens_[ahead].kind == TokenKind::LeftParen;
}

std::unique_ptr<StructDecl> Parser::parseStruct() {
    auto decl = std::make_unique<StructDecl>();
    decl->range = previous().range;

    // Check for qualified name: ClassName::StructName
    if (check(TokenKind::Identifier) && tokens_[current_ + 1].kind == TokenKind::DoubleColon) {
        decl->parent_class = parseIdentifier("expected class name");
        consume(TokenKind::DoubleColon, "expected '::'");
        decl->name = parseIdentifier("expected struct name");
    } else {
        decl->name = parseIdentifier("expected struct name");
    }

    consume(TokenKind::LeftBrace, "expected '{' after struct name");

    while (!check(TokenKind::RightBrace) && !isAtEnd()) {
        // action: section — 方法（含 @constructor 构造器；函数名==struct 名即隐式构造器）。
        // 遇到属性（type name ;）退出，交由下方主循环按属性解析。
        if (match(TokenKind::Keyword_action)) {
            consume(TokenKind::Colon, "expected ':' after 'action'");
            while (!check(TokenKind::RightBrace) && !isAtEnd() &&
                   (checkType() || check(TokenKind::Keyword_void) || check(TokenKind::At))) {
                if (check(TokenKind::At)) {
                    auto func = parseFunction();
                    if (func) {
                        if (func->name == decl->name) func->has_constructor = true;
                        decl->functions.push_back(std::move(*func));
                    }
                    continue;
                }
                // 前瞻：方法（type [<...>] name ( ... )）vs 属性（type name ;）
                if (!aheadIsStructMethod()) break;  // 属性 → 退出 action: 段，交主循环
                auto func = parseFunction();
                if (func) {
                    if (func->name == decl->name) func->has_constructor = true;
                    decl->functions.push_back(std::move(*func));
                }
            }
            continue;
        }
        // operator: section — 数学算子方法 (@op 绑定符号)
        // 只解析 @op 注解的方法; 裸属性/普通方法退出交由主循环处理
        if (match(TokenKind::Keyword_operator)) {
            consume(TokenKind::Colon, "expected ':' after 'operator'");
            while (!check(TokenKind::RightBrace) && !isAtEnd() && check(TokenKind::At)) {
                auto func = parseFunction();
                if (func) decl->functions.push_back(std::move(*func));
            }
            continue;
        }
        if (checkType() || check(TokenKind::Keyword_void) ||
            (check(TokenKind::At) && peekNext().value == "weak")) {
            // Look ahead to distinguish property vs method:
            //   property: type name ; or type name = expr ;
            //   method:   type [<...>] name ( params ) { body }
            bool is_method = aheadIsStructMethod();

            if (is_method) {
                auto func = parseFunction();
                if (func) {
                    // 函数名==struct 名 → 隐式构造器（可省略 @constructor）
                    if (func->name == decl->name) func->has_constructor = true;
                    decl->functions.push_back(std::move(*func));
                }
            } else {
                decl->properties.push_back(parsePropertyDecl());
            }
        } else {
            break;
        }
    }

    consume(TokenKind::RightBrace, "expected '}' after struct body");
    return decl;
}

// ==============================
// Bitfield: "bitfield" id "{" { bit [N] id ";" } "}"（§5.1）
// 字段类型只能是 bit（1 位）或 bit[N]（N 位）；按声明顺序从 LSB 起打包。
// ==============================

std::unique_ptr<BitfieldDecl> Parser::parseBitfieldDecl() {
    auto decl = std::make_unique<BitfieldDecl>();
    decl->range = previous().range;   // 'bitfield' token
    decl->name = parseIdentifier("expected bitfield name");
    consume(TokenKind::LeftBrace, "expected '{' after bitfield name");
    int offset = 0;
    while (!check(TokenKind::RightBrace) && !isAtEnd()) {
        BitfieldField f;
        f.range = peek().range;
        if (!match(TokenKind::Type_bit)) {
            diag_.error(peek().range,
                std::string("expected 'bit' or 'bit[N]' field in bitfield (got '") +
                Token::kindName(peek().kind) + "')");
            break;
        }
        f.bit_width = 1;
        if (match(TokenKind::LeftBracket)) {
            if (check(TokenKind::IntegerLiteral)) {
                try { f.bit_width = (int)std::stoll(advance().value); }
                catch (...) { f.bit_width = 0; }
            } else {
                diag_.error(peek().range, "expected integer width in bit[N]");
            }
            consume(TokenKind::RightBracket, "expected ']' after bit width");
        }
        if (f.bit_width <= 0) {
            diag_.error(f.range, "bitfield field width must be >= 1");
            f.bit_width = 1;
        }
        f.name = parseIdentifier("expected bitfield field name");
        f.offset = offset;
        offset += f.bit_width;
        decl->fields.push_back(std::move(f));
        consume(TokenKind::Semicolon, "expected ';' after bitfield field");
    }
    consume(TokenKind::RightBrace, "expected '}' after bitfield body");
    decl->total_bits = offset;
    return decl;
}

// ==============================
// Function: type id "(" params ")" block
// ==============================

std::unique_ptr<FuncDecl> Parser::parseFunction(bool allow_void_return) {
    auto func = std::make_unique<FuncDecl>();
    func->range = previous().range;
    // Check for @ annotation
    if (peek().kind == TokenKind::At) {
        advance();
        std::string annot;
        if (check(TokenKind::Keyword_macro)) { annot = "macro"; advance(); }
        else annot = parseIdentifier("expected annotation name");
        if (annot == "test") func->has_test = true;
        else if (annot == "constructor") func->has_constructor = true;
        else if (annot == "async") func->has_async = true;
        else if (annot == "region") func->has_region = true;
        else if (annot == "eval") func->has_eval = true;
        else if (annot == "macro") func->has_proc_macro = true;   // M4 过程宏
        else if (annot == "coro") {
            func->has_coro = true;
            // Optional: @coro(stack=N) — N = coroutine stack size in KB (default 128)
            if (check(TokenKind::LeftParen)) {
                consume(TokenKind::LeftParen, "expected '(' after '@coro'");
                std::string kw = parseIdentifier("expected 'stack' in @coro(stack=N)");
                if (kw != "stack") {
                    diag_.error(previous().range, "expected 'stack' in @coro(stack=N)");
                }
                consume(TokenKind::Equal, "expected '=' in @coro(stack=N)");
                if (check(TokenKind::IntegerLiteral)) {
                    try {
                        func->coro_stack_kb = (int)std::stoll(advance().value);
                    } catch (...) {
                        diag_.error(previous().range, "invalid stack size in @coro(stack=N)");
                    }
                } else {
                    diag_.error(peek().range, "expected integer stack size (KB) in @coro(stack=N)");
                }
                consume(TokenKind::RightParen, "expected ')' after @coro(stack=N)");
            }
        }
        else if (annot == "op") {
            // @op("+") — operator symbol is a string literal
            consume(TokenKind::LeftParen, "expected '(' after '@op'");
            if (check(TokenKind::StringLiteral)) {
                std::string s = advance().value;  // e.g. "+"
                static const std::string kOps[] = {
                    "+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">="
                };
                bool valid = false;
                for (auto& op : kOps) if (op == s) { valid = true; break; }
                if (valid) func->op_symbol = s;
                else diag_.error(previous().range,
                    "unsupported operator symbol '@op(\"" + s + "\")'");
            } else {
                diag_.error(peek().range,
                    "expected operator symbol string in @op(\"...\")");
            }
            consume(TokenKind::RightParen, "expected ')' after operator symbol");
        }
    }
    // Check for generic type params before return type
    if (check(TokenKind::Identifier) && peekNext().kind == TokenKind::Less) {
        // Generic function: T foo<T>(...) — type param before function name
        // Actually, generic func syntax: T max<T>(T a, T b) —  parse it differently
        // For now, we detect <T> after the function name
    }
    if (func->has_constructor) {
        // 构造器：无返回类型（隐含 void），名称必须==类名（sema 校验）
        TypeNode vt;
        vt.basic_type = BuiltinType::Void;
        func->return_type = vt;
        func->name = parseIdentifier("expected constructor name");
    } else {
        func->return_type = parseType();
        func->name = parseIdentifier("expected function name");
        // Generic function type params: foo<T, U>(...)
        if (match(TokenKind::Less)) {
            if (check(TokenKind::Identifier)) {
                func->type_params.push_back(parseIdentifier("expected type parameter name"));
                while (match(TokenKind::Comma)) {
                    func->type_params.push_back(parseIdentifier("expected type parameter name"));
                }
            }
            // §9 泛型函数 where 约束：fn<T where T : Numeric>(...)，在 <...> 内
            parseGenericWhereClause(func->type_params, func->type_param_constraints);
            consumeGenericClose("expected '>' after generic type parameters");
        }
    }

    consume(TokenKind::LeftParen, "expected '(' after function name");
    if (!check(TokenKind::RightParen)) {
        func->params = parseParamList(true);  // 顶层函数：允许默认值
    }
    consume(TokenKind::RightParen, "expected ')' after parameters");

    consume(TokenKind::LeftBrace, "expected '{' for function body");
    func->body = parseBlock();
    return func;
}

// ==============================
// Mapping: "mapping" "(" ")" block
// ==============================

std::unique_ptr<MappingDecl> Parser::parseMapping() {
    auto decl = std::make_unique<MappingDecl>();
    decl->range = previous().range;

    consume(TokenKind::LeftParen, "expected '(' after 'mapping'");
    consume(TokenKind::RightParen, "expected ')' after '('");

    // Check for @scope annotation
    if (match(TokenKind::At)) {
        std::string annot = parseIdentifier("expected annotation name");
        if (annot == "scope") {
            decl->has_scope = true;
        } else {
            diag_.error(previous().range,
                std::string("unknown annotation '@" + annot + "'"));
        }
    }

    consume(TokenKind::LeftBrace, "expected '{' for mapping body");

    while (!check(TokenKind::RightBrace) && !isAtEnd()) {
        size_t before = current_;
        // Parse the event source (first node)
        MappingNode first_node;
        first_node.range = peek().range;
        first_node.source_name = parseIdentifier("expected instance/class name in mapping");
        consume(TokenKind::Dot, "expected '.' in mapping chain");
        first_node.member_name = parseIdentifier("expected event/action name in mapping");

        // Optional 'where' clause
        std::shared_ptr<Expr> where_expr;
        if (match(TokenKind::Keyword_where)) {
            where_expr = std::shared_ptr<Expr>(parseExpr().release());
        }

        // Parse arrow
        consume(TokenKind::Arrow, "expected '->' after event");

        // Parse one or more targets separated by commas
        bool first_target = true;
        do {
            if (!first_target) {
                match(TokenKind::Comma); // consume the comma
            }
            first_target = false;

            MappingChain chain;
            chain.range = peek().range;
            chain.where_expr = where_expr;
            // Clone the first node for each chain
            MappingNode ev_node;
            ev_node.range = first_node.range;
            ev_node.source_name = first_node.source_name;
            ev_node.member_name = first_node.member_name;
            chain.nodes.push_back(std::move(ev_node));

            // Parse target: instance.action, functionName, or lambda
            // Support chained targets: source.e -> t1.a1 -> t2.a2, t3.a3;
            // Also: source.e -> processData -> output.show;
            // Also: source.e -> (int v) => { v * 2 } -> output.show;
            while (true) {
                MappingNode target_node;
                target_node.range = peek().range;

                if (check(TokenKind::LeftParen)) {
                    // Lambda expression as mapping chain node
                    match(TokenKind::LeftParen); // consume '(' (parseLambdaExpr expects it consumed)
                    auto lambda_expr = parseLambdaExpr();
                    target_node.is_lambda = true;
                    target_node.lambda = std::shared_ptr<LambdaExpr>(
                        static_cast<LambdaExpr*>(lambda_expr.release()));
                } else {
                    target_node.source_name = parseIdentifier("expected target name in mapping");

                    // Check for delay(ms) or throttle(ms) transformer
                    if (match(TokenKind::LeftParen)) {
                        int param = 0;
                        if (!check(TokenKind::RightParen)) {
                            auto param_expr = parseExpr();
                            if (auto* int_expr = dynamic_cast<IntegerLiteralExpr*>(param_expr.get()))
                                param = (int)int_expr->value;
                        }
                        consume(TokenKind::RightParen, "expected ')' after transformer param");
                        target_node.is_transformer = true;
                        target_node.transformer_param = param;
                        if (target_node.source_name == "delay")
                            target_node.transformer_kind = 0;
                        else if (target_node.source_name == "throttle")
                            target_node.transformer_kind = 1;
                        else
                            diag_.error(target_node.range,
                                "unknown transformer '" + target_node.source_name + "'");
                    } else if (match(TokenKind::Dot)) {
                        // instance.action — class method call
                        target_node.member_name = parseIdentifier("expected action name");
                        target_node.is_function = false;
                    } else {
                        // Bare identifier — file-level function call
                        target_node.is_function = true;
                    }
                }

                chain.nodes.push_back(std::move(target_node));

                if (match(TokenKind::Arrow)) {
                    continue; // more chained targets
                } else {
                    break; // end of this chain
                }
            }

            decl->chains.push_back(std::move(chain));
        } while (check(TokenKind::Comma));

        consume(TokenKind::Semicolon, "expected ';' after mapping chain");

        // Error-recovery progress guarantee (same as parseBlock): if this chain
        // consumed nothing, advance one token so the mapping loop terminates.
        if (current_ == before) {
            advance();
        }
    }

    consume(TokenKind::RightBrace, "expected '}' after mapping body");
    return decl;
}

// ==============================
// Statements
// ==============================


// (T1 a, T2 b) = expr;   (declaration)   or   (a, b) = expr;   (assignment)






// for (x in coll) / for (T x in coll) / for x in coll — 集合迭代（§四-2）






// ==============================
// Expressions (precedence climbing)
// ==============================

// Deep-clone an expression (used for compound assignment desugaring)


// Pipe: expr "|>" expr  (low precedence, left-assoc) — A |> Op1 |> Op2


// Ternary conditional: cond ? true_expr : false_expr










// Range: expr ".." expr  (lower precedence than additive)





// ==============================
// Types
// ==============================


// Assumes the '(' introducing the lambda's parameter list is already consumed.

// ==============================
// Macro: "macro" id "(" { "$" id } ")" block
// ==============================




// ==============================
// Helpers
// ==============================



// Diagnostic-free lookahead for a generic-call arg list: `<Type, ...> (`.
// Assumes the current token is '<'. Never emits errors — probes must be silent
// so comparisons like `E < energies[mid]` don't get spurious diagnostics.

// Diagnostic-free lookahead for a function type: `(Type, ...) ->`.
// Assumes the current token is '('. Never emits errors.

// Diagnostic-free lookahead for a tuple type: `(Type, ...)` with a top-level
// comma and ')' NOT followed by '->' (function types are handled by
// scanFunctionType first; `(int)` stays a parenthesized single type).

// Diagnostic-free lookahead for a tuple literal expression: `(a, b, ...)`
// with a top-level comma before the matching ')' and ')' NOT followed by
// '=>' (a lambda `(x, y) => ...` has params, not a tuple).

// Diagnostic-free lookahead for a destructuring statement:
// `(...) = expr;` — a top-level comma inside parens, ')' followed by '='.

// Diagnostic-free lookahead for a tuple-typed variable declaration:
// `(int, int) name = ...` — top-level comma inside parens, ')' followed by an
// Identifier (the variable name). This disambiguates from destructuring
// `(int a, int b) = ...` (')' followed by '=').


SourceRange Parser::tokenRange(const Token& tok) const {
    return tok.range;
}

// ==============================
// Utility
// ==============================

const Token& Parser::peek() const {
    // Synthetic '>' pushed by nested-generic split takes precedence.
    if (!pending_.empty()) return pending_.back();
    // Return EOF token if out of bounds
    if (current_ >= tokens_.size()) {
        static Token eof(TokenKind::EndOfFile, SourceRange{}, "");
        return eof;
    }
    return tokens_[current_];
}

const Token& Parser::peekNext() const {
    if (!pending_.empty()) {
        // Pending token is the current one; the next is the real current token.
        if (current_ >= tokens_.size()) {
            static Token eof(TokenKind::EndOfFile, SourceRange{}, "");
            return eof;
        }
        return tokens_[current_];
    }
    size_t idx = current_ + 1;
    if (idx >= tokens_.size()) {
        static Token eof(TokenKind::EndOfFile, SourceRange{}, "");
        return eof;
    }
    return tokens_[idx];
}

const Token& Parser::peekNext2() const {
    size_t idx = current_ + 2;
    if (idx >= tokens_.size()) {
        static Token eof(TokenKind::EndOfFile, SourceRange{}, "");
        return eof;
    }
    return tokens_[idx];
}

const Token& Parser::peekNext3() const {
    size_t idx = current_ + 3;
    if (idx >= tokens_.size()) {
        static Token eof(TokenKind::EndOfFile, SourceRange{}, "");
        return eof;
    }
    return tokens_[idx];
}

const Token& Parser::previous() const {
    static Token eof(TokenKind::EndOfFile, SourceRange{}, "");
    if (current_ == 0 || current_ > tokens_.size()) return eof;
    return tokens_[current_ - 1];
}

Token Parser::advance() {
    if (!pending_.empty()) {
        Token t = pending_.back();
        pending_.pop_back();
        return t;
    }
    if (!isAtEnd()) ++current_;
    return previous();
}

bool Parser::check(TokenKind kind) const {
    return peek().kind == kind;
}

bool Parser::match(TokenKind kind) {
    if (check(kind)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::matchAny(std::initializer_list<TokenKind> kinds) {
    for (auto kind : kinds) {
        if (match(kind)) return true;
    }
    return false;
}

Token Parser::consume(TokenKind kind, const std::string& error_msg) {
    if (check(kind)) return advance();
    diag_.error(peek().range, error_msg);
    return peek();
}

Token Parser::consumeGenericClose(const std::string& error_msg) {
    if (check(TokenKind::Greater)) return advance();
    if (check(TokenKind::GreaterGreater)) {
        // Nested generics: `Box<Vec<int>>` lexes the closing `>>` as one token.
        // Consume it and push a synthetic '>' so the enclosing generic context
        // still sees its own close (peek/advance consult pending_ first).
        Token split = advance();
        split.kind = TokenKind::Greater;
        pending_.push_back(std::move(split));
        return split;
    }
    diag_.error(peek().range, error_msg);
    return peek();
}

bool Parser::isAtEnd() const {
    if (current_ >= tokens_.size()) return true;
    return peek().kind == TokenKind::EndOfFile;
}

Token Parser::synchronize() {
    advance();
    while (!isAtEnd()) {
        if (previous().kind == TokenKind::Semicolon) break;
        switch (peek().kind) {
            case TokenKind::Keyword_class:
            case TokenKind::Keyword_enum:
            case TokenKind::Keyword_match:
            case TokenKind::Keyword_import:
            case TokenKind::Keyword_mapping:
            case TokenKind::Keyword_if:
            case TokenKind::Keyword_while:
            case TokenKind::Keyword_for:
            case TokenKind::Keyword_return:
                return previous();
            default:
                advance();
        }
    }
    return previous();
}

// ==============================
// Enum: "enum" id "{" { id ["(" params ")"] ";" } "}"
// ==============================


/// Parse the optional `where T : Interface [, U : J]` clause after generic
/// params. Fills type_param_constraints on \p cls. Returns false on error.





// ==============================
// Type alias: "type" Identifier "=" Type ";"
// (contextual keyword — dispatched from parseProgram only in this exact shape)
// ==============================


// ==============================
// Match: "match" "(" expr ")" "{" { arm } "}"
// arm: EnumType.Variant [ "(" id ("," id)* ")" ] "=>" block
// ==============================



} // namespace mylang

