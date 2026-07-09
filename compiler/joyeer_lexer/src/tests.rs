use crate::{tokenize, TokenKind};

/// A short, stable tag for a token kind, independent of `Debug` formatting.
fn tag(kind: &TokenKind) -> &'static str {
    use TokenKind::*;
    match kind {
        Whitespace => "WS",
        LineComment => "LineComment",
        BlockComment { .. } => "BlockComment",
        Ident => "Ident",
        Int => "Int",
        Float => "Float",
        Str { .. } => "Str",
        Char { .. } => "Char",
        Byte { .. } => "Byte",
        Plus => "Plus",
        Minus => "Minus",
        Star => "Star",
        Slash => "Slash",
        Percent => "Percent",
        Eq => "Eq",
        Bang => "Bang",
        Lt => "Lt",
        Gt => "Gt",
        Amp => "Amp",
        Pipe => "Pipe",
        Caret => "Caret",
        Tilde => "Tilde",
        Question => "Question",
        Dot => "Dot",
        Comma => "Comma",
        Semi => "Semi",
        Colon => "Colon",
        At => "At",
        OpenParen => "OpenParen",
        CloseParen => "CloseParen",
        OpenBrace => "OpenBrace",
        CloseBrace => "CloseBrace",
        OpenBracket => "OpenBracket",
        CloseBracket => "CloseBracket",
        Unknown => "Unknown",
        Eof => "Eof",
    }
}

fn render(input: &str) -> String {
    tokenize(input)
        .map(|t| format!("{}:{} ", tag(&t.kind), t.len))
        .collect::<String>()
        .trim_end()
        .to_string()
}

#[test]
fn binding() {
    assert_eq!(
        render("let x = 1"),
        "Ident:3 WS:1 Ident:1 WS:1 Eq:1 WS:1 Int:1"
    );
}

#[test]
fn arithmetic() {
    assert_eq!(
        render("1 + 2 * 3"),
        "Int:1 WS:1 Plus:1 WS:1 Int:1 WS:1 Star:1 WS:1 Int:1"
    );
}

#[test]
fn string_literal() {
    assert_eq!(render("\"hello\""), "Str:7");
    // Quote, a, backslash, quote, b, quote -- the escaped quote is not a
    // terminator.
    assert_eq!(render("\"a\\\"b\""), "Str:6");
}

#[test]
fn number_literals() {
    assert_eq!(render("3.14"), "Float:4");
    assert_eq!(render("1e3"), "Float:3");
    assert_eq!(render("0xFF"), "Int:4");
    // Range syntax must not be lexed as a float.
    assert_eq!(render("1..5"), "Int:1 Dot:1 Dot:1 Int:1");
}

#[test]
fn comments_nest() {
    assert_eq!(render("/* a /* b */ c */"), "BlockComment:17");
    assert_eq!(render("// hi"), "LineComment:5");
}

#[test]
fn call_punctuation() {
    assert_eq!(render("print(x)"), "Ident:5 OpenParen:1 Ident:1 CloseParen:1");
}
