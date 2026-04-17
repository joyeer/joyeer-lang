use crate::compiler::token::*;
use crate::diagnostic::Diagnostics;

pub struct Lexer {
    source: Vec<char>,
    pos: usize,
    line: u32,
    line_start: usize,
    tokens: Vec<Token>,
    diagnostics: Diagnostics,
}

impl Lexer {
    pub fn new(source: &str) -> Self {
        Self {
            source: source.chars().collect(),
            pos: 0,
            line: 0,
            line_start: 0,
            tokens: Vec::new(),
            diagnostics: Diagnostics::new(),
        }
    }

    pub fn parse(mut self) -> Result<Vec<Token>, Diagnostics> {
        while self.pos < self.source.len() {
            let ch = self.source[self.pos];
            self.pos += 1;
            match ch {
                '\u{0000}' | '\u{0009}' | '\u{000B}' | '\u{000C}' | '\u{0020}' => {}
                '\u{000A}' => {
                    self.line += 1;
                    self.line_start = self.pos - 1;
                }
                '\u{000D}' => {
                    self.line += 1;
                    self.line_start = self.pos - 1;
                    if self.pos < self.source.len() && self.source[self.pos] == '\u{000A}' {
                        self.pos += 1;
                    }
                }
                'a'..='z' | 'A'..='Z' | '_' => self.parse_identifier(),
                '1'..='9' => self.parse_number(self.pos - 1),
                '0' => self.parse_zero(),
                '/' => {
                    if self.pos < self.source.len() && self.source[self.pos] == '/' {
                        self.parse_cpp_comment();
                    } else if self.pos < self.source.len() && self.source[self.pos] == '*' {
                        self.pos += 1;
                        self.parse_c_comment();
                    } else {
                        self.push_single_operator(self.pos - 1);
                    }
                }
                '=' => {
                    if self.pos < self.source.len() && self.source[self.pos] == '=' {
                        self.pos += 1;
                        self.push_operator(operators::EQUAL_EQUAL);
                    } else {
                        self.push_single_operator(self.pos - 1);
                    }
                }
                '!' => {
                    if self.pos < self.source.len() && self.source[self.pos] == '=' {
                        self.pos += 1;
                        self.push_operator(operators::NOT_EQUALS);
                    } else {
                        self.push_single_operator(self.pos - 1);
                    }
                }
                '<' => {
                    if self.pos < self.source.len() && self.source[self.pos] == '=' {
                        self.pos += 1;
                        self.push_operator(operators::LESS_EQ);
                    } else {
                        self.push_single_operator(self.pos - 1);
                    }
                }
                '>' => {
                    if self.pos < self.source.len() && self.source[self.pos] == '=' {
                        self.pos += 1;
                        self.push_operator(operators::GREATER_EQ);
                    } else {
                        self.push_single_operator(self.pos - 1);
                    }
                }
                '&' => {
                    if self.pos < self.source.len() && self.source[self.pos] == '&' {
                        self.pos += 1;
                        self.push_operator(operators::AND_AND);
                    } else {
                        self.push_single_operator(self.pos - 1);
                    }
                }
                '-' | '+' | '*' | '%' | '|' | '^' | '~' | '?' => {
                    self.push_single_operator(self.pos - 1);
                }
                '(' | ')' | '{' | '}' | '[' | ']' | '.' | ',' | ':' | '@' | '#' | ';' => {
                    self.push_punctuation(self.pos - 1);
                }
                '"' => self.parse_string_literal(),
                _ => {}
            }
        }

        if self.diagnostics.has_errors() {
            Err(self.diagnostics)
        } else {
            Ok(self.tokens)
        }
    }

    fn column_at(&self, pos: usize) -> u32 {
        (pos - self.line_start) as u32
    }

    fn parse_zero(&mut self) {
        if self.pos >= self.source.len() {
            let mut token = Token::new(
                TokenKind::DecimalLiteral,
                "0".into(),
                self.line,
                self.column_at(self.pos - 1),
            );
            token.int_value = 0;
            self.tokens.push(token);
            return;
        }
        match self.source[self.pos] {
            '0'..='7' => self.parse_octal(self.pos),
            '8' | '9' => {
                self.diagnostics.error(self.line, self.column_at(self.pos), "invalid octal number");
            }
            _ => {
                let mut token = Token::new(
                    TokenKind::DecimalLiteral,
                    "0".into(),
                    self.line,
                    self.column_at(self.pos - 1),
                );
                token.int_value = 0;
                self.tokens.push(token);
            }
        }
    }

    fn parse_octal(&mut self, start: usize) {
        while self.pos < self.source.len() {
            match self.source[self.pos] {
                '0'..='7' => self.pos += 1,
                '8' | '9' => {
                    self.diagnostics
                        .error(self.line, self.column_at(start), "invalid octal number");
                    return;
                }
                _ => break,
            }
        }
        let s: String = self.source[start..self.pos].iter().collect();
        let val = i64::from_str_radix(&s, 8).unwrap_or(0);
        let mut token = Token::new(
            TokenKind::DecimalLiteral,
            s,
            self.line,
            self.column_at(start),
        );
        token.int_value = val;
        self.tokens.push(token);
    }

    fn parse_number(&mut self, start: usize) {
        while self.pos < self.source.len() && self.source[self.pos].is_ascii_digit() {
            self.pos += 1;
        }

        // Check for decimal fraction
        if self.pos < self.source.len() && self.source[self.pos] == '.' {
            let dot_pos = self.pos;
            self.pos += 1;
            let mut has_fraction = false;
            while self.pos < self.source.len() && self.source[self.pos].is_ascii_digit() {
                has_fraction = true;
                self.pos += 1;
            }
            if !has_fraction {
                self.pos = dot_pos; // revert the dot
            }
        }

        let s: String = self.source[start..self.pos].iter().collect();
        let val: i64 = s.parse().unwrap_or(0);
        let mut token = Token::new(
            TokenKind::DecimalLiteral,
            s,
            self.line,
            self.column_at(start),
        );
        token.int_value = val;
        self.tokens.push(token);
    }

    fn push_single_operator(&mut self, pos: usize) {
        let s = self.source[pos].to_string();
        self.tokens.push(Token::new(
            TokenKind::Operator,
            s,
            self.line,
            self.column_at(pos),
        ));
    }

    fn push_operator(&mut self, op: &str) {
        self.tokens.push(Token::new(
            TokenKind::Operator,
            op.into(),
            self.line,
            self.column_at(self.pos),
        ));
    }

    fn push_punctuation(&mut self, pos: usize) {
        let s = self.source[pos].to_string();
        self.tokens.push(Token::new(
            TokenKind::Punctuation,
            s,
            self.line,
            self.column_at(pos),
        ));
    }

    fn parse_identifier(&mut self) {
        let start = self.pos - 1;
        while self.pos < self.source.len() {
            match self.source[self.pos] {
                'a'..='z' | 'A'..='Z' | '_' | '0'..='9' => self.pos += 1,
                _ => break,
            }
        }
        let s: String = self.source[start..self.pos].iter().collect();
        let kind = if keywords::is_keyword(&s) {
            TokenKind::Keyword
        } else if s == literals::NIL {
            TokenKind::NilLiteral
        } else if s == literals::TRUE || s == literals::FALSE {
            TokenKind::BooleanLiteral
        } else {
            TokenKind::Identifier
        };
        self.tokens
            .push(Token::new(kind, s, self.line, self.column_at(start)));
    }

    fn parse_string_literal(&mut self) {
        let start = self.pos;
        loop {
            if self.pos >= self.source.len() {
                self.diagnostics
                    .error(self.line, self.column_at(self.pos), "unterminated string literal");
                return;
            }
            let ch = self.source[self.pos];
            if ch == '\\' {
                self.pos += 1;
                if self.pos >= self.source.len() {
                    self.diagnostics.error(
                        self.line,
                        self.column_at(self.pos),
                        "unterminated string literal",
                    );
                    return;
                }
                self.pos += 1;
            } else if ch == '"' {
                let s: String = self.source[start..self.pos].iter().collect();
                self.pos += 1; // skip closing quote
                self.tokens.push(Token::new(
                    TokenKind::StringLiteral,
                    s,
                    self.line,
                    self.column_at(start),
                ));
                return;
            } else {
                self.pos += 1;
            }
        }
    }

    fn parse_cpp_comment(&mut self) {
        while self.pos < self.source.len() && self.source[self.pos] != '\n' {
            self.pos += 1;
        }
    }

    fn parse_c_comment(&mut self) {
        while self.pos + 1 < self.source.len() {
            if self.source[self.pos] == '*' && self.source[self.pos + 1] == '/' {
                self.pos += 2;
                return;
            }
            if self.source[self.pos] == '\n' {
                self.line += 1;
                self.line_start = self.pos;
            }
            self.pos += 1;
        }
        self.diagnostics
            .error(self.line, self.column_at(self.pos), "unterminated block comment");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_lex_basic() {
        let source = "var i = 100";
        let tokens = Lexer::new(source).parse().unwrap();
        assert_eq!(tokens.len(), 4);
        assert_eq!(tokens[0].kind, TokenKind::Keyword);
        assert_eq!(tokens[0].raw_value, "var");
        assert_eq!(tokens[1].kind, TokenKind::Identifier);
        assert_eq!(tokens[1].raw_value, "i");
        assert_eq!(tokens[2].kind, TokenKind::Operator);
        assert_eq!(tokens[2].raw_value, "=");
        assert_eq!(tokens[3].kind, TokenKind::DecimalLiteral);
        assert_eq!(tokens[3].int_value, 100);
    }
}
