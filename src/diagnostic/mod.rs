/// Diagnostic error reporting
#[derive(Debug)]
pub struct DiagnosticMessage {
    pub line: u32,
    pub column: u32,
    pub message: String,
}

#[derive(Debug)]
pub struct Diagnostics {
    pub errors: Vec<DiagnosticMessage>,
}

impl Diagnostics {
    pub fn new() -> Self {
        Self {
            errors: Vec::new(),
        }
    }

    pub fn error(&mut self, line: u32, column: u32, message: &str) {
        self.errors.push(DiagnosticMessage {
            line,
            column,
            message: message.to_string(),
        });
    }

    pub fn has_errors(&self) -> bool {
        !self.errors.is_empty()
    }

    pub fn print_errors(&self) {
        for err in &self.errors {
            eprintln!("error[{}:{}]: {}", err.line + 1, err.column, err.message);
        }
    }
}
