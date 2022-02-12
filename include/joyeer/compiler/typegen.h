#ifndef __joyeer_compiler_syntax_typegen_h__
#define __joyeer_compiler_syntax_typegen_h__


#include "joyeer/compiler/node+visitor.h"
#include "joyeer/compiler/context.h"


struct SourceFile;

// Bind all types and symbols during analyzing AST tree
// Reduce the expression's depth level
class TypeGen: public NodeVisitor {
public:
    
    explicit TypeGen(CompileContext::Ptr context);
    
    Node::Ptr visit(const ModuleDecl::Ptr& decl) override;
protected:
    // recursive bind down node
    Node::Ptr visit(const Node::Ptr& node) override;

    // Declaration section
    Node::Ptr visit(const ClassDecl::Ptr& classDecl) override;
    Node::Ptr visit(const VarDecl::Ptr& varDecl) override;
    Node::Ptr visit(const TypeIdentifier::Ptr& decl) override;
    Node::Ptr visit(const OptionalType::Ptr& decl) override;
    Node::Ptr visit(const FuncCallExpr::Ptr& decl) override;
    Node::Ptr visit(const MemberFuncCallExpr::Ptr& decl) override;
    Node::Ptr visit(const ArguCallExpr::Ptr& decl) override;
    Node::Ptr visit(const LiteralExpr::Ptr& decl) override;

    // Expression section
    Node::Ptr visit(const Expr::Ptr& decl) override;
    Node::Ptr visit(const PrefixExpr::Ptr& decl) override;
    Node::Ptr visit(const PostfixExpr::Ptr& decl) override;
    Node::Ptr visit(const Self::Ptr& decl) override;
    Node::Ptr visit(const SelfExpr::Ptr& decl) override;
    Node::Ptr visit(const IdentifierExpr::Ptr& decl) override;
    Node::Ptr visit(const AssignExpr::Ptr& decl) override;
    Node::Ptr visit(const BinaryExpr::Ptr& decl) override;
    Node::Ptr visit(const OperatorExpr::Ptr& decl) override;
    Node::Ptr visit(const ParenthesizedExpr::Ptr& decl) override;
    Node::Ptr visit(const IfStmt::Ptr& decl) override;
    Node::Ptr visit(const WhileStmt::Ptr& decl) override;
    Node::Ptr visit(const StmtsBlock::Ptr& decl) override;
    Node::Ptr visit(const FuncDecl::Ptr& decl) override;
    Node::Ptr visit(const ParameterClause::Ptr& decl) override;
    Node::Ptr visit(const Pattern::Ptr& decl) override;
    Node::Ptr visit(const ReturnStmt::Ptr& decl) override;
    Node::Ptr visit(const ArrayLiteralExpr::Ptr& decl) override;
    Node::Ptr visit(const DictLiteralExpr::Ptr& decl) override;
    Node::Ptr visit(const MemberAccessExpr::Ptr& decl) override;
    Node::Ptr visit(const MemberAssignExpr::Ptr& decl) override;
    Node::Ptr visit(const ForceUnwrappingExpr::Ptr& decl) override;
    Node::Ptr visit(const OptionalChainingExpr::Ptr& decl) override;
    Node::Ptr visit(const SubscriptExpr::Ptr& decl) override;
    Node::Ptr visit(const ArrayType::Ptr& decl) override;
    Node::Ptr visit(const ImportStmt::Ptr& decl) override;

    void processClassConstructors(const ClassDecl::Ptr& decl);

    // process class member func, e.g. self
    Node::Ptr processClassMemberFunc(const FuncDecl::Ptr& decl);

    // generate optional type in global
    void generateOptionalGlobally(const OptionalType::Ptr& optionalType);

private:
    CompileContext::Ptr context;
    CompilerService* compiler;
    Diagnostics* diagnostics;
};


//------------------------------------------------------------------------
// process Member func, add "self" for each member field/class
//------------------------------------------------------------------------

class TypeGenSelfForMemberFunc : public NodeVisitor {
public:
    // Type declaration

    Node::Ptr visit(const FuncDecl::Ptr& decl) override;

    // Expressions
    Node::Ptr visit(const Expr::Ptr& decl) override;
    Node::Ptr visit(const FuncCallExpr::Ptr& decl) override;
    Node::Ptr visit(const ArguCallExpr::Ptr& decl) override;
    Node::Ptr visit(const IdentifierExpr::Ptr& decl) override;
    Node::Ptr visit(const ReturnStmt::Ptr& decl) override;

    Node::Ptr visit(const VarDecl::Ptr& varDecl) override { assert(false); }
    Node::Ptr visit(const TypeIdentifier::Ptr& decl) override { assert(false); }

    // Type annotation
    Node::Ptr visit(const ArrayType::Ptr& decl) override { assert(false); }
    Node::Ptr visit(const OptionalType::Ptr& decl) override { assert(false); }

    // FuncCallExpr visitor
    Node::Ptr visit(const MemberFuncCallExpr::Ptr& decl) override { assert(false); }

    Node::Ptr visit(const ModuleDecl::Ptr& decl) override { assert(false); }
    Node::Ptr visit(const ClassDecl::Ptr& classDecl) override  { assert(false); }

    Node::Ptr visit(const LiteralExpr::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const PrefixExpr::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const PostfixExpr::Ptr& decl) override  { assert(false); }


    Node::Ptr visit(const AssignExpr::Ptr& decl) override { assert(false); }
    Node::Ptr visit(const BinaryExpr::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const OperatorExpr::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const ParenthesizedExpr::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const IfStmt::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const WhileStmt::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const StmtsBlock::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const ParameterClause::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const Pattern::Ptr& decl) override  { assert(false); }

    Node::Ptr visit(const Self::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const SelfExpr::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const ArrayLiteralExpr::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const DictLiteralExpr::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const MemberAccessExpr::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const MemberAssignExpr::Ptr& decl) override  { assert(false); }
    Node::Ptr visit(const ForceUnwrappingExpr::Ptr& decl) override { assert(false); }
    Node::Ptr visit(const OptionalChainingExpr::Ptr& decl) override { assert(false); }
    Node::Ptr visit(const SubscriptExpr::Ptr& decl) override { assert(false); }
    Node::Ptr visit(const ImportStmt::Ptr& decl) override { assert(false); }

protected:
    friend class TypeGen;
    CompileContext::Ptr context;
    CompilerService* compiler;

};

#endif
