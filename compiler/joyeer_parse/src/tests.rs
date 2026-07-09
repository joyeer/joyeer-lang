use crate::parse_module;
use joyeer_ast::StmtKind;
use joyeer_errors::DiagnosticSink;

#[test]
fn parses_binding_and_call() {
    let mut diags = DiagnosticSink::new();
    let module = parse_module("let x = 1 + 2\nprint(value: x)", &mut diags);
    assert!(!diags.has_errors(), "{:?}", diags.diagnostics());
    assert_eq!(module.items.len(), 2);
    assert!(matches!(module.items[0].kind, StmtKind::Let { .. }));
    assert!(matches!(module.items[1].kind, StmtKind::Expr(_)));
}

#[test]
fn reports_missing_equals() {
    let mut diags = DiagnosticSink::new();
    let _ = parse_module("let x 1", &mut diags);
    assert!(diags.has_errors());
}
