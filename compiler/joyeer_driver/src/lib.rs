//! Compiler driver: wires the pipeline (lex -> parse -> interpret) together and
//! exposes both a library API ([`run_source`]) and the process entry point
//! ([`main`]) used by the `joyeer` binary.

use std::path::Path;
use std::process::ExitCode;

use joyeer_errors::{Diagnostic, DiagnosticSink};
use joyeer_interp::Interp;
use joyeer_span::SourceFile;

/// The outcome of running a Joyeer source unit.
pub struct RunResult {
    /// Everything the program wrote via `print`.
    pub stdout: String,
    /// Rendered diagnostics (may be empty).
    pub diagnostics: String,
    /// `true` when the program compiled and ran without errors.
    pub success: bool,
}

/// Parses and interprets `src`, capturing output and diagnostics.
pub fn run_source(name: &str, src: &str) -> RunResult {
    let source = SourceFile::new(name, src);
    let mut diags = DiagnosticSink::new();

    let module = joyeer_parse::parse_module(src, &mut diags);
    if diags.has_errors() {
        return RunResult {
            stdout: String::new(),
            diagnostics: diags.render(&source),
            success: false,
        };
    }

    let mut interp = Interp::new();
    match interp.run(&module) {
        Ok(()) => RunResult {
            stdout: interp.output,
            diagnostics: diags.render(&source),
            success: true,
        },
        Err(message) => {
            diags.emit(Diagnostic::error(message));
            RunResult {
                stdout: interp.output,
                diagnostics: diags.render(&source),
                success: false,
            }
        }
    }
}

/// Reads `path` and runs it through [`run_source`].
pub fn run_file(path: &Path) -> std::io::Result<RunResult> {
    let src = std::fs::read_to_string(path)?;
    let name = path
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or("<input>");
    Ok(run_source(name, &src))
}

/// Process entry point used by the thin `joyeer` binary.
pub fn main() -> ExitCode {
    let mut args = std::env::args_os().skip(1);
    let path = match args.next() {
        Some(path) => std::path::PathBuf::from(path),
        None => {
            eprintln!("usage: joyeer <file.joyeer>");
            return ExitCode::from(2);
        }
    };

    match run_file(&path) {
        Ok(result) => {
            print!("{}", result.stdout);
            if !result.diagnostics.is_empty() {
                eprint!("{}", result.diagnostics);
            }
            if result.success {
                ExitCode::SUCCESS
            } else {
                ExitCode::FAILURE
            }
        }
        Err(error) => {
            eprintln!("error: cannot read {}: {error}", path.display());
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::run_source;

    #[test]
    fn runs_print() {
        let result = run_source("t.joyeer", "print(message: \"hi\")");
        assert!(result.success, "{}", result.diagnostics);
        assert_eq!(result.stdout, "hi\n");
    }

    #[test]
    fn evaluates_arithmetic_with_precedence() {
        let result = run_source("t.joyeer", "let x = 1 + 2 * 3\nprint(value: x)");
        assert!(result.success, "{}", result.diagnostics);
        assert_eq!(result.stdout, "7\n");
    }

    #[test]
    fn reports_undefined_variable() {
        let result = run_source("t.joyeer", "print(value: y)");
        assert!(!result.success);
        assert!(result.diagnostics.contains("undefined variable `y`"));
    }
}
