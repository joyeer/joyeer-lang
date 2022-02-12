#include "joyeer/compiler/typebinding.h"
#include "joyeer/diagnostic/diagnostic.h"
#include "joyeer/runtime/types.h"
#include "joyeer/compiler/compiler+service.h"
#include <cassert>
#include <utility>

#define CHECK_ERROR_RETURN(typeSlot) \
    if((typeSlot) == -1) {           \
        return decl;                 \
    }

TypeBinding::TypeBinding(CompileContext::Ptr context):
context(std::move(context)) {
    compiler = this->context->compiler;
    diagnostics = this->context->diagnostics;
}

Node::Ptr TypeBinding::visit(const Node::Ptr& node) {
    return NodeVisitor::visit(node);
}

Node::Ptr TypeBinding::visit(const ModuleDecl::Ptr& node) {
    
    context->visit(CompileStage::visitModule, node, [this, node](){
        auto statements = std::vector<Node::Ptr>();
        for(const auto& statement: node->statements) {
            statements.push_back(visit(statement));
        }
        node->statements =  statements;
    });
    
    return node;
}


Node::Ptr TypeBinding::visit(const ClassDecl::Ptr& decl) {

    context->visit(CompileStage::visitClassDecl, decl, [this, decl]() {
        std::vector<Node::Ptr> statements{};
        for(auto const& statement: decl->statements) {
            statements.push_back(visit(statement));
        }
        decl->statements = statements;
    });

    return decl;
}

Node::Ptr TypeBinding::visit(const FuncDecl::Ptr& decl) {

    context->visit(CompileStage::visitFuncDecl, decl, [this, decl]() {
        auto funcType = context->curFuncType();

        if(funcType->funcType != FuncType::C_CInit) {
            // if not a class constructor, analyze it's return type
            if(decl->returnType == nullptr) {
                // no return
                funcType->returnTypeSlot = compiler->getType(ValueType::Void)->slot;
            } else {
                decl->returnType = visit(decl->returnType);
                funcType->returnTypeSlot = decl->returnType->typeSlot;
                assert(funcType->returnTypeSlot != -1);
            }
        }

        // visit parameters decls
        decl->parameterClause = visit(decl->parameterClause);

        auto parameterClause = std::static_pointer_cast<ParameterClause>(decl->parameterClause);
        auto paramIndex = 0;
        for(const auto& parameter: parameterClause->parameters) {
            auto paramVar = funcType->getParamByIndex(paramIndex);
            paramVar->typeSlot = parameter->typeSlot;
            assert(paramVar->typeSlot != -1);
            paramIndex ++;
        }

        // visit FuncDecl statements
        std::vector<Node::Ptr> statements;
        for(const auto& s: decl->statements) {
            statements.push_back(visit(s));
        }
        decl->statements = statements;
    });

    return decl;
}


Node::Ptr TypeBinding::visit(const VarDecl::Ptr& decl) {

    auto simpleName = decl->getSimpleName();
    auto symtable = context->curSymTable();
    auto symbol = context->lookup(simpleName);

    assert(decl->typeSlot == -1);

    // e.g. var index, without initializer and type, report an error
    if(decl->initializer == nullptr && decl->pattern->typeExpr == nullptr) {
        diagnostics->reportError(ErrorLevel::failure, -1, -1, Diagnostics::errorVarDeclMissingAnnotation);
        return decl;
    }

    if(decl->initializer != nullptr) {
        decl->initializer = visit(decl->initializer);
    }

    context->visit(CompileStage::visitVarDecl, decl, [this, decl]() {
        decl->pattern = std::static_pointer_cast<Pattern>(visit(decl->pattern));
    });

    // type verification
    if(!verifyIfAssignExpressionIsLegal(decl->pattern, decl->initializer)) {
        return decl;
    }

    // if the pattern specify the Types, VarDecl will follow the pattern
    auto patternType = compiler->getType(decl->pattern->typeSlot);
    if(patternType->kind == ValueType::Unspecified) {
        if(decl->initializer != nullptr) {
            decl->typeSlot = decl->initializer->typeSlot;
        }
    } else {
        // follow the initializer expression's typedef
        decl->typeSlot = decl->pattern->typeSlot;
    }

    assert(decl->typeSlot != -1);
    // Update the Variable's type base on declaration
    symbol->typeSlot = decl->typeSlot;

    auto declType = context->curDeclType();
    auto variableType = new Variable(simpleName);
    variableType->typeSlot = symbol->typeSlot;
    switch (declType->kind) {
        case ValueType::Func: {
            auto funcType = (Function*)declType;
            symbol->locationInParent = funcType->getLocalVarCount();
            funcType->localVars.push_back(variableType);
        }
            break;
        case ValueType::Module: {
            auto module = (ModuleClass*)declType;
            symbol->locationInParent = module->staticFields.size();
            module->staticFields.push_back(variableType);
        }
            break;
        case ValueType::Class: {
            auto klass = (Class*)declType;
            symbol->locationInParent = klass->instanceFields.size();
            klass->instanceFields.push_back(variableType);
        }
            break;
        default:
            assert(false);
    }

    return decl;
}


Node::Ptr TypeBinding::visit(const FuncCallExpr::Ptr& decl) {

    if (decl->identifier->kind == SyntaxKind::dictLiteralExpr) {
        return visitDictFuncCallExpr(decl);
    } else if (decl->identifier->kind == SyntaxKind::arrayLiteralExpr) {
        return visitArrayFuncCallExpr(decl);
    } else {
        return visitFuncCallExpr(decl);
    }
}

Node::Ptr TypeBinding::visitArrayFuncCallExpr(const FuncCallExpr::Ptr &decl) {
    assert(decl->identifier->kind == SyntaxKind::arrayLiteralExpr);
    visit(decl->identifier);
    auto arrayLiteral = std::static_pointer_cast<ArrayLiteralExpr>(decl->identifier);
    if (arrayLiteral->items.size() == 1) {
        auto type = arrayLiteral->items[0];
//            if(kind->symbol->flag == SymbolFlag::typeSymbol) {
//                auto arrayType = std::make_shared<ArrayType>(kind);
//                node->identifier = arrayType;
//            }

        assert(false);
    }
    return decl;
}

Node::Ptr TypeBinding::visitDictFuncCallExpr(const FuncCallExpr::Ptr &decl) {
    assert(decl->identifier->kind == SyntaxKind::dictLiteralExpr);
    decl->identifier = visit(decl->identifier);
    auto dictLiteral = std::static_pointer_cast<DictLiteralExpr>(decl->identifier);
    if( dictLiteral->items.size() == 1) {
        auto item = dictLiteral->items[0];
        auto key = std::get<0>(item);
        auto value = std::get<1>(item);

        auto keySymbol = context->lookup(key->getSimpleName());
        if(keySymbol->flag != SymbolFlag::klass) {
            diagnostics->reportError(ErrorLevel::failure, "[Error] Dictionary Key is not a class");
            return decl;
        }
        auto valueSymbol = context->lookup(value->getSimpleName());
        if(valueSymbol->flag != SymbolFlag::klass) {
            diagnostics->reportError(ErrorLevel::failure, "[Error] Dictionary Value is not a class");
            return decl;
        }

        decl->typeSlot = compiler->getType(BuildIns::Object_Dict)->slot;
        decl->funcTypeSlot = compiler->getType(BuildIns::Object_Dict_Init)->slot;

    }
    return decl;
}

Node::Ptr TypeBinding::visitFuncCallExpr(const FuncCallExpr::Ptr &decl) {
    assert(decl->identifier->kind != SyntaxKind::dictLiteralExpr);
    assert(decl->identifier->kind != SyntaxKind::arrayLiteralExpr);

    auto name = decl->getCalleeFuncSimpleName();
    auto symbol = context->lookup(name);

    if(symbol == nullptr) {
        diagnostics->reportError(ErrorLevel::failure, Diagnostics::errorCannotFindFunction, name.c_str());
        return decl;
    }

    decl->funcTypeSlot = symbol->typeSlot;
    auto funcType = reinterpret_cast<Function*>(compiler->getType(decl->funcTypeSlot));
    assert(funcType != nullptr && funcType->returnTypeSlot != -1);
    decl->typeSlot = funcType->returnTypeSlot;

    // visit the param
    for(const auto& argument: decl->arguments) {
        visit(argument);
    }

    return decl;
}


Node::Ptr TypeBinding::visit(const MemberFuncCallExpr::Ptr& node) {
    visit(node->callee);
    
    auto typeSlot = node->callee->typeSlot;
    assert(typeSlot != -1);

    auto symtable = compiler->getExportingSymbolTable(typeSlot);

    auto funcCallExpr = std::static_pointer_cast<FuncCallExpr>( node->member);
    auto name = funcCallExpr->getCalleeFuncSimpleName();

    auto symbol = symtable->find(name);

    if(symbol == nullptr) {
        diagnostics->reportError(ErrorLevel::failure, "[Error]Cannot find the function");
    }

    auto funcType = (Function*)(compiler->getType(symbol->typeSlot));
    assert(funcType != nullptr && funcType->returnTypeSlot);
    node->typeSlot = funcType->returnTypeSlot;
    funcCallExpr->funcTypeSlot = funcType->slot;
    node->funcTypeSlot = funcCallExpr->funcTypeSlot;

    for(const auto& argument: funcCallExpr->arguments) {
        visit(argument);
    }

    return node;
}

Node::Ptr TypeBinding::visit(const OptionalType::Ptr &decl) {
    auto name = decl->getSimpleName();
    auto symbol = context->lookup(name);
    if(symbol == nullptr) {
        diagnostics->reportError(ErrorLevel::failure, -1, -1, Diagnostics::errorCannotFindType, name.c_str());
        return decl;
    }

    decl->typeSlot = symbol->typeSlot;
    return decl;
}

Node::Ptr TypeBinding::visit(const ParameterClause::Ptr& node) {

    auto parameters = std::vector<Pattern::Ptr>();
    for(const auto& param: node->parameters) {
        parameters.push_back(std::static_pointer_cast<Pattern>(visit(param)));
    }
    node->parameters = parameters;

    // enrich the symbol's type
    for(const auto& param: node->parameters) {
        auto parameterName = param->getSimpleName();
        auto symbol = context->lookup(parameterName);
        assert(symbol != nullptr);
        assert(param->typeSlot != -1);
        symbol->typeSlot = param->typeSlot;
    }
    return node;
}

Node::Ptr TypeBinding::visit(const Pattern::Ptr& node) {
    if(node->typeExpr != nullptr) {
        context->visit(CompileStage::visitPatternType, node, [this, node]{
            node->typeExpr = visit(node->typeExpr);
        });
        node->typeSlot = node->typeExpr->typeSlot;
    } else {
        node->typeSlot = compiler->getType(ValueType::Unspecified)->slot;
    }

    assert(node->typeSlot != -1);
    return node;
}

Node::Ptr TypeBinding::visit(const IdentifierExpr::Ptr& node) {
    auto name = node->getSimpleName();
    switch (context->curStage()) {
        case CompileStage::visitModule:
        case CompileStage::visitExpr:
        case CompileStage::visitCodeBlock:
        case CompileStage::visitFuncDecl:
        case CompileStage::visitMemberAccess:
        case CompileStage::visitPatternType:{
            auto symbol = context->lookup(name);
            if(symbol == nullptr) {
                diagnostics->reportError(ErrorLevel::failure, "[TODO][Error] Cannot find variable");
            }
            assert(symbol->typeSlot != -1);
            assert(node->typeSlot == -1);
            node->typeSlot = symbol->typeSlot;
        }
            break;
        case CompileStage::visitVarDecl:
        case CompileStage::visitAssignExpr:
            // nothing to do
            break;
        default:
            break;
    }
    return node;
}

Node::Ptr TypeBinding::visit(const TypeIdentifier::Ptr& node) {
    auto simpleName = node->getSimpleName();
    auto symbol = context->lookup(simpleName);
    if(symbol == nullptr) {
        diagnostics->reportError(ErrorLevel::failure, "[Error]Cannot find type");
    } else {
        node->typeSlot = symbol->typeSlot;
    }

    return node;
}

Node::Ptr TypeBinding::visit(const StmtsBlock::Ptr& node) {
    assert(node->symtable != nullptr);

    context->visit(CompileStage::visitCodeBlock, node, [this, node]() {
        auto statements = std::vector<Node::Ptr>();
        for(const auto& statement: node->statements) {
            statements.push_back(visit(statement));
        }
        node->statements = statements;
    });
    return node;
}

Node::Ptr TypeBinding::visit(const ReturnStmt::Ptr& decl) {
    if(decl->expr != nullptr) {
        decl->expr = visit(decl->expr);
    }

    return decl;
}

Node::Ptr TypeBinding::visit(const Expr::Ptr& node) {
    context->visit(CompileStage::visitExpr, node, [this, node]() {
        auto nodes = std::vector<Node::Ptr>();
        for(const auto& n: node->nodes) {
            nodes.push_back(visit(n));
        }
        node->nodes = nodes;

        node->typeSlot = typeOf(node)->slot;
    });
    
    return node;
}


Node::Ptr TypeBinding::visit(const LiteralExpr::Ptr& node) {
    switch(node->literal->kind) {
        case TokenKind::decimalLiteral:
            node->typeSlot = compiler->getType(ValueType::Int)->slot;
            break;
        case TokenKind::stringLiteral:
            node->typeSlot = compiler->getType(ValueType::String)->slot;
            break;
        case TokenKind::booleanLiteral:
            node->typeSlot = compiler->getType(ValueType::Bool)->slot;
            break;
        case TokenKind::nilLiteral:
            node->typeSlot = compiler->getType(ValueType::Nil)->slot;
            break;
        default:
            assert(false);
    }
    return node;
}

Node::Ptr TypeBinding::visit(const AssignExpr::Ptr& node) {
    node->left = visit(node->left);
    node->expr = visit(node->expr);
    return node;
}

Node::Ptr TypeBinding::visit(const ParenthesizedExpr::Ptr& node) {
    node->expr = visit(node->expr);
    return node;
}

Node::Ptr TypeBinding::visit(const IfStmt::Ptr& node) {
    node->condition = visit(node->condition);
    
    context->visit(CompileStage::visitCodeBlock, node->ifCodeBlock, [this, node](){
        node->ifCodeBlock = visit(node->ifCodeBlock);
    });
    
    if(node->elseCodeBlock != nullptr) {
        context->visit(CompileStage::visitCodeBlock, node->elseCodeBlock, [this, node](){
            node->elseCodeBlock = visit(node->elseCodeBlock);
        });
    }
    
    return node;
}

Node::Ptr TypeBinding::visit(const WhileStmt::Ptr& node) {
    node->expr = visit(node->expr);
    
    context->visit(CompileStage::visitCodeBlock, node->codeBlock, [this, node]() {
        node->codeBlock = visit(node->codeBlock);
    });
    return node;
}

Node::Ptr TypeBinding::visit(const ArguCallExpr::Ptr& node) {
    node->expr = visit(node->expr);
    node->typeSlot = node->expr->typeSlot;
    return node;
}

Node::Ptr TypeBinding::visit(const Self::Ptr& decl) {
    auto symbol = context->lookup(decl->getSimpleName());
    if(symbol == nullptr) {
        diagnostics->reportError(ErrorLevel::failure, "self not found", -1, -1);
    }

    decl->typeSlot = symbol->typeSlot;
    return decl;
}

Node::Ptr TypeBinding::visit(const SelfExpr::Ptr& node) {
    if(node->identifier != nullptr) {
        node->identifier = std::static_pointer_cast<IdentifierExpr>(visit(node->identifier));
    }

    node->self = std::static_pointer_cast<Self>(visit(node->self));

    if(node->identifier != nullptr) {
        assert(node->identifier->typeSlot != -1);
        node->typeSlot = node->identifier->typeSlot;
    } else {
        assert(node->self->typeSlot != -1);
        node->typeSlot = node->self->typeSlot;
    }
    return node;
}

Node::Ptr TypeBinding::visit(const ArrayLiteralExpr::Ptr& node) {
    node->typeSlot = compiler->getType(BuildIns::Object_Array)->slot;
    auto items = std::vector<Node::Ptr>();
    for(const auto& item: node->items) {
        items.push_back(visit(item));
    }
    node->items = items;
    return node;
}

Node::Ptr TypeBinding::visit(const DictLiteralExpr::Ptr& node) {
    node->typeSlot = compiler->getType(BuildIns::Object_Dict)->slot;
    auto items = std::vector<std::tuple<Node::Ptr, Node::Ptr>>();
    for(auto item: node->items) {
        items.emplace_back(
            visit(std::get<0>(item)),
            visit(std::get<1>(item))
        );
    }
    node->items = items;
    return node;
}

Node::Ptr TypeBinding::visit(const MemberAccessExpr::Ptr& node) {
    node->callee = visit(node->callee);

    assert(node->callee->typeSlot != -1);
    auto calleeType = compiler->getType(node->callee->typeSlot);
    if(calleeType->kind == ValueType::Optional) {
        auto optionalType = (Optional*)calleeType;
        auto wrappedType = compiler->getType((int)optionalType->wrappedTypeSlot);
        // report an error
        diagnostics->reportError(
                ErrorLevel::failure,
                Diagnostics::errorOptionalAccessWithoutUnwrapping,
                wrappedType->name.c_str(),
                node->member->getSimpleName().c_str(),
                wrappedType->name.c_str()
                );
        return node;
    }

    auto symtable = compiler->getExportingSymbolTable(node->callee->typeSlot);
    assert(symtable != nullptr);

    context->symbols.push_back(symtable);
    context->visit(CompileStage::visitMemberAccess, node->callee, [this, node](){
        node->member = visit(node->member);
    });
    context->symbols.pop_back();

    node->typeSlot = node->member->typeSlot;

    return node;
}

Node::Ptr TypeBinding::visit(const MemberAssignExpr::Ptr& node) {
    return node;
}

Node::Ptr TypeBinding::visit(const SubscriptExpr::Ptr& node) {
    node->identifier = visit(node->identifier);
    node->indexExpr = visit(node->indexExpr);

    auto simpleName = node->identifier->getSimpleName();
    auto symbol = context->lookup(simpleName);

    assert(symbol->typeSlot == compiler->getType(BuildIns::Object_Array)->slot ||
        symbol->typeSlot == compiler ->getType(BuildIns::Object_Dict)->slot);

    if(symbol->typeSlot == compiler->getType(BuildIns::Object_Array)->slot) {
        node->typeSlot = compiler->getType(ValueType::Int)->slot;  // TODO: get the right ValueType
    } else if(symbol->typeSlot == compiler ->getType(BuildIns::Object_Dict)->slot){
        node->typeSlot = compiler->getType(BuildIns::Object_Optional_Int)->slot;  // TODO: get the right ValueType
    } else {
        assert(false);
    }

    return node;
}

Node::Ptr TypeBinding::visit(const ArrayType::Ptr& node) {
    node->valueType = visit(node->valueType);
    node->typeSlot = compiler->getType(BuildIns::Object_Array)->slot;
    return node;
}

Node::Ptr TypeBinding::visit(const PrefixExpr::Ptr& node) {
    node->expr = visit(node->expr);
    node->typeSlot = node->expr->typeSlot;
    assert(node->typeSlot > -1);
    return node;
}

Node::Ptr TypeBinding::visit(const PostfixExpr::Ptr &node) {
    node->expr = visit(node->expr);
    if(node->op->token->rawValue == "!") {
        node->typeSlot = node->expr->typeSlot;
    } else {
        assert(false);
    };
    return node;
}

Node::Ptr TypeBinding::visit(const ForceUnwrappingExpr::Ptr& decl) {
    decl->wrappedExpr = visit(decl->wrappedExpr);

    CHECK_ERROR_RETURN(decl->wrappedExpr->typeSlot)

    auto type = compiler->getType(decl->wrappedExpr->typeSlot);
    if(type->kind == ValueType::Optional) {
        auto optionalType = (Optional*)type;
        decl->typeSlot = optionalType->wrappedTypeSlot;
    } else {
        // TODO: report an error
    }
    return decl;
}

Node::Ptr TypeBinding::visit(const OptionalChainingExpr::Ptr& decl) {
    return decl;
}

Node::Ptr TypeBinding::visit(const ImportStmt::Ptr& node) {
    return node;
}

Node::Ptr TypeBinding::visit(const  BinaryExpr::Ptr& decl) {
    assert(false);
}

Node::Ptr TypeBinding::visit(const OperatorExpr::Ptr& decl) {
    return decl;
}

Type* TypeBinding::typeOf(const Node::Ptr& node) {
    switch (node->kind) {
        case SyntaxKind::identifierExpr:
            assert(node->typeSlot != -1);
            return compiler->getType(node->typeSlot);
        case SyntaxKind::expr:
            return typeOf(std::static_pointer_cast<Expr>(node));
        case SyntaxKind::funcCallExpr:
            return typeOf(std::static_pointer_cast<FuncCallExpr>(node));
        case SyntaxKind::literalExpr:
            return typeOf(std::static_pointer_cast<LiteralExpr>(node));
        case SyntaxKind::parenthesizedExpr:
            return typeOf(std::static_pointer_cast<ParenthesizedExpr>(node));
        case SyntaxKind::selfExpr:
            return typeOf(std::static_pointer_cast<SelfExpr>(node));
        case SyntaxKind::pattern:
            return typeOf(std::static_pointer_cast<Pattern>(node));
        case SyntaxKind::type:
            return typeOf(std::static_pointer_cast<TypeIdentifier>(node));
        case SyntaxKind::arrayLiteralExpr:
            return typeOf(std::static_pointer_cast<ArrayLiteralExpr>(node));
        case SyntaxKind::dictLiteralExpr:
            return typeOf(std::static_pointer_cast<DictLiteralExpr>(node));
        case SyntaxKind::memberAccessExpr:
            return typeOf(std::static_pointer_cast<MemberAccessExpr>(node));
        case SyntaxKind::memberFuncCallExpr:
            return typeOf(std::static_pointer_cast<MemberFuncCallExpr>(node));
        case SyntaxKind::subscriptExpr:
            return typeOf(std::static_pointer_cast<SubscriptExpr>(node));
        case SyntaxKind::arrayType:
            return typeOf(std::static_pointer_cast<ArrayType>(node));
        case SyntaxKind::prefixExpr:
            return typeOf(std::static_pointer_cast<PrefixExpr>(node));
        case SyntaxKind::postfixExpr:
            return typeOf(std::static_pointer_cast<PostfixExpr>(node));
        case SyntaxKind::forceUnwrapExpr:
            return typeOf(std::static_pointer_cast<ForceUnwrappingExpr>(node));
        default:
            assert(false);
    }
}

Type* TypeBinding::typeOf(const Expr::Ptr& node) {
    assert(node->binaries.empty());
    assert(node->prefix == nullptr);

    if(node->typeSlot != -1) {
        return compiler->getType(node->typeSlot);
    }

    std::stack<Type*> stack;
    for (const auto &n: node->nodes)
        if (n->kind == SyntaxKind::operatorExpr) {
            auto leftType = stack.top();
            stack.pop();
            auto rightType = stack.top();
            stack.pop();
            if(leftType->kind == rightType->kind) {
                stack.push(leftType);
            } else {
                stack.push(leftType);
            }

        } else {
            auto type = typeOf(n);
            assert(type != nullptr);
            stack.push(type);
        };

    assert(stack.size() == 1);
    return stack.top();
}

Type* TypeBinding::typeOf(const LiteralExpr::Ptr& node) {
    switch(node->literal->kind) {
        case booleanLiteral:
            return context->compiler->getType(ValueType::Bool);
        case decimalLiteral:
            return context->compiler->getType(ValueType::Int);
        case nilLiteral:
            return context->compiler->getType(ValueType::Nil);
        case stringLiteral:
            return context->compiler->getType(ValueType::String);
        default:
            assert(false);
    }
}

Type* TypeBinding::typeOf(const FuncCallExpr::Ptr& node) {
    auto funcName = node->getSimpleName();
    auto symbol = context->lookup(funcName);
    assert(symbol->flag == SymbolFlag::func);
    return compiler->getType(symbol->typeSlot);
}

Type* TypeBinding::typeOf(const MemberFuncCallExpr::Ptr& node) {
    assert(node->funcTypeSlot != -1);
    auto funcType = (Function*)( compiler->getType(node->funcTypeSlot));
    auto type = compiler->getType(funcType->returnTypeSlot);
    assert(type != nullptr);
    return type;
}

Type* TypeBinding::typeOf(const ParenthesizedExpr::Ptr& node) {
    return typeOf(node->expr);
}

Type* TypeBinding::typeOf(const SelfExpr::Ptr& node) {
    return typeOf(node->identifier);
}

Type* TypeBinding::typeOf(const Pattern::Ptr& node) {
    if(node->typeSlot == -1) {
        return compiler->getType(ValueType::Any);
    }
    return typeOf(node->typeExpr);
}

Type* TypeBinding::typeOf(const TypeIdentifier::Ptr& node) {
    assert(node->typeSlot != -1);
    return compiler->getType(node->typeSlot);
}

Type* TypeBinding::typeOf(const DictLiteralExpr::Ptr& node) {
    assert(false);
}

Type* TypeBinding::typeOf(const ArrayLiteralExpr::Ptr& node) {
    Type* previousType = nullptr;
    for(const auto& item: node->items) {
        auto type = typeOf(item);
        if(previousType != nullptr) {
            if(type->kind != previousType->kind) {
                return context->compiler->getType(ValueType::Any);
            }
        }

        previousType = type;
    }

    assert(false);
    return previousType;
}

Type* TypeBinding::typeOf(const MemberAccessExpr::Ptr& node) {
    return typeOf(node->member);
}

Type* TypeBinding::typeOf(const SubscriptExpr::Ptr& node) {
    return typeOf(node->identifier);
}

Type* TypeBinding::typeOf(const ArrayType::Ptr& node) {
    assert(false);
}

Type* TypeBinding::typeOf(const PrefixExpr::Ptr& node) {
    return typeOf(node->expr);
}

Type* TypeBinding::typeOf(const PostfixExpr::Ptr& node) {
    return typeOf(node->expr);
}

Type* TypeBinding::typeOf(const ForceUnwrappingExpr::Ptr &node) {
    return compiler->getType(node->typeSlot);
}

bool TypeBinding::verifyIfAssignExpressionIsLegal(const Node::Ptr &left, const Node::Ptr &right) {

    // rule 1: if left not specific the type, always, the right's type is assigned to left
    if(left->typeSlot == compiler->getType(ValueType::Unspecified)->slot) {
        // var i = nil
        if(right->typeSlot == compiler->getType(ValueType::Nil)->slot) {
            diagnostics->reportError(ErrorLevel::failure, -1, -1, Diagnostics::errorNilRequireContextualType);
            return false;
        }
        return true;
    }

    if(right != nullptr) {
        // rule 2: if right type is nil
        if(right->typeSlot == compiler->getType(ValueType::Nil)->slot) {
            auto leftType = compiler->getType(left->typeSlot);
            if(leftType->kind != ValueType::Optional) {
                diagnostics->reportError(ErrorLevel::failure, -1, -1, Diagnostics::errorNilCannotInitializeSpecifiedType, leftType->name.c_str());
                return false;
            }
        }
    }

    // rule 3: if right is nil

    return true;
}