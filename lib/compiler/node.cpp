#include "joyeer/compiler/node.h"

#include <utility>

Node::Node(SyntaxKind k): kind(k) {
}

std::string Node::getSimpleName() {
    return "";
}

//--------------------------------------------------
// Self implementation
//--------------------------------------------------

Self::Self():
Node(SyntaxKind::self) {
    token = std::make_shared<Token>(TokenKind::keyword, Keywords::SELF, -1, -1);
}

Self::Self(const Token::Ptr &token):
Node(SyntaxKind::self),
token(token) {
}

std::string Self::getSimpleName() {
    return token->rawValue;
}

//--------------------------------------------------
// IdentifierExpr implementation
//--------------------------------------------------

IdentifierExpr::IdentifierExpr(const Token::Ptr& token) :
        Node(SyntaxKind::identifierExpr),
        token(token) {
    assert(token->kind == TokenKind::identifier);
}

std::string IdentifierExpr::getSimpleName() {
    return token->rawValue;
}

OperatorExpr::OperatorExpr(Token::Ptr token):
Node(SyntaxKind::operatorExpr),
token(std::move(token)) {

}

ParameterClause::ParameterClause(std::vector<Pattern::Ptr> parameters):
Node(SyntaxKind::parameterClause),
parameters(std::move(parameters)) {
}

Pattern::Pattern(IdentifierExpr::Ptr identifier, Node::Ptr type):
        Node(SyntaxKind::pattern),
        identifier(std::move(identifier)),
        typeExpr(std::move(type)) {
}

const std::string& Pattern::getIdentifierName() const {
    return identifier->token->rawValue;
}

TypeIdentifier::TypeIdentifier(Node::Ptr identifier):
Node(SyntaxKind::type),
identifier(std::move(identifier)) {
}

ArrayType::ArrayType(Node::Ptr type):
Node(SyntaxKind::arrayType),
valueType(std::move(type)) {
}


DictType::DictType(Node::Ptr keyType, Node::Ptr valueType):
Node(SyntaxKind::dictType),
keyType(std::move(keyType)),
valueType(std::move(valueType)) {
}

//--------------------------------------------------
// SelfExpr implementation
//--------------------------------------------------

SelfExpr::SelfExpr(const Self::Ptr& self, const IdentifierExpr::Ptr& identifier) :
        Node(SyntaxKind::selfExpr),
        self(self),
        identifier(identifier) {
}

//--------------------------------------------------
// ForceUnwrappingExpr implementation
//--------------------------------------------------

ForceUnwrappingExpr::ForceUnwrappingExpr(const Node::Ptr& wrappedExpr):
Node(SyntaxKind::forceUnwrapExpr),
wrappedExpr(wrappedExpr){
}

std::string ForceUnwrappingExpr::getSimpleName() {
    return wrappedExpr->getSimpleName();
}

//--------------------------------------------------
// OptionalChainingExpr implementation
//--------------------------------------------------

OptionalChainingExpr::OptionalChainingExpr(const Node::Ptr &wrappedExpr):
Node(SyntaxKind::optionalChainingExpr),
wrappedExpr(wrappedExpr) {
}

//--------------------------------------------------
// OptionalType implementation
//--------------------------------------------------

OptionalType::OptionalType(Node::Ptr type, bool required):
Node(SyntaxKind::optionalType),
type(std::move(type)),
required(required) {
}

std::string OptionalType::getSimpleName() {
    return "Optional<" + type->getSimpleName() + ">";
}

//-------------------------------------
// ClassDecl implementation
//-------------------------------------

ClassDecl::ClassDecl(Token::Ptr name, const std::vector<Node::Ptr>& statements) :
        StmtsBlock(statements),
        name(std::move(name)) {
    symtable = std::make_shared<SymbolTable>();
    kind = SyntaxKind::classDecl;
}

std::string ClassDecl::getSimpleName() {
    return name->rawValue;
}


//-------------------------------------
//FuncDecl implementation
//-------------------------------------

FuncDecl::FuncDecl(Node::Ptr identifier, Node::Ptr parameterClause, Node::Ptr returnType, StmtsBlock::Ptr stmts) :
StmtsBlock(stmts->statements),
identifier(std::move(identifier)),
parameterClause(std::move(parameterClause)),
returnType(std::move(returnType)) {
    kind = SyntaxKind::funcDecl;
}


void FuncDecl::bindClass(const Class* klass) {
    assert(symtable->find(Keywords::SELF) == nullptr);
    // self symbol

    assert(returnType == nullptr);

    auto clause = std::static_pointer_cast<ParameterClause>(this->parameterClause);

    auto selfIdentifier = std::make_shared<IdentifierExpr>(std::make_shared<Token>(TokenKind::identifier, Keywords::SELF, -1, -1));
    auto typeIdentifier = std::make_shared<IdentifierExpr>(std::make_shared<Token>(TokenKind::identifier, klass->name, -1, -1));
    auto pattern = std::make_shared<Pattern>(selfIdentifier, typeIdentifier);
    clause->parameters.insert(clause->parameters.begin(), pattern);
    
    // return type is ClassDecl
    if(isConstructor) {
        auto token = std::make_shared<Token>(TokenKind::identifier, klass->name, -1, -1);
        returnType = std::make_shared<IdentifierExpr>(token);
    }
}

FuncDecl::Ptr FuncDecl::createDefaultConstructor() {
    /**
     * default constructor looks like follow code:
     * init() {
     *   return self
     * }
     */
    auto parameterClause = std::make_shared<ParameterClause>( std::vector<Pattern::Ptr>());

    auto returnStmt = std::make_shared<ReturnStmt>(std::make_shared<Self>());
    auto stmts = std::make_shared<StmtsBlock>( std::vector<Node::Ptr> { returnStmt });

    return createConstructor(parameterClause, nullptr, stmts);
}

// create a Constructor FuncDecl
FuncDecl::Ptr FuncDecl::createConstructor( const Node::Ptr& parameterClause, const Node::Ptr& returnType, const StmtsBlock::Ptr& stmts) {
    auto token = std::make_shared<Token>(TokenKind::identifier, "init", -1, -1);
    auto nameIdentifier = std::make_shared<IdentifierExpr>(token);
    auto decl = std::make_shared<FuncDecl>(nameIdentifier, parameterClause, returnType, stmts);
    decl->isConstructor = true;
    return decl;
}

std::string FuncDecl::getSimpleName() {
    std::stringstream ss;

    // basis name
    if (identifier != nullptr) {
        ss << identifier->getSimpleName();
    }

    // parameters
    ss << DescriptorConstants::ParenthesisOpen;
    if (parameterClause) {
        for (const auto& p: std::static_pointer_cast<ParameterClause>(parameterClause)->parameters) {
            ss << p->getSimpleName() << DescriptorConstants::Colon;
        }
    }
    ss << DescriptorConstants::ParenthesisClose;

    return ss.str();
}

std::string FuncDecl::getSimpleConstructorName(const std::string& name) {
    std::stringstream ss;

    // basis name
    ss << name;

    // parameters
    ss << DescriptorConstants::ParenthesisOpen;
    if (parameterClause) {
        auto i = 0;
        for (const auto& p: std::static_pointer_cast<ParameterClause>(parameterClause)->parameters) {
            if(i++ == 0) {
                continue;
            }
            ss << p->getSimpleName() << DescriptorConstants::Colon;
            i++;
        }
    }
    ss << DescriptorConstants::ParenthesisClose;

    return ss.str();
}