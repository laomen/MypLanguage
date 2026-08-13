// statement — part of the Parser implementation, split from src/parser/parser.cpp.
// Pure refactor: member definitions moved out of a large monolith;
// no behavior change. See parser.h for the class.

#include "mylang/Parser.h"

namespace mylang {

std::unique_ptr<Stmt> Parser::parseStatement() {
    ParserDepthGuard g(recursion_depth_, 300);
    if (g.exceeded) {
        diag_.error(peek().range, "statement nested too deeply");
        // Skip the deeply-nested '{...}' block in one shot (balanced-brace
        // consumption) so the parser terminates instead of re-descending.
        int nest = 0;
        while (!isAtEnd()) {
            TokenKind k = peek().kind;
            if (k == TokenKind::LeftBrace) nest++;
            else if (k == TokenKind::RightBrace) {
                if (--nest <= 0) { advance(); break; }
            }
            advance();
        }
        return std::make_unique<ExprStmt>(
            std::make_unique<IdentifierExpr>("__error__", peek().range),
            peek().range);
    }
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
                // @gpu tile (float[32][32] smem) [grid(nb)] { body }
                if (check(TokenKind::Identifier) && peek().value == "tile") {
                    advance(); // consume 'tile'
                    return parseGpuTileStmt();
                }
                // @gpu stride for (long i = 0L; i < n; i = i + nTh) { body }
                if (check(TokenKind::Identifier) && peek().value == "stride") {
                    advance(); // consume 'stride'
                    if (!check(TokenKind::Keyword_for)) {
                        diag_.error(previous().range,
                            "'@gpu stride' must be followed by 'for'");
                    } else {
                        advance(); // consume 'for'
                        auto stmt = parseForStmt();
                        if (auto* fs = dynamic_cast<ForStmt*>(stmt.get())) {
                            fs->gpu = true;
                            fs->stride = true;
                        }
                        return stmt;
                    }
                }
                // §8.2 @gpu reduce (acc, x) => { ... } init V over a[lo..hi) -> out
                if (check(TokenKind::Identifier) && peek().value == "reduce") {
                    advance(); // consume 'reduce'
                    return parseGpuReduceStmt();
                }
                // §8.3 @gpu scan (acc, x) => { ... } init V over a[lo..hi) -> b
                if (check(TokenKind::Identifier) && peek().value == "scan") {
                    advance(); // consume 'scan'
                    return parseGpuScanStmt();
                }
                // §8.4 @gpu scatter [(unique|atomic_add|any)] a[lo..hi) to b by idx[lo..hi)
                if (check(TokenKind::Identifier) && peek().value == "scatter") {
                    advance(); // consume 'scatter'
                    return parseGpuScatterStmt();
                }
                diag_.error(previous().range,
                    "'@gpu' must be followed by 'for', 'tile', 'stride for', or 'reduce'");
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
    if (match(TokenKind::Keyword_nonlocal)) {
        // nonlocal k, m;  — 仅 lambda 内合法（sema 校验）。按引用捕获外层变量。
        std::vector<std::string> names;
        SourceRange r = previous().range;
        while (true) {
            names.push_back(parseIdentifier("expected variable name after 'nonlocal'"));
            if (!match(TokenKind::Comma)) break;
        }
        consume(TokenKind::Semicolon, "expected ';' after nonlocal declaration");
        return std::make_unique<NonlocalStmt>(std::move(names), r);
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

    // @gpu for 设备驻留子句（M3）：resident(arr1 = dev1, arr2 = dev2)
    // 仅在 "for (...)" 后紧跟标识符 resident + '(' 时识别（否则当作普通语句体）。
    std::vector<std::pair<std::string, std::string>> resident;
    if (check(TokenKind::Identifier) && peek().value == "resident" &&
        peekNext().kind == TokenKind::LeftParen) {
        advance(); // resident
        consume(TokenKind::LeftParen, "expected '(' after 'resident'");
        while (true) {
            std::string arr = parseIdentifier("expected array name in resident clause");
            consume(TokenKind::Equal, "expected '=' in resident clause");
            std::string dev = parseIdentifier("expected device-pointer variable after '='");
            resident.push_back({arr, dev});
            if (!match(TokenKind::Comma)) break;
        }
        consume(TokenKind::RightParen, "expected ')' after resident clause");
    }

    // @gpu for 异步流子句（§4.1）：stream(s) —— s 为 GpuStream 实例（排队到该流）。
    std::unique_ptr<Expr> stream_expr;
    bool has_stream = false;
    if (check(TokenKind::Identifier) && peek().value == "stream" &&
        peekNext().kind == TokenKind::LeftParen) {
        advance(); // stream
        consume(TokenKind::LeftParen, "expected '(' after 'stream'");
        stream_expr = parseExpr();
        consume(TokenKind::RightParen, "expected ')' after stream expression");
        has_stream = true;
    }

    // §3.7 @gpu for block(n)：块大小（默认 256）。
    int64_t block_val = 0;
    if (check(TokenKind::Identifier) && peek().value == "block" &&
        peekNext().kind == TokenKind::LeftParen) {
        advance(); // block
        consume(TokenKind::LeftParen, "expected '(' after 'block'");
        if (check(TokenKind::IntegerLiteral)) {
            auto tok = advance();
            try { block_val = (int64_t)std::stoll(tok.value); }
            catch (...) { block_val = 0; }
        } else {
            diag_.error(peek().range, "block size must be an integer literal");
        }
        consume(TokenKind::RightParen, "expected ')' after block size");
    }

    auto body = parseStatement();
    auto st = std::make_unique<ForStmt>(std::move(init), std::move(cond),
                                      std::move(step), std::move(body), r,
                                      false, false, std::move(resident),
                                      std::move(stream_expr), has_stream);
    st->block_val = block_val;
    return st;
}

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

// @gpu tile (float[32][32] smem) [grid(nb)] { body }
// 解析共享内存声明：元素类型 + [dim]... 数组维度（编译期字面量）+ 名字，
// 可选 grid(nb) 子句，最后是协作 body（块或单语句）。
std::unique_ptr<Stmt> Parser::parseGpuTileStmt() {
    auto start = previous().range;
    consume(TokenKind::LeftParen, "expected '(' after '@gpu tile'");
    TypeNode shared_type = parseType();
    while (match(TokenKind::LeftBracket)) {
        int arr_size = 0;
        if (check(TokenKind::IntegerLiteral)) {
            auto tok = advance();
            try { arr_size = (int)std::stoll(tok.value); }
            catch (...) { arr_size = 0; }
        }
        consume(TokenKind::RightBracket, "expected ']' in shared array type");
        auto elem = std::make_unique<TypeNode>(std::move(shared_type));
        shared_type = TypeNode{};
        shared_type.element_type = std::move(elem);
        shared_type.array_size = arr_size;
        shared_type.range = peek().range;
    }
    std::string name = parseIdentifier("expected shared array name");
    consume(TokenKind::RightParen, "expected ')' after shared array declaration");

    // 可选 grid(nb)：块数（kernel.bx 取值 0..nb-1），默认 1
    bool has_grid = false;
    std::unique_ptr<Expr> grid_expr;
    if (check(TokenKind::Identifier) && peek().value == "grid") {
        advance(); // consume 'grid'
        consume(TokenKind::LeftParen, "expected '(' after 'grid'");
        grid_expr = parseExpr();
        consume(TokenKind::RightParen, "expected ')' after grid expression");
        has_grid = true;
    }

    // 设备驻留子句（M3）：resident(arr1 = dev1, ...) —— 同 @gpu for
    std::vector<std::pair<std::string, std::string>> resident;
    if (check(TokenKind::Identifier) && peek().value == "resident" &&
        peekNext().kind == TokenKind::LeftParen) {
        advance(); // resident
        consume(TokenKind::LeftParen, "expected '(' after 'resident'");
        while (true) {
            std::string arr = parseIdentifier("expected array name in resident clause");
            consume(TokenKind::Equal, "expected '=' in resident clause");
            std::string dev = parseIdentifier("expected device-pointer variable after '='");
            resident.push_back({arr, dev});
            if (!match(TokenKind::Comma)) break;
        }
        consume(TokenKind::RightParen, "expected ')' after resident clause");
    }

    // @gpu tile 异步流子句（§4.1）：stream(s)。
    std::unique_ptr<Expr> stream_expr;
    bool has_stream = false;
    if (check(TokenKind::Identifier) && peek().value == "stream" &&
        peekNext().kind == TokenKind::LeftParen) {
        advance(); // stream
        consume(TokenKind::LeftParen, "expected '(' after 'stream'");
        stream_expr = parseExpr();
        consume(TokenKind::RightParen, "expected ')' after stream expression");
        has_stream = true;
    }

    // §3.7 @gpu tile block(n)：块大小（默认 256）。
    int64_t block_val = 0;
    if (check(TokenKind::Identifier) && peek().value == "block" &&
        peekNext().kind == TokenKind::LeftParen) {
        advance(); // block
        consume(TokenKind::LeftParen, "expected '(' after 'block'");
        if (check(TokenKind::IntegerLiteral)) {
            auto tok = advance();
            try { block_val = (int64_t)std::stoll(tok.value); }
            catch (...) { block_val = 0; }
        } else {
            diag_.error(peek().range, "block size must be an integer literal");
        }
        consume(TokenKind::RightParen, "expected ')' after block size");
    }

    // body：parseBlock 不消费开 '{'，须先 match 消费（同 parseStatement）
    std::unique_ptr<Stmt> body;
    if (match(TokenKind::LeftBrace)) {
        body = parseBlock();
    } else {
        body = parseStatement();
    }

    SourceRange range;
    range.begin_offset = start.begin_offset;
    range.end_offset = previous().range.end_offset;
    auto st = std::make_unique<GpuTileStmt>(std::move(shared_type), name,
        std::move(grid_expr), has_grid, std::move(resident), std::move(body), range,
        std::move(stream_expr), has_stream);
    st->block_val = block_val;
    return st;
}

// §8.2 @gpu reduce (acc, x) => { return <op>; } init V over a[lo..hi) -> out;
// 声明式归约：out = fold(init, a[lo..hi))，op 为 (acc, x) => acc⊕x。
std::unique_ptr<Stmt> Parser::parseGpuReduceStmt() {
    auto start = previous().range;
    // (acc, x)
    consume(TokenKind::LeftParen, "expected '(' after '@gpu reduce'");
    std::string acc = parseIdentifier("expected accumulator parameter name");
    consume(TokenKind::Comma, "expected ',' between op parameters");
    std::string x = parseIdentifier("expected element parameter name");
    consume(TokenKind::RightParen, "expected ')' after op parameters");
    consume(TokenKind::FatArrow, "expected '=>' after op parameters");
    consume(TokenKind::LeftBrace, "expected '{' for op body");
    auto body = parseBlock();
    // init V
    if (check(TokenKind::Identifier) && peek().value == "init") {
        advance(); // consume 'init'
    } else {
        diag_.error(peek().range, "expected 'init <expr>' in '@gpu reduce'");
    }
    auto init = parseExpr();
    // over a[lo..hi)
    if (check(TokenKind::Identifier) && peek().value == "over") {
        advance(); // consume 'over'
    } else {
        diag_.error(peek().range, "expected 'over <array>[<lo>..<hi>)' in '@gpu reduce'");
    }
    std::string arr = parseIdentifier("expected array name after 'over'");
    consume(TokenKind::LeftBracket, "expected '[' after array name");
    auto begin = parseExpr();   // 0..n → RangeExpr（parseExpr 消费 '..'）
    std::unique_ptr<Expr> lo, hi;
    if (begin->kind == ExprKind::Range) {
        auto& r = static_cast<RangeExpr&>(*begin);
        lo = std::move(r.start);
        hi = std::move(r.end);
    } else {
        diag_.error(begin->range, "expected '<lo>..<hi>' range in '@gpu reduce'");
    }
    consume(TokenKind::RightParen, "expected ')' after reduce range");
    // -> out
    consume(TokenKind::Arrow, "expected '->' after reduce range");
    std::string out = parseIdentifier("expected output variable after '->'");
    // 可选 block(n) 子句（§3.7 块大小）
    int64_t block_val = 0;
    if (check(TokenKind::Identifier) && peek().value == "block" &&
        peekNext().kind == TokenKind::LeftParen) {
        advance(); // block
        consume(TokenKind::LeftParen, "expected '(' after 'block'");
        if (check(TokenKind::IntegerLiteral)) {
            auto tok = advance();
            try { block_val = (int64_t)std::stoll(tok.value); }
            catch (...) { block_val = 0; }
        } else {
            diag_.error(peek().range, "block size must be an integer literal");
        }
        consume(TokenKind::RightParen, "expected ')' after block size");
    }
    match(TokenKind::Semicolon);
    SourceRange range;
    range.begin_offset = start.begin_offset;
    range.end_offset = previous().range.end_offset;
    auto st = std::make_unique<GpuReduceStmt>(acc, x, std::move(body),
        std::move(init), arr, std::move(lo), std::move(hi), out, range);
    st->block_val = block_val;
    return st;
}

// §8.3 @gpu scan (acc, x) => { return <op>; } init V over a[lo..hi) -> b;
// 声明式前缀和：b[lo+i] = init∘a[lo]∘…∘a[lo+i]。语法同 reduce，-> 后是输出数组名。
std::unique_ptr<Stmt> Parser::parseGpuScanStmt() {
    auto start = previous().range;
    consume(TokenKind::LeftParen, "expected '(' after '@gpu scan'");
    std::string acc = parseIdentifier("expected accumulator parameter name");
    consume(TokenKind::Comma, "expected ',' between op parameters");
    std::string x = parseIdentifier("expected element parameter name");
    consume(TokenKind::RightParen, "expected ')' after op parameters");
    consume(TokenKind::FatArrow, "expected '=>' after op parameters");
    consume(TokenKind::LeftBrace, "expected '{' for op body");
    auto body = parseBlock();
    if (check(TokenKind::Identifier) && peek().value == "init") {
        advance();
    } else {
        diag_.error(peek().range, "expected 'init <expr>' in '@gpu scan'");
    }
    auto init = parseExpr();
    if (check(TokenKind::Identifier) && peek().value == "over") {
        advance();
    } else {
        diag_.error(peek().range, "expected 'over <array>[<lo>..<hi>)' in '@gpu scan'");
    }
    std::string arr = parseIdentifier("expected input array name after 'over'");
    consume(TokenKind::LeftBracket, "expected '[' after input array name");
    auto begin = parseExpr();
    std::unique_ptr<Expr> lo, hi;
    if (begin->kind == ExprKind::Range) {
        auto& r = static_cast<RangeExpr&>(*begin);
        lo = std::move(r.start);
        hi = std::move(r.end);
    } else {
        diag_.error(begin->range, "expected '<lo>..<hi>' range in '@gpu scan'");
    }
    consume(TokenKind::RightParen, "expected ')' after scan range");
    consume(TokenKind::Arrow, "expected '->' after scan range");
    std::string out = parseIdentifier("expected output array after '->'");
    int64_t block_val = 0;
    if (check(TokenKind::Identifier) && peek().value == "block" &&
        peekNext().kind == TokenKind::LeftParen) {
        advance();
        consume(TokenKind::LeftParen, "expected '(' after 'block'");
        if (check(TokenKind::IntegerLiteral)) {
            auto tok = advance();
            try { block_val = (int64_t)std::stoll(tok.value); }
            catch (...) { block_val = 0; }
        } else {
            diag_.error(peek().range, "block size must be an integer literal");
        }
        consume(TokenKind::RightParen, "expected ')' after block size");
    }
    match(TokenKind::Semicolon);
    SourceRange range;
    range.begin_offset = start.begin_offset;
    range.end_offset = previous().range.end_offset;
    auto st = std::make_unique<GpuScanStmt>(acc, x, std::move(body),
        std::move(init), arr, std::move(lo), std::move(hi), out, range);
    st->block_val = block_val;
    return st;
}

// §8.4 @gpu scatter [(unique|atomic_add|any)] a[lo..hi) to b by idx[lo..hi) [block(n)];
// 声明式散点：b[idx[lo_i+p]] = a[lo_a+p]（冲突语义显式，默认 any）。
std::unique_ptr<Stmt> Parser::parseGpuScatterStmt() {
    auto start = previous().range;
    // 可选冲突模式：scatter(unique) / scatter(atomic_add) / scatter(any)
    int mode = 0;   // 0=any（默认）, 1=unique, 2=atomic_add
    if (match(TokenKind::LeftParen)) {
        std::string m = parseIdentifier("expected conflict mode (unique|atomic_add|any)");
        if (m == "unique") mode = 1;
        else if (m == "atomic_add") mode = 2;
        else if (m == "any") mode = 0;
        else diag_.error(previous().range,
            "unknown scatter conflict mode '" + m + "' (expected unique|atomic_add|any)");
        consume(TokenKind::RightParen, "expected ')' after scatter mode");
    }
    // a[lo..hi)
    std::string a = parseIdentifier("expected input array name after 'scatter'");
    consume(TokenKind::LeftBracket, "expected '[' after input array name");
    auto ab = parseExpr();
    std::unique_ptr<Expr> a_lo, a_hi;
    if (ab->kind == ExprKind::Range) {
        auto& r = static_cast<RangeExpr&>(*ab);
        a_lo = std::move(r.start);
        a_hi = std::move(r.end);
    } else {
        diag_.error(ab->range, "expected '<lo>..<hi>' range for scatter input");
    }
    consume(TokenKind::RightParen, "expected ')' after scatter input range");
    // to b
    if (check(TokenKind::Identifier) && peek().value == "to") {
        advance(); // consume 'to'
    } else {
        diag_.error(peek().range, "expected 'to <output>' in '@gpu scatter'");
    }
    std::string b = parseIdentifier("expected output array name after 'to'");
    // by idx[lo..hi)
    if (check(TokenKind::Identifier) && peek().value == "by") {
        advance(); // consume 'by'
    } else {
        diag_.error(peek().range, "expected 'by <index-array>[<lo>..<hi>)' in '@gpu scatter'");
    }
    std::string idx = parseIdentifier("expected index array name after 'by'");
    consume(TokenKind::LeftBracket, "expected '[' after index array name");
    auto ib = parseExpr();
    std::unique_ptr<Expr> i_lo, i_hi;
    if (ib->kind == ExprKind::Range) {
        auto& r = static_cast<RangeExpr&>(*ib);
        i_lo = std::move(r.start);
        i_hi = std::move(r.end);
    } else {
        diag_.error(ib->range, "expected '<lo>..<hi>' range for scatter index");
    }
    consume(TokenKind::RightParen, "expected ')' after scatter index range");
    // 可选 block(n)
    int64_t block_val = 0;
    if (check(TokenKind::Identifier) && peek().value == "block" &&
        peekNext().kind == TokenKind::LeftParen) {
        advance();
        consume(TokenKind::LeftParen, "expected '(' after 'block'");
        if (check(TokenKind::IntegerLiteral)) {
            auto tok = advance();
            try { block_val = (int64_t)std::stoll(tok.value); }
            catch (...) { block_val = 0; }
        } else {
            diag_.error(peek().range, "block size must be an integer literal");
        }
        consume(TokenKind::RightParen, "expected ')' after block size");
    }
    match(TokenKind::Semicolon);
    SourceRange range;
    range.begin_offset = start.begin_offset;
    range.end_offset = previous().range.end_offset;
    auto st = std::make_unique<GpuScatterStmt>(a, std::move(a_lo), std::move(a_hi),
        b, idx, std::move(i_lo), std::move(i_hi), mode, range);
    st->block_val = block_val;
    return st;
}

}  // namespace {ns}
