#ifndef MYLANG_PARSER_H
#define MYLANG_PARSER_H

#include "AST.h"
#include "DiagnosticEngine.h"
#include "Token.h"

#include <memory>
#include <vector>

namespace mylang {

/// Recursive descent parser for MYP language.
class Parser {
public:
    Parser(const std::vector<Token>& tokens, DiagnosticEngine& diag);

    /// Parse the entire translation unit.
    std::unique_ptr<TranslationUnit> parse();

private:
    // Utility
    const Token& peek() const;
    const Token& peekNext() const;
    const Token& peekNext2() const;
    const Token& previous() const;
    Token advance();
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);
    bool matchAny(std::initializer_list<TokenKind> kinds);
    Token consume(TokenKind kind, const std::string& error_msg);
    bool isAtEnd() const;

    // Error recovery
    Token synchronize();

    // Top-level
    std::unique_ptr<TranslationUnit> parseProgram();
    ImportDecl parseImport();
    std::unique_ptr<ClassDecl> parseClass();
    std::unique_ptr<StructDecl> parseStruct();
    std::unique_ptr<InterfaceDecl> parseInterface();
    std::unique_ptr<MappingDecl> parseMapping();
    std::unique_ptr<FuncDecl> parseFunction(bool allow_void_return = true);
    std::unique_ptr<EnumDecl> parseEnumDecl();
    std::unique_ptr<FFIDecl> parseFFIDecl();
    std::vector<std::string> parseTypeParamList();
    std::vector<TypeNode> parseTypeArgList();
    std::unique_ptr<Stmt> parseMatchStmt();
    std::unique_ptr<Stmt> parseTryStmt();
    std::unique_ptr<Expr> parseLambdaExpr();
    bool isGenericIdentifier();

    // Class internals
    void parseClassSection(ClassDecl& cls);
    ActionDecl parseActionDecl();
    EventDecl parseEventDecl();
    PropertyDecl parsePropertyDecl();
    void parseInterfaceClassDecl(ClassDecl& cls);

    // Statements
    std::unique_ptr<Stmt> parseStatement();
    std::unique_ptr<BlockStmt> parseBlock();
    std::unique_ptr<Stmt> parseVarDeclStmt();
    std::unique_ptr<Stmt> parseIfStmt();
    std::unique_ptr<Stmt> parseWhileStmt();
    std::unique_ptr<Stmt> parseForStmt();
    std::unique_ptr<Stmt> parseReturnStmt();
    std::unique_ptr<Stmt> parseBreakStmt();
    std::unique_ptr<Stmt> parseContinueStmt();
    std::unique_ptr<Stmt> parseAwaitStmt();
    std::unique_ptr<Stmt> parseMappingStmt();

    // Expressions (precedence climbing)
    std::unique_ptr<Expr> parseExpr();
    std::unique_ptr<Expr> parseAssignment();
    std::unique_ptr<Expr> parseConditional();
    std::unique_ptr<Expr> parseLogicalOr();
    std::unique_ptr<Expr> parseLogicalAnd();
    std::unique_ptr<Expr> parseBitwiseOr();
    std::unique_ptr<Expr> parseBitwiseXor();
    std::unique_ptr<Expr> parseBitwiseAnd();
    std::unique_ptr<Expr> parseEquality();
    std::unique_ptr<Expr> parseRelational();
    std::unique_ptr<Expr> parseShift();
    std::unique_ptr<Expr> parseAdditive();
    std::unique_ptr<Expr> parseRange();
    std::unique_ptr<Expr> parseMultiplicative();
    std::unique_ptr<Expr> parseUnary();
    std::unique_ptr<Expr> parsePostfix();
    std::unique_ptr<Expr> parsePrimary();

    // Types
    TypeNode parseType();
    TypeNode parseBasicType();

    // Parameters
    ParamDecl parseParam();
    std::vector<ParamDecl> parseParamList();

    // Helpers
    std::string parseIdentifier(const std::string& error_msg);
    SourceRange tokenRange(const Token& tok) const;
    bool checkType() const;

    const std::vector<Token>& tokens_;
    DiagnosticEngine& diag_;
    size_t current_ = 0;
};

} // namespace mylang

#endif // MYLANG_PARSER_H
