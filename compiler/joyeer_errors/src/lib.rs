//! Diagnostics for the Joyeer compiler.
//!
//! A [`DiagnosticSink`] collects [`Diagnostic`]s produced by any pass and can
//! render them against a [`SourceFile`]. Passes report recoverable problems
//! here instead of panicking (see AGENTS.md).

use joyeer_span::{SourceFile, Span};

/// Severity of a [`Diagnostic`].
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Level {
    Error,
    Warning,
    Note,
}

impl Level {
    fn as_str(self) -> &'static str {
        match self {
            Level::Error => "error",
            Level::Warning => "warning",
            Level::Note => "note",
        }
    }
}

/// A single diagnostic message, optionally anchored to a source span.
#[derive(Clone, Debug)]
pub struct Diagnostic {
    pub level: Level,
    pub message: String,
    pub span: Option<Span>,
}

impl Diagnostic {
    /// An error with no attached span.
    pub fn error(message: impl Into<String>) -> Diagnostic {
        Diagnostic {
            level: Level::Error,
            message: message.into(),
            span: None,
        }
    }

    /// Attaches a source span to this diagnostic.
    pub fn with_span(mut self, span: Span) -> Diagnostic {
        self.span = Some(span);
        self
    }
}

/// Collects diagnostics emitted during a compilation.
#[derive(Default)]
pub struct DiagnosticSink {
    diagnostics: Vec<Diagnostic>,
}

impl DiagnosticSink {
    /// Creates an empty sink.
    pub fn new() -> DiagnosticSink {
        DiagnosticSink::default()
    }

    /// Records a diagnostic.
    pub fn emit(&mut self, diagnostic: Diagnostic) {
        self.diagnostics.push(diagnostic);
    }

    /// Convenience for the common "error at a span" case.
    pub fn error(&mut self, message: impl Into<String>, span: Span) {
        self.emit(Diagnostic::error(message).with_span(span));
    }

    /// Whether any error-level diagnostic was recorded.
    pub fn has_errors(&self) -> bool {
        self.diagnostics.iter().any(|d| d.level == Level::Error)
    }

    /// All recorded diagnostics, in emission order.
    pub fn diagnostics(&self) -> &[Diagnostic] {
        &self.diagnostics
    }

    /// Renders every diagnostic as `name:line:col: level: message`, one per line.
    pub fn render(&self, source: &SourceFile) -> String {
        let mut out = String::new();
        for d in &self.diagnostics {
            match d.span {
                Some(span) => {
                    let (line, col) = source.location(span.lo);
                    out.push_str(&format!(
                        "{}:{}:{}: {}: {}\n",
                        source.name,
                        line,
                        col,
                        d.level.as_str(),
                        d.message
                    ));
                }
                None => {
                    out.push_str(&format!("{}: {}\n", d.level.as_str(), d.message));
                }
            }
        }
        out
    }
}
