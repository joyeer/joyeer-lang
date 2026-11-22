# 0030: Literal patterns are not promoted into an Optional

- Status: accepted
- Spec: [§7.1.3 Literal pattern](../../specs/07-patterns.md#713-literal-pattern)

## Context

A literal in an expression is implicitly wrapped where an Optional is expected (§2.4): `let x: Int? = 1`
is `.Some(1)`. The type checker reused that assignability check for literal patterns, so
`match value { 1 => 10, _ => 0 }` with `value: Int?` type-checked. Lowering then tested the Optional
scrutinee itself against the integer literal. Before the IR verifier checked pattern kinds against
their types, the backend built an integer constant of the Optional's struct type (invalid LLVM IR);
after it, the program failed as `ir-lowering.verification-failed`, a compiler bug from the user's
point of view. The same held for string, Bool, byte and floating literals on an Optional, and for a
literal compared against an `Any` scrutinee, which no runtime type test can implement (§7).

## Options considered

1. Reject: a literal pattern's type must equal the scrutinee's type. `1` on `Int?` is
   `type-checking.type-mismatch`, with help to write `.Some(1)`. No lowering change; one rule that
   covers Optional, `Any`, and enum payload positions alike.
2. Promote: treat `1` on `Int?` as `.Some(1)`, rewriting in lowering (or recording the wrapping in
   the typed model). Shorter to write, but it adds a second, implicit meaning to a pattern, needs the
   coverage check to agree that `1` covers nothing of `.None`, and must decide how many layers to
   wrap for `Optional<Int?>` (expressions wrap only one).

## Decision

A literal pattern matches only a scrutinee of the literal's own type; an integer or floating literal
takes the scrutinee's numeric type when it is one. No implicit Optional promotion or `Any` conversion
applies in a pattern. A mismatch is `type-checking.type-mismatch`; on an Optional scrutinee the help
says to write the literal inside `.Some(...)`.

## Consequences

- Patterns stay structural: `.Some(1)` says in the source which case it tests, which is easier for a
  reviewer of AI-written code than an implicit wrap, and matches how `==` on an Optional is already
  rejected in favour of `match` (§5.2).
- Programs that previously failed in IR verification now fail in type checking with a span on the
  pattern. User code has no way to name `Any`, so the `Any` case is a guard for compiler-internal
  types rather than a user-visible change.
- Tests: `TypeCheckingTest.ALiteralPatternIsNotPromotedIntoAnOptionalScrutinee`,
  `TypeCheckingTest.ALiteralPatternMatchesItsOwnTypeAndInsideSome`, and the rejected fixture
  `tests/type-checking/err/optional_literal_pattern.joyeer`.
- Option 2 can be added later without breaking any accepted program.

## Evidence

The IR verifier's pattern-kind rule (`docs/impl/ir.md` section 4) rejects a literal case typed as an
enumeration, which exposed the gap; `.Some(literal)` patterns already lower and run (for example
`tests/e2e/optional_nested_layers.joyeer`).
