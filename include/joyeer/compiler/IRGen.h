#ifndef __joyeer_compiler_irgen_h__
#define __joyeer_compiler_irgen_h__

#include "joyeer/compiler/context.h"
#include "joyeer/runtime/bytecode.h"


// Joyeer IR code generator
class IRGen {
public:
    explicit IRGen(CompileContext::Ptr context);

    ModuleClass* emit(const ModuleDecl::Ptr& block);
    
protected:
    void emit(const Node::Ptr& node);
    void emit(const FuncCallExpr::Ptr& node);
    void emit(const MemberFuncCallExpr::Ptr& node);
    void emit(const ArguCallExpr::Ptr& node);
    void emit(const LiteralExpr::Ptr& node);
    void emit(const VarDecl::Ptr& node);
    void emit(const Self::Ptr& decl);
    void emit(const SelfExpr::Ptr& decl);
    void emit(const PrefixExpr::Ptr& node);
    void emit(const PostfixExpr::Ptr& node);
    void emit(const IdentifierExpr::Ptr& node);
    void emit(const AssignExpr::Ptr& node);
    void emit(const Expr::Ptr& node);
    void emit(const OperatorExpr::Ptr& node);
    void emit(const ParenthesizedExpr::Ptr& node);
    void emit(const ForceUnwrappingExpr::Ptr& decl);
    void emit(const OptionalChainingExpr::Ptr& decl);
    void emit(const IfStmt::Ptr& node);
    void emit(const WhileStmt::Ptr& node);
    void emit(const StmtsBlock::Ptr& node);
    void emit(const FuncDecl::Ptr& node);
    void emit(const ReturnStmt::Ptr& node);
    void emit(const MemberAccessExpr::Ptr& node);
    void emit(const ClassDecl::Ptr& node);
    void emit(const ArrayLiteralExpr::Ptr& node);
    void emit(const DictLiteralExpr::Ptr& node);
    void emit(const SubscriptExpr::Ptr& node);
    void emit(const ImportStmt::Ptr& node);

    // auto wrapping source type -> destine type;
    void autoWrapping(int srcTypeSlot, int destTypeSlot);

    // gen IR that force unwrapping a value in stack
    void forceUnwrapping();
    void forceWrapping();
    
private:
    BytecodeWriter writer;
    CompileContext::Ptr context;
    CompilerService* compiler;
    Diagnostics* diagnostics;
};

#endif
