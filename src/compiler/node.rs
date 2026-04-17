use crate::compiler::token::Token;

pub type NodeId = usize;

/// All AST node types in joyeer
#[derive(Debug)]
pub enum Node {
    // Module (top-level)
    Module(ModuleDecl),

    // Declarations
    VarDecl(VarDecl),
    FuncDecl(FuncDecl),
    ClassDecl(ClassDecl),

    // Types
    TypeIdentifier(TypeIdentifier),
    ArrayType(ArrayTypeNode),
    DictType(DictTypeNode),
    OptionalType(OptionalTypeNode),

    // Statements
    StmtsBlock(StmtsBlock),
    IfStmt(IfStmt),
    WhileStmt(WhileStmt),
    ForInStmt(ForInStmt),
    ReturnStmt(ReturnStmt),
    ImportStmt(ImportStmt),

    // Expressions
    Expr(Expr),
    BinaryExpr(BinaryExpr),
    AssignExpr(AssignExpr),
    PrefixExpr(PrefixExpr),
    PostfixExpr(PostfixExpr),
    IdentifierExpr(IdentifierExpr),
    LiteralExpr(LiteralExpr),
    ParenthesizedExpr(ParenthesizedExpr),
    FuncCallExpr(FuncCallExpr),
    MemberFuncCallExpr(MemberFuncCallExpr),
    MemberAccessExpr(MemberAccessExpr),
    MemberAssignExpr(MemberAssignExpr),
    ArguCallExpr(ArguCallExpr),
    SubscriptExpr(SubscriptExpr),
    ArrayLiteralExpr(ArrayLiteralExpr),
    DictLiteralExpr(DictLiteralExpr),
    SelfExpr(SelfExpr),
    SelfNode(SelfNode),
    ForceUnwrapExpr(ForceUnwrapExpr),
    OptionalChainingExpr(OptionalChainingExpr),
    OperatorExpr(OperatorExpr),
    Pattern(Pattern),
    ParameterClause(ParameterClause),
}

impl Node {
    /// The type slot assigned during type checking (-1 = unresolved)
    pub fn type_slot(&self) -> i64 {
        match self {
            Node::Expr(e) => e.type_slot,
            Node::BinaryExpr(e) => e.type_slot,
            Node::AssignExpr(e) => e.type_slot,
            Node::PrefixExpr(e) => e.type_slot,
            Node::PostfixExpr(e) => e.type_slot,
            Node::IdentifierExpr(e) => e.type_slot,
            Node::LiteralExpr(e) => e.type_slot,
            Node::ParenthesizedExpr(e) => e.type_slot,
            Node::FuncCallExpr(e) => e.type_slot,
            Node::MemberFuncCallExpr(e) => e.type_slot,
            Node::MemberAccessExpr(e) => e.type_slot,
            Node::MemberAssignExpr(e) => e.type_slot,
            Node::ArguCallExpr(e) => e.type_slot,
            Node::SubscriptExpr(e) => e.type_slot,
            Node::ArrayLiteralExpr(e) => e.type_slot,
            Node::DictLiteralExpr(e) => e.type_slot,
            Node::SelfExpr(e) => e.type_slot,
            Node::SelfNode(e) => e.type_slot,
            Node::ForceUnwrapExpr(e) => e.type_slot,
            Node::OptionalChainingExpr(e) => e.type_slot,
            Node::OperatorExpr(e) => e.type_slot,
            Node::VarDecl(d) => d.type_slot,
            Node::FuncDecl(d) => d.type_slot,
            Node::ClassDecl(d) => d.type_slot,
            Node::Module(d) => d.type_slot,
            _ => -1,
        }
    }

    pub fn set_type_slot(&mut self, slot: i64) {
        match self {
            Node::Expr(e) => e.type_slot = slot,
            Node::BinaryExpr(e) => e.type_slot = slot,
            Node::AssignExpr(e) => e.type_slot = slot,
            Node::PrefixExpr(e) => e.type_slot = slot,
            Node::PostfixExpr(e) => e.type_slot = slot,
            Node::IdentifierExpr(e) => e.type_slot = slot,
            Node::LiteralExpr(e) => e.type_slot = slot,
            Node::ParenthesizedExpr(e) => e.type_slot = slot,
            Node::FuncCallExpr(e) => e.type_slot = slot,
            Node::MemberFuncCallExpr(e) => e.type_slot = slot,
            Node::MemberAccessExpr(e) => e.type_slot = slot,
            Node::MemberAssignExpr(e) => e.type_slot = slot,
            Node::ArguCallExpr(e) => e.type_slot = slot,
            Node::SubscriptExpr(e) => e.type_slot = slot,
            Node::ArrayLiteralExpr(e) => e.type_slot = slot,
            Node::DictLiteralExpr(e) => e.type_slot = slot,
            Node::SelfExpr(e) => e.type_slot = slot,
            Node::SelfNode(e) => e.type_slot = slot,
            Node::ForceUnwrapExpr(e) => e.type_slot = slot,
            Node::OptionalChainingExpr(e) => e.type_slot = slot,
            Node::OperatorExpr(e) => e.type_slot = slot,
            Node::VarDecl(d) => d.type_slot = slot,
            Node::FuncDecl(d) => d.type_slot = slot,
            Node::ClassDecl(d) => d.type_slot = slot,
            Node::Module(d) => d.type_slot = slot,
            _ => {}
        }
    }

    pub fn simple_name(&self) -> String {
        match self {
            Node::IdentifierExpr(e) => e.name.clone(),
            Node::SelfNode(_) => "self".into(),
            Node::TypeIdentifier(t) => t.name.clone(),
            Node::Pattern(p) => p.name.clone(),
            Node::FuncDecl(d) => d.name.clone(),
            Node::ClassDecl(d) => d.name.clone(),
            Node::Module(d) => d.filename.clone(),
            Node::VarDecl(d) => d.pattern_name().into(),
            _ => String::new(),
        }
    }
}

// ── Module ──────────────────────────────────────────

#[derive(Debug)]
pub struct ModuleDecl {
    pub filename: String,
    pub statements: Vec<Box<Node>>,
    pub type_slot: i64,
}

// ── Declarations ────────────────────────────────────

#[derive(Debug)]
pub struct VarDecl {
    pub pattern: Pattern,
    pub initializer: Option<Box<Node>>,
    pub is_let: bool,
    pub type_slot: i64,
}

impl VarDecl {
    pub fn pattern_name(&self) -> &str {
        &self.pattern.name
    }
}

#[derive(Debug)]
pub struct FuncDecl {
    pub name: String,
    pub params: Vec<Pattern>,
    pub return_type: Option<Box<Node>>,
    pub body: Box<Node>,
    pub is_constructor: bool,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct ClassDecl {
    pub name: String,
    pub members: Vec<Box<Node>>,
    pub type_slot: i64,
}

// ── Type Nodes ──────────────────────────────────────

#[derive(Debug)]
pub struct TypeIdentifier {
    pub name: String,
}

#[derive(Debug)]
pub struct ArrayTypeNode {
    pub element_type: Box<Node>,
}

#[derive(Debug)]
pub struct DictTypeNode {
    pub key_type: Box<Node>,
    pub value_type: Box<Node>,
}

#[derive(Debug)]
pub struct OptionalTypeNode {
    pub inner_type: Box<Node>,
    pub required: bool, // ! vs ?
}

// ── Pattern ─────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct Pattern {
    pub name: String,
    pub type_annotation: Option<String>,
}

#[derive(Debug)]
pub struct ParameterClause {
    pub params: Vec<Pattern>,
}

// ── Statements ──────────────────────────────────────

#[derive(Debug)]
pub struct StmtsBlock {
    pub statements: Vec<Box<Node>>,
}

#[derive(Debug)]
pub struct IfStmt {
    pub condition: Box<Node>,
    pub then_block: Box<Node>,
    pub else_block: Option<Box<Node>>,
}

#[derive(Debug)]
pub struct WhileStmt {
    pub condition: Box<Node>,
    pub body: Box<Node>,
}

#[derive(Debug)]
pub struct ForInStmt {
    pub pattern: Pattern,
    pub iterable: Box<Node>,
    pub body: Box<Node>,
}

#[derive(Debug)]
pub struct ReturnStmt {
    pub expr: Option<Box<Node>>,
}

#[derive(Debug)]
pub struct ImportStmt {
    pub path: Token,
}

// ── Expressions ─────────────────────────────────────

#[derive(Debug)]
pub struct Expr {
    pub prefix: Box<Node>,
    pub binaries: Vec<Box<Node>>,
    /// After type binding, prefix+binaries are merged here
    pub nodes: Vec<Box<Node>>,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct BinaryExpr {
    pub op: Token,
    pub expr: Box<Node>,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct AssignExpr {
    pub expr: Box<Node>,
    pub left: Option<Box<Node>>,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct PrefixExpr {
    pub op: Token,
    pub expr: Box<Node>,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct PostfixExpr {
    pub expr: Box<Node>,
    pub op: Token,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct IdentifierExpr {
    pub name: String,
    pub token: Token,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct LiteralExpr {
    pub token: Token,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct ParenthesizedExpr {
    pub expr: Box<Node>,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct FuncCallExpr {
    pub callee: Box<Node>,
    pub arguments: Vec<ArguCallExprData>,
    pub func_type_slot: i64,
    pub type_slot: i64,
}

impl FuncCallExpr {
    pub fn callee_name(&self) -> String {
        let mut s = self.callee.simple_name();
        s.push('(');
        for arg in &self.arguments {
            s.push_str(&arg.label);
            s.push(':');
        }
        s.push(')');
        s
    }
}

#[derive(Debug)]
pub struct ArguCallExprData {
    pub label: String,
    pub expr: Box<Node>,
}

#[derive(Debug)]
pub struct ArguCallExpr {
    pub label: String,
    pub expr: Box<Node>,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct MemberFuncCallExpr {
    pub callee: Box<Node>,
    pub member: Box<Node>, // FuncCallExpr
    pub func_type_slot: i64,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct MemberAccessExpr {
    pub callee: Box<Node>,
    pub member: Box<Node>,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct MemberAssignExpr {
    pub callee: Box<Node>,
    pub member: String,
    pub expr: Box<Node>,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct SubscriptExpr {
    pub callee: Box<Node>,
    pub index: Box<Node>,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct ArrayLiteralExpr {
    pub elements: Vec<Box<Node>>,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct DictLiteralExpr {
    pub entries: Vec<(Box<Node>, Box<Node>)>,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct SelfNode {
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct SelfExpr {
    pub member: Box<Node>, // IdentifierExpr
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct ForceUnwrapExpr {
    pub expr: Box<Node>,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct OptionalChainingExpr {
    pub expr: Box<Node>,
    pub type_slot: i64,
}

#[derive(Debug)]
pub struct OperatorExpr {
    pub token: Token,
    pub type_slot: i64,
}
