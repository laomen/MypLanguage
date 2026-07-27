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
        } else if (peek().kind == TokenKind::At) {
            // @ annotation at top level → parse as function (@test, @startup)
            auto func = parseFunction();
            if (func) tu->functions.push_back(std::move(*func));
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
            std::vector<std::unique_ptr<Stmt>> fb;
            fb.push_back(std::make_unique<ReturnStmt>(std::move(ie), SourceRange{}));
            fd.body = std::make_unique<BlockStmt>(std::move(fb), SourceRange{});
            tu->functions.push_back(std::move(fd));
        } else if (checkType() || check(TokenKind::Keyword_void)) {
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
                cls.actions.push_back(parseActionDecl());
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
            if (checkType()) {
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
                if (func) cls.functions.push_back(std::move(*func));
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
        if (annot == "test") decl.has_test = true;
        if (annot == "coro") decl.has_coro = true;
    }

    decl.return_type = parseType();
    decl.name = parseIdentifier("expected action name");

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
        if (checkType() || check(TokenKind::Keyword_void)) {
            // Look ahead up to 3 tokens to distinguish property vs method:
            //   property: type name ; or type name = expr ;
            //   method:   type name ( params ) { body }
            // Skip ahead to peek after the type and identifier tokens
            int ahead = current_;
            // Skip type tokens (may be void too)
            auto isTypeKind = [](TokenKind k) {
                return (k >= TokenKind::Type_byte && k <= TokenKind::Type_string) ||
                       k == TokenKind::Keyword_void;
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
                if (func) decl->functions.push_back(std::move(*func));
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
        std::string annot = parseIdentifier("expected annotation name");
        if (annot == "test") func->has_test = true;
    }
    // Check for generic type params before return type
    if (check(TokenKind::Identifier) && peekNext().kind == TokenKind::Less) {
        // Generic function: T foo<T>(...) — type param before function name
        // Actually, generic func syntax: T max<T>(T a, T b) —  parse it differently
        // For now, we detect <T> after the function name
    }
    func->return_type = parseType();
    func->name = parseIdentifier("expected function name");

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
    if (match(TokenKind::Keyword_const)) {
        // const Type name [= expr]; — consume const, parse rest normally
        return parseVarDeclStmt();
    }
    if (checkType()) {
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
            // Check if next token is [ (array type): IOperator[2] name
            // Use 5-token lookahead: Identifier [ Integer ] Identifier → var decl
            if (next < tokens_.size() && tokens_[next].kind == TokenKind::LeftBracket) {
                size_t n2 = next + 1;
                size_t n3 = next + 2;
                size_t n4 = next + 3;
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

    auto expr = parseExpr();
    consume(TokenKind::Semicolon, "expected ';' after expression");
    return std::make_unique<ExprStmt>(std::move(expr), previous().range);
}

std::unique_ptr<BlockStmt> Parser::parseBlock() {
    std::vector<std::unique_ptr<Stmt>> stmts;
    SourceRange r = previous().range;

    while (!check(TokenKind::RightBrace) && !isAtEnd()) {
        auto stmt = parseStatement();
        if (stmt) stmts.push_back(std::move(stmt));
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
        decl.name = parseIdentifier("expected variable name");

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
    auto expr = parseExpr();
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

std::unique_ptr<Expr> Parser::parseAssignment() {
    auto expr = parseConditional();
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
            std::string member = parseIdentifier("expected member name");
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
    if (match(TokenKind::Keyword_new)) {
        std::string class_name = parseIdentifier("expected class name after 'new'");
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
    if (match(TokenKind::LeftParen)) {
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
    if (check(TokenKind::Identifier)) {
        return std::make_unique<IdentifierExpr>(
            parseIdentifier("expected identifier"), previous().range);
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

TypeNode Parser::parseType() {
    TypeNode node;
    node.range = peek().range;

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
    return params;
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

    // Parse optional catch clause
    std::string catch_var_name;
    std::string catch_var_type;
    std::unique_ptr<BlockStmt> catch_block;

    if (match(TokenKind::Keyword_catch)) {
        consume(TokenKind::LeftParen, "expected '(' after 'catch'");
        // Parse catch variable type (identifier or type keyword)
        if (check(TokenKind::Identifier)) {
            catch_var_type = advance().value;
        } else if (check(TokenKind::Type_string)) {
            catch_var_type = "string";
            advance(); // consume the type token
        } else {
            diag_.error(peek().range, "expected type in catch");
        }
        catch_var_name = parseIdentifier("expected variable name in catch");
        consume(TokenKind::RightParen, "expected ')' after catch variable");
        consume(TokenKind::LeftBrace, "expected '{' for catch body");
        catch_block = parseBlock();
    }

    // Parse optional finally clause
    std::unique_ptr<BlockStmt> finally_block;
    if (match(TokenKind::Keyword_finally)) {
        consume(TokenKind::LeftBrace, "expected '{' after 'finally'");
        finally_block = parseBlock();
    }

    if (!catch_block && !finally_block) {
        diag_.error(peek().range, "expected 'catch' or 'finally' after 'try' block");
    }

    return std::make_unique<TryStmt>(std::move(try_block), catch_var_name,
        catch_var_type, std::move(catch_block), std::move(finally_block), r);
}

} // namespace mylang
