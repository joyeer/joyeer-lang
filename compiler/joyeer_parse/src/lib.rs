//! Recursive-descent parser: turns Joyeer source into a [`joyeer_ast::Module`].
//!
//! The parser drives the pure [`joyeer_lexer`], discards trivia, attaches
//! spans, recognises keywords, and reports problems through a
//! [`DiagnosticSink`]. Operator precedence is handled by precedence climbing in
//! [`Parser::parse_expr`].

#[cfg(test)]
mod tests;

use joyeer_ast::{Arg, BinOp, Expr, ExprKind, Module, Stmt, StmtKind, UnOp};
use joyeer_errors::DiagnosticSink;
use joyeer_lexer::{tokenize, TokenKind};
use joyeer_span::Span;

/// A lexed token with its absolute byte range, trivia already removed.
#[derive(Clone, Copy)]
struct Lexed {
    kind: TokenKind,
    lo: u32,
    hi: u32,
}

/// Parses `src` into a [`Module`], reporting any errors into `diags`.
pub fn parse_module(src: &str, diags: &mut DiagnosticSink) -> Module {
    let tokens = lex(src);
    let mut parser = Parser {
        src,
        tokens,
        pos: 0,
        diags,
    };
    parser.parse_module()
}

/// Runs the raw lexer and drops trivia, recording absolute byte offsets.
fn lex(src: &str) -> Vec<Lexed> {
    let mut out = Vec::new();
    let mut offset = 0u32;
    for token in tokenize(src) {
        let lo = offset;
        let hi = offset + token.len;
        offset = hi;
        match token.kind {
            TokenKind::Whitespace | TokenKind::LineComment | TokenKind::BlockComment { .. } => {}
            kind => out.push(Lexed { kind, lo, hi }),
        }
    }
    out
}

struct Parser<'a> {
    src: &'a str,
    tokens: Vec<Lexed>,
    pos: usize,
    diags: &'a mut DiagnosticSink,
}

impl Parser<'_> {
    fn peek(&self) -> Option<Lexed> {
        self.tokens.get(self.pos).copied()
    }

    fn peek2(&self) -> Option<Lexed> {
        self.tokens.get(self.pos + 1).copied()
    }

    fn text(&self, token: Lexed) -> &str {
        &self.src[token.lo as usize..token.hi as usize]
    }

    fn at_end(&self) -> bool {
        self.pos >= self.tokens.len()
    }

    fn eof_span(&self) -> Span {
        let n = self.src.len() as u32;
        Span::new(n, n)
    }

    fn advance(&mut self) -> Option<Lexed> {
        let token = self.peek();
        if token.is_some() {
            self.pos += 1;
        }
        token
    }

    fn expect(&mut self, kind: TokenKind, what: &str) -> Option<Lexed> {
        match self.peek() {
            Some(token) if token.kind == kind => {
                self.pos += 1;
                Some(token)
            }
            Some(token) => {
                let span = Span::new(token.lo, token.hi);
                self.diags.error(format!("expected {what}"), span);
                None
            }
            None => {
                let span = self.eof_span();
                self.diags
                    .error(format!("expected {what} but reached end of input"), span);
                None
            }
        }
    }

    fn parse_module(&mut self) -> Module {
        let mut items = Vec::new();
        while !self.at_end() {
            let before = self.pos;
            match self.parse_stmt() {
                Some(stmt) => items.push(stmt),
                None => {
                    // Error recovery: guarantee forward progress.
                    if self.pos == before {
                        self.pos += 1;
                    }
                }
            }
        }
        Module { items }
    }

    fn parse_stmt(&mut self) -> Option<Stmt> {
        if let Some(token) = self.peek() {
            if token.kind == TokenKind::Ident {
                let is_binding = matches!(self.text(token), "let" | "var");
                if is_binding {
                    return self.parse_let();
                }
            }
        }
        let expr = self.parse_expr(0)?;
        let span = expr.span;
        Some(Stmt {
            kind: StmtKind::Expr(expr),
            span,
        })
    }

    fn parse_let(&mut self) -> Option<Stmt> {
        let keyword = self.advance()?; // `let` or `var`
        let mutable = self.text(keyword) == "var";
        let name_token = self.expect(TokenKind::Ident, "a variable name")?;
        let name = self.text(name_token).to_string();
        self.expect(TokenKind::Eq, "'='")?;
        let value = self.parse_expr(0)?;
        let span = Span::new(keyword.lo, value.span.hi());
        Some(Stmt {
            kind: StmtKind::Let {
                mutable,
                name,
                value,
            },
            span,
        })
    }

    /// Precedence-climbing expression parser. `min_bp` is the minimum binding
    /// power a binary operator must have to be consumed at this level.
    fn parse_expr(&mut self, min_bp: u8) -> Option<Expr> {
        let mut lhs = self.parse_prefix()?;
        while let Some((op, l_bp, r_bp)) = self.peek_binop() {
            if l_bp < min_bp {
                break;
            }
            self.pos += 1; // consume the operator
            let rhs = self.parse_expr(r_bp)?;
            let span = Span::new(lhs.span.lo(), rhs.span.hi());
            lhs = Expr {
                kind: ExprKind::Binary {
                    op,
                    lhs: Box::new(lhs),
                    rhs: Box::new(rhs),
                },
                span,
            };
        }
        Some(lhs)
    }

    /// Maps the current token to a binary operator and its `(left, right)`
    /// binding powers (left-associative: `right = left + 1`).
    fn peek_binop(&self) -> Option<(BinOp, u8, u8)> {
        let kind = self.peek()?.kind;
        let result = match kind {
            TokenKind::Plus => (BinOp::Add, 1, 2),
            TokenKind::Minus => (BinOp::Sub, 1, 2),
            TokenKind::Star => (BinOp::Mul, 3, 4),
            TokenKind::Slash => (BinOp::Div, 3, 4),
            TokenKind::Percent => (BinOp::Rem, 3, 4),
            _ => return None,
        };
        Some(result)
    }

    fn parse_prefix(&mut self) -> Option<Expr> {
        if let Some(token) = self.peek() {
            let op = match token.kind {
                TokenKind::Minus => Some(UnOp::Neg),
                TokenKind::Bang => Some(UnOp::Not),
                _ => None,
            };
            if let Some(op) = op {
                self.pos += 1;
                let expr = self.parse_prefix()?;
                let span = Span::new(token.lo, expr.span.hi());
                return Some(Expr {
                    kind: ExprKind::Unary {
                        op,
                        expr: Box::new(expr),
                    },
                    span,
                });
            }
        }
        self.parse_postfix()
    }

    fn parse_postfix(&mut self) -> Option<Expr> {
        let mut expr = self.parse_primary()?;
        while let Some(token) = self.peek() {
            match token.kind {
                TokenKind::OpenParen => {
                    expr = self.parse_call(expr)?;
                }
                _ => break,
            }
        }
        Some(expr)
    }

    fn parse_call(&mut self, callee: Expr) -> Option<Expr> {
        self.advance(); // `(`
        let mut args = Vec::new();
        if self.peek().map(|t| t.kind) != Some(TokenKind::CloseParen) {
            loop {
                let label = self.try_parse_label();
                let value = self.parse_expr(0)?;
                args.push(Arg { label, value });
                if self.peek().map(|t| t.kind) == Some(TokenKind::Comma) {
                    self.pos += 1;
                    continue;
                }
                break;
            }
        }
        let close = self.expect(TokenKind::CloseParen, "')'")?;
        let span = Span::new(callee.span.lo(), close.hi);
        Some(Expr {
            kind: ExprKind::Call {
                callee: Box::new(callee),
                args,
            },
            span,
        })
    }

    /// Consumes an argument label `ident :` if present.
    fn try_parse_label(&mut self) -> Option<String> {
        let first = self.peek()?;
        let second = self.peek2()?;
        if first.kind == TokenKind::Ident && second.kind == TokenKind::Colon {
            let label = self.text(first).to_string();
            self.pos += 2;
            Some(label)
        } else {
            None
        }
    }

    fn parse_primary(&mut self) -> Option<Expr> {
        let token = match self.peek() {
            Some(token) => token,
            None => {
                let span = self.eof_span();
                self.diags.error("unexpected end of input", span);
                return None;
            }
        };
        let span = Span::new(token.lo, token.hi);
        let text = self.text(token).to_string();
        let kind = match token.kind {
            TokenKind::Int => {
                self.pos += 1;
                ExprKind::Int(parse_int(&text))
            }
            TokenKind::Float => {
                self.pos += 1;
                ExprKind::Float(parse_float(&text))
            }
            TokenKind::Str { .. } => {
                self.pos += 1;
                ExprKind::Str(unquote_string(&text))
            }
            TokenKind::Ident => {
                self.pos += 1;
                match text.as_str() {
                    "true" => ExprKind::Bool(true),
                    "false" => ExprKind::Bool(false),
                    other => ExprKind::Ident(other.to_string()),
                }
            }
            TokenKind::OpenParen => {
                self.pos += 1;
                let inner = self.parse_expr(0)?;
                let close = self.expect(TokenKind::CloseParen, "')'")?;
                return Some(Expr {
                    kind: inner.kind,
                    span: Span::new(span.lo(), close.hi),
                });
            }
            _ => {
                self.diags.error(format!("unexpected token `{text}`"), span);
                return None;
            }
        };
        Some(Expr { kind, span })
    }
}

/// Parses an integer literal, honouring `0x`/`0b`/`0o` prefixes and `_`
/// separators. Defaults to `0` on overflow (the lexer already validated the
/// digits).
fn parse_int(text: &str) -> i64 {
    let cleaned = text.replace('_', "");
    let (radix, digits) = if let Some(rest) = cleaned.strip_prefix("0x").or_else(|| cleaned.strip_prefix("0X")) {
        (16, rest)
    } else if let Some(rest) = cleaned.strip_prefix("0b").or_else(|| cleaned.strip_prefix("0B")) {
        (2, rest)
    } else if let Some(rest) = cleaned.strip_prefix("0o").or_else(|| cleaned.strip_prefix("0O")) {
        (8, rest)
    } else {
        (10, cleaned.as_str())
    };
    i64::from_str_radix(digits, radix).unwrap_or(0)
}

fn parse_float(text: &str) -> f64 {
    text.replace('_', "").parse().unwrap_or(0.0)
}

/// Strips the surrounding quotes and resolves the escape sequences the lexer
/// recognised.
fn unquote_string(text: &str) -> String {
    let inner = text
        .strip_prefix('"')
        .and_then(|s| s.strip_suffix('"'))
        .unwrap_or(text);
    let mut out = String::new();
    let mut chars = inner.chars();
    while let Some(c) = chars.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        match chars.next() {
            Some('n') => out.push('\n'),
            Some('t') => out.push('\t'),
            Some('r') => out.push('\r'),
            Some('0') => out.push('\0'),
            Some('"') => out.push('"'),
            Some('\\') => out.push('\\'),
            Some(other) => {
                out.push('\\');
                out.push(other);
            }
            None => out.push('\\'),
        }
    }
    out
}
