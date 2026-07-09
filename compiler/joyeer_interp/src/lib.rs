//! Tree-walking interpreter -- the bootstrap backend.
//!
//! It evaluates a [`joyeer_ast::Module`] directly, maintaining a flat
//! environment of variable bindings and a captured output buffer. The only
//! built-in is `print`, which appends its arguments (space-separated) followed
//! by a newline.

use std::collections::HashMap;

use joyeer_ast::{Arg, BinOp, Expr, ExprKind, Module, Stmt, StmtKind, UnOp};

/// A runtime value.
#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Int(i64),
    Float(f64),
    Str(String),
    Bool(bool),
    Unit,
}

impl Value {
    fn type_name(&self) -> &'static str {
        match self {
            Value::Int(_) => "Int",
            Value::Float(_) => "Float",
            Value::Str(_) => "String",
            Value::Bool(_) => "Bool",
            Value::Unit => "()",
        }
    }

    fn display(&self) -> String {
        match self {
            Value::Int(n) => n.to_string(),
            Value::Float(f) => format!("{f}"),
            Value::Str(s) => s.clone(),
            Value::Bool(b) => b.to_string(),
            Value::Unit => "()".to_string(),
        }
    }
}

/// The interpreter state: bindings plus captured standard output.
#[derive(Default)]
pub struct Interp {
    env: HashMap<String, Value>,
    /// Everything written by `print`, ready for the driver to flush.
    pub output: String,
}

impl Interp {
    /// Creates an interpreter with an empty environment.
    pub fn new() -> Interp {
        Interp::default()
    }

    /// Executes every statement in `module`. Returns a runtime error message on
    /// the first failure.
    pub fn run(&mut self, module: &Module) -> Result<(), String> {
        for stmt in &module.items {
            self.exec_stmt(stmt)?;
        }
        Ok(())
    }

    fn exec_stmt(&mut self, stmt: &Stmt) -> Result<(), String> {
        match &stmt.kind {
            StmtKind::Let { name, value, .. } => {
                let value = self.eval(value)?;
                self.env.insert(name.clone(), value);
                Ok(())
            }
            StmtKind::Expr(expr) => {
                self.eval(expr)?;
                Ok(())
            }
        }
    }

    fn eval(&mut self, expr: &Expr) -> Result<Value, String> {
        match &expr.kind {
            ExprKind::Int(n) => Ok(Value::Int(*n)),
            ExprKind::Float(f) => Ok(Value::Float(*f)),
            ExprKind::Str(s) => Ok(Value::Str(s.clone())),
            ExprKind::Bool(b) => Ok(Value::Bool(*b)),
            ExprKind::Ident(name) => self
                .env
                .get(name)
                .cloned()
                .ok_or_else(|| format!("undefined variable `{name}`")),
            ExprKind::Unary { op, expr } => {
                let value = self.eval(expr)?;
                apply_unop(*op, value)
            }
            ExprKind::Binary { op, lhs, rhs } => {
                let lhs = self.eval(lhs)?;
                let rhs = self.eval(rhs)?;
                apply_binop(*op, lhs, rhs)
            }
            ExprKind::Call { callee, args } => self.eval_call(callee, args),
        }
    }

    fn eval_call(&mut self, callee: &Expr, args: &[Arg]) -> Result<Value, String> {
        let name = match &callee.kind {
            ExprKind::Ident(name) => name.as_str(),
            _ => return Err("call target is not a function".to_string()),
        };
        match name {
            "print" => {
                let mut rendered = Vec::with_capacity(args.len());
                for arg in args {
                    rendered.push(self.eval(&arg.value)?.display());
                }
                self.output.push_str(&rendered.join(" "));
                self.output.push('\n');
                Ok(Value::Unit)
            }
            other => Err(format!("unknown function `{other}`")),
        }
    }
}

fn apply_unop(op: UnOp, value: Value) -> Result<Value, String> {
    match (op, value) {
        (UnOp::Neg, Value::Int(n)) => Ok(Value::Int(-n)),
        (UnOp::Neg, Value::Float(f)) => Ok(Value::Float(-f)),
        (UnOp::Not, Value::Bool(b)) => Ok(Value::Bool(!b)),
        (op, value) => Err(format!("cannot apply `{op:?}` to {}", value.type_name())),
    }
}

fn apply_binop(op: BinOp, lhs: Value, rhs: Value) -> Result<Value, String> {
    use Value::{Float, Int, Str};
    match (op, lhs, rhs) {
        (BinOp::Add, Int(a), Int(b)) => a
            .checked_add(b)
            .map(Int)
            .ok_or_else(|| "integer overflow".to_string()),
        (BinOp::Sub, Int(a), Int(b)) => a
            .checked_sub(b)
            .map(Int)
            .ok_or_else(|| "integer overflow".to_string()),
        (BinOp::Mul, Int(a), Int(b)) => a
            .checked_mul(b)
            .map(Int)
            .ok_or_else(|| "integer overflow".to_string()),
        (BinOp::Div, Int(a), Int(b)) => {
            if b == 0 {
                Err("division by zero".to_string())
            } else {
                Ok(Int(a / b))
            }
        }
        (BinOp::Rem, Int(a), Int(b)) => {
            if b == 0 {
                Err("remainder by zero".to_string())
            } else {
                Ok(Int(a % b))
            }
        }
        (BinOp::Add, Float(a), Float(b)) => Ok(Float(a + b)),
        (BinOp::Sub, Float(a), Float(b)) => Ok(Float(a - b)),
        (BinOp::Mul, Float(a), Float(b)) => Ok(Float(a * b)),
        (BinOp::Div, Float(a), Float(b)) => Ok(Float(a / b)),
        (BinOp::Add, Str(a), Str(b)) => Ok(Str(a + &b)),
        (op, lhs, rhs) => Err(format!(
            "cannot apply `{:?}` to {} and {}",
            op,
            lhs.type_name(),
            rhs.type_name()
        )),
    }
}
