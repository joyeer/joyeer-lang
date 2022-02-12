#ifndef __joyeer_compiler_node_visitor_h__
#define __joyeer_compiler_node_visitor_h__

#include "joyeer/compiler/node.h"

struct NodeVisitor {
protected:
    virtual Node::Ptr visit(const Node::Ptr& node);

    // Types
    virtual Node::Ptr visit(const ArrayType::Ptr& decl) = 0;
    virtual Node::Ptr visit(const OptionalType::Ptr& decl) = 0;

    // Expression
    virtual Node::Ptr visit(const Expr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const FuncCallExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const MemberFuncCallExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const ArguCallExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const LiteralExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const PrefixExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const PostfixExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const IdentifierExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const AssignExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const BinaryExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const OperatorExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const ParenthesizedExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const Self::Ptr& decl) = 0;
    virtual Node::Ptr visit(const SelfExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const ArrayLiteralExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const DictLiteralExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const MemberAccessExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const MemberAssignExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const SubscriptExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const ForceUnwrappingExpr::Ptr& decl) = 0;
    virtual Node::Ptr visit(const OptionalChainingExpr::Ptr& decl) = 0;

    // Statements
    virtual Node::Ptr visit(const IfStmt::Ptr& decl) = 0;
    virtual Node::Ptr visit(const WhileStmt::Ptr& decl) = 0;
    virtual Node::Ptr visit(const ReturnStmt::Ptr& decl) = 0;
    virtual Node::Ptr visit(const ImportStmt::Ptr& decl) = 0;

    // Declarations
    virtual Node::Ptr visit(const StmtsBlock::Ptr& decl) = 0;
    virtual Node::Ptr visit(const FuncDecl::Ptr& decl) = 0;
    virtual Node::Ptr visit(const ModuleDecl::Ptr& decl) = 0;
    virtual Node::Ptr visit(const ClassDecl::Ptr& classDecl) = 0;
    virtual Node::Ptr visit(const VarDecl::Ptr& varDecl) = 0;

    // Misc
    virtual Node::Ptr visit(const Pattern::Ptr& decl) = 0;
    virtual Node::Ptr visit(const TypeIdentifier::Ptr& decl) = 0;
    virtual Node::Ptr visit(const ParameterClause::Ptr& decl) = 0;

};

#endif
