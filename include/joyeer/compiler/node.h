#ifndef __joyeer_compiler_AST_node_h__
#define __joyeer_compiler_AST_node_h__

#include <utility>
#include <sstream>
#include <fstream>
#include <cassert>
#include <filesystem>

#include "joyeer/runtime/descriptor.h"
#include "joyeer/runtime/types.h"
#include "joyeer/compiler/token.h"
#include "joyeer/compiler/symtable.h"

enum class SyntaxKind {
    module = 1L,

    type,
    arrayType,
    dictType,
    optionalType,
    pattern,

    varDecl,
    funcDecl,
    classDecl,

    parameterClause,

    stmtsBlock,
    forInStmt,
    ifStmt,
    whileStmt,
    returnStmt,
    importStmt,

    self,
    expr,
    assignExpr,
    binaryExpr,
    selfExpr,
    postfixExpr,
    prefixExpr,
    identifierExpr,
    parenthesizedExpr,
    arguCallExpr,
    funcCallExpr,
    memberFuncCallExpr,
    memberAccessExpr,
    memberAssignExpr,
    subscriptExpr,
    forceUnwrapExpr,
    optionalChainingExpr,

    literalExpr,
    arrayLiteralExpr,
    dictLiteralExpr,
    operatorExpr

};

struct Node : std::enable_shared_from_this<Node> {
    using Ptr = std::shared_ptr<Node>;

    SyntaxKind kind;
    SymbolTable::Ptr symtable = nullptr;

    // represent the Node's Type, only available in Expr Node
    Slot typeSlot = -1;

    // return the name of Node, it will be used as symbol in some cases
    virtual std::string getSimpleName();

    // detect if its a declaration node
    bool isDeclNode() const {
        switch (kind) {
            case SyntaxKind::varDecl:
            case SyntaxKind::funcDecl:
            case SyntaxKind::classDecl:
            case SyntaxKind::module:
                return true;
            default:
                return false;
        }
    }

protected:
    explicit Node(SyntaxKind kind);
};

// represent a AST for *self*
struct Self : public Node {
    using Ptr = std::shared_ptr<Self>;
    Token::Ptr token;

    explicit Self();
    Self(const Token::Ptr& token);

    std::string getSimpleName() override;
};

// represent a
struct IdentifierExpr : public Node {
    using Ptr = std::shared_ptr<IdentifierExpr>;

    Token::Ptr token;

    explicit IdentifierExpr(const Token::Ptr& token);

    // Return identifier's name
    std::string getSimpleName() override;
};

struct OperatorExpr : Node {
    using Ptr = std::shared_ptr<OperatorExpr>;

    Token::Ptr token;

    explicit OperatorExpr(Token::Ptr token);
};

struct TypeIdentifier : Node {
    using Ptr = std::shared_ptr<TypeIdentifier>;

    Node::Ptr identifier; // identifierExpr

    explicit TypeIdentifier(Node::Ptr identifier);

    std::string getSimpleName() override  {
        return identifier->getSimpleName();
    }

};

struct ArrayType : Node {
    using Ptr = std::shared_ptr<ArrayType>;

    Node::Ptr valueType;

    explicit ArrayType(Node::Ptr type);

    std::string getSimpleName() override {
        return "Array";
    }

};

struct DictType : Node {
    using Ptr = std::shared_ptr<DictType>;

    Node::Ptr keyType;
    Node::Ptr valueType;

    DictType(Node::Ptr keyType, Node::Ptr valueType);

};

// represent a Type likes AType?
struct OptionalType: Node {
    using Ptr = std::shared_ptr<OptionalType>;

    Node::Ptr type;

    bool required;

    OptionalType(Node::Ptr type, bool required);

    std::string getSimpleName() override;
};

struct Pattern : public Node {
    using Ptr = std::shared_ptr<Pattern>;

    IdentifierExpr::Ptr identifier; // the name of pattern
    Node::Ptr typeExpr; // the kind of pattern, optional nullptr

    Pattern(IdentifierExpr::Ptr identifier, Node::Ptr typeExpr);

    const std::string &getIdentifierName() const;

    std::string getSimpleName() override {
        return getIdentifierName();
    }

};

struct ParameterClause : Node {
    using Ptr = std::shared_ptr<ParameterClause>;

    std::vector<Pattern::Ptr> parameters;

    explicit ParameterClause(std::vector<Pattern::Ptr> parameters);

};

/********************************************************
 *  Expression Section
 *********************************************************/

struct Expr : Node {
    using Ptr = std::shared_ptr<Expr>;

    Node::Ptr prefix;
    std::vector<Node::Ptr> binaries;

    // After binder, the 'binaries' and prefix will be merged into nodes
    std::vector<Node::Ptr> nodes;

    Expr(Node::Ptr prefix, std::vector<Node::Ptr> binaries) :
            Node(SyntaxKind::expr),
            prefix(std::move(prefix)),
            binaries(std::move(binaries)) {
    }

    explicit Expr(std::vector<Node::Ptr> nodes) :
            Node(SyntaxKind::expr),
            prefix(nullptr),
            binaries(),
            nodes(std::move(nodes)) {
    }
};

struct PostfixExpr : Node {
    using Ptr = std::shared_ptr<PostfixExpr>;

    Node::Ptr expr;
    OperatorExpr::Ptr op;

    PostfixExpr(Node::Ptr expr, OperatorExpr::Ptr op) :
            Node(SyntaxKind::postfixExpr),
            expr(std::move(expr)),
            op(std::move(op)) {
    }

};

struct PrefixExpr : Node {
    using Ptr = std::shared_ptr<PrefixExpr>;

    OperatorExpr::Ptr op;
    Node::Ptr expr; // postfix-expression

    PrefixExpr(OperatorExpr::Ptr op, Node::Ptr expr) :
            Node(SyntaxKind::prefixExpr),
            op(std::move(op)),
            expr(std::move(expr)) {
    }

};

struct BinaryExpr : Node {
    using Ptr = std::shared_ptr<BinaryExpr>;

    OperatorExpr::Ptr op;
    Node::Ptr expr;

    BinaryExpr(OperatorExpr::Ptr op, Node::Ptr expr) :
            Node(SyntaxKind::binaryExpr),
            op(std::move(op)),
            expr(std::move(expr)) {
    }

};

struct AssignExpr : Node {
    using Ptr = std::shared_ptr<AssignExpr>;

    Node::Ptr expr;

    // After bound, the identifier will be bound
    Node::Ptr left;

    explicit AssignExpr(Node::Ptr expr) :
            Node(SyntaxKind::assignExpr),
            expr(std::move(expr)) {
    }

};

struct ArguCallExpr : Node {
    using Ptr = std::shared_ptr<ArguCallExpr>;
    IdentifierExpr::Ptr label;
    Node::Ptr expr;

    ArguCallExpr(IdentifierExpr::Ptr label, Node::Ptr expr) :
            Node(SyntaxKind::arguCallExpr),
            label(std::move(label)),
            expr(std::move(expr)) {
    }

};

// represent an expr that force unwrapping a optional value,
// e.g.
//  var anOptionalInt: Int? = 1
//  var anInt = anOptionalInt!

struct ForceUnwrappingExpr : public Node {
    using Ptr = std::shared_ptr<ForceUnwrappingExpr>;

    Node::Ptr wrappedExpr;

    ForceUnwrappingExpr(Node::Ptr  wrappedExpr);

    std::string getSimpleName() override;
};

// represent an expr that force unwrapping a optional value,
// e.g.
//  var anOptionalAClass: A? = ...
//  anOptionalAClass?.memberA = 10

struct OptionalChainingExpr: public Node {
    using Ptr = std::shared_ptr<OptionalChainingExpr>;

    Node::Ptr wrappedExpr;

    OptionalChainingExpr(Node::Ptr  wrappedExpr);
};

// represent a func call expression
// e.g. print(message: "hello world")
struct FuncCallExpr : Node {
    using Ptr = std::shared_ptr<FuncCallExpr>;

    Node::Ptr identifier;
    std::vector<ArguCallExpr::Ptr> arguments;
    int funcTypeSlot = -1;

    FuncCallExpr(Node::Ptr expr, std::vector<ArguCallExpr::Ptr> arguments) :
            Node(SyntaxKind::funcCallExpr),
            identifier(std::move(expr)),
            arguments(std::move(arguments)) {
    }

    /// get callee FuncDecl 's simple-name
    std::string getCalleeFuncSimpleName() const {
        auto ss = std::stringstream();
        ss << identifier->getSimpleName();
        ss << "(";
        for(auto& argument: arguments) {
            ss << argument->label->getSimpleName() << ":";
        }
        ss << ")";
        return ss.str();
    }

};

struct MemberFuncCallExpr : Node {
    using Ptr = std::shared_ptr<MemberFuncCallExpr>;

    Node::Ptr callee;
    Node::Ptr member; // FuncCallExpr

    int funcTypeSlot = -1;

    MemberFuncCallExpr(Node::Ptr callee, FuncCallExpr::Ptr funcCallExpr) :
            Node(SyntaxKind::memberFuncCallExpr),
            callee(std::move(callee)),
            member(std::move(funcCallExpr)) {
    }

};

struct MemberAccessExpr : Node {
    using Ptr = std::shared_ptr<MemberAccessExpr>;

    Node::Ptr callee;
    Node::Ptr member;

    MemberAccessExpr(Node::Ptr callee, Node::Ptr member) :
            Node(SyntaxKind::memberAccessExpr),
            callee(std::move(callee)),
            member(std::move(member)) {
    }

};

// assign expr to member, it's automatically generated by compiler
struct MemberAssignExpr : Node {
    using Ptr = std::shared_ptr<MemberAssignExpr>;

    Node::Ptr callee; // Member's owner class
    IdentifierExpr::Ptr member; // member self
    Node::Ptr expr; //  initializer expression

    MemberAssignExpr(Node::Ptr callee, IdentifierExpr::Ptr member, Node::Ptr expr) :
            Node(SyntaxKind::memberAssignExpr),
            callee(std::move(callee)),
            member(std::move(member)),
            expr(std::move(expr)) {}

};

struct LiteralExpr : Node {
    using Ptr = std::shared_ptr<LiteralExpr>;

    Token::Ptr literal;

    explicit LiteralExpr(Token::Ptr literal) :
            Node(SyntaxKind::literalExpr),
            literal(std::move(literal)) {
    }

};

struct ArrayLiteralExpr : Node {
    using Ptr = std::shared_ptr<ArrayLiteralExpr>;

    std::vector<Node::Ptr> items;

    explicit ArrayLiteralExpr(std::vector<Node::Ptr> items) :
            Node(SyntaxKind::arrayLiteralExpr),
            items(std::move(items)) {
    }

};

struct DictLiteralExpr : Node {
    using Ptr = std::shared_ptr<DictLiteralExpr>;

    std::vector<std::tuple<Node::Ptr, Node::Ptr>> items;

    explicit DictLiteralExpr(std::vector<std::tuple<Node::Ptr, Node::Ptr>> items) :
            Node(SyntaxKind::dictLiteralExpr),
            items(std::move(items)) {
    }

};

struct ParenthesizedExpr : Node {
    using Ptr = std::shared_ptr<ParenthesizedExpr>;

    Node::Ptr expr;

    explicit ParenthesizedExpr(Node::Ptr expr) :
            Node(SyntaxKind::parenthesizedExpr),
            expr(std::move(expr)) {
    }

};

// represent self.someIdentifier expr
struct SelfExpr : Node {
    using Ptr = std::shared_ptr<SelfExpr>;

    Self::Ptr self;
    IdentifierExpr::Ptr identifier;

    SelfExpr(Self::Ptr  self, IdentifierExpr::Ptr  identifier);
};

struct SubscriptExpr : Node {
    using Ptr = std::shared_ptr<SubscriptExpr>;

    Node::Ptr identifier;
    Node::Ptr indexExpr;

    SubscriptExpr(Node::Ptr identifier, Node::Ptr indexExpr) :
            Node(SyntaxKind::subscriptExpr),
            identifier(std::move(identifier)),
            indexExpr(std::move(indexExpr)) {
    }

};

/********************************************************
 *  Statement Section
 *********************************************************/

struct ImportStmt : Node {
    using Ptr = std::shared_ptr<ImportStmt>;

    Token::Ptr stringLiteral;

    explicit ImportStmt(Token::Ptr stringLiteral) :
            Node(SyntaxKind::importStmt),
            stringLiteral(std::move(stringLiteral)) {
    }

    std::string getImportedFilename() const {
        return stringLiteral->rawValue;
    }

};

// StmtsBlock represent an { ... } code block
struct StmtsBlock : Node {
    using Ptr = std::shared_ptr<StmtsBlock>;

    std::vector<Node::Ptr> statements;

    explicit StmtsBlock(std::vector<Node::Ptr> statements) :
            Node(SyntaxKind::stmtsBlock),
            statements(std::move(statements)) {
        symtable = std::make_shared<SymbolTable>();
    }

};

// For In statement
struct ForInStmt : Node {
    using Ptr = std::shared_ptr<ForInStmt>;

    Node::Ptr pattern;
    Node::Ptr inExpr;
    Node::Ptr codeBlock;

    ForInStmt(Node::Ptr pattern, Node::Ptr inExpr, Node::Ptr codeBlock) :
            Node(SyntaxKind::forInStmt),
            pattern(std::move(pattern)),
            inExpr(std::move(inExpr)),
            codeBlock(std::move(codeBlock)) {
    }

};

struct WhileStmt : Node {
    using Ptr = std::shared_ptr<WhileStmt>;

    Node::Ptr expr;
    Node::Ptr codeBlock;

    WhileStmt(Node::Ptr expr, Node::Ptr codeBlock) :
            Node(SyntaxKind::whileStmt),
            expr(std::move(expr)),
            codeBlock(std::move(codeBlock)) {
    }

};

struct IfStmt : Node {
    using Ptr = std::shared_ptr<IfStmt>;

    Node::Ptr condition;
    Node::Ptr ifCodeBlock;
    Node::Ptr elseCodeBlock;

    IfStmt(Node::Ptr condition, Node::Ptr ifCodeBlock, Node::Ptr elseCodeBlock) :
            Node(SyntaxKind::ifStmt),
            condition(std::move(condition)),
            ifCodeBlock(std::move(ifCodeBlock)),
            elseCodeBlock(std::move(elseCodeBlock)) {
    }

};

struct ReturnStmt : Node {
    using Ptr = std::shared_ptr<ReturnStmt>;

    Node::Ptr expr;

    explicit ReturnStmt(Node::Ptr expr) :
            Node(SyntaxKind::returnStmt),
            expr(std::move(expr)) {
    }


};

/********************************************************
 *  Decl Section
 *********************************************************/

struct ClassDecl : public StmtsBlock {
    using Ptr = std::shared_ptr<ClassDecl>;

    Token::Ptr name = nullptr;

    ClassDecl(Token::Ptr name, const std::vector<Node::Ptr>& statements);

    std::string getSimpleName() override;
};


// Represent a Function in Ast tree.
struct FuncDecl : public StmtsBlock {
public:
    using Ptr = std::shared_ptr<FuncDecl>;

    Node::Ptr identifier = nullptr;               //IdentifierExpr
    Node::Ptr parameterClause;
    Node::Ptr returnType = nullptr;
    bool isConstructor = false;
    bool isStatic = false;
    Symbol::Ptr  symbol;

    FuncDecl(Node::Ptr identifier, Node::Ptr parameterClause, Node::Ptr returnType, StmtsBlock::Ptr stmts);

    // bind self symbol
    void bindClass(const Class* klass);

    // make a default constructor
    static Ptr createDefaultConstructor();

    // create a Constructor FuncDecl
    static Ptr createConstructor(const Node::Ptr& parameterClause, const Node::Ptr& returnType, const StmtsBlock::Ptr& stmts);

    // return func name
    std::string getSimpleName() override ;

    // get constructor simple name
    std::string getSimpleConstructorName(const std::string& className) ;

};

// Represent an Module in Ast tree, each xxx.joyeer file is a file module
class ModuleDecl : public StmtsBlock {
public:
    using Ptr = std::shared_ptr<ModuleDecl>;

public:
    explicit ModuleDecl(std::vector<Node::Ptr>& statements) :
            StmtsBlock(statements) {
        kind = SyntaxKind::module;
        assert(symtable != nullptr);
    }

    std::string filename;

    std::string getSimpleName() override {
        std::filesystem::path p = filename;
        return p.filename().replace_extension().string();
    }

};


// `let` or `var` declaration
struct VarDecl : public Node {
    using Ptr = std::shared_ptr<VarDecl>;

    Pattern::Ptr pattern;
    Node::Ptr initializer;

    VarDecl(Pattern::Ptr pattern, Node::Ptr initializer) :
            Node(SyntaxKind::varDecl),
            pattern(std::move(pattern)),
            initializer(std::move(initializer)) {
    }

    std::string getSimpleName() override {
        return pattern->getSimpleName();
    }

};

#endif
