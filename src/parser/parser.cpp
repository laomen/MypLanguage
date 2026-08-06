#include "mylang/Parser.h"

namespace mylang {

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
            // Check if it's ClassName::StructName form
            if (check(TokenKind::Identifier) && tokens_[current_ + 1].kind == TokenKind::DoubleColon) {
                // External struct definition: ClassName::StructName { fields }
                // Push back struct token and let parseStruct handle qualified names
                current_--; // go back to 'struct'
                auto st = parseStruct();
                if (st) tu->structs.push_back(std::move(*st));
            } else {
                // File-level struct
                auto st = parseStruct();
                if (st) tu->structs.push_back(std::move(*st));
            }
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

    // Support two forms:
    //   import ModuleName;        -- stdlib module (loaded from stdlib/ModuleName.myp)
    //   import "path/file.myp";   -- user file path (loaded relative to source file)
    if (check(TokenKind::StringLiteral)) {
        auto tok = advance();
        decl.is_path = true;
        decl.file_path = tok.value;
        decl.range = tok.range;
    } else {
        decl.module_name = parseIdentifier("expected module name after 'import'");
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
        consume(TokenKind::Greater, "expected '>' after generic parameters");
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
            if (checkType() || check(TokenKind::Keyword_const)) {
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
            consume(TokenKind::Greater, "expected '>' after generic type parameters");
        }
    }

    consume(TokenKind::LeftParen, "expected '(' after action name");
    if (!check(TokenKind::RightParen)) {
        decl.params = parseParamList();
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
        decl.params = parseParamList();
    }
    consume(TokenKind::RightParen, "expected ')' after parameters");
    consume(TokenKind::Semicolon, "expected ';' after event declaration");

    return decl;
}

// Parse param list where names are optional (for events)
std::vector<ParamDecl> Parser::parseParamList() {
    std::vector<ParamDecl> params;
    params.push_back(parseParam());
    while (match(TokenKind::Comma)) {
        params.push_back(parseParam());
    }
    return params;
}

ParamDecl Parser::parseParam() {
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
        }
    }
    return param;
}

PropertyDecl Parser::parsePropertyDecl() {
    PropertyDecl decl;
    decl.range = previous().range;
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
                // 前瞻：方法（type name ( ... )）vs 属性（type name ;）
                int ahead = current_;
                auto isTypeKind = [](TokenKind k) {
                    return (k >= TokenKind::Type_byte && k <= TokenKind::Type_string) ||
                           k == TokenKind::Keyword_void || k == TokenKind::Identifier;
                };
                if (ahead < (int)tokens_.size() && isTypeKind(tokens_[ahead].kind)) {
                    ahead++;
                    if (ahead < (int)tokens_.size() && tokens_[ahead].kind == TokenKind::Identifier)
                        ahead++;
                }
                bool is_method = (ahead < (int)tokens_.size() &&
                                 tokens_[ahead].kind == TokenKind::LeftParen);
                if (!is_method) break;  // 属性 → 退出 action: 段，交主循环
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
        if (checkType() || check(TokenKind::Keyword_void)) {
            // Look ahead up to 3 tokens to distinguish property vs method:
            //   property: type name ; or type name = expr ;
            //   method:   type name ( params ) { body }
            // Skip ahead to peek after the type and identifier tokens
            int ahead = current_;
            // Skip type tokens (may be void too)
            auto isTypeKind = [](TokenKind k) {
                return (k >= TokenKind::Type_byte && k <= TokenKind::Type_string) ||
                       k == TokenKind::Keyword_void ||
                       k == TokenKind::Identifier;
            };
            if (ahead < (int)tokens_.size() && isTypeKind(tokens_[ahead].kind)) {
                ahead++;
                // Skip identifier (the name)
                if (ahead < (int)tokens_.size() && tokens_[ahead].kind == TokenKind::Identifier)
                    ahead++;
            }
            bool is_method = (ahead < (int)tokens_.size() &&
                             tokens_[ahead].kind == TokenKind::LeftParen);

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
            consume(TokenKind::Greater, "expected '>' after generic type parameters");
        }
    }

    consume(TokenKind::LeftParen, "expected '(' after function name");
    if (!check(TokenKind::RightParen)) {
        func->params = parseParamList();
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

std::unique_ptr<Stmt> Parser::parseStatement() {
    if (match(TokenKind::LeftBrace)) {
        return parseBlock();
    }
    if (match(TokenKind::Keyword_if)) {
        return parseIfStmt();
    }
    if (match(TokenKind::Keyword_while)) {
        return parseWhileStmt();
    }
    // @parallel for
    if (check(TokenKind::At)) {
        advance(); // consume @
        std::string annot = parseIdentifier("expected annotation name");
        if (annot == "parallel") {
            if (!check(TokenKind::Keyword_for)) {
                diag_.error(previous().range, "'@parallel' must be followed by 'for'");
                // Fall through to normal statement parsing
                // Don't consume the for — we'll return an error expression
            } else {
                advance(); // consume 'for'
                auto stmt = parseForStmt();
                if (auto* fs = dynamic_cast<ForStmt*>(stmt.get())) {
                    fs->parallel = true;
                }
                return stmt;
            }
        } else if (annot == "gpu") {
            if (!check(TokenKind::Keyword_for)) {
                diag_.error(previous().range, "'@gpu' must be followed by 'for'");
            } else {
                advance();
                auto stmt = parseForStmt();
                if (auto* fs = dynamic_cast<ForStmt*>(stmt.get())) {
                    fs->gpu = true;
                }
                return stmt;
            }
        } else {
            diag_.error(previous().range,
                std::string("unknown annotation '@" + annot + "' in statement"));
        }
    }

    if (match(TokenKind::Keyword_for)) {
        return parseForStmt();
    }
    if (match(TokenKind::Keyword_return)) {
        return parseReturnStmt();
    }
    if (match(TokenKind::Keyword_break)) {
        return parseBreakStmt();
    }
    if (match(TokenKind::Keyword_continue)) {
        return parseContinueStmt();
    }
    if (match(TokenKind::Keyword_await)) {
        return parseAwaitStmt();
    }
    if (match(TokenKind::Keyword_mapping)) {
        return parseMappingStmt();
    }
    if (match(TokenKind::Keyword_match)) {
        return parseMatchStmt();
    }
    if (match(TokenKind::Keyword_try)) {
        return parseTryStmt();
    }
    if (match(TokenKind::Keyword_throw)) {
        // throw;  → bare rethrow of the current exception (only valid in a catch)
        // throw expr;  → string shortcut or exception object
        if (check(TokenKind::Semicolon)) {
            advance();
            return std::make_unique<ThrowStmt>(nullptr, previous().range);
        }
        auto e = parseExpr();
        consume(TokenKind::Semicolon, "expected ';' after throw expression");
        return std::make_unique<ThrowStmt>(std::move(e), previous().range);
    }
    if (match(TokenKind::Keyword_const)) {
        // const Type name [= expr]; — consume const, parse rest normally
        return parseVarDeclStmt();
    }
    if (check(TokenKind::Dollar) && isMacroStmtPlaceholder()) {
        // Macro statement placeholder: `$param` as a standalone statement in a
        // macro body (e.g. `repeat($n, $body)`). No trailing ';' required.
        // Only matched when `$param` is followed by '}' / ';' / EOF; otherwise
        // `$param` is an expression (e.g. `$x = ...`) handled by parsePrimary.
        advance();  // consume $
        std::string pname = parseIdentifier("expected macro parameter name after '$'");
        if (match(TokenKind::Semicolon)) { /* optional */ }
        return std::make_unique<ExprStmt>(
            std::make_unique<MacroParamExpr>(pname, previous().range),
            previous().range);
    }
    if (checkType()) {
        // `Type $name ...` — quote interpolation variable decl (M4): always a
        // variable declaration (an expression can't start with `Type $`).
        if (peekNext().kind == TokenKind::Dollar) {
            return parseVarDeclStmt();
        }
        // For identifiers: use lookahead to disambiguate
        // Identifier Identifier → type + var name (var decl)
        // Identifier [ Integer ] Identifier → type[size] name (array var decl)
        // Identifier [ or ( or =  → expression (subscript/call/assign)
        if (peek().kind == TokenKind::Identifier) {
            // Check if next token is also an Identifier (var decl)
            size_t next = current_ + 1;
            if (next < tokens_.size() && tokens_[next].kind == TokenKind::Identifier) {
                return parseVarDeclStmt();
            }
            // 限定类型：ClassName::StructType name / ClassName::AssocType name
            //（:: 仅类型用途——嵌套 struct / 关联类型引用）
            if (next < tokens_.size() && tokens_[next].kind == TokenKind::DoubleColon &&
                next + 1 < tokens_.size() && tokens_[next + 1].kind == TokenKind::Identifier) {
                return parseVarDeclStmt();
            }
            // Check if next token is [ (array type): IOperator[2] name
            // Use 5-token lookahead: Identifier [ Integer ] Identifier → var decl
            if (next < tokens_.size() && tokens_[next].kind == TokenKind::LeftBracket) {
                size_t n2 = next + 1;
                size_t n3 = next + 2;
                size_t n4 = next + 3;
                // Identifier [ ] Identifier → dynamic array var decl (T[] name)
                // e.g. `T[] nd = new T[n];` / `T[] nd;` inside a generic body.
                if (n2 < tokens_.size() && tokens_[n2].kind == TokenKind::RightBracket
                    && n3 < tokens_.size() && tokens_[n3].kind == TokenKind::Identifier) {
                    return parseVarDeclStmt();
                }
                if (n3 < tokens_.size() && tokens_[n2].kind == TokenKind::IntegerLiteral
                    && tokens_[n3].kind == TokenKind::RightBracket
                    && n4 < tokens_.size() && tokens_[n4].kind == TokenKind::Identifier) {
                    // Identifier [ Integer ] Identifier → type[size] name: var decl
                    return parseVarDeclStmt();
                }
                // For Identifier [ Integer ] = → could be var decl with init or subscript.
                // Probe: save, parse as type, check if next is identifier or =
                if (n3 < tokens_.size() && tokens_[n2].kind == TokenKind::IntegerLiteral
                    && tokens_[n3].kind == TokenKind::RightBracket
                    && n4 < tokens_.size() && tokens_[n4].kind == TokenKind::Equal) {
                    auto saved = current_;
                    TypeNode dummy = parseType();
                    if (!isAtEnd() && (peek().kind == TokenKind::Identifier
                                       || check(TokenKind::Keyword_void)
                                       || checkType())) {
                        current_ = saved;
                        return parseVarDeclStmt();
                    }
                    // Not a var decl — restore, fall through to expression parsing
                    current_ = saved;
                }
            }
            // Check if next token is < (generic type args): Box<int> varname
            if (next < tokens_.size() && tokens_[next].kind == TokenKind::Less) {
                // Try parsing as type; if after that we see an identifier, it's a var decl
                auto saved = current_;
                TypeNode dummy = parseType();
                if (!isAtEnd() && peek().kind == TokenKind::Identifier) {
                    current_ = saved;
                    return parseVarDeclStmt();
                }
                current_ = saved;
            }
            // Otherwise it's an expression (subscript, call, etc.)
        } else {
            auto saved = current_;
            TypeNode dummy = parseType();
            if (!isAtEnd() && peek().kind == TokenKind::Identifier) {
                current_ = saved;
                return parseVarDeclStmt();
            }
            current_ = saved;
        }
    }

    // Function-type var decl: `(A) -> B name = ...` (starts with '(').
    if (check(TokenKind::LeftParen) && scanFunctionType()) {
        auto saved = current_;
        TypeNode dummy = parseType();
        if (!isAtEnd() && peek().kind == TokenKind::Identifier) {
            current_ = saved;
            return parseVarDeclStmt();
        }
        current_ = saved;
    }

    // Destructure statement: (A a, B b) = expr;  or  (a, b) = expr;
    if (check(TokenKind::LeftParen) && scanDestructureAssign()) {
        return parseDestructureStmt();
    }

    // Tuple-type variable declaration: (int, int) name = ...;
    if (check(TokenKind::LeftParen) && scanTupleVarDecl()) {
        auto saved = current_;
        TypeNode dummy = parseType();
        if (!isAtEnd() && peek().kind == TokenKind::Identifier) {
            current_ = saved;
            return parseVarDeclStmt();
        }
        current_ = saved;
    }

    auto expr = parseExpr();
    consume(TokenKind::Semicolon, "expected ';' after expression");
    return std::make_unique<ExprStmt>(std::move(expr), previous().range);
}

// (T1 a, T2 b) = expr;   (declaration)   or   (a, b) = expr;   (assignment)
std::unique_ptr<Stmt> Parser::parseDestructureStmt() {
    DestructureTarget root;
    root.range = peek().range;
    advance(); // consume '('

    // Parse one element (recursive): nested ( ... ) or [Type] name leaf.
    std::function<void(DestructureTarget&)> parse_el =
        [&](DestructureTarget& t) {
            t.range = peek().range;
            if (check(TokenKind::LeftParen)) {
                advance();
                while (true) {
                    DestructureTarget child;
                    parse_el(child);
                    t.elements.push_back(std::move(child));
                    if (!match(TokenKind::Comma)) break;
                    if (check(TokenKind::RightParen)) break; // trailing comma
                }
                consume(TokenKind::RightParen, "expected ')' in destructure target");
                return;
            }
            // Leaf: [Type] name
            bool has_type = false;
            if (peek().kind == TokenKind::Identifier) {
                // Type Name (custom class / generic) vs bare Name:
                // `Foo bar` / `Foo<...> bar` / `Foo[] bar` → typed leaf.
                auto nt = peekNext().kind;
                if (nt == TokenKind::Identifier || nt == TokenKind::Less ||
                    nt == TokenKind::LeftBracket) {
                    t.type = parseType();
                    has_type = true;
                }
            } else if (isTypeToken(peek().kind)) {
                t.type = parseType();
                has_type = true;
            }
            t.has_type = has_type;
            t.name = parseIdentifier("expected variable name in destructure target");
        };

    while (true) {
        DestructureTarget child;
        parse_el(child);
        root.elements.push_back(std::move(child));
        if (!match(TokenKind::Comma)) break;
        if (check(TokenKind::RightParen)) break; // trailing comma
    }
    consume(TokenKind::RightParen, "expected ')' after destructure target");
    consume(TokenKind::Equal, "expected '=' after destructure target");

    auto value = parseExpr();
    consume(TokenKind::Semicolon, "expected ';' after destructure assignment");

    // is_decl = any leaf carries a type annotation
    bool is_decl = false;
    std::function<void(const DestructureTarget&)> any_type =
        [&](const DestructureTarget& t) {
            if (is_decl) return;
            if (!t.name.empty() && t.has_type) is_decl = true;
            for (auto& c : t.elements) any_type(c);
        };
    for (auto& c : root.elements) any_type(c);
    return std::make_unique<DestructureStmt>(std::move(root), std::move(value), is_decl, previous().range);
}

std::unique_ptr<BlockStmt> Parser::parseBlock() {
    std::vector<std::unique_ptr<Stmt>> stmts;
    SourceRange r = previous().range;

    while (!check(TokenKind::RightBrace) && !isAtEnd()) {
        size_t before = current_;
        auto stmt = parseStatement();
        if (stmt) stmts.push_back(std::move(stmt));
        // Error-recovery progress guarantee: if parseStatement consumed nothing
        // (e.g. a bare keyword like `class` that parsePrimary rejects without
        // advancing), force-advance one token so the block loop terminates.
        // Without this, malformed input (common mid-edit / LSP) loops forever
        // and exhausts memory.
        if (current_ == before) {
            advance();
        }
    }

    consume(TokenKind::RightBrace, "expected '}' after block");
    return std::make_unique<BlockStmt>(std::move(stmts), r);
}

std::unique_ptr<Stmt> Parser::parseVarDeclStmt() {
    // Parse the shared type first
    TypeNode shared_type = parseType();

    std::vector<VarDecl> decls;

    // Parse one or more comma-separated variable names
    do {
        VarDecl decl;
        decl.range = peek().range;
        decl.type = shared_type;
        // `$name` = quote interpolation placeholder for the variable name (M4).
        if (match(TokenKind::Dollar)) {
            decl.name = "$" + parseIdentifier("expected variable name after '$'");
        } else {
            decl.name = parseIdentifier("expected variable name");
        }

        // Init expression only allowed on the first variable
        // (or on each? C allows int a=1, b=2; — support that too)
        if (match(TokenKind::Equal)) {
            decl.init_expr = parseExpr();
        }

        // Annotation (@thread, @threadpool)
        if (match(TokenKind::At)) {
            std::string annot = parseIdentifier("expected annotation name");
            if (annot == "thread") {
                decl.has_thread_annotation = true;
            } else if (annot == "threadpool") {
                decl.has_threadpool_annotation = true;
            } else {
                diag_.error(previous().range,
                    std::string("unknown annotation '@" + annot + "'"));
            }
        }

        decls.push_back(std::move(decl));
    } while (match(TokenKind::Comma));

    consume(TokenKind::Semicolon, "expected ';' after variable declaration");

    // If only one decl, return it directly
    if (decls.size() == 1) {
        return std::make_unique<VarDeclStmt>(std::move(decls[0]));
    }

    // Multiple decls: single VarDeclStmt with all decls
    return std::make_unique<VarDeclStmt>(std::move(decls), shared_type.range);
}

std::unique_ptr<Stmt> Parser::parseIfStmt() {
    SourceRange r = previous().range;
    consume(TokenKind::LeftParen, "expected '(' after 'if'");
    auto cond = parseExpr();
    consume(TokenKind::RightParen, "expected ')' after condition");
    auto then_block = parseStatement();
    std::unique_ptr<Stmt> else_block;
    if (match(TokenKind::Keyword_else)) {
        else_block = parseStatement();
    }
    return std::make_unique<IfStmt>(std::move(cond), std::move(then_block),
                                     std::move(else_block), r);
}

std::unique_ptr<Stmt> Parser::parseWhileStmt() {
    SourceRange r = previous().range;
    consume(TokenKind::LeftParen, "expected '(' after 'while'");
    auto cond = parseExpr();
    consume(TokenKind::RightParen, "expected ')' after condition");
    auto body = parseStatement();
    return std::make_unique<WhileStmt>(std::move(cond), std::move(body), r);
}

std::unique_ptr<Stmt> Parser::parseForStmt() {
    SourceRange r = previous().range;

    // Parenthesized for-in: "for (x in expr)" / "for (T x in expr)"（§四-2）
    // 检测：peek()=x, peekNext()="in"（无类型）；peekNext()=x, peekNext2()="in"（有类型）
    if (check(TokenKind::LeftParen)) {
        // for (x in ...)：当前 token 是 '('，变量在 peekNext()，'in' 在 peekNext2()
        if (peekNext().kind == TokenKind::Identifier && peekNext2().value == "in")
            return parseForInStmt(true, false);  // for (x in ...)
        // for (T x in ...)：类型在 peekNext()，变量在 peekNext2()，'in' 在 peekNext3()
        if (peekNext2().kind == TokenKind::Identifier && peekNext3().value == "in")
            return parseForInStmt(true, true);   // for (T x in ...)
    }

    // Range for: "for id in expr body" (no parentheses)
    // Check if next token is an identifier (not '(')
    if (!check(TokenKind::LeftParen) && check(TokenKind::Identifier)) {
        auto save = current_;
        std::string var_name = parseIdentifier("expected variable name");
        if (check(TokenKind::Identifier) && peek().value == "in") {
            advance(); // consume 'in'
            auto range_expr = parseExpr();
            auto body = parseStatement();

            // Transform: for i in start..end → for (int i = start; i < end; i = i + 1)
            if (range_expr->kind == ExprKind::Range) {
                auto& re = static_cast<RangeExpr&>(*range_expr);
                // Build init: int i = start
                VarDecl init_decl;
                init_decl.name = var_name;
                init_decl.type.basic_type = BuiltinType::Int;
                init_decl.init_expr = std::move(re.start);
                std::vector<VarDecl> init_vec;
                init_vec.push_back(std::move(init_decl));
                auto init_var = std::make_unique<VarDeclStmt>(std::move(init_vec), r);

                // Build condition: i < end
                auto* id_end = new IdentifierExpr(var_name, r);
                auto* end_raw = re.end.release();
                auto cond = std::make_unique<BinaryOpExpr>(
                    std::unique_ptr<Expr>(id_end), BinaryOpKind::Lt,
                    std::unique_ptr<Expr>(end_raw), r);

                // Build step: i = i + 1
                auto* id_step = new IdentifierExpr(var_name, r);
                auto* one = new IntegerLiteralExpr(1, r);
                auto* add = new BinaryOpExpr(
                    std::unique_ptr<Expr>(id_step), BinaryOpKind::Add,
                    std::unique_ptr<Expr>(one), r);
                auto* id_target = new IdentifierExpr(var_name, r);
                auto step = std::make_unique<AssignmentExpr>(
                    std::unique_ptr<Expr>(id_target),
                    std::unique_ptr<Expr>(add), r);

                return std::make_unique<ForStmt>(
                    std::move(init_var), std::move(cond),
                    std::move(step), std::move(body), r);
            }
            // 集合迭代（§四-2）：for x in coll → ForInStmt（sema 解析类型/访问）
            return std::make_unique<ForInStmt>(var_name, TypeNode(), false,
                std::move(range_expr), std::move(body), r);
        }
        current_ = save; // restore, not a range for
    }

    // Standard for (;;)
    consume(TokenKind::LeftParen, "expected '(' after 'for'");
    // Init: optional variable declaration
    std::unique_ptr<Stmt> init;
    if (checkType() || check(TokenKind::Keyword_var) || check(TokenKind::Keyword_const)) {
        init = parseVarDeclStmt();
    } else {
        consume(TokenKind::Semicolon, "expected ';' after for init");
    }
    // Condition: optional
    std::unique_ptr<Expr> cond;
    if (!check(TokenKind::Semicolon)) {
        cond = parseExpr();
    }
    consume(TokenKind::Semicolon, "expected ';' after for condition");
    // Step: optional
    std::unique_ptr<Expr> step;
    if (!check(TokenKind::RightParen)) {
        step = parseExpr();
    }
    consume(TokenKind::RightParen, "expected ')' after for clauses");
    auto body = parseStatement();
    return std::make_unique<ForStmt>(std::move(init), std::move(cond),
                                      std::move(step), std::move(body), r);
}

// for (x in coll) / for (T x in coll) / for x in coll — 集合迭代（§四-2）
std::unique_ptr<Stmt> Parser::parseForInStmt(bool parenthesized, bool has_explicit_type) {
    SourceRange r = previous().range;
    if (parenthesized)
        consume(TokenKind::LeftParen, "expected '(' after 'for'");

    TypeNode var_type;
    bool has_type = has_explicit_type;
    std::string var_name;
    if (has_explicit_type) {
        // 显式元素类型：for (T x in ...)
        var_type = parseType();
        var_name = parseIdentifier("expected variable name");
    } else {
        var_name = parseIdentifier("expected variable name");
    }

    if (!(check(TokenKind::Identifier) && peek().value == "in"))
        diag_.error(peek().range, "expected 'in' in for-in");
    else
        advance(); // consume 'in'

    auto iterable = parseExpr();
    if (parenthesized)
        consume(TokenKind::RightParen, "expected ')' after for-in iterable");
    auto body = parseStatement();
    return std::make_unique<ForInStmt>(var_name, std::move(var_type), has_type,
                                        std::move(iterable), std::move(body), r);
}

std::unique_ptr<Stmt> Parser::parseReturnStmt() {
    SourceRange r = previous().range;
    std::unique_ptr<Expr> value;
    if (!check(TokenKind::Semicolon)) {
        value = parseExpr();
    }
    consume(TokenKind::Semicolon, "expected ';' after return");
    return std::make_unique<ReturnStmt>(std::move(value), r);
}

std::unique_ptr<Stmt> Parser::parseBreakStmt() {
    SourceRange r = previous().range;
    consume(TokenKind::Semicolon, "expected ';' after break");
    return std::make_unique<BreakStmt>(r);
}

std::unique_ptr<Stmt> Parser::parseContinueStmt() {
    SourceRange r = previous().range;
    consume(TokenKind::Semicolon, "expected ';' after continue");
    return std::make_unique<ContinueStmt>(r);
}

std::unique_ptr<Stmt> Parser::parseAwaitStmt() {
    SourceRange r = previous().range;
    if (check(TokenKind::Semicolon)) {
        consume(TokenKind::Semicolon, "expected ';' after await expression");
        return std::make_unique<AwaitStmt>(r);  // await; — simple suspend
    }
    auto expr = parseExpr();
    // Optional timeout: `await Signal.go timeout 100;` (C10)
    if (check(TokenKind::Identifier) && peek().value == "timeout") {
        advance();
        auto to = parseExpr();
        consume(TokenKind::Semicolon, "expected ';' after await timeout");
        return std::make_unique<AwaitStmt>(std::move(expr), std::move(to), r);
    }
    consume(TokenKind::Semicolon, "expected ';' after await expression");
    return std::make_unique<AwaitStmt>(std::move(expr), r);
}

std::unique_ptr<Stmt> Parser::parseMappingStmt() {
    auto decl = parseMapping();
    return std::make_unique<MappingStmt>(std::move(*decl), decl->range);
}

// ==============================
// Expressions (precedence climbing)
// ==============================

// Deep-clone an expression (used for compound assignment desugaring)
static std::unique_ptr<Expr> cloneExpr(const Expr& e) {
    switch (e.kind) {
        case ExprKind::IntegerLiteral: {
            auto& v = static_cast<const IntegerLiteralExpr&>(e);
            return std::make_unique<IntegerLiteralExpr>(v.value, v.range);
        }
        case ExprKind::FloatLiteral: {
            auto& v = static_cast<const FloatLiteralExpr&>(e);
            return std::make_unique<FloatLiteralExpr>(v.value, v.range);
        }
        case ExprKind::BoolLiteral: {
            auto& v = static_cast<const BoolLiteralExpr&>(e);
            return std::make_unique<BoolLiteralExpr>(v.value, v.range);
        }
        case ExprKind::StringLiteral: {
            auto& v = static_cast<const StringLiteralExpr&>(e);
            return std::make_unique<StringLiteralExpr>(v.value, v.range);
        }
        case ExprKind::Identifier: {
            auto& v = static_cast<const IdentifierExpr&>(e);
            return std::make_unique<IdentifierExpr>(v.name, v.range);
        }
        case ExprKind::NullLiteral:
            return std::make_unique<NullLiteralExpr>(e.range);
        case ExprKind::ThisExpr:
            return std::make_unique<ThisExpr>(e.range);
        case ExprKind::MemberAccess: {
            auto& v = static_cast<const MemberAccessExpr&>(e);
            return std::make_unique<MemberAccessExpr>(
                cloneExpr(*v.object), v.member_name, v.range);
        }
        case ExprKind::Subscript: {
            auto& v = static_cast<const SubscriptExpr&>(e);
            auto arr = cloneExpr(*v.array);
            auto idx = cloneExpr(*v.index);
            if (!arr || !idx) return nullptr;
            return std::make_unique<SubscriptExpr>(
                std::move(arr), std::move(idx), v.range);
        }
        case ExprKind::BinaryOp: {
            auto& v = static_cast<const BinaryOpExpr&>(e);
            auto l = cloneExpr(*v.lhs);
            auto r = cloneExpr(*v.rhs);
            if (!l || !r) return nullptr;
            return std::make_unique<BinaryOpExpr>(
                std::move(l), v.op, std::move(r), v.range);
        }
        case ExprKind::UnaryOp: {
            auto& v = static_cast<const UnaryOpExpr&>(e);
            auto o = cloneExpr(*v.operand);
            if (!o) return nullptr;
            return std::make_unique<UnaryOpExpr>(v.op, std::move(o), v.range);
        }
        case ExprKind::Ternary: {
            auto& v = static_cast<const TernaryExpr&>(e);
            auto c = cloneExpr(*v.condition);
            auto t = cloneExpr(*v.true_expr);
            auto f = cloneExpr(*v.false_expr);
            if (!c || !t || !f) return nullptr;
            return std::make_unique<TernaryExpr>(
                std::move(c), std::move(t), std::move(f), v.range);
        }
        case ExprKind::Lambda:
            return nullptr; // can't clone lambda
        case ExprKind::Pipe: {
            auto& v = static_cast<const PipeExpr&>(e);
            auto l = cloneExpr(*v.lhs);
            auto r = cloneExpr(*v.rhs);
            if (!l || !r) return nullptr;
            return std::make_unique<PipeExpr>(
                std::move(l), std::move(r), v.range);
        }
        case ExprKind::Assignment: {
            auto& v = static_cast<const AssignmentExpr&>(e);
            auto t = cloneExpr(*v.target);
            auto val = cloneExpr(*v.value);
            if (!t || !val) return nullptr;
            return std::make_unique<AssignmentExpr>(
                std::move(t), std::move(val), v.range);
        }
        case ExprKind::Call: {
            auto& v = static_cast<const CallExpr&>(e);
            auto callee = cloneExpr(*v.callee);
            if (!callee) return nullptr;
            std::vector<std::unique_ptr<Expr>> args;
            for (auto& a : v.args) {
                auto ca = cloneExpr(*a);
                if (!ca) return nullptr;
                args.push_back(std::move(ca));
            }
            return std::make_unique<CallExpr>(std::move(callee), std::move(args), v.range);
        }
        case ExprKind::NewExpr: {
            auto& v = static_cast<const NewExpr&>(e);
            std::vector<std::unique_ptr<Expr>> args;
            for (auto& a : v.args) {
                auto ca = cloneExpr(*a);
                if (!ca) return nullptr;
                args.push_back(std::move(ca));
            }
            return std::make_unique<NewExpr>(v.class_name, v.type_args, std::move(args), v.range);
        }
        case ExprKind::NewArrayExpr: {
            auto& v = static_cast<const NewArrayExpr&>(e);
            std::vector<std::unique_ptr<Expr>> dims;
            for (auto& d : v.dimensions) {
                auto cd = cloneExpr(*d);
                if (!cd) return nullptr;
                dims.push_back(std::move(cd));
            }
            return std::make_unique<NewArrayExpr>(v.element_type, std::move(dims), v.range);
        }
        case ExprKind::EnumVariant: {
            auto& v = static_cast<const EnumVariantExpr&>(e);
            std::vector<std::unique_ptr<Expr>> args;
            for (auto& a : v.args) {
                auto ca = cloneExpr(*a);
                if (!ca) return nullptr;
                args.push_back(std::move(ca));
            }
            return std::make_unique<EnumVariantExpr>(
                v.enum_name, v.variant_index, std::move(args), v.range);
        }
        case ExprKind::Range: {
            auto& v = static_cast<const RangeExpr&>(e);
            auto s = cloneExpr(*v.start);
            auto e = cloneExpr(*v.end);
            if (!s || !e) return nullptr;
            return std::make_unique<RangeExpr>(std::move(s), std::move(e), v.range);
        }
        default:
            return nullptr; // cannot clone complex expressions
    }
}

std::unique_ptr<Expr> Parser::parseExpr() {
    return parseAssignment();
}

// Pipe: expr "|>" expr  (low precedence, left-assoc) — A |> Op1 |> Op2
std::unique_ptr<Expr> Parser::parsePipe() {
    auto expr = parseConditional();
    while (match(TokenKind::PipeForward)) {
        auto rhs = parseConditional();
        expr = std::make_unique<PipeExpr>(
            std::move(expr), std::move(rhs), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseAssignment() {
    auto expr = parsePipe();
    if (match(TokenKind::Equal)) {
        auto value = parseAssignment();
        return std::make_unique<AssignmentExpr>(
            std::move(expr), std::move(value), previous().range);
    }
    // Compound assignment: a += b  →  a = a + b
    BinaryOpKind compound_op;
    bool is_compound = true;
    if (match(TokenKind::PlusEqual))        compound_op = BinaryOpKind::Add;
    else if (match(TokenKind::MinusEqual))  compound_op = BinaryOpKind::Sub;
    else if (match(TokenKind::StarEqual))   compound_op = BinaryOpKind::Mul;
    else if (match(TokenKind::SlashEqual))  compound_op = BinaryOpKind::Div;
    else if (match(TokenKind::PercentEqual))compound_op = BinaryOpKind::Mod;
    else is_compound = false;

    if (is_compound) {
        auto rhs = parseAssignment();
        auto lhs_clone = cloneExpr(*expr);
        if (!lhs_clone) {
            diag_.error(expr->range, "cannot use compound assignment on this expression");
            return rhs;
        }
        auto binop = std::make_unique<BinaryOpExpr>(
            std::move(lhs_clone), compound_op, std::move(rhs), previous().range);
        return std::make_unique<AssignmentExpr>(
            std::move(expr), std::move(binop), previous().range);
    }
    return expr;
}

// Ternary conditional: cond ? true_expr : false_expr
std::unique_ptr<Expr> Parser::parseConditional() {
    auto expr = parseLogicalOr();
    if (match(TokenKind::Question)) {
        auto true_expr = parseExpr();
        consume(TokenKind::Colon, "expected ':' in ternary expression");
        auto false_expr = parseConditional();
        return std::make_unique<TernaryExpr>(
            std::move(expr), std::move(true_expr), std::move(false_expr),
            previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseLogicalOr() {
    auto expr = parseLogicalAnd();
    while (match(TokenKind::OrOr)) {
        auto rhs = parseLogicalAnd();
        expr = std::make_unique<BinaryOpExpr>(
            std::move(expr), BinaryOpKind::Or, std::move(rhs), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseLogicalAnd() {
    auto expr = parseBitwiseOr();
    while (match(TokenKind::AndAnd)) {
        auto rhs = parseBitwiseOr();
        expr = std::make_unique<BinaryOpExpr>(
            std::move(expr), BinaryOpKind::And, std::move(rhs), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseBitwiseOr() {
    auto expr = parseBitwiseXor();
    while (match(TokenKind::Pipe)) {
        auto rhs = parseBitwiseXor();
        expr = std::make_unique<BinaryOpExpr>(
            std::move(expr), BinaryOpKind::BitOr, std::move(rhs), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseBitwiseXor() {
    auto expr = parseBitwiseAnd();
    while (match(TokenKind::Caret)) {
        auto rhs = parseBitwiseAnd();
        expr = std::make_unique<BinaryOpExpr>(
            std::move(expr), BinaryOpKind::BitXor, std::move(rhs), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseBitwiseAnd() {
    auto expr = parseEquality();
    while (match(TokenKind::Amp)) {
        auto rhs = parseEquality();
        expr = std::make_unique<BinaryOpExpr>(
            std::move(expr), BinaryOpKind::BitAnd, std::move(rhs), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseEquality() {
    auto expr = parseRelational();
    while (true) {
        if (match(TokenKind::EqualEqual)) {
            auto rhs = parseRelational();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Eq, std::move(rhs), previous().range);
        } else if (match(TokenKind::NotEqual)) {
            auto rhs = parseRelational();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Ne, std::move(rhs), previous().range);
        } else {
            break;
        }
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseRelational() {
    auto expr = parseShift();
    while (true) {
        if (match(TokenKind::Less)) {
            auto rhs = parseShift();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Lt, std::move(rhs), previous().range);
        } else if (match(TokenKind::Greater)) {
            auto rhs = parseShift();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Gt, std::move(rhs), previous().range);
        } else if (match(TokenKind::LessEqual)) {
            auto rhs = parseShift();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Le, std::move(rhs), previous().range);
        } else if (match(TokenKind::GreaterEqual)) {
            auto rhs = parseShift();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Ge, std::move(rhs), previous().range);
        } else {
            break;
        }
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseShift() {
    auto expr = parseAdditive();
    while (true) {
        if (match(TokenKind::LessLess)) {
            auto rhs = parseAdditive();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Shl, std::move(rhs), previous().range);
        } else if (match(TokenKind::GreaterGreater)) {
            auto rhs = parseAdditive();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Shr, std::move(rhs), previous().range);
        } else {
            break;
        }
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseAdditive() {
    auto expr = parseRange();
    while (true) {
        if (match(TokenKind::Plus)) {
            auto rhs = parseMultiplicative();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Add, std::move(rhs), previous().range);
        } else if (match(TokenKind::Minus)) {
            auto rhs = parseMultiplicative();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Sub, std::move(rhs), previous().range);
        } else {
            break;
        }
    }
    return expr;
}

// Range: expr ".." expr  (lower precedence than additive)
std::unique_ptr<Expr> Parser::parseRange() {
    auto expr = parseMultiplicative();
    if (match(TokenKind::DoubleDot)) {
        auto end = parseRange();
        return std::make_unique<RangeExpr>(std::move(expr), std::move(end), previous().range);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseMultiplicative() {
    auto expr = parseUnary();
    while (true) {
        if (match(TokenKind::Star)) {
            auto rhs = parseUnary();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Mul, std::move(rhs), previous().range);
        } else if (match(TokenKind::Slash)) {
            auto rhs = parseUnary();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Div, std::move(rhs), previous().range);
        } else if (match(TokenKind::Percent)) {
            auto rhs = parseUnary();
            expr = std::make_unique<BinaryOpExpr>(
                std::move(expr), BinaryOpKind::Mod, std::move(rhs), previous().range);
        } else {
            break;
        }
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parseUnary() {
    // await expr — coroutine suspend with value passing (expression form, C2).
    // The operand is a FULL expression: `await n * 2` == `await (n * 2)`.
    if (match(TokenKind::Keyword_await)) {
        // `await;` / `await,` / `await)` — suspend with no value passed out
        if (check(TokenKind::Semicolon) || check(TokenKind::RightParen) ||
            check(TokenKind::Comma)) {
            return std::make_unique<AwaitExpr>(nullptr, previous().range);
        }
        auto operand = parseExpr();
        // Optional timeout: `long r = await Signal.go timeout 100;` (C10)
        if (check(TokenKind::Identifier) && peek().value == "timeout") {
            advance();
            auto to = parseExpr();
            return std::make_unique<AwaitExpr>(std::move(operand), std::move(to),
                                               previous().range);
        }
        return std::make_unique<AwaitExpr>(std::move(operand), previous().range);
    }
    if (match(TokenKind::Bang)) {
        auto operand = parseUnary();
        return std::make_unique<UnaryOpExpr>(
            UnaryOpKind::Not, std::move(operand), previous().range);
    }
    if (match(TokenKind::Minus)) {
        auto operand = parseUnary();
        return std::make_unique<UnaryOpExpr>(
            UnaryOpKind::Negate, std::move(operand), previous().range);
    }
    // Prefix ++/--: desugar as assignment
    if (match(TokenKind::PlusPlus)) {
        auto operand = parseUnary();
        auto one = std::make_unique<IntegerLiteralExpr>(1, previous().range);
        auto binop = std::make_unique<BinaryOpExpr>(
            cloneExpr(*operand), BinaryOpKind::Add, std::move(one), previous().range);
        return std::make_unique<AssignmentExpr>(
            std::move(operand), std::move(binop), previous().range);
    }
    if (match(TokenKind::MinusMinus)) {
        auto operand = parseUnary();
        auto one = std::make_unique<IntegerLiteralExpr>(1, previous().range);
        auto binop = std::make_unique<BinaryOpExpr>(
            cloneExpr(*operand), BinaryOpKind::Sub, std::move(one), previous().range);
        return std::make_unique<AssignmentExpr>(
            std::move(operand), std::move(binop), previous().range);
    }
    return parsePostfix();
}

std::unique_ptr<Expr> Parser::parsePostfix() {
    auto expr = parsePrimary();

    while (true) {
        // Generic call: foo<int>(args) or ClassName.method<T>(args) — probe with
        // a diagnostic-free token scan (parseType can emit spurious errors on
        // `E < energies[mid]` etc. when the probe fails, so we must not call it
        // during lookahead).
        if ((expr->kind == ExprKind::Identifier || expr->kind == ExprKind::MemberAccess) &&
            check(TokenKind::Less) && scanGenericTypeArgs()) {
            std::vector<TypeNode> targs;
            advance(); // consume '<'
            targs.push_back(parseType());
            while (match(TokenKind::Comma)) {
                targs.push_back(parseType());
            }
            consume(TokenKind::Greater, "expected '>' after generic arguments");
            std::vector<std::unique_ptr<Expr>> args;
            consume(TokenKind::LeftParen, "expected '(' after generic arguments");
            if (!check(TokenKind::RightParen)) {
                args.push_back(parseExpr());
                while (match(TokenKind::Comma)) {
                    args.push_back(parseExpr());
                }
            }
            consume(TokenKind::RightParen, "expected ')' after arguments");
            auto call = std::make_unique<CallExpr>(
                std::move(expr), std::move(args), previous().range);
            call->call_type_args = std::move(targs);
            expr = std::move(call);
            continue;
        }
        if (match(TokenKind::LeftParen)) {
            std::vector<std::unique_ptr<Expr>> args;
            if (!check(TokenKind::RightParen)) {
                args.push_back(parseExpr());
                while (match(TokenKind::Comma)) {
                    args.push_back(parseExpr());
                }
            }
            consume(TokenKind::RightParen, "expected ')' after arguments");
            expr = std::make_unique<CallExpr>(
                std::move(expr), std::move(args), previous().range);

        } else if (match(TokenKind::Dot)) {
            // Tuple field access: t.0, t.1 — allow an integer after '.'.
            std::string member;
            if (check(TokenKind::IntegerLiteral)) {
                member = advance().value;
            } else {
                member = parseIdentifier("expected member name");
            }
            expr = std::make_unique<MemberAccessExpr>(
                std::move(expr), member, previous().range);

        } else if (match(TokenKind::LeftBracket)) {
            auto index = parseExpr();
            consume(TokenKind::RightBracket, "expected ']' after index");
            expr = std::make_unique<SubscriptExpr>(
                std::move(expr), std::move(index), previous().range);

        } else if (match(TokenKind::PlusPlus)) {
            // Postfix ++: desugar as a = a + 1
            auto one = std::make_unique<IntegerLiteralExpr>(1, previous().range);
            auto binop = std::make_unique<BinaryOpExpr>(
                cloneExpr(*expr), BinaryOpKind::Add, std::move(one), previous().range);
            expr = std::make_unique<AssignmentExpr>(
                std::move(expr), std::move(binop), previous().range);

        } else if (match(TokenKind::MinusMinus)) {
            // Postfix --: desugar as a = a - 1
            auto one = std::make_unique<IntegerLiteralExpr>(1, previous().range);
            auto binop = std::make_unique<BinaryOpExpr>(
                cloneExpr(*expr), BinaryOpKind::Sub, std::move(one), previous().range);
            expr = std::make_unique<AssignmentExpr>(
                std::move(expr), std::move(binop), previous().range);

        } else {
            break;
        }
    }

    return expr;
}

std::unique_ptr<Expr> Parser::parsePrimary() {
    if (match(TokenKind::IntegerLiteral)) {
        int64_t val = std::stoll(previous().value, nullptr, 0);
        return std::make_unique<IntegerLiteralExpr>(val, previous().range);
    }
    if (match(TokenKind::LongLiteral)) {
        int64_t val = std::stoll(previous().value, nullptr, 0);
        return std::make_unique<IntegerLiteralExpr>(val, previous().range, true);
    }
    if (match(TokenKind::FloatLiteral)) {
        double val = std::stod(previous().value);
        return std::make_unique<FloatLiteralExpr>(val, previous().range);
    }
    if (match(TokenKind::BoolLiteral) || match(TokenKind::Keyword_true)) {
        return std::make_unique<BoolLiteralExpr>(true, previous().range);
    }
    if (match(TokenKind::Keyword_false)) {
        return std::make_unique<BoolLiteralExpr>(false, previous().range);
    }
    if (match(TokenKind::StringLiteral)) {
        auto val = previous().value;
        // Check for string interpolation: "Hello, $name"
        auto dollar_pos = val.find('$');
        if (dollar_pos != std::string::npos) {
            // Expand into concatenation
            std::unique_ptr<Expr> result;
            size_t pos = 0;
            while (pos < val.size()) {
                auto dpos = val.find('$', pos);
                if (dpos == std::string::npos) {
                    // Remaining literal text
                    auto part = val.substr(pos);
                    auto lit = std::make_unique<StringLiteralExpr>(part, previous().range);
                    if (result)
                        result = std::make_unique<BinaryOpExpr>(
                            std::move(result), BinaryOpKind::Add, std::move(lit), previous().range);
                    else
                        result = std::move(lit);
                    break;
                }
                if (dpos > pos) {
                    // Text before $
                    auto part = val.substr(pos, dpos - pos);
                    auto lit = std::make_unique<StringLiteralExpr>(part, previous().range);
                    if (result)
                        result = std::make_unique<BinaryOpExpr>(
                            std::move(result), BinaryOpKind::Add, std::move(lit), previous().range);
                    else
                        result = std::move(lit);
                }
                // Variable name after $
                pos = dpos + 1;
                size_t end = pos;
                while (end < val.size() && (std::isalnum(val[end]) || val[end] == '_')) end++;
                if (end > pos) {
                    auto var_name = val.substr(pos, end - pos);
                    auto var_expr = std::make_unique<IdentifierExpr>(var_name, previous().range);
                    if (result)
                        result = std::make_unique<BinaryOpExpr>(
                            std::move(result), BinaryOpKind::Add, std::move(var_expr), previous().range);
                    else
                        result = std::move(var_expr);
                }
                pos = end;
            }
            if (result) return result;
        }
        return std::make_unique<StringLiteralExpr>(previous().value, previous().range);
    }
    if (match(TokenKind::CharLiteral)) {
        // char literal value is a single-character string
        int64_t val = previous().value.empty() ? 0 : (int64_t)(unsigned char)previous().value[0];
        return std::make_unique<IntegerLiteralExpr>(val, previous().range);
    }
    if (match(TokenKind::NullLiteral) || match(TokenKind::Keyword_null)) {
        return std::make_unique<NullLiteralExpr>(previous().range);
    }
    if (match(TokenKind::Keyword_this)) {
        return std::make_unique<ThisExpr>(previous().range);
    }
    if (match(TokenKind::Keyword_try)) {
        // Expression try: try <expr> catch (e) <expr>  (value on success, fallback on error)
        auto te = parseExpr();
        if (match(TokenKind::Keyword_catch)) {
            consume(TokenKind::LeftParen, "expected '(' after 'catch'");
            std::string cvn = parseIdentifier("expected catch variable name");
            consume(TokenKind::RightParen, "expected ')' after catch variable");
            auto ce = parseExpr();
            return std::make_unique<TryExpr>(std::move(te), cvn, std::move(ce), previous().range);
        }
        diag_.error(peek().range, "expected 'catch' in expression try");
        return std::make_unique<IntegerLiteralExpr>(0, previous().range);
    }
    if (match(TokenKind::Keyword_new)) {
        // new double[n] — dynamic array allocation (parse type manually, avoid parseType which eats [])
        if (peek().kind >= TokenKind::Type_byte && peek().kind <= TokenKind::Type_string) {
            TypeNode elem_type;
            elem_type.range = peek().range;
            auto tk = advance();
            switch (tk.kind) {
                case TokenKind::Type_byte:   elem_type.basic_type = BuiltinType::Byte; break;
                case TokenKind::Type_short:  elem_type.basic_type = BuiltinType::Short; break;
                case TokenKind::Type_int:    elem_type.basic_type = BuiltinType::Int; break;
                case TokenKind::Type_long:   elem_type.basic_type = BuiltinType::Long; break;
                case TokenKind::Type_ubyte:  elem_type.basic_type = BuiltinType::UByte; break;
                case TokenKind::Type_ushort: elem_type.basic_type = BuiltinType::UShort; break;
                case TokenKind::Type_uint:   elem_type.basic_type = BuiltinType::UInt; break;
                case TokenKind::Type_ulong:  elem_type.basic_type = BuiltinType::ULong; break;
                case TokenKind::Type_char:   elem_type.basic_type = BuiltinType::Char; break;
                case TokenKind::Type_float:  elem_type.basic_type = BuiltinType::Float; break;
                case TokenKind::Type_double: elem_type.basic_type = BuiltinType::Double; break;
                case TokenKind::Type_bool:   elem_type.basic_type = BuiltinType::Bool; break;
                case TokenKind::Type_string: elem_type.basic_type = BuiltinType::String; break;
                default: break;
            }
            std::vector<std::unique_ptr<Expr>> dims;
            while (match(TokenKind::LeftBracket)) {
                auto size_expr = parseExpr();
                consume(TokenKind::RightBracket, "expected ']' after array size");
                dims.push_back(std::move(size_expr));
            }
            if (dims.empty()) {
                diag_.error(previous().range, "expected '[size]' after type in new expression");
                return std::make_unique<NullLiteralExpr>(previous().range);
            }
            return std::make_unique<NewArrayExpr>(std::move(elem_type), std::move(dims), previous().range);
        }
        // new ClassName(args) — class allocation
        std::string class_name = parseIdentifier("expected class name after 'new'");
        // new Type[n] where Type is a generic param or user class (e.g. new T[n],
        // new Foo[n]): identifier followed by '[' — build a dynamic array instead
        // of a class allocation. Purely additive; previously this errored.
        if (check(TokenKind::LeftBracket)) {
            TypeNode elem_type;
            elem_type.range = peek().range;
            elem_type.class_name = class_name;
            std::vector<std::unique_ptr<Expr>> dims;
            while (match(TokenKind::LeftBracket)) {
                auto size_expr = parseExpr();
                consume(TokenKind::RightBracket, "expected ']' after array size");
                dims.push_back(std::move(size_expr));
            }
            if (dims.empty()) {
                diag_.error(previous().range, "expected '[size]' after type in new expression");
                return std::make_unique<NullLiteralExpr>(previous().range);
            }
            return std::make_unique<NewArrayExpr>(std::move(elem_type), std::move(dims), previous().range);
        }
        std::vector<TypeNode> type_args;
        if (match(TokenKind::Less)) {
            type_args = parseTypeArgList();
            consume(TokenKind::Greater, "expected '>' after generic arguments");
        }
        std::vector<std::unique_ptr<Expr>> args;
        consume(TokenKind::LeftParen, "expected '(' after class name");
        if (!check(TokenKind::RightParen)) {
            args.push_back(parseExpr());
            while (match(TokenKind::Comma)) {
                args.push_back(parseExpr());
            }
        }
        consume(TokenKind::RightParen, "expected ')' after arguments");
        return std::make_unique<NewExpr>(class_name, std::move(type_args), std::move(args), previous().range);
    }
    if (check(TokenKind::LeftParen)) {
        // Tuple literal: (a, b, ...) — top-level comma before the matching ')'
        // (checked before consuming '(' so scanTupleLiteral sees the '(').
        if (scanTupleLiteral()) {
            advance(); // consume '('
            std::vector<std::unique_ptr<Expr>> elems;
            if (!check(TokenKind::RightParen)) {
                elems.push_back(parseExpr());
                while (match(TokenKind::Comma)) {
                    if (check(TokenKind::RightParen)) { advance(); break; } // trailing comma
                    elems.push_back(parseExpr());
                }
            }
            consume(TokenKind::RightParen, "expected ')' after tuple literal");
            return std::make_unique<TupleExpr>(std::move(elems), previous().range);
        }
        // consume '('
        advance();
        // Check if this might be a lambda: look ahead for ') =>'
        size_t saved = current_;
        if (!isAtEnd()) {
            for (size_t i = saved; i < tokens_.size(); i++) {
                if (tokens_[i].kind == TokenKind::RightParen && i + 1 < tokens_.size()
                    && tokens_[i + 1].kind == TokenKind::FatArrow) {
                    return parseLambdaExpr();
                }
                if (tokens_[i].kind == TokenKind::RightParen) break;
            }
        }
        // Not a lambda, parse (expr)
        current_ = saved;
        auto expr = parseExpr();
        consume(TokenKind::RightParen, "expected ')' after expression");
        return expr;
    }
    if (check(TokenKind::Identifier) && peek().value == "quote" &&
        peekNext().kind == TokenKind::LeftBrace) {
        // Contextual keyword `quote { ... }` — compile-time AST template (M4).
        // Only recognized when followed by '{'; `char quote = ...` stays a var.
        advance();  // consume 'quote'
        consume(TokenKind::LeftBrace, "expected '{' after 'quote'");
        auto body = parseBlock();
        return std::make_unique<QuoteExpr>(std::move(body), previous().range);
    }
    if (check(TokenKind::Identifier)) {
        return std::make_unique<IdentifierExpr>(
            parseIdentifier("expected identifier"), previous().range);
    }
    if (check(TokenKind::Dollar)) {
        // Macro template parameter: $name → MacroParamExpr placeholder.
        advance();  // consume $
        std::string pname = parseIdentifier("expected macro parameter name after '$'");
        return std::make_unique<MacroParamExpr>(pname, previous().range);
    }

    diag_.error(peek().range,
        std::string("expected expression, got '") + Token::kindName(peek().kind) + "'");
    return std::make_unique<IdentifierExpr>("__error__", peek().range);
}

// ==============================
// Types
// ==============================

std::unique_ptr<Expr> Parser::parseLambdaExpr() {
    std::vector<ParamDecl> params;
    if (!check(TokenKind::RightParen)) {
        params.push_back(parseParam());
        while (match(TokenKind::Comma)) {
            params.push_back(parseParam());
        }
    }
    consume(TokenKind::RightParen, "expected ')' after parameters");
    consume(TokenKind::FatArrow, "expected '=>' after lambda parameters");
    consume(TokenKind::LeftBrace, "expected '{' for lambda body");
    auto body = parseBlock();
    return std::make_unique<LambdaExpr>(std::move(params), std::move(body), previous().range);
}

// ==============================
// Macro: "macro" id "(" { "$" id } ")" block
// ==============================

bool Parser::isMacroStmtPlaceholder() {
    // current_ is at '$'. A standalone statement placeholder is:
    //   '$' Identifier   followed by  '}' | ';' | EOF
    // (otherwise `$x` is an expression, e.g. `$x = $x + $inc` or `foo($a)`)
    if (!check(TokenKind::Dollar)) return false;
    if (current_ + 1 >= tokens_.size()) return false;
    if (tokens_[current_ + 1].kind != TokenKind::Identifier) return false;
    if (current_ + 2 >= tokens_.size()) return true;  // $param at EOF
    auto nk = tokens_[current_ + 2].kind;
    return nk == TokenKind::RightBrace || nk == TokenKind::Semicolon ||
           nk == TokenKind::Dollar;   // next placeholder (multi-stmt macro body)
}

std::unique_ptr<MacroDecl> Parser::parseMacroDecl() {    auto decl = std::make_unique<MacroDecl>();
    decl->range = previous().range;  // 'macro' token
    decl->name = parseIdentifier("expected macro name after 'macro'");

    consume(TokenKind::LeftParen, "expected '(' after macro name");
    if (!check(TokenKind::RightParen)) {
        do {
            if (!match(TokenKind::Dollar)) {
                diag_.error(peek().range,
                    "expected '$' before macro parameter name");
                break;
            }
            decl->params.push_back(
                parseIdentifier("expected macro parameter name after '$'"));
        } while (match(TokenKind::Comma));
    }
    consume(TokenKind::RightParen, "expected ')' after macro parameters");
    consume(TokenKind::LeftBrace, "expected '{' for macro body");
    decl->body = parseBlock();
    return decl;
}

TypeNode Parser::parseType() {
    TypeNode node;
    node.range = peek().range;

    // Function type: (A, B) -> R (additive; diagnostic-free lookahead)
    if (check(TokenKind::LeftParen) && scanFunctionType()) {
        advance(); // consume '('
        if (!check(TokenKind::RightParen)) {
            node.func_param_types.push_back(parseType());
            while (match(TokenKind::Comma)) {
                node.func_param_types.push_back(parseType());
            }
        }
        consume(TokenKind::RightParen, "expected ')' in function type");
        consume(TokenKind::Arrow, "expected '->' in function type");
        node.func_return_type = std::make_shared<TypeNode>(parseType());
        node.range = peek().range;
        return node;
    }

    // Tuple type: (A, B) — top-level comma, ')' NOT followed by '->'
    // (function types handled above; `(int)` stays a parenthesized single type).
    if (check(TokenKind::LeftParen) && scanTupleType()) {
        advance(); // consume '('
        if (!check(TokenKind::RightParen)) {
            node.func_param_types.push_back(parseType());
            while (match(TokenKind::Comma)) {
                if (check(TokenKind::RightParen)) { advance(); break; } // trailing comma (int,)
                node.func_param_types.push_back(parseType());
            }
        }
        consume(TokenKind::RightParen, "expected ')' in tuple type");
        node.is_tuple = true;
        node.range = peek().range;
        // (A,B)[] — array of tuples
        if (match(TokenKind::LeftBracket)) {
            int arr_size = 0;
            if (check(TokenKind::IntegerLiteral)) {
                auto tok = advance();
                arr_size = std::stoi(tok.value);
            }
            consume(TokenKind::RightBracket, "expected ']' in array type");
            auto elem = std::make_unique<TypeNode>(std::move(node));
            node = TypeNode{};
            node.element_type = std::move(elem);
            node.array_size = arr_size;
            node.range = peek().range;
        }
        // (A,B)? — Option<(A,B)> sugar
        if (match(TokenKind::Question)) {
            TypeNode inner = node;
            node = TypeNode{};
            node.class_name = "Option";
            node.type_args.push_back(inner);
            node.range = inner.range;
        }
        return node;
    }

    if (match(TokenKind::Type_byte))       node.basic_type = BuiltinType::Byte;
    else if (match(TokenKind::Type_short)) node.basic_type = BuiltinType::Short;
    else if (match(TokenKind::Type_int))   node.basic_type = BuiltinType::Int;
    else if (match(TokenKind::Type_long))  node.basic_type = BuiltinType::Long;
    else if (match(TokenKind::Type_ubyte)) node.basic_type = BuiltinType::UByte;
    else if (match(TokenKind::Type_ushort))node.basic_type = BuiltinType::UShort;
    else if (match(TokenKind::Type_uint))  node.basic_type = BuiltinType::UInt;
    else if (match(TokenKind::Type_ulong)) node.basic_type = BuiltinType::ULong;
    else if (match(TokenKind::Type_char))  node.basic_type = BuiltinType::Char;
    else if (match(TokenKind::Type_float)) node.basic_type = BuiltinType::Float;
    else if (match(TokenKind::Type_double))node.basic_type = BuiltinType::Double;
    else if (match(TokenKind::Type_bool))  node.basic_type = BuiltinType::Bool;
    else if (match(TokenKind::Type_string))node.basic_type = BuiltinType::String;
    else if (match(TokenKind::Keyword_void)) node.basic_type = BuiltinType::Void;
    else if (match(TokenKind::Keyword_var)) {
        // var — type inference marker (actual type resolved in Sema)
        node.basic_type = BuiltinType::Int; // placeholder
        node.is_inferred = true;
    }
    else if (check(TokenKind::Identifier)) {
        node.class_name = parseIdentifier("expected type name");
        // Type alias substitution: `type Name = Type;` — resolve at parse time
        // so every downstream path (sema/codegen, incl. basic_type shortcuts)
        // sees the fully expanded type. Alias must be declared before use.
        if (node.class_name.size() > 0) {
            auto it = aliases_.find(node.class_name);
            if (it != aliases_.end()) node = it->second;
        }
        // Check for generic type arguments: ClassName<Type>
        if (match(TokenKind::Less)) {
            node.type_args = parseTypeArgList();
            consume(TokenKind::Greater, "expected '>' after generic arguments");
        }
        // Check for qualified name: ClassName::StructName
        if (match(TokenKind::DoubleColon)) {
            std::string nested = parseIdentifier("expected struct name");
            node.class_name = node.class_name + "::" + nested;
        }
    } else {
        diag_.error(peek().range,
            std::string("expected type, got '") + Token::kindName(peek().kind) + "'");
    }

    // Array type: Type[] or Type[size]
    if (match(TokenKind::LeftBracket)) {
        int arr_size = 0;
        // Check for fixed size like Type[10]
        if (check(TokenKind::IntegerLiteral)) {
            auto tok = advance();
            arr_size = std::stoi(tok.value);
            if (arr_size <= 0) {
                // No error — probing case (e.g. arr[0] parsed as type probe)
            }
        }
        consume(TokenKind::RightBracket, "expected ']' in array type");
        auto elem = std::make_unique<TypeNode>(std::move(node));
        node = TypeNode{};
        node.element_type = std::move(elem);
        node.array_size = arr_size;
        node.range = peek().range;
    }

    // Option sugar: Type? ≡ Option<Type> (additive; requires `import option;`)
    // e.g. int? == Option<int>; int[]? == Option<int[]>
    if (match(TokenKind::Question)) {
        TypeNode inner = node;
        node = TypeNode{};
        node.class_name = "Option";
        node.type_args.push_back(inner);
        node.range = inner.range;
    }

    return node;
}

// ==============================
// Helpers
// ==============================

std::string Parser::parseIdentifier(const std::string& error_msg) {
    if (check(TokenKind::Identifier)) {
        auto tok = advance();
        return tok.value;
    }
    diag_.error(peek().range, error_msg);
    return "__error__";
}

bool Parser::isTypeToken(TokenKind k) const {
    if (k >= TokenKind::Type_byte && k <= TokenKind::Type_string) return true;
    if (k == TokenKind::Keyword_void) return true;
    if (k == TokenKind::Keyword_var) return true;
    if (k == TokenKind::Identifier) return true;
    return false;
}

// Diagnostic-free lookahead for a generic-call arg list: `<Type, ...> (`.
// Assumes the current token is '<'. Never emits errors — probes must be silent
// so comparisons like `E < energies[mid]` don't get spurious diagnostics.
bool Parser::scanGenericTypeArgs() {
    size_t i = current_;
    if (i >= tokens_.size() || tokens_[i].kind != TokenKind::Less) return false;
    i++; // consume '<'
    if (i >= tokens_.size() || tokens_[i].kind == TokenKind::Greater) return false; // <>()
    int depth = 1;
    while (i < tokens_.size()) {
        TokenKind k = tokens_[i].kind;
        if (k == TokenKind::Less) { depth++; i++; continue; }
        if (k == TokenKind::Greater) {
            depth--;
            if (depth == 0) {
                i++;
                return i < tokens_.size() && tokens_[i].kind == TokenKind::LeftParen;
            }
            i++; continue;
        }
        // Tokens that cannot appear inside a MYP type → not a generic call.
        if (k == TokenKind::LeftParen || k == TokenKind::RightParen ||
            k == TokenKind::LeftBrace || k == TokenKind::RightBrace ||
            k == TokenKind::AndAnd || k == TokenKind::OrOr ||
            k == TokenKind::Equal || k == TokenKind::EqualEqual ||
            k == TokenKind::NotEqual || k == TokenKind::Semicolon ||
            k == TokenKind::Colon) {
            return false;
        }
        if (k == TokenKind::LeftBracket) {
            i++;
            // Inside []: only an integer or an immediate ']' is a valid type
            // (`[ident]` is a subscript expression → not a generic call).
            if (i < tokens_.size() && tokens_[i].kind == TokenKind::IntegerLiteral) i++;
            else if (i < tokens_.size() && tokens_[i].kind == TokenKind::RightBracket) {}
            else return false;
            if (i >= tokens_.size() || tokens_[i].kind != TokenKind::RightBracket) return false;
            i++;
            continue;
        }
        i++;
    }
    return false;
}

// Diagnostic-free lookahead for a function type: `(Type, ...) ->`.
// Assumes the current token is '('. Never emits errors.
bool Parser::scanFunctionType() {
    size_t i = current_;
    if (i >= tokens_.size() || tokens_[i].kind != TokenKind::LeftParen) return false;
    i++; // consume '('
    int depth = 1;
    while (i < tokens_.size()) {
        TokenKind k = tokens_[i].kind;
        if (k == TokenKind::LeftParen) { depth++; i++; continue; }
        if (k == TokenKind::RightParen) {
            depth--;
            if (depth == 0) {
                i++;
                return i < tokens_.size() && tokens_[i].kind == TokenKind::Arrow;
            }
            i++; continue;
        }
        if (k == TokenKind::Semicolon) return false; // statement boundary
        i++;
    }
    return false;
}

// Diagnostic-free lookahead for a tuple type: `(Type, ...)` with a top-level
// comma and ')' NOT followed by '->' (function types are handled by
// scanFunctionType first; `(int)` stays a parenthesized single type).
bool Parser::scanTupleType() {
    size_t i = current_;
    if (i >= tokens_.size() || tokens_[i].kind != TokenKind::LeftParen) return false;
    i++;
    int depth = 1;
    bool saw_comma = false;
    while (i < tokens_.size()) {
        TokenKind k = tokens_[i].kind;
        if (k == TokenKind::LeftParen) { depth++; i++; continue; }
        if (k == TokenKind::RightParen) {
            depth--;
            if (depth == 0) {
                i++;
                if (!saw_comma) return false;
                return !(i < tokens_.size() && tokens_[i].kind == TokenKind::Arrow);
            }
            i++; continue;
        }
        if (depth == 1 && k == TokenKind::Comma) saw_comma = true;
        i++;
    }
    return false;
}

// Diagnostic-free lookahead for a tuple literal expression: `(a, b, ...)`
// with a top-level comma before the matching ')' and ')' NOT followed by
// '=>' (a lambda `(x, y) => ...` has params, not a tuple).
bool Parser::scanTupleLiteral() {
    size_t i = current_;
    if (i >= tokens_.size() || tokens_[i].kind != TokenKind::LeftParen) return false;
    i++;
    int depth = 1;
    bool saw_comma = false;
    while (i < tokens_.size()) {
        TokenKind k = tokens_[i].kind;
        if (k == TokenKind::LeftParen) { depth++; i++; continue; }
        if (k == TokenKind::RightParen) {
            depth--;
            if (depth == 0) {
                i++;
                if (!saw_comma) return false;
                return !(i < tokens_.size() && tokens_[i].kind == TokenKind::FatArrow);
            }
            i++; continue;
        }
        if (depth == 1 && k == TokenKind::Comma) saw_comma = true;
        i++;
    }
    return false;
}

// Diagnostic-free lookahead for a destructuring statement:
// `(...) = expr;` — a top-level comma inside parens, ')' followed by '='.
bool Parser::scanDestructureAssign() {
    size_t i = current_;
    if (i >= tokens_.size() || tokens_[i].kind != TokenKind::LeftParen) return false;
    i++;
    int depth = 1;
    bool saw_comma = false;
    while (i < tokens_.size()) {
        TokenKind k = tokens_[i].kind;
        if (k == TokenKind::LeftParen) { depth++; i++; continue; }
        if (k == TokenKind::RightParen) {
            depth--;
            if (depth == 0) {
                i++;
                return saw_comma && i < tokens_.size() &&
                       tokens_[i].kind == TokenKind::Equal;
            }
            i++; continue;
        }
        if (depth == 1 && k == TokenKind::Comma) saw_comma = true;
        i++;
    }
    return false;
}

// Diagnostic-free lookahead for a tuple-typed variable declaration:
// `(int, int) name = ...` — top-level comma inside parens, ')' followed by an
// Identifier (the variable name). This disambiguates from destructuring
// `(int a, int b) = ...` (')' followed by '=').
bool Parser::scanTupleVarDecl() {
    size_t i = current_;
    if (i >= tokens_.size() || tokens_[i].kind != TokenKind::LeftParen) return false;
    i++;
    int depth = 1;
    bool saw_comma = false;
    while (i < tokens_.size()) {
        TokenKind k = tokens_[i].kind;
        if (k == TokenKind::LeftParen) { depth++; i++; continue; }
        if (k == TokenKind::RightParen) {
            depth--;
            if (depth == 0) {
                i++;
                return saw_comma && i < tokens_.size() &&
                       tokens_[i].kind == TokenKind::Identifier;
            }
            i++; continue;
        }
        if (depth == 1 && k == TokenKind::Comma) saw_comma = true;
        i++;
    }
    return false;
}

bool Parser::checkType() const {
    if (isAtEnd()) return false;
    switch (peek().kind) {
        case TokenKind::Type_byte:
        case TokenKind::Type_short:
        case TokenKind::Type_int:
        case TokenKind::Type_long:
        case TokenKind::Type_ubyte:
        case TokenKind::Type_ushort:
        case TokenKind::Type_uint:
        case TokenKind::Type_ulong:
        case TokenKind::Type_char:
        case TokenKind::Type_float:
        case TokenKind::Type_double:
        case TokenKind::Type_bool:
        case TokenKind::Type_string:
        case TokenKind::Identifier:
        case TokenKind::Keyword_void:
        case TokenKind::Keyword_var:
            return true;
        default:
            return false;
    }
}

SourceRange Parser::tokenRange(const Token& tok) const {
    return tok.range;
}

// ==============================
// Utility
// ==============================

const Token& Parser::peek() const {
    // Return EOF token if out of bounds
    if (current_ >= tokens_.size()) {
        static Token eof(TokenKind::EndOfFile, SourceRange{}, "");
        return eof;
    }
    return tokens_[current_];
}

const Token& Parser::peekNext() const {
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

std::vector<std::string> Parser::parseTypeParamList() {
    std::vector<std::string> params;
    do {
        params.push_back(parseIdentifier("expected type parameter name"));
    } while (match(TokenKind::Comma));
    // Optional constraint clause: `<T where T : Interface>` or `<T : Interface>`.
    // The parsed constraints are stored into the caller's ClassDecl via the
    // out-parameter style: this function returns names; constraints are handled
    // by parseClass which calls this then parses the optional where-clause.
    return params;
}

/// Parse the optional `where T : Interface [, U : J]` clause after generic
/// params. Fills type_param_constraints on \p cls. Returns false on error.
bool Parser::parseTypeParamConstraints(ClassDecl& cls) {
    if (!match(TokenKind::Keyword_where)) return true;
    do {
        std::string param = parseIdentifier("expected type parameter in where clause");
        consume(TokenKind::Colon, "expected ':' after type parameter in where clause");
        std::string iface = parseIdentifier("expected interface name in where clause");
        // Validate the param is one of the declared type params.
        bool found = false;
        for (auto& tp : cls.type_params) if (tp == param) { found = true; break; }
        if (!found) {
            diag_.error(previous().range,
                "where clause references unknown type parameter '" + param + "'");
            return false;
        }
        cls.type_param_constraints[param] = iface;
    } while (match(TokenKind::Comma));
    return true;
}

std::vector<TypeNode> Parser::parseTypeArgList() {
    std::vector<TypeNode> args;
    do {
        args.push_back(parseType());
    } while (match(TokenKind::Comma));
    return args;
}

bool Parser::isGenericIdentifier() {
    // Check if the current token sequence looks like an identifier followed by <
    if (!check(TokenKind::Identifier)) return false;
    size_t saved = current_;
    advance();
    bool result = check(TokenKind::Less);
    current_ = saved;
    return result;
}

std::unique_ptr<FFIDecl> Parser::parseFFIDecl() {
    auto decl = std::make_unique<FFIDecl>();
    decl->range = previous().range;
    decl->return_type = parseType();
    decl->name = parseIdentifier("expected function name");
    consume(TokenKind::LeftParen, "expected '('");
    if (!check(TokenKind::RightParen)) {
        decl->params.push_back(parseParam());
        while (match(TokenKind::Comma)) {
            decl->params.push_back(parseParam());
        }
    }
    consume(TokenKind::RightParen, "expected ')' after parameters");
    consume(TokenKind::Semicolon, "expected ';' after FFI declaration");
    return decl;
}

std::unique_ptr<EnumDecl> Parser::parseEnumDecl() {
    auto decl = std::make_unique<EnumDecl>();
    decl->range = previous().range;
    decl->name = parseIdentifier("expected enum name");

    consume(TokenKind::LeftBrace, "expected '{' after enum name");

    int vi = 0;
    while (!check(TokenKind::RightBrace) && !isAtEnd()) {
        EnumVariant variant;
        variant.range = peek().range;
        variant.name = parseIdentifier("expected variant name");

        // Optional data fields: Variant(type name, ...)
        if (match(TokenKind::LeftParen)) {
            if (!check(TokenKind::RightParen)) {
                variant.params = parseParamList();
            }
            consume(TokenKind::RightParen, "expected ')' after variant parameters");
        }

        consume(TokenKind::Semicolon, "expected ';' after variant");
        decl->variants.push_back(std::move(variant));
        vi++;
    }

    consume(TokenKind::RightBrace, "expected '}' after enum body");
    return decl;
}

// ==============================
// Type alias: "type" Identifier "=" Type ";"
// (contextual keyword — dispatched from parseProgram only in this exact shape)
// ==============================

std::unique_ptr<TypeAliasDecl> Parser::parseTypeAlias() {
    auto decl = std::make_unique<TypeAliasDecl>();
    decl->range = peek().range;

    advance(); // consume 'type'
    decl->name = parseIdentifier("expected alias name after 'type'");
    consume(TokenKind::Equal, "expected '=' in type alias");
    decl->alias_type = parseType();
    consume(TokenKind::Semicolon, "expected ';' after type alias");
    // Register for parse-time substitution (before subsequent decls).
    aliases_[decl->name] = decl->alias_type;
    return decl;
}

// ==============================
// Match: "match" "(" expr ")" "{" { arm } "}"
// arm: EnumType.Variant [ "(" id ("," id)* ")" ] "=>" block
// ==============================

std::unique_ptr<Stmt> Parser::parseMatchStmt() {
    SourceRange r = previous().range;

    consume(TokenKind::LeftParen, "expected '(' after 'match'");
    auto subject = parseExpr();
    consume(TokenKind::RightParen, "expected ')' after match subject");

    consume(TokenKind::LeftBrace, "expected '{' for match body");

    std::vector<MatchArm> arms;

    while (!check(TokenKind::RightBrace) && !isAtEnd()) {
        MatchArm arm;
        arm.range = peek().range;

        // Parse: EnumType.Variant
        arm.enum_name = parseIdentifier("expected enum type name in match arm");
        consume(TokenKind::Dot, "expected '.' after enum type");
        arm.variant_name = parseIdentifier("expected variant name");

        // The variant index will be resolved in Sema.
        arm.variant_index = -1; // resolved in Sema

        // Optional data bindings: (v1, v2, ...)
        if (match(TokenKind::LeftParen)) {
            if (!check(TokenKind::RightParen)) {
                arm.bindings.push_back(parseIdentifier("expected binding name"));
                while (match(TokenKind::Comma)) {
                    arm.bindings.push_back(parseIdentifier("expected binding name"));
                }
            }
            consume(TokenKind::RightParen, "expected ')' after bindings");
        }

        consume(TokenKind::FatArrow, "expected '=>' after match arm pattern");

        // Body is a block (not a single statement)
        consume(TokenKind::LeftBrace, "expected '{' for match arm body");
        arm.body = parseBlock();
        arms.push_back(std::move(arm));
    }

    consume(TokenKind::RightBrace, "expected '}' after match body");
    return std::make_unique<MatchStmt>(std::move(subject), std::move(arms), r);
}

std::unique_ptr<Stmt> Parser::parseTryStmt() {
    SourceRange r = previous().range;

    // Parse try block
    consume(TokenKind::LeftBrace, "expected '{' after 'try'");
    auto try_block = parseBlock();

    // Parse one or more catch clauses
    std::vector<CatchClause> catches;
    while (match(TokenKind::Keyword_catch)) {
        consume(TokenKind::LeftParen, "expected '(' after 'catch'");
        CatchClause cc;
        // catch (e)            → catch-all, e is the string message
        // catch (string e)     → string message
        // catch (FileError e)  → typed catch (matches that exception class)
        if (check(TokenKind::Identifier) && peekNext().kind == TokenKind::RightParen) {
            cc.var_name = parseIdentifier("expected variable name in catch");
            cc.var_type = "";
        } else if (check(TokenKind::Type_string)) {
            cc.var_type = "string";
            advance();
            cc.var_name = parseIdentifier("expected variable name in catch");
        } else if (check(TokenKind::Identifier)) {
            cc.var_type = advance().value;
            cc.var_name = parseIdentifier("expected variable name in catch");
        } else {
            diag_.error(peek().range, "expected type or variable in catch");
            cc.var_name = "e";
        }
        consume(TokenKind::RightParen, "expected ')' after catch variable");
        consume(TokenKind::LeftBrace, "expected '{' for catch body");
        cc.block = parseBlock();
        catches.push_back(std::move(cc));
    }

    // Parse optional finally clause
    std::unique_ptr<BlockStmt> finally_block;
    if (match(TokenKind::Keyword_finally)) {
        consume(TokenKind::LeftBrace, "expected '{' after 'finally'");
        finally_block = parseBlock();
    }

    if (catches.empty() && !finally_block) {
        diag_.error(peek().range, "expected 'catch' or 'finally' after 'try' block");
    }

    return std::make_unique<TryStmt>(std::move(try_block), std::move(catches),
        std::move(finally_block), r);
}

} // namespace mylang
