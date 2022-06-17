#include "joyeer/compiler/typegen.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/compiler/compiler+service.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <cassert>
#include <utility>
#include <deque>


//---------------------------------------------
// TypeGen
//---------------------------------------------

TypeGen::TypeGen(CompileContext::Ptr context):
        NodeVisitor(),
        context(std::move(context)) {
    compiler = this->context->compiler;
    diagnostics = this->context->diagnostics;
}

Node::Ptr TypeGen::visit(const Node::Ptr& node) {
    return NodeVisitor::visit(node);
}

Node::Ptr TypeGen::visit(const ModuleDecl::Ptr& decl) {
    // register Module

    auto module = new ModuleClass(decl->getSimpleName());
    context->compiler->declare(module);
    decl->typeSlot = module->slot;

    context->visit(CompileStage::visitModule, decl, [decl, this]() {
        auto statements = std::vector<Node::Ptr>();
        for(const auto& statement : decl->statements) {
            statements.push_back(NodeVisitor::visit(statement));
        }
        decl->statements = statements;
    });
    
    return decl;
}

Node::Ptr TypeGen::visit(const ClassDecl::Ptr& decl) {
    
    auto symtable = context->curSymTable();
    auto name = decl->getSimpleName();
    
    if(symtable->find(name) != nullptr) {
        diagnostics->reportError(ErrorLevel::failure, -1, -1 , Diagnostics::errorInvalidRedeclaration, name.c_str());
        return decl;
    }

    auto klass = new Class(name);
    compiler->declare(klass);

    decl->typeSlot = klass->slot;

    auto symbol = std::make_shared<Symbol>(SymbolFlag::klass, name, klass->slot);
    symtable->insert(symbol);

    context->visit(CompileStage::visitClassDecl, decl, [this, decl]() {
        std::vector<Node::Ptr> statements{};
        for(auto const& statement: decl->statements) {
            statements.push_back(visit(statement));
        }
        decl->statements = statements;

        // check weather have class constructor
        processClassConstructors(decl);
    });

    // all ClassDecl's constructors should be exported as Symbol
    for(const auto& entry : *decl->symtable ) {
        if(entry.second->flag == SymbolFlag::constructor) {
            symtable->insert(entry.second);
        }
    }

    compiler->exportClassDecl(decl);

    return decl;
}

void TypeGen::processClassConstructors(const ClassDecl::Ptr& decl) {
    bool hasConstructor = false;
    for(const auto & iterator : *decl->symtable) {
        auto symbol = iterator.second;
        if(symbol->flag == SymbolFlag::constructor) {
            hasConstructor = true;
            break;
        }
    };

    // build default <init> function
    auto defaultCInit = new Function(decl->getSimpleName() + ".<cinit>", true);
    {
        defaultCInit->funcType = FuncType::VM_CInit;
        defaultCInit->paramCount = 1;
        // set self variable
        auto selfVariable = new Variable("self");
        selfVariable->typeSlot = decl->typeSlot;
        selfVariable->loc = 0;
        defaultCInit->localVars.push_back(selfVariable);

        // set the return type slot
        defaultCInit->returnTypeSlot = decl->typeSlot;

        compiler->declare(defaultCInit);
    }

    auto klass = context->curClassType();
    assert(klass != nullptr);
    klass->defaultInitializerSlot = defaultCInit->slot;

    auto symtable = context->curSymTable();
    if(!hasConstructor) {
        auto defaultConstructor = FuncDecl::createDefaultConstructor();
        decl->statements.push_back(visit(defaultConstructor));
    }



}

Node::Ptr TypeGen::visit(const FuncDecl::Ptr& decl) {
    auto symtable = context->curSymTable();

    auto funcSimpleName = decl->getSimpleName();

    // try to bind class for func
    switch (context->curStage()) {
        case CompileStage::visitClassDecl:
            decl->bindClass(context->curClassType());
            break;
        default:
            break;
    }

    if(decl->isConstructor) {
        auto classType = context->curClassType();
        funcSimpleName = decl->getSimpleConstructorName(classType->name);
    }

    // check if the function name duplicated
    if(symtable->find(funcSimpleName) != nullptr) {
        diagnostics->reportError(ErrorLevel::failure, "[Error] Duplicate function name");
        return decl;
    }

    // define Function
    auto funcType = new Function(funcSimpleName, false);
    compiler->declare(funcType);
    decl->typeSlot = funcType->slot;

    if(decl->isConstructor) {
        funcType->funcType = FuncType::VM_CInit;
    }

    // all Module's func are static
    if( context->curDeclType()->kind == ValueType::Module) {
        funcType->isStatic = true;
    }

    auto symbolFlag = decl->isConstructor ? SymbolFlag::constructor : SymbolFlag::func;

    // prepare the symbol, register the symbol into parentTypeSlot
    auto symbol = std::make_shared<Symbol>(symbolFlag, funcSimpleName, funcType->slot);
    symtable->insert(symbol);
    
    // visit func declaration
    context->visit(CompileStage::visitFuncDecl, decl, [this, funcType, decl]() {
        // start to process function name
        context->visit(CompileStage::visitFuncNameDecl, decl->identifier, [this, decl]() {
            if(decl->identifier != nullptr) {
                decl->identifier = visit(decl->identifier);
            }
        });
        
        // start to process function parameters
        context->visit(CompileStage::visitPatternType, decl->parameterClause, [this, decl]() {
            if(decl->parameterClause != nullptr) {
                decl->parameterClause = visit(decl->parameterClause);
            }
        });

        // generate parameter types
        auto parameterClause = std::static_pointer_cast<ParameterClause>(decl->parameterClause);
        for(const auto& parameter: parameterClause->parameters) {
            auto parameterName = parameter->getSimpleName();
            auto variable = new Variable(parameterName);
            funcType->paramCount ++;
            funcType->localVars.push_back(variable);
        }

        // visit FuncDecl statements
        std::vector<Node::Ptr> statements;
        for(const auto& s: decl->statements) {
            statements.push_back(visit(s));
        }
        decl->statements = statements;


        // check the function return statement
        if(decl->returnType == nullptr) {

            if( !decl->statements.empty() ) {
                auto lastStatement = decl->statements.back();
                if(lastStatement->kind != SyntaxKind::returnStmt) {
                    decl->statements.push_back(std::make_shared<ReturnStmt>(nullptr));
                }
            } else {
                decl->statements.push_back(std::make_shared<ReturnStmt>(nullptr));
            }
        } else {
            decl->returnType = visit(decl->returnType);
        }
    });

    // post process the class member func
    if(!funcType->isStatic) {
        return processClassMemberFunc(decl);
    }
    return decl;
}


Node::Ptr TypeGen::visit(const VarDecl::Ptr& decl) {

    if(decl->initializer != nullptr) {
        decl->initializer = visit(decl->initializer);
    }

    context->visit(CompileStage::visitVarDecl, decl, [this, decl]() {
        decl->pattern = std::static_pointer_cast<Pattern>(visit(decl->pattern));
    });

    auto pattern = decl->pattern;
    auto symtable = context->curSymTable();
    auto name = pattern->identifier->getSimpleName();
    
    if(symtable->find(name) != nullptr) {
        diagnostics->reportError(ErrorLevel::failure, "[Error] duplicate variable name");
    }

    // if the closest declaration type, is the ModuleClass/Class,
    // the variable will be treated as field in Symbol
    auto declType = context->curDeclType();
    auto flag = SymbolFlag::var;
    switch (declType->kind) {
        case ValueType::Module:
            flag = SymbolFlag::field;
            break;
        case ValueType::Func:
            flag = SymbolFlag::var;
            break;
        case ValueType::Class:
            flag = SymbolFlag::field;
            break;
        default:
            assert(false);
    }

    auto symbol = std::make_shared<Symbol>(flag, name, compiler->getType(ValueType::Unspecified)->slot);
    symtable->insert(symbol);
    symbol->parentTypeSlot = declType->slot;

    switch (context->curStage()) {
        case CompileStage::visitModule:
            symbol->isStatic = true;
            break;
        case CompileStage::visitClassDecl:
            symbol->isStatic = false; // TODO: update after introduce static field
            break;
        case CompileStage::visitFuncDecl:
        case CompileStage::visitCodeBlock:
            symbol->isStatic = false;
            break;
        default:
            assert(false);
    }

    return decl;
}

Node::Ptr TypeGen::visit(const OptionalType::Ptr& decl) {
    if(decl->required) {
        return decl->type;
    }

    return decl;
}

Node::Ptr TypeGen::visit(const FuncCallExpr::Ptr& decl) {
    
    if(decl->identifier->kind == SyntaxKind::memberAccessExpr) {
        // Transfer to MemberF
        auto memberAccessExpr = std::static_pointer_cast<MemberAccessExpr>(decl->identifier);
        auto funcCallExpr = std::make_shared<FuncCallExpr>(memberAccessExpr->member, decl->arguments);
        auto memberFuncCallExpr = std::make_shared<MemberFuncCallExpr>(memberAccessExpr->callee, funcCallExpr);
        return visit(memberFuncCallExpr);
    }

    // go down to bind argument
    std::vector<ArguCallExpr::Ptr> argus;
    for(auto& parameter: decl->arguments) {
        argus.push_back(std::static_pointer_cast<ArguCallExpr>(visit(parameter)));
    }
    decl->arguments = argus;
    
    return decl;
}

Node::Ptr TypeGen::visit(const MemberFuncCallExpr::Ptr& decl) {
    decl->callee = visit(decl->callee);
    decl->member = visit(decl->member);
    return decl;
}

Node::Ptr TypeGen::visit(const ArguCallExpr::Ptr& decl) {
    decl->expr = visit(decl->expr);
    
    return decl;
}

Node::Ptr TypeGen::visit(const LiteralExpr::Ptr& decl) {
    return decl;
}

Node::Ptr TypeGen::visit(const PrefixExpr::Ptr& decl) {
    decl->expr = visit(decl->expr);
    return decl;
}

Node::Ptr TypeGen::visit(const PostfixExpr::Ptr& decl) {
    decl->op = std::static_pointer_cast<OperatorExpr>(visit(decl->op));
    decl->expr = visit(decl->expr);

    auto op = decl->op->token->rawValue;
    if (op == Operators::POINT) {
        return std::make_shared<ForceUnwrappingExpr>(decl->expr);
    } else if( op == Operators::QUESTION) {
        return std::make_shared<OptionalChainingExpr>(decl->expr);
    }
    return decl;
}

Node::Ptr TypeGen::visit(const ForceUnwrappingExpr::Ptr& decl) {
    // ForceUnwrappingExpr is generated by TypeGen
    return decl;
}

Node::Ptr TypeGen::visit(const OptionalChainingExpr::Ptr &decl) {
    // OptionalChainingExpr is generated by TypeGen
    return decl;
}

Node::Ptr TypeGen::visit(const IdentifierExpr::Ptr& decl) {
    auto name = decl->getSimpleName();

    switch (context->curStage()) {
        case CompileStage::visitPatternType: {
            // verify the func declaration's parameter duplicated name
            auto table = context->curSymTable();
            if(table->find(name) != nullptr) {
                diagnostics->reportError(ErrorLevel::failure, "[Error] duplicate variable declaration in function");
                return nullptr;
            }
            return decl;
        }
        case CompileStage::visitModule:
        case CompileStage::visitCodeBlock: {
            auto symbol = context->lookup(name);
            if(symbol == nullptr) {
                diagnostics->reportError(ErrorLevel::failure, "[Error] cannot find variable declaration in function");
            }
        }
            return decl;
        case CompileStage::visitFuncDecl:
        case CompileStage::visitMemberAccess:
        case CompileStage::visitVarDecl:
        case CompileStage::visitFuncNameDecl:
            return decl;
        default:
            assert(false);
    }
    return nullptr;
}

Node::Ptr TypeGen::visit(const Expr::Ptr& decl) {
    
    // If binary is assignment
    if(decl->binaries.size() == 1 && decl->binaries[0]->kind == SyntaxKind::assignExpr) {
        if(decl->prefix->kind == SyntaxKind::identifierExpr) {
            auto identifier = std::static_pointer_cast<IdentifierExpr>(visit(decl->prefix));
            auto assignmentExpr = std::static_pointer_cast<AssignExpr>(visit(decl->binaries[0]));
            assignmentExpr->left = identifier;
            return assignmentExpr;
        }
        
        if(decl->prefix->kind == SyntaxKind::selfExpr) {
            auto selfExpr = std::static_pointer_cast<SelfExpr>(visit(decl->prefix));
            auto assignmentExpr = std::static_pointer_cast<AssignExpr>(visit(decl->binaries[0]));
            assignmentExpr->left = selfExpr;
            return assignmentExpr;
        }
        
        if(decl->prefix->kind == SyntaxKind::memberAccessExpr) {
            auto memberAccessExpr = std::static_pointer_cast<MemberAccessExpr>(visit(decl->prefix));
            auto assignmentExpr = std::static_pointer_cast<AssignExpr>(visit(decl->binaries[0]));
            assignmentExpr->left = memberAccessExpr;
            return assignmentExpr;
        }
        
        if(decl->prefix->kind == SyntaxKind::subscriptExpr) {
            auto subscriptExpr = std::static_pointer_cast<SubscriptExpr>(visit(decl->prefix));
            auto assignmentExpr = std::static_pointer_cast<AssignExpr>(visit(decl->binaries[0]));
            assignmentExpr->left = subscriptExpr;
            return assignmentExpr;
        }
        diagnostics->reportError(ErrorLevel::failure, "[Error] left of assignment expression must be a variable");
        return decl;
        
    }
    
    
    // visit sub nodes first;
    if(decl->prefix != nullptr) {
        decl->prefix = visit(decl->prefix);
    }
    
    std::vector<Node::Ptr> binaries;
    for(const auto& n: decl->binaries) {
        binaries.push_back(visit(n));
    }
    decl->binaries = binaries;
    
    
    // Flat the expr sub-nodes
    std::vector<Node::Ptr> nodes;
    nodes.push_back(decl->prefix);
    
    for(const auto& node: decl->binaries) {
        if(node->kind != SyntaxKind::binaryExpr) {
            diagnostics->reportError(ErrorLevel::failure, "[Error] Except an binary expression");
            return decl;
        }
        
        auto bn = std::static_pointer_cast<BinaryExpr>(node);
        nodes.push_back(bn->op);
        nodes.push_back(bn->expr);
    }
    
    std::deque<Node::Ptr> temps {};
    std::deque<OperatorExpr::Ptr> operators {};
    
    
    for(auto iterator = nodes.begin(); iterator != nodes.end(); iterator ++ ) {
        auto n  = *iterator;
        if(n->kind != SyntaxKind::operatorExpr) {
            temps.push_back(n);
            continue;
        }
        
        auto opNode = std::static_pointer_cast<OperatorExpr>(n);
        if(Operators::getPriority(opNode->token->rawValue) == low) {
            operators.push_back(opNode);
            continue;
        }
        
        // for high priority operator
        if(temps.size() == 0) {
            diagnostics->reportError(ErrorLevel::failure, "[Error] Except");
        }
        
        auto l = temps.back();
        temps.pop_back();
        iterator ++;
        if(iterator == nodes.end()) {
            diagnostics->reportError(ErrorLevel::failure, "[Error] Except");
        }
        auto r = *iterator;
        
        auto expr = std::make_shared<Expr>(std::vector<Node::Ptr> {l, r, opNode});
        temps.push_back(expr);
    }
    
    // assembly all
    std::vector<Node::Ptr> result;
    while(!temps.empty() || !operators.empty() ) {
        if(temps.size() > operators.size() ) {
            auto t = temps.front();
            temps.pop_front();
            result.push_back(t);
    
            continue;
        }
        
        auto t = temps.front();
        auto o = operators.front();
        temps.pop_front();
        operators.pop_front();
        result.push_back(t);
        result.push_back(o);
        
    }
    
    decl->prefix = nullptr;
    decl->binaries.clear();
    decl->nodes = result;
    
    return decl;
}

Node::Ptr TypeGen::visit(const AssignExpr::Ptr& decl) {
    decl->expr = visit(decl->expr);
    return decl;
}

Node::Ptr TypeGen::visit(const BinaryExpr::Ptr& decl) {
    decl->expr = visit(decl->expr);
    return decl;
}

Node::Ptr TypeGen::visit(const OperatorExpr::Ptr& decl) {
    return decl;
}

Node::Ptr TypeGen::visit(const ParenthesizedExpr::Ptr& decl) {
    if(decl->expr->kind == SyntaxKind::parenthesizedExpr) {
        auto n = std::static_pointer_cast<ParenthesizedExpr>(decl->expr);
        return visit(n);
    }
    
    decl->expr = visit(decl->expr);
    return decl;
}

Node::Ptr TypeGen::visit(const IfStmt::Ptr& decl) {
    decl->condition = visit(decl->condition);
    decl->ifCodeBlock = std::static_pointer_cast<StmtsBlock>(visit(decl->ifCodeBlock));
    if(decl->elseCodeBlock != nullptr) {
        decl->elseCodeBlock = std::static_pointer_cast<StmtsBlock>(visit(decl->elseCodeBlock));
    }
    return decl;
}

Node::Ptr TypeGen::visit(const WhileStmt::Ptr& decl) {
    decl->expr = visit(decl->expr);
    decl->codeBlock = visit(decl->codeBlock);
    return decl;
}

Node::Ptr TypeGen::visit(const StmtsBlock::Ptr& decl) {

    // start to process code block
    context->visit(CompileStage::visitCodeBlock, decl,[decl, this]() {
        std::vector<Node::Ptr> statements;
        for(const auto& s: decl->statements) {
            statements.push_back(visit(s));
        }
        decl->statements = statements;
    });
    
    return decl;
}


Node::Ptr TypeGen::visit(const ParameterClause::Ptr& decl) {
    std::vector<Pattern::Ptr> parameters;
    for(const auto& parameter: decl->parameters) {
        auto pattern = std::static_pointer_cast<Pattern>(visit(parameter));
        parameters.push_back(pattern);

        // register the parameter into Symbol table
        auto symtable = context->curSymTable();
        auto parameterSimpleName = pattern->identifier->getSimpleName();

        symtable->insert(std::make_shared<Symbol>(SymbolFlag::var, parameterSimpleName, -1));
    }
    
    decl->parameters = parameters;
    return decl;
}

Node::Ptr TypeGen::visit(const Pattern::Ptr& decl) {
    decl->identifier = std::static_pointer_cast<IdentifierExpr>(visit(decl->identifier));

    if(decl->typeExpr != nullptr && decl->typeExpr->kind == SyntaxKind::optionalType) {
        auto typeName = decl->typeExpr->getSimpleName();
        if(context->lookup(typeName) == nullptr) {
            // for these optional types, dynamically generate Optional<?> type for it.
            generateOptionalGlobally(std::static_pointer_cast<OptionalType>(decl->typeExpr));
        }
    }

    return decl;
}

Node::Ptr TypeGen::visit(const TypeIdentifier::Ptr& decl) {
    return decl;
}

Node::Ptr TypeGen::visit(const ReturnStmt::Ptr& decl) {
    if(decl->expr != nullptr) {
        decl->expr = visit(decl->expr);
    }
    return decl;
}

Node::Ptr TypeGen::visit(const Self::Ptr& decl) {
    return decl;
}

Node::Ptr TypeGen::visit(const SelfExpr::Ptr& decl) {
    assert(decl->identifier != nullptr);
    
    decl->identifier = std::static_pointer_cast<IdentifierExpr>(visit(decl->identifier));
    return decl;
}

Node::Ptr TypeGen::visit(const ArrayLiteralExpr::Ptr& decl) {
    std::vector<Node::Ptr> result;
    for(const auto& item: decl->items) {
        result.push_back(visit(item));
    }
    decl->items = result;
    return decl;
}

Node::Ptr TypeGen::visit(const DictLiteralExpr::Ptr& decl) {
    std::vector<std::tuple<Node::Ptr, Node::Ptr>> result;
    for(auto item: decl->items) {
        auto keyItem = visit(std::get<0>(item));
        auto valueItem = visit(std::get<1>(item));
        result.emplace_back(keyItem , valueItem);
    }
    decl->items = result;
    return decl;
}

Node::Ptr TypeGen::visit(const MemberAccessExpr::Ptr& decl) {
    decl->callee = visit(decl->callee);
    context->visit(CompileStage::visitMemberAccess, decl->callee, [this, decl] {
        decl->member = visit(decl->member);
    });

    return decl;
}

Node::Ptr TypeGen::visit(const MemberAssignExpr::Ptr& decl) {
    return decl;
}

Node::Ptr TypeGen::visit(const SubscriptExpr::Ptr& decl) {
    decl->identifier = visit(decl->identifier);
    decl->indexExpr = visit(decl->indexExpr);
    
    return decl;
}

Node::Ptr TypeGen::visit(const ArrayType::Ptr& decl) {
    decl->valueType = visit(decl->valueType);
    return decl;
}

Node::Ptr TypeGen::visit(const ImportStmt::Ptr& decl) {
    if(context->curStage() != CompileStage::visitModule) {
        diagnostics->reportError(ErrorLevel::failure, Diagnostics::errorFileImportShouldAtTopOfSourceFile);
        return nullptr;
    }
    
    auto moduleName = decl->stringLiteral->rawValue;
    //TODO
    //

    return decl;
}

Node::Ptr TypeGen::processClassMemberFunc(const FuncDecl::Ptr &decl) {
    TypeGenSelfForMemberFunc visitor;
    visitor.context = context;
    visitor.compiler = compiler;
    return visitor.visit(decl);
}

void TypeGen::generateOptionalGlobally(const OptionalType::Ptr &optionalType) {
    auto typeSimpleName = optionalType->type->getSimpleName();
    auto symbol = context->lookup(typeSimpleName);
    auto optional = new Optional(optionalType->getSimpleName(), symbol->typeSlot);
    compiler->registerOptionalTypeGlobally(optional);
}

//---------------------------------------------
// TypeGenSelfForMemberFunc
//---------------------------------------------

Node::Ptr TypeGenSelfForMemberFunc::visit(const FuncDecl::Ptr& decl) {
    assert(decl->isStatic == false);
    context->visit(CompileStage::visitFuncDecl, decl, [decl, this] {
        std::vector<Node::Ptr> statements;
        for(const auto& statement: decl->statements) {
            statements.push_back(NodeVisitor::visit(statement));
        }
        decl->statements = statements;
    });

    return decl;
}

Node::Ptr TypeGenSelfForMemberFunc::visit(const Expr::Ptr& decl) {
    return decl;
}

Node::Ptr TypeGenSelfForMemberFunc::visit(const ArguCallExpr::Ptr& decl) {
    decl->expr = NodeVisitor::visit(decl->expr);
    return decl;
}

Node::Ptr TypeGenSelfForMemberFunc::visit(const ReturnStmt::Ptr& decl) {
    if(decl->expr != nullptr) {
        decl->expr = NodeVisitor::visit(decl->expr);
    }
    return decl;
}

Node::Ptr TypeGenSelfForMemberFunc::visit(const IdentifierExpr::Ptr& decl) {
    auto name = decl->getSimpleName();
    auto symbol = context->lookup(name);
    if(symbol != nullptr && symbol->flag == SymbolFlag::field) {
        auto self = std::make_shared<Self>();
        return std::make_shared<SelfExpr>(self, decl);
    }
    return decl;
}

Node::Ptr TypeGenSelfForMemberFunc::visit(const FuncCallExpr::Ptr& decl) {
    auto name = decl->getCalleeFuncSimpleName();
    auto symbol = context->lookup(name);

    std::vector<ArguCallExpr::Ptr> arguments;

    for(const auto& arguCallExpr : decl->arguments) {
        arguments.push_back(std::static_pointer_cast<ArguCallExpr>(visit(arguCallExpr)));
    }
    decl->arguments = arguments;

    auto funcType = (Function*)compiler->getType(symbol->typeSlot);
    assert(funcType->kind == ValueType::Func);
    if(!funcType->isStatic) {
        auto self = std::make_shared<Self>();
        return std::make_shared<MemberFuncCallExpr>(self, decl);
    }
    return decl;
}

Node::Ptr TypeGenSelfForMemberFunc::visit(const AssignExpr::Ptr& decl) {
    decl->left = NodeVisitor::visit(decl->left);
    decl->expr = NodeVisitor::visit(decl->expr);
    return decl;
}

Node::Ptr TypeGenSelfForMemberFunc::visit(const SelfExpr::Ptr& decl) {
    return decl;
}