//! Pure tokenizer for Joyeer source.
//!
//! Mirrors the design of `rustc_lexer`: this crate operates directly on `&str`,
//! produces simple tokens that pair a [`TokenKind`] tag with the byte length of
//! the lexeme, and intentionally depends on no other Joyeer crate. It knows
//! nothing about spans, interning, keywords, or error reporting -- the parser
//! (`joyeer_parse`) layers those concerns on top.

#[cfg(test)]
mod tests;

use std::str::Chars;

/// End-of-input sentinel returned by [`Cursor::first`] and friends.
const EOF_CHAR: char = '\0';

/// A single token: a [`TokenKind`] and the byte length of its lexeme.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Token {
    pub kind: TokenKind,
    pub len: u32,
}

impl Token {
    fn new(kind: TokenKind, len: u32) -> Token {
        Token { kind, len }
    }
}

/// The classification of a raw lexeme.
///
/// Keywords are *not* distinguished here -- they lex as [`TokenKind::Ident`]
/// and are recognised by the parser. Multi-character operators likewise lex as
/// a sequence of single-character punctuation tokens and are glued by the
/// parser.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TokenKind {
    // Trivia.
    Whitespace,
    LineComment,
    BlockComment { terminated: bool },

    // Literals and identifiers.
    Ident,
    Int,
    Float,
    Str { terminated: bool },
    Char { terminated: bool },
    Byte { terminated: bool },

    // One-character punctuation and operators.
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Eq,
    Bang,
    Lt,
    Gt,
    Amp,
    Pipe,
    Caret,
    Tilde,
    Question,
    Dot,
    Comma,
    Semi,
    Colon,
    At,
    OpenParen,
    CloseParen,
    OpenBrace,
    CloseBrace,
    OpenBracket,
    CloseBracket,

    /// Anything the lexer does not recognise.
    Unknown,
    /// Returned once the input is exhausted.
    Eof,
}

/// Tokenizes `input`, yielding every token including trivia (whitespace and
/// comments). The parser is responsible for discarding trivia.
pub fn tokenize(input: &str) -> impl Iterator<Item = Token> + '_ {
    let mut cursor = Cursor::new(input);
    std::iter::from_fn(move || {
        let token = cursor.advance_token();
        if token.kind == TokenKind::Eof {
            None
        } else {
            Some(token)
        }
    })
}

/// Returns `true` for characters treated as whitespace by Joyeer.
pub fn is_whitespace(c: char) -> bool {
    matches!(c, ' ' | '\t' | '\n' | '\r' | '\u{000B}' | '\u{000C}')
}

/// `ident_head ::= 'a'..'z' | 'A'..'Z' | '_'` (spec section 1.3).
pub fn is_ident_start(c: char) -> bool {
    c == '_' || c.is_ascii_alphabetic()
}

/// `ident_tail ::= ident_head | '0'..'9'` (spec section 1.3).
pub fn is_ident_continue(c: char) -> bool {
    c == '_' || c.is_ascii_alphanumeric()
}

/// A cursor over the remaining characters of the input.
struct Cursor<'a> {
    len_remaining: usize,
    chars: Chars<'a>,
}

impl<'a> Cursor<'a> {
    fn new(input: &'a str) -> Cursor<'a> {
        Cursor {
            len_remaining: input.len(),
            chars: input.chars(),
        }
    }

    /// The next character without consuming it, or [`EOF_CHAR`] at the end.
    fn first(&self) -> char {
        self.chars.clone().next().unwrap_or(EOF_CHAR)
    }

    /// The character after next without consuming it.
    fn second(&self) -> char {
        let mut iter = self.chars.clone();
        iter.next();
        iter.next().unwrap_or(EOF_CHAR)
    }

    fn is_eof(&self) -> bool {
        self.chars.as_str().is_empty()
    }

    /// Consumes and returns the next character.
    fn bump(&mut self) -> Option<char> {
        self.chars.next()
    }

    /// Bytes consumed since the last [`Cursor::reset_pos`].
    fn pos_within_token(&self) -> u32 {
        (self.len_remaining - self.chars.as_str().len()) as u32
    }

    fn reset_pos(&mut self) {
        self.len_remaining = self.chars.as_str().len();
    }

    fn eat_while(&mut self, mut predicate: impl FnMut(char) -> bool) {
        while predicate(self.first()) && !self.is_eof() {
            self.bump();
        }
    }

    fn advance_token(&mut self) -> Token {
        let first = match self.bump() {
            Some(c) => c,
            None => return Token::new(TokenKind::Eof, 0),
        };

        let kind = match first {
            c if is_whitespace(c) => {
                self.eat_while(is_whitespace);
                TokenKind::Whitespace
            }

            '/' => match self.first() {
                '/' => self.line_comment(),
                '*' => self.block_comment(),
                _ => TokenKind::Slash,
            },

            // Byte literal `b'...'`; a bare `b` is an ordinary identifier.
            'b' if self.first() == '\'' => {
                self.bump();
                TokenKind::Byte {
                    terminated: self.single_quoted(),
                }
            }

            c if is_ident_start(c) => {
                self.eat_while(is_ident_continue);
                TokenKind::Ident
            }

            '0'..='9' => self.number(first),

            '"' => self.string(),
            '\'' => TokenKind::Char {
                terminated: self.single_quoted(),
            },

            '+' => TokenKind::Plus,
            '-' => TokenKind::Minus,
            '*' => TokenKind::Star,
            '%' => TokenKind::Percent,
            '=' => TokenKind::Eq,
            '!' => TokenKind::Bang,
            '<' => TokenKind::Lt,
            '>' => TokenKind::Gt,
            '&' => TokenKind::Amp,
            '|' => TokenKind::Pipe,
            '^' => TokenKind::Caret,
            '~' => TokenKind::Tilde,
            '?' => TokenKind::Question,
            '.' => TokenKind::Dot,
            ',' => TokenKind::Comma,
            ';' => TokenKind::Semi,
            ':' => TokenKind::Colon,
            '@' => TokenKind::At,
            '(' => TokenKind::OpenParen,
            ')' => TokenKind::CloseParen,
            '{' => TokenKind::OpenBrace,
            '}' => TokenKind::CloseBrace,
            '[' => TokenKind::OpenBracket,
            ']' => TokenKind::CloseBracket,

            _ => TokenKind::Unknown,
        };

        let len = self.pos_within_token();
        self.reset_pos();
        Token::new(kind, len)
    }

    fn line_comment(&mut self) -> TokenKind {
        self.bump(); // the second '/'
        self.eat_while(|c| c != '\n');
        TokenKind::LineComment
    }

    fn block_comment(&mut self) -> TokenKind {
        self.bump(); // the '*'
        let mut depth: usize = 1;
        while let Some(c) = self.bump() {
            match c {
                '/' if self.first() == '*' => {
                    self.bump();
                    depth += 1;
                }
                '*' if self.first() == '/' => {
                    self.bump();
                    depth -= 1;
                    if depth == 0 {
                        break;
                    }
                }
                _ => {}
            }
        }
        TokenKind::BlockComment {
            terminated: depth == 0,
        }
    }

    fn number(&mut self, first: char) -> TokenKind {
        if first == '0' {
            match self.first() {
                'x' | 'X' => {
                    self.bump();
                    self.eat_while(|c| c.is_ascii_hexdigit() || c == '_');
                    return TokenKind::Int;
                }
                'b' | 'B' => {
                    self.bump();
                    self.eat_while(|c| matches!(c, '0' | '1' | '_'));
                    return TokenKind::Int;
                }
                'o' | 'O' => {
                    self.bump();
                    self.eat_while(|c| matches!(c, '0'..='7' | '_'));
                    return TokenKind::Int;
                }
                _ => {}
            }
        }

        self.eat_while(|c| c.is_ascii_digit() || c == '_');

        let mut is_float = false;

        // Fractional part: only if a digit follows the '.', so `1.method` and
        // range syntax `1..<5` are not swallowed.
        if self.first() == '.' && self.second().is_ascii_digit() {
            is_float = true;
            self.bump();
            self.eat_while(|c| c.is_ascii_digit() || c == '_');
        }

        if matches!(self.first(), 'e' | 'E') {
            is_float = true;
            self.bump();
            if matches!(self.first(), '+' | '-') {
                self.bump();
            }
            self.eat_while(|c| c.is_ascii_digit() || c == '_');
        }

        if is_float {
            TokenKind::Float
        } else {
            TokenKind::Int
        }
    }

    fn string(&mut self) -> TokenKind {
        // Opening '"' already consumed.
        while let Some(c) = self.bump() {
            match c {
                '"' => return TokenKind::Str { terminated: true },
                // Skip the escaped character so `\"` and `\\` do not end the
                // string prematurely.
                '\\' => {
                    self.bump();
                }
                _ => {}
            }
        }
        TokenKind::Str { terminated: false }
    }

    /// Consumes the body of a `'...'` literal, assuming the opening quote is
    /// already consumed. Returns whether it was terminated.
    fn single_quoted(&mut self) -> bool {
        loop {
            match self.bump() {
                None => return false,
                Some('\'') => return true,
                Some('\\') => {
                    self.bump();
                }
                Some(_) => {}
            }
        }
    }
}
