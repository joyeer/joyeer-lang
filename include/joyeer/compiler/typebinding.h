#ifndef __joyeer_compiler_typebinding_h__
#define __joyeer_compiler_typebinding_h__

#include "joyeer/compiler/context.h"
#include "joyeer/compiler/node+visitor.h"

class TypeBinding : public NodeVisitor {
public:
    explicit TypeBinding(CompileContext::Ptr context);

    Node::Ptr visit(const Node::Ptr& node) override;

protected:
    // Type declaration
    Node::Ptr visit(const ModuleDecl::Ptr& decl) override;
    Node::Ptr visit(const ClassDecl::Ptr& classDecl) override;
    Node::Ptr visit(const FuncDecl::Ptr& decl) override;

    Node::Ptr visit(const VarDecl::Ptr& varDecl) override;
    Node::Ptr visit(const TypeIdentifier::Ptr& decl) override;
    Node::Ptr visit(const FuncCallExpr::Ptr& decl) override;

    // Type annotation
    Node::Ptr visit(const ArrayType::Ptr& decl) override;
    Node::Ptr visit(const OptionalType::Ptr& decl) override;

    // FuncCallExpr visitor
    Node::Ptr visit(const MemberFuncCallExpr::Ptr& decl) override;
    Node::Ptr visitDictFuncCallExpr(const FuncCallExpr::Ptr& decl);
    Node::Ptr visitArrayFuncCallExpr(const FuncCallExpr::Ptr& decl);
    Node::Ptr visitFuncCallExpr(const FuncCallExpr::Ptr& decl);

    Node::Ptr visit(const ArguCallExpr::Ptr& decl) override;
    Node::Ptr visit(const LiteralExpr::Ptr& decl) override;
    Node::Ptr visit(const PrefixExpr::Ptr& decl) override;
    Node::Ptr visit(const PostfixExpr::Ptr& decl) override;
    Node::Ptr visit(const IdentifierExpr::Ptr& decl) override;
    Node::Ptr visit(const Expr::Ptr& decl) override;
    Node::Ptr visit(const AssignExpr::Ptr& decl) override;
    Node::Ptr visit(const BinaryExpr::Ptr& decl) override;
    Node::Ptr visit(const OperatorExpr::Ptr& decl) override;
    Node::Ptr visit(const ParenthesizedExpr::Ptr& decl) override;
    Node::Ptr visit(const IfStmt::Ptr& decl) override;
    Node::Ptr visit(const WhileStmt::Ptr& decl) override;
    Node::Ptr visit(const StmtsBlock::Ptr& decl) override;
    Node::Ptr visit(const ParameterClause::Ptr& decl) override;
    Node::Ptr visit(const Pattern::Ptr& decl) override;
    Node::Ptr visit(const ReturnStmt::Ptr& decl) override;
    Node::Ptr visit(const Self::Ptr& decl) override;
    Node::Ptr visit(const SelfExpr::Ptr& decl) override;
    Node::Ptr visit(const ArrayLiteralExpr::Ptr& decl) override;
    Node::Ptr visit(const DictLiteralExpr::Ptr& decl) override;
    Node::Ptr visit(const MemberAccessExpr::Ptr& decl) override;
    Node::Ptr visit(const MemberAssignExpr::Ptr& decl) override;
    Node::Ptr visit(const ForceUnwrappingExpr::Ptr& decl) override;
    Node::Ptr visit(const OptionalChainingExpr::Ptr& decl) override;
    Node::Ptr visit(const SubscriptExpr::Ptr& decl) override;
    Node::Ptr visit(const ImportStmt::Ptr& decl) override;

    Type* typeOf(const Node::Ptr& node);
    Type* typeOf(const Expr::Ptr& node);
    Type* typeOf(const LiteralExpr::Ptr& node);
    Type* typeOf(const FuncCallExpr::Ptr& node);
    Type* typeOf(const ParenthesizedExpr::Ptr& node);
    Type* typeOf(const SelfExpr::Ptr& node);
    Type* typeOf(const Pattern::Ptr& node);
    Type* typeOf(const TypeIdentifier::Ptr& node);
    Type* typeOf(const ArrayLiteralExpr::Ptr& node);
    Type* typeOf(const DictLiteralExpr::Ptr& node);
    Type* typeOf(const MemberAccessExpr::Ptr& node);
    Type* typeOf(const MemberFuncCallExpr::Ptr& node);
    Type* typeOf(const SubscriptExpr::Ptr& node);
    Type* typeOf(const ArrayType::Ptr& node);
    Type* typeOf(const PrefixExpr::Ptr& node);
    Type* typeOf(const PostfixExpr::Ptr& node);
    Type* typeOf(const ForceUnwrappingExpr::Ptr& node);

    // verify that assignment expressions is legal
    // left = right => expression
    bool verifyIfAssignExpressionIsLegal(const Node::Ptr& left, const Node::Ptr& right);

    // verify that Arithmetic expression is legal
    // left op(+-*/...) right
    bool verifyIfArithmeticExpressionIsLegal(const Node::Ptr& left, const Node::Ptr& right);

    CompileContext::Ptr context;
    CompilerService* compiler;
    Diagnostics* diagnostics;
};

#endif // __joyeer_compiler_typebinding_h__
