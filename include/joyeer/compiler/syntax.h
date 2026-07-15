#ifndef __joyeer_compiler_syntax_h__
#define __joyeer_compiler_syntax_h__

#include "joyeer/compiler/token.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace joyeer::syntax {

enum class AccessEffect {
    borrowing,
    inout,
    consuming,
    initializing,
};

enum class Kind {
    sourceFile,

    errorDecl,
    bindingDecl,
    functionDecl,
    parameterDecl,
    structDecl,
    structFieldDecl,
    enumDecl,
    enumCaseDecl,
    associatedType,

    errorType,
    nominalType,
    arrayType,
    dictionaryType,
    optionalType,

    errorExpr,
    nameExpr,
    literalExpr,
    parenthesizedExpr,
    prefixExpr,
    accessExpr,
    binaryExpr,
    assignmentExpr,
    memberExpr,
    callExpr,
    callArgument,
    subscriptExpr,
    arrayExpr,
    dictionaryExpr,
    dictionaryEntry,
    contextualCaseExpr,
    blockExpr,
    whileStmt,
    ifExpr,
    returnExpr,
    matchExpr,
    matchArm,

    errorPattern,
    wildcardPattern,
    literalPattern,
    bindingPattern,
    enumCasePattern,
    patternArgument,
};

struct Node {
    using Ptr = std::shared_ptr<Node>;

    Kind kind;
    SourceSpan span;

    virtual ~Node() = default;

protected:
    Node(Kind kind, SourceSpan span): kind(kind), span(span) {}
};

using NodePtr = Node::Ptr;

struct DeclSyntax : Node {
    using Ptr = std::shared_ptr<DeclSyntax>;

protected:
    DeclSyntax(Kind kind, SourceSpan span): Node(kind, span) {}
};

using DeclPtr = DeclSyntax::Ptr;

struct TypeSyntax : Node {
    using Ptr = std::shared_ptr<TypeSyntax>;

protected:
    TypeSyntax(Kind kind, SourceSpan span): Node(kind, span) {}
};

using TypePtr = TypeSyntax::Ptr;

struct ExprSyntax : Node {
    using Ptr = std::shared_ptr<ExprSyntax>;

protected:
    ExprSyntax(Kind kind, SourceSpan span): Node(kind, span) {}
};

using ExprPtr = ExprSyntax::Ptr;

struct PatternSyntax : Node {
    using Ptr = std::shared_ptr<PatternSyntax>;

protected:
    PatternSyntax(Kind kind, SourceSpan span): Node(kind, span) {}
};

using PatternPtr = PatternSyntax::Ptr;

struct SourceFileSyntax final : Node {
    using Ptr = std::shared_ptr<SourceFileSyntax>;

    std::vector<NodePtr> items;

    SourceFileSyntax(SourceSpan span, std::vector<NodePtr> items):
            Node(Kind::sourceFile, span), items(std::move(items)) {}
};

struct ErrorDeclSyntax final : DeclSyntax {
    using Ptr = std::shared_ptr<ErrorDeclSyntax>;

    explicit ErrorDeclSyntax(SourceSpan span): DeclSyntax(Kind::errorDecl, span) {}
};

struct ErrorTypeSyntax final : TypeSyntax {
    using Ptr = std::shared_ptr<ErrorTypeSyntax>;

    explicit ErrorTypeSyntax(SourceSpan span): TypeSyntax(Kind::errorType, span) {}
};

struct ErrorExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<ErrorExprSyntax>;

    explicit ErrorExprSyntax(SourceSpan span): ExprSyntax(Kind::errorExpr, span) {}
};

struct ErrorPatternSyntax final : PatternSyntax {
    using Ptr = std::shared_ptr<ErrorPatternSyntax>;

    explicit ErrorPatternSyntax(SourceSpan span): PatternSyntax(Kind::errorPattern, span) {}
};

struct NominalTypeSyntax final : TypeSyntax {
    using Ptr = std::shared_ptr<NominalTypeSyntax>;

    Token::Ptr name;
    std::vector<TypePtr> arguments;

    NominalTypeSyntax(SourceSpan span, Token::Ptr name, std::vector<TypePtr> arguments):
            TypeSyntax(Kind::nominalType, span),
            name(std::move(name)),
            arguments(std::move(arguments)) {}
};

struct ArrayTypeSyntax final : TypeSyntax {
    using Ptr = std::shared_ptr<ArrayTypeSyntax>;

    TypePtr element;

    ArrayTypeSyntax(SourceSpan span, TypePtr element):
            TypeSyntax(Kind::arrayType, span), element(std::move(element)) {}
};

struct DictionaryTypeSyntax final : TypeSyntax {
    using Ptr = std::shared_ptr<DictionaryTypeSyntax>;

    TypePtr key;
    TypePtr value;

    DictionaryTypeSyntax(SourceSpan span, TypePtr key, TypePtr value):
            TypeSyntax(Kind::dictionaryType, span),
            key(std::move(key)),
            value(std::move(value)) {}
};

struct OptionalTypeSyntax final : TypeSyntax {
    using Ptr = std::shared_ptr<OptionalTypeSyntax>;

    TypePtr wrapped;

    OptionalTypeSyntax(SourceSpan span, TypePtr wrapped):
            TypeSyntax(Kind::optionalType, span), wrapped(std::move(wrapped)) {}
};

struct BindingDeclSyntax final : DeclSyntax {
    using Ptr = std::shared_ptr<BindingDeclSyntax>;

    Token::Ptr bindingKeyword;
    Token::Ptr name;
    TypePtr annotation;
    ExprPtr initializer;

    BindingDeclSyntax(SourceSpan span,
                      Token::Ptr bindingKeyword,
                      Token::Ptr name,
                      TypePtr annotation,
                      ExprPtr initializer):
            DeclSyntax(Kind::bindingDecl, span),
            bindingKeyword(std::move(bindingKeyword)),
            name(std::move(name)),
            annotation(std::move(annotation)),
            initializer(std::move(initializer)) {}

    [[nodiscard]] bool isMutable() const {
        return bindingKeyword != nullptr && bindingKeyword->kind == kwVar;
    }
};

struct ParameterDeclSyntax final : Node {
    using Ptr = std::shared_ptr<ParameterDeclSyntax>;

    Token::Ptr label;
    Token::Ptr name;
    Token::Ptr accessKeyword;
    TypePtr type;

    ParameterDeclSyntax(SourceSpan span,
                        Token::Ptr label,
                        Token::Ptr name,
                        Token::Ptr accessKeyword,
                        TypePtr type):
            Node(Kind::parameterDecl, span),
            label(std::move(label)),
            name(std::move(name)),
            accessKeyword(std::move(accessKeyword)),
            type(std::move(type)) {}

    [[nodiscard]] AccessEffect accessEffect() const {
        if (accessKeyword == nullptr) return AccessEffect::borrowing;
        switch (accessKeyword->kind) {
            case kwInout: return AccessEffect::inout;
            case kwBorrowing: return AccessEffect::borrowing;
            case kwConsuming: return AccessEffect::consuming;
            default: return AccessEffect::borrowing;
        }
    }
};

struct BlockExprSyntax;

struct FunctionDeclSyntax final : DeclSyntax {
    using Ptr = std::shared_ptr<FunctionDeclSyntax>;

    Token::Ptr name;
    std::vector<ParameterDeclSyntax::Ptr> parameters;
    TypePtr returnType;
    std::shared_ptr<BlockExprSyntax> body;

    FunctionDeclSyntax(SourceSpan span,
                       Token::Ptr name,
                       std::vector<ParameterDeclSyntax::Ptr> parameters,
                       TypePtr returnType,
                       std::shared_ptr<BlockExprSyntax> body):
            DeclSyntax(Kind::functionDecl, span),
            name(std::move(name)),
            parameters(std::move(parameters)),
            returnType(std::move(returnType)),
            body(std::move(body)) {}
};

struct StructFieldDeclSyntax final : Node {
    using Ptr = std::shared_ptr<StructFieldDeclSyntax>;

    Token::Ptr bindingKeyword;
    Token::Ptr name;
    TypePtr type;
    ExprPtr initializer;

    StructFieldDeclSyntax(SourceSpan span,
                          Token::Ptr bindingKeyword,
                          Token::Ptr name,
                          TypePtr type,
                          ExprPtr initializer):
            Node(Kind::structFieldDecl, span),
            bindingKeyword(std::move(bindingKeyword)),
            name(std::move(name)),
            type(std::move(type)),
            initializer(std::move(initializer)) {}
};

struct StructDeclSyntax final : DeclSyntax {
    using Ptr = std::shared_ptr<StructDeclSyntax>;

    Token::Ptr name;
    std::vector<StructFieldDeclSyntax::Ptr> fields;

    StructDeclSyntax(SourceSpan span,
                     Token::Ptr name,
                     std::vector<StructFieldDeclSyntax::Ptr> fields):
            DeclSyntax(Kind::structDecl, span),
            name(std::move(name)),
            fields(std::move(fields)) {}
};

struct AssociatedTypeSyntax final : Node {
    using Ptr = std::shared_ptr<AssociatedTypeSyntax>;

    Token::Ptr label;
    TypePtr type;

    AssociatedTypeSyntax(SourceSpan span, Token::Ptr label, TypePtr type):
            Node(Kind::associatedType, span),
            label(std::move(label)),
            type(std::move(type)) {}
};

struct EnumCaseDeclSyntax final : Node {
    using Ptr = std::shared_ptr<EnumCaseDeclSyntax>;

    Token::Ptr name;
    bool hasPayloadClause;
    std::vector<AssociatedTypeSyntax::Ptr> associatedTypes;

    EnumCaseDeclSyntax(SourceSpan span,
                       Token::Ptr name,
                       bool hasPayloadClause,
                       std::vector<AssociatedTypeSyntax::Ptr> associatedTypes):
            Node(Kind::enumCaseDecl, span),
            name(std::move(name)),
            hasPayloadClause(hasPayloadClause),
            associatedTypes(std::move(associatedTypes)) {}
};

struct EnumDeclSyntax final : DeclSyntax {
    using Ptr = std::shared_ptr<EnumDeclSyntax>;

    Token::Ptr name;
    std::vector<EnumCaseDeclSyntax::Ptr> cases;

    EnumDeclSyntax(SourceSpan span,
                   Token::Ptr name,
                   std::vector<EnumCaseDeclSyntax::Ptr> cases):
            DeclSyntax(Kind::enumDecl, span),
            name(std::move(name)),
            cases(std::move(cases)) {}
};

struct NameExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<NameExprSyntax>;

    Token::Ptr name;

    NameExprSyntax(SourceSpan span, Token::Ptr name):
            ExprSyntax(Kind::nameExpr, span), name(std::move(name)) {}
};

struct LiteralExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<LiteralExprSyntax>;

    Token::Ptr literal;

    LiteralExprSyntax(SourceSpan span, Token::Ptr literal):
            ExprSyntax(Kind::literalExpr, span), literal(std::move(literal)) {}
};

struct ParenthesizedExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<ParenthesizedExprSyntax>;

    ExprPtr expression;

    ParenthesizedExprSyntax(SourceSpan span, ExprPtr expression):
            ExprSyntax(Kind::parenthesizedExpr, span), expression(std::move(expression)) {}
};

struct PrefixExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<PrefixExprSyntax>;

    Token::Ptr op;
    ExprPtr operand;

    PrefixExprSyntax(SourceSpan span, Token::Ptr op, ExprPtr operand):
            ExprSyntax(Kind::prefixExpr, span),
            op(std::move(op)),
            operand(std::move(operand)) {}
};

struct AccessExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<AccessExprSyntax>;

    Token::Ptr marker;
    ExprPtr operand;

    AccessExprSyntax(SourceSpan span, Token::Ptr marker, ExprPtr operand):
            ExprSyntax(Kind::accessExpr, span),
            marker(std::move(marker)),
            operand(std::move(operand)) {}
};

struct BinaryExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<BinaryExprSyntax>;

    Token::Ptr op;
    ExprPtr left;
    ExprPtr right;

    BinaryExprSyntax(SourceSpan span, Token::Ptr op, ExprPtr left, ExprPtr right):
            ExprSyntax(Kind::binaryExpr, span),
            op(std::move(op)),
            left(std::move(left)),
            right(std::move(right)) {}
};

struct AssignmentExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<AssignmentExprSyntax>;

    Token::Ptr op;
    ExprPtr target;
    ExprPtr value;

    AssignmentExprSyntax(SourceSpan span, Token::Ptr op, ExprPtr target, ExprPtr value):
            ExprSyntax(Kind::assignmentExpr, span),
            op(std::move(op)),
            target(std::move(target)),
            value(std::move(value)) {}
};

struct MemberExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<MemberExprSyntax>;

    ExprPtr base;
    Token::Ptr member;

    MemberExprSyntax(SourceSpan span, ExprPtr base, Token::Ptr member):
            ExprSyntax(Kind::memberExpr, span),
            base(std::move(base)),
            member(std::move(member)) {}
};

struct CallArgumentSyntax final : Node {
    using Ptr = std::shared_ptr<CallArgumentSyntax>;

    Token::Ptr label;
    Token::Ptr accessMarker;
    ExprPtr value;

    CallArgumentSyntax(SourceSpan span,
                       Token::Ptr label,
                       Token::Ptr accessMarker,
                       ExprPtr value):
            Node(Kind::callArgument, span),
            label(std::move(label)),
            accessMarker(std::move(accessMarker)),
            value(std::move(value)) {}
};

struct CallExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<CallExprSyntax>;

    ExprPtr callee;
    std::vector<CallArgumentSyntax::Ptr> arguments;

    CallExprSyntax(SourceSpan span,
                   ExprPtr callee,
                   std::vector<CallArgumentSyntax::Ptr> arguments):
            ExprSyntax(Kind::callExpr, span),
            callee(std::move(callee)),
            arguments(std::move(arguments)) {}
};

struct SubscriptExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<SubscriptExprSyntax>;

    ExprPtr base;
    ExprPtr index;

    SubscriptExprSyntax(SourceSpan span, ExprPtr base, ExprPtr index):
            ExprSyntax(Kind::subscriptExpr, span),
            base(std::move(base)),
            index(std::move(index)) {}
};

struct ArrayExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<ArrayExprSyntax>;

    std::vector<ExprPtr> elements;

    ArrayExprSyntax(SourceSpan span, std::vector<ExprPtr> elements):
            ExprSyntax(Kind::arrayExpr, span), elements(std::move(elements)) {}
};

struct DictionaryEntrySyntax final : Node {
    using Ptr = std::shared_ptr<DictionaryEntrySyntax>;

    ExprPtr key;
    ExprPtr value;

    DictionaryEntrySyntax(SourceSpan span, ExprPtr key, ExprPtr value):
            Node(Kind::dictionaryEntry, span),
            key(std::move(key)),
            value(std::move(value)) {}
};

struct DictionaryExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<DictionaryExprSyntax>;

    std::vector<DictionaryEntrySyntax::Ptr> entries;

    DictionaryExprSyntax(SourceSpan span, std::vector<DictionaryEntrySyntax::Ptr> entries):
            ExprSyntax(Kind::dictionaryExpr, span), entries(std::move(entries)) {}
};

struct ContextualCaseExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<ContextualCaseExprSyntax>;

    Token::Ptr name;
    bool hasPayloadClause;
    std::vector<CallArgumentSyntax::Ptr> arguments;

    ContextualCaseExprSyntax(SourceSpan span,
                             Token::Ptr name,
                             bool hasPayloadClause,
                             std::vector<CallArgumentSyntax::Ptr> arguments):
            ExprSyntax(Kind::contextualCaseExpr, span),
            name(std::move(name)),
            hasPayloadClause(hasPayloadClause),
            arguments(std::move(arguments)) {}
};

struct BlockExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<BlockExprSyntax>;

    std::vector<NodePtr> items;

    BlockExprSyntax(SourceSpan span, std::vector<NodePtr> items):
            ExprSyntax(Kind::blockExpr, span), items(std::move(items)) {}
};

struct WhileStmtSyntax final : Node {
    using Ptr = std::shared_ptr<WhileStmtSyntax>;

    ExprPtr condition;
    BlockExprSyntax::Ptr body;

    WhileStmtSyntax(SourceSpan span, ExprPtr condition, BlockExprSyntax::Ptr body):
            Node(Kind::whileStmt, span),
            condition(std::move(condition)),
            body(std::move(body)) {}
};

struct IfExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<IfExprSyntax>;

    ExprPtr condition;
    BlockExprSyntax::Ptr thenBranch;
    ExprPtr elseBranch;

    IfExprSyntax(SourceSpan span,
                 ExprPtr condition,
                 BlockExprSyntax::Ptr thenBranch,
                 ExprPtr elseBranch):
            ExprSyntax(Kind::ifExpr, span),
            condition(std::move(condition)),
            thenBranch(std::move(thenBranch)),
            elseBranch(std::move(elseBranch)) {}
};

struct ReturnExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<ReturnExprSyntax>;

    ExprPtr value;

    ReturnExprSyntax(SourceSpan span, ExprPtr value):
            ExprSyntax(Kind::returnExpr, span), value(std::move(value)) {}
};

struct WildcardPatternSyntax final : PatternSyntax {
    using Ptr = std::shared_ptr<WildcardPatternSyntax>;

    Token::Ptr token;

    WildcardPatternSyntax(SourceSpan span, Token::Ptr token):
            PatternSyntax(Kind::wildcardPattern, span), token(std::move(token)) {}
};

struct LiteralPatternSyntax final : PatternSyntax {
    using Ptr = std::shared_ptr<LiteralPatternSyntax>;

    Token::Ptr literal;

    LiteralPatternSyntax(SourceSpan span, Token::Ptr literal):
            PatternSyntax(Kind::literalPattern, span), literal(std::move(literal)) {}
};

struct BindingPatternSyntax final : PatternSyntax {
    using Ptr = std::shared_ptr<BindingPatternSyntax>;

    Token::Ptr name;

    BindingPatternSyntax(SourceSpan span, Token::Ptr name):
            PatternSyntax(Kind::bindingPattern, span), name(std::move(name)) {}
};

struct PatternArgumentSyntax final : Node {
    using Ptr = std::shared_ptr<PatternArgumentSyntax>;

    Token::Ptr label;
    PatternPtr pattern;

    PatternArgumentSyntax(SourceSpan span, Token::Ptr label, PatternPtr pattern):
            Node(Kind::patternArgument, span),
            label(std::move(label)),
            pattern(std::move(pattern)) {}
};

struct EnumCasePatternSyntax final : PatternSyntax {
    using Ptr = std::shared_ptr<EnumCasePatternSyntax>;

    Token::Ptr qualifier;
    Token::Ptr name;
    bool hasPayloadClause;
    std::vector<PatternArgumentSyntax::Ptr> arguments;

    EnumCasePatternSyntax(SourceSpan span,
                          Token::Ptr qualifier,
                          Token::Ptr name,
                          bool hasPayloadClause,
                          std::vector<PatternArgumentSyntax::Ptr> arguments):
            PatternSyntax(Kind::enumCasePattern, span),
            qualifier(std::move(qualifier)),
            name(std::move(name)),
            hasPayloadClause(hasPayloadClause),
            arguments(std::move(arguments)) {}
};

struct MatchArmSyntax final : Node {
    using Ptr = std::shared_ptr<MatchArmSyntax>;

    PatternPtr pattern;
    ExprPtr body;

    MatchArmSyntax(SourceSpan span, PatternPtr pattern, ExprPtr body):
            Node(Kind::matchArm, span),
            pattern(std::move(pattern)),
            body(std::move(body)) {}
};

struct MatchExprSyntax final : ExprSyntax {
    using Ptr = std::shared_ptr<MatchExprSyntax>;

    ExprPtr scrutinee;
    std::vector<MatchArmSyntax::Ptr> arms;

    MatchExprSyntax(SourceSpan span,
                    ExprPtr scrutinee,
                    std::vector<MatchArmSyntax::Ptr> arms):
            ExprSyntax(Kind::matchExpr, span),
            scrutinee(std::move(scrutinee)),
            arms(std::move(arms)) {}
};

[[nodiscard]] const char* kindName(Kind kind);
[[nodiscard]] std::string dump(const NodePtr& node);

} // namespace joyeer::syntax

#endif
