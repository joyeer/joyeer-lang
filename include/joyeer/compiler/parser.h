#ifndef __joyeer_compiler_parser_h__
#define __joyeer_compiler_parser_h__

#include "joyeer/compiler/syntax.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace joyeer::parser {

enum class DiagnosticId {
    expectedDeclaration,
    expectedIdentifier,
    expectedType,
    expectedExpression,
    expectedPattern,
    expectedBlock,
    expectedToken,
    unexpectedToken,
    missingComma,
    missingListElement,
    invalidAssignmentTarget,
    chainedComparison,
    sameLineItems,
    unexpectedEndOfFile,
    unsupportedSyntax,
};

struct Diagnostic {
    DiagnosticId id;
    SourceSpan span;
    std::string message;
    std::optional<std::string> help;
    std::optional<DiagnosticFixIt> fixIt;
};

struct ParseResult {
    syntax::SourceFileSyntax::Ptr root;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool succeeded() const {
        return diagnostics.empty();
    }
};

class TokenCursor {
public:
    using Checkpoint = size_t;

    explicit TokenCursor(const std::vector<Token::Ptr>& tokens);

    [[nodiscard]] const Token::Ptr& peek(size_t offset = 0) const;
    [[nodiscard]] const Token::Ptr& previous() const;
    [[nodiscard]] const Token::Ptr& tokenAt(size_t position) const;
    [[nodiscard]] bool at(TokenKind kind, size_t offset = 0) const;
    Token::Ptr eat(TokenKind kind);
    Token::Ptr advance();

    [[nodiscard]] bool atEnd() const;
    [[nodiscard]] size_t position() const;
    [[nodiscard]] Checkpoint checkpoint() const;
    void rewind(Checkpoint checkpoint);

private:
    const std::vector<Token::Ptr>& tokens;
    Token::Ptr fallbackEof;
    size_t index = 0;
};

class Parser {
public:
    explicit Parser(const std::vector<Token::Ptr>& tokens);

    ParseResult parse();

private:
    struct InfixInfo {
        int bindingPower;
        bool rightAssociative;
        bool nonAssociative;
        bool assignment;
    };

    syntax::NodePtr parseTopLevelItem();
    syntax::BindingDeclSyntax::Ptr parseBindingDecl(bool requireType);
    syntax::FunctionDeclSyntax::Ptr parseFunctionDecl();
    syntax::ParameterDeclSyntax::Ptr parseParameter();
    syntax::StructDeclSyntax::Ptr parseStructDecl();
    syntax::StructFieldDeclSyntax::Ptr parseStructField();
    syntax::EnumDeclSyntax::Ptr parseEnumDecl();
    syntax::EnumCaseDeclSyntax::Ptr parseEnumCaseDecl();
    syntax::AssociatedTypeSyntax::Ptr parseAssociatedType();

    syntax::TypePtr parseType();
    syntax::TypePtr parseTypePrimary();

    syntax::BlockExprSyntax::Ptr parseBlock();
    syntax::NodePtr parseBlockItem();
    syntax::WhileStmtSyntax::Ptr parseWhileStmt();

    syntax::ExprPtr parseExpression();
    syntax::ExprPtr parsePrecedence(int minimumBindingPower);
    syntax::ExprPtr parsePrefixExpr();
    syntax::ExprPtr parsePostfixExpr();
    syntax::ExprPtr parsePrimaryExpr();
    syntax::ExprPtr parseParenthesizedExpr();
    syntax::ExprPtr parseArrayOrDictionaryExpr();
    syntax::IfExprSyntax::Ptr parseIfExpr();
    syntax::ReturnExprSyntax::Ptr parseReturnExpr();
    syntax::MatchExprSyntax::Ptr parseMatchExpr();
    syntax::CallArgumentSyntax::Ptr parseCallArgument();
    std::vector<syntax::CallArgumentSyntax::Ptr> parseArgumentClause();

    syntax::PatternPtr parsePattern();
    syntax::EnumCasePatternSyntax::Ptr parseEnumCasePattern();
    syntax::PatternArgumentSyntax::Ptr parsePatternArgument();
    syntax::MatchArmSyntax::Ptr parseMatchArm();

    [[nodiscard]] bool isLiteral(TokenKind kind) const;
    [[nodiscard]] bool isExpressionStart(const Token::Ptr& token) const;
    [[nodiscard]] bool isTopLevelStart(const Token::Ptr& token) const;
    [[nodiscard]] bool isBlockItemStart(const Token::Ptr& token) const;
    [[nodiscard]] bool isAssignmentTarget(const syntax::ExprPtr& expression) const;
    [[nodiscard]] bool shouldParseReturnValue() const;
    [[nodiscard]] InfixInfo infixInfo(TokenKind kind) const;
    [[nodiscard]] bool isInfix(TokenKind kind) const;

    Token::Ptr expect(TokenKind kind, const std::string& spelling);
    Token::Ptr synthetic(TokenKind kind) const;
    void report(DiagnosticId id, SourceSpan span, std::string message);
    void addExpectedTokenFixIt(TokenKind kind);
    void reportMissingComma(std::string message);
    void reportExpected(DiagnosticId id, const std::string& expected);

    void enforceItemBoundary(bool topLevel);
    void synchronizeTopLevel();
    void synchronizeBlock();
    void synchronizeList(TokenKind closingKind);
    void synchronizeMatchArm();

    [[nodiscard]] SourceSpan spanFrom(size_t start) const;
    [[nodiscard]] SourceSpan insertionSpan() const;
    [[nodiscard]] SourceSpan tokenSpan(const Token::Ptr& token) const;

private:
    TokenCursor cursor;
    std::vector<Diagnostic> diagnostics;
};

[[nodiscard]] const char* diagnosticName(DiagnosticId id);
[[nodiscard]] std::string dump(const std::vector<Diagnostic>& diagnostics);

} // namespace joyeer::parser

#endif
