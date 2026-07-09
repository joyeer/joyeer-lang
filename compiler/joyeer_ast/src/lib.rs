//! Abstract syntax tree for Joyeer.
//!
//! The node set is deliberately small for the bootstrap: a module is a sequence
//! of statements, and expressions cover literals, identifiers, unary/binary
//! operators, and calls. It will grow (items, patterns, types) as the front end
//! matures. Every consumer (`joyeer_parse`, `joyeer_interp`) matches these
//! enums exhaustively so new variants surface as compile errors.

use joyeer_span::Span;

/// A parsed source file: a sequence of statements.
#[derive(Clone, Debug)]
pub struct Module {
    pub items: Vec<Stmt>,
}

/// A statement plus its source span.
#[derive(Clone, Debug)]
pub struct Stmt {
    pub kind: StmtKind,
    pub span: Span,
}

#[derive(Clone, Debug)]
pub enum StmtKind {
    /// `let name = value` (or `var name = value` when `mutable`).
    Let {
        mutable: bool,
        name: String,
        value: Expr,
    },
    /// A bare expression evaluated for its side effects.
    Expr(Expr),
}

/// An expression plus its source span.
#[derive(Clone, Debug)]
pub struct Expr {
    pub kind: ExprKind,
    pub span: Span,
}

#[derive(Clone, Debug)]
pub enum ExprKind {
    Int(i64),
    Float(f64),
    Str(String),
    Bool(bool),
    Ident(String),
    Unary {
        op: UnOp,
        expr: Box<Expr>,
    },
    Binary {
        op: BinOp,
        lhs: Box<Expr>,
        rhs: Box<Expr>,
    },
    Call {
        callee: Box<Expr>,
        args: Vec<Arg>,
    },
}

/// A call argument with an optional label, e.g. `message: x`.
#[derive(Clone, Debug)]
pub struct Arg {
    pub label: Option<String>,
    pub value: Expr,
}

/// Binary operators supported by the bootstrap.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Rem,
}

/// Unary operators supported by the bootstrap.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum UnOp {
    Neg,
    Not,
}
