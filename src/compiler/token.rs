#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TokenKind {
    Identifier,
    Keyword,
    Punctuation,
    Operator,
    BooleanLiteral,
    NilLiteral,
    FloatLiteral,
    DecimalLiteral,
    StringLiteral,
}

#[derive(Debug, Clone)]
pub struct Token {
    pub kind: TokenKind,
    pub raw_value: String,
    pub int_value: i64,
    pub line: u32,
    pub column: u32,
}

impl Token {
    pub fn new(kind: TokenKind, raw_value: String, line: u32, column: u32) -> Self {
        Self {
            kind,
            raw_value,
            int_value: 0,
            line,
            column,
        }
    }
}

// Keywords
pub mod keywords {
    pub const FUNC: &str = "func";
    pub const CLASS: &str = "class";
    pub const STRUCT: &str = "struct";
    pub const VAR: &str = "var";
    pub const LET: &str = "let";
    pub const IF: &str = "if";
    pub const ELSE: &str = "else";
    pub const FOR: &str = "for";
    pub const WHILE: &str = "while";
    pub const IMPORT: &str = "import";
    pub const TRY: &str = "try";
    pub const IN: &str = "in";
    pub const INIT: &str = "init";
    pub const SELF: &str = "self";
    pub const RETURN: &str = "return";
    pub const FILEIMPORT: &str = "fileimport";

    pub fn is_keyword(s: &str) -> bool {
        matches!(
            s,
            FUNC | CLASS | STRUCT | VAR | LET | IF | ELSE | FOR | WHILE | IMPORT | TRY | IN
                | INIT | SELF | RETURN | FILEIMPORT
        )
    }
}

// Punctuations
pub mod punctuations {
    pub const OPEN_CURLY: &str = "{";
    pub const CLOSE_CURLY: &str = "}";
    pub const OPEN_PAREN: &str = "(";
    pub const CLOSE_PAREN: &str = ")";
    pub const OPEN_BRACKET: &str = "[";
    pub const CLOSE_BRACKET: &str = "]";
    pub const COLON: &str = ":";
    pub const COMMA: &str = ",";
    pub const DOT: &str = ".";
}

// Operators
pub mod operators {
    pub const EQUALS: &str = "=";
    pub const NOT_EQUALS: &str = "!=";
    pub const EQUAL_EQUAL: &str = "==";
    pub const AND_AND: &str = "&&";
    pub const QUESTION: &str = "?";
    pub const BANG: &str = "!";
    pub const PLUS: &str = "+";
    pub const MINUS: &str = "-";
    pub const MULTIPLY: &str = "*";
    pub const DIV: &str = "/";
    pub const PERCENTAGE: &str = "%";
    pub const LESS: &str = "<";
    pub const LESS_EQ: &str = "<=";
    pub const GREATER: &str = ">";
    pub const GREATER_EQ: &str = ">=";

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum OperatorPriority {
        High,
        Low,
    }

    pub fn get_priority(op: &str) -> OperatorPriority {
        match op {
            PLUS | MINUS | AND_AND => OperatorPriority::Low,
            _ => OperatorPriority::High,
        }
    }
}

pub mod literals {
    pub const TRUE: &str = "true";
    pub const FALSE: &str = "false";
    pub const NIL: &str = "nil";
}
