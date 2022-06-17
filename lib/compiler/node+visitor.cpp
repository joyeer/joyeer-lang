#include "joyeer/compiler/node+visitor.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

Node::Ptr NodeVisitor::visit(const Node::Ptr& node) {

#define NODE_VISITOR(flag, type) \
    case flag: \
        return visit(std::static_pointer_cast< type >(node));
    
    switch (node->kind) {
        NODE_VISITOR(SyntaxKind::module,                ModuleDecl)
        NODE_VISITOR(SyntaxKind::funcDecl,              FuncDecl)
        NODE_VISITOR(SyntaxKind::classDecl,             ClassDecl)
        NODE_VISITOR(SyntaxKind::type,                  TypeIdentifier)
        NODE_VISITOR(SyntaxKind::pattern,               Pattern)
        NODE_VISITOR(SyntaxKind::varDecl,               VarDecl)
        NODE_VISITOR(SyntaxKind::parameterClause,       ParameterClause)
        NODE_VISITOR(SyntaxKind::stmtsBlock,            StmtsBlock)
        NODE_VISITOR(SyntaxKind::ifStmt,                IfStmt)
        NODE_VISITOR(SyntaxKind::whileStmt,             WhileStmt)
        NODE_VISITOR(SyntaxKind::returnStmt,            ReturnStmt)
        NODE_VISITOR(SyntaxKind::self,                  Self)
        NODE_VISITOR(SyntaxKind::expr,                  Expr)
        NODE_VISITOR(SyntaxKind::selfExpr,              SelfExpr)
        NODE_VISITOR(SyntaxKind::prefixExpr,            PrefixExpr)
        NODE_VISITOR(SyntaxKind::postfixExpr,           PostfixExpr)
        NODE_VISITOR(SyntaxKind::identifierExpr,        IdentifierExpr)
        NODE_VISITOR(SyntaxKind::parenthesizedExpr,     ParenthesizedExpr)
        NODE_VISITOR(SyntaxKind::arguCallExpr,          ArguCallExpr)
        NODE_VISITOR(SyntaxKind::funcCallExpr,          FuncCallExpr)
        NODE_VISITOR(SyntaxKind::forceUnwrapExpr,       ForceUnwrappingExpr)
        NODE_VISITOR(SyntaxKind::optionalChainingExpr,  OptionalChainingExpr)
        NODE_VISITOR(SyntaxKind::memberAccessExpr,      MemberAccessExpr)
        NODE_VISITOR(SyntaxKind::memberAssignExpr,      MemberAssignExpr)
        NODE_VISITOR(SyntaxKind::memberFuncCallExpr,    MemberFuncCallExpr)
        NODE_VISITOR(SyntaxKind::literalExpr,           LiteralExpr)
        NODE_VISITOR(SyntaxKind::arrayLiteralExpr,      ArrayLiteralExpr)
        NODE_VISITOR(SyntaxKind::dictLiteralExpr,       DictLiteralExpr)
        NODE_VISITOR(SyntaxKind::assignExpr,            AssignExpr)
        NODE_VISITOR(SyntaxKind::binaryExpr,            BinaryExpr)
        NODE_VISITOR(SyntaxKind::operatorExpr,          OperatorExpr)
        NODE_VISITOR(SyntaxKind::subscriptExpr,         SubscriptExpr)
        NODE_VISITOR(SyntaxKind::arrayType,             ArrayType)
        NODE_VISITOR(SyntaxKind::optionalType,          OptionalType)
        NODE_VISITOR(SyntaxKind::importStmt,            ImportStmt)
        default:
            assert(false);
    }
    return nullptr;
}
