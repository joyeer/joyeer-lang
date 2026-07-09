//! End-to-end golden tests: run every `tests/bootstrap/**/*.joyeer` through the
//! built `joyeer` binary and diff combined stdout+stderr against the sibling
//! `*.result.txt`. Set `JOYEER_BLESS=1` to regenerate the expected files.

use std::path::{Path, PathBuf};
use std::process::Command;

/// `<workspace>/tests/bootstrap`, derived from this crate's manifest dir.
fn bootstrap_tests_dir() -> PathBuf {
    // CARGO_MANIFEST_DIR = <workspace>/compiler/joyeer
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    manifest
        .parent()
        .and_then(Path::parent)
        .expect("workspace root")
        .join("tests")
        .join("bootstrap")
}

fn collect(dir: &Path, out: &mut Vec<PathBuf>) {
    let entries = match std::fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(_) => return,
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            collect(&path, out);
        } else if path.extension().and_then(|e| e.to_str()) == Some("joyeer") {
            out.push(path);
        }
    }
}

fn normalize(text: &str) -> String {
    text.replace("\r\n", "\n").trim_end().to_string()
}

#[test]
fn golden() {
    let binary = env!("CARGO_BIN_EXE_joyeer");
    let dir = bootstrap_tests_dir();

    let mut cases = Vec::new();
    collect(&dir, &mut cases);
    cases.sort();
    assert!(
        !cases.is_empty(),
        "no .joyeer cases found under {}",
        dir.display()
    );

    let bless = std::env::var_os("JOYEER_BLESS").is_some();
    let mut failures = Vec::new();

    for case in cases {
        let output = Command::new(binary)
            .arg(&case)
            .output()
            .expect("failed to run joyeer");
        let mut actual = String::from_utf8_lossy(&output.stdout).into_owned();
        actual.push_str(&String::from_utf8_lossy(&output.stderr));

        let expected_path = case.with_extension("result.txt");
        if bless {
            std::fs::write(&expected_path, &actual).expect("write expected file");
            continue;
        }

        let expected = std::fs::read_to_string(&expected_path).unwrap_or_default();
        if normalize(&actual) != normalize(&expected) {
            failures.push(format!(
                "case {} mismatch\n--- expected ---\n{}\n--- actual ---\n{}",
                case.display(),
                expected,
                actual
            ));
        }
    }

    assert!(
        failures.is_empty(),
        "golden test failures:\n{}",
        failures.join("\n\n")
    );
}
