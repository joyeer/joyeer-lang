# 0024: Optional and Result as role-bound library enums

- Status: proposed (2026-11-14), revised after a language-designer review (see "Review").
  Phase 1 of the [library containers plan](../plan/0011-library-containers.md).
- Spec: amendments listed under "Spec changes"; linked here once accepted.

## Context

`Optional<T>` and `Result<T, E>` are specified as enums (§2.4, §2.5) but implemented as compiler type
kinds. `TypeKind::optional` and `TypeKind::result` are registered as generic builtins, their case
symbols are synthesized by name resolution's `buildPrelude`, and the compiler recovers payloads and
tags by comparing case spellings with `"Some"`, `"None"`, `"Ok"` and `"Err"`. There are about 20 such
name comparisons and about 48 uses of the two kinds across the frontend, IR lowering, the verifier
and the backend; the implementation starts by listing them (see "Consequences"). The backend already
lays both out as ordinary enums (an 8-byte tag and the largest payload).

Generic enums ([0022](0022-generic-types-and-layout-intrinsics.md)) and a bundled source prelude with
closed compiler roles ([0023](0023-library-concurrency.md)) now exist, so the declarations can move to
Joyeer. What a plain generic enum cannot express is the language attached to these two types:

- `T?` sugar, `nil` (literal and pattern), implicit wrapping of `T` into `T?`, force-unwrap `x!`,
  optional chaining `x?.field`, the `value?(...)` call form, and `??` once implemented (§8.5);
- postfix `?` propagation (§8.3), which needs to know which case continues and which returns;
- linear payloads: an `Optional` or `Result` with a linear payload is itself linear (§4.1.1), whereas
  a generic type may not take a linear type argument (0022 C1);
- `Never` payloads: `Result<Never, E>`, `Result<T, Never>` and `Never?` are valid today, whereas a
  generic type may not take `Never` (0022, §20.12.3);
- compiler-created values: `as?` produces `Result<T, ConversionError>`, and `Array.removeLast` and
  `Dict.get` produce an `Optional`.

The plan forbids replacing builtin-name dispatch with enum-name dispatch: a user `enum Optional<T>` is
an ordinary type in its scope (0022), and `T?` must not change meaning when one is declared.

## Options considered

1. **Keep the builtin type kinds.** No migration risk, but leaves every name comparison in place and
   blocks the Array and Dict phases, which produce and consume `Optional`.
2. **Prelude declarations found by spelling.** The compiler looks the declaration up by the name
   `Optional` and its cases by `Some` and `None`. Small change, but it moves the name dispatch rather
   than removing it, and is ambiguous under user shadowing.
3. **Prelude declarations bound by a closed enum role, cases bound by position.** The declaration
   carries `@compiler_role(optional)` or `@compiler_role(result)`. The compiler validates its shape once
   and records which case is which. Syntax, propagation and compiler-made values refer to the role,
   never to a name.
4. **As 3, with a role on each case** (`@compiler_role(optional_some)`). Member-level roles already
   exist for methods, so the parser cost is modest, but it doubles the roles and adds case-level
   grammar while positional binding already identifies every case without ambiguity.

## Decision

Option 3.

### Declarations and load order

A toolchain-owned source prelude declares:

```joyeer
@compiler_role(optional)
public enum Optional<T> {
    Some(T),
    None,
}

@compiler_role(result)
public enum Result<T, E> {
    Ok(T),
    Err(E),
}
```

It is bundled into the compiler like the concurrency prelude. Name resolution's `buildPrelude`, which
today declares builtin types, functions and members in one step before any module is collected, is
split into three steps:

1. builtin types and functions;
2. the role prelude's top-level collection, which reads the role attributes and exports `Optional` and
   `Result` to the prelude scope (their case symbols are created later by the ordinary member pass);
3. builtin members that name `Optional` (`Array.removeLast`, `Dict.get`), declared against the
   role-bound type symbol.

So no "runtime prelude conflicts with" error arises. `TypeContext::optionalType` and `resultType`
find the declaration by role, not with `builtinSymbol("Optional")`. Standard modules (`lib/System`,
`lib/Compression`), the concurrency prelude, and single-file mode all see the same declarations through
the prelude scope. Runtime core units (§19.4.7) do not use these types today; making them available
there is a separate change.

A missing role declaration is `name-resolution.invalid-prelude`, reported when step 3 needs it. A
malformed one is `type-checking.invalid-prelude`, reported by the prelude metadata pass, which runs
before function bodies are checked. Either way `T?` does not fall back to a cascade of unrelated
errors.

Cases are declared in today's tag order (`Some` = 0, `None` = 1, `Ok` = 0, `Err` = 1), so layouts and
generated code are unchanged. Tag numbering is not a language guarantee and has no C ABI (§19). The
§2.4 example's `None, Some` order is illustrative and does not change.

### Roles

`optional` and `result` join the closed role set of 0023, with the same authorization: only the
compiler-bundled prelude, which is the only source parsed with prelude annotations, may use them:

- **In a user unit or an explicit runtime module,** the ordinary parser rejects a role on an enum with
  `parser.unsupported-syntax`, as it does for structs.
- **In a non-prelude source parsed with prelude annotations** (the parser-mode unit tests),
  `resolveEnumeration` reports `type-checking.invalid-annotation`, mirroring `resolveStructure`.
- **In the prelude,** a role used twice, an unrecognized role, a shape mismatch, or
  `@compiler_transfer()` on either enum is `type-checking.invalid-prelude`.
- **The prelude metadata validation pass** (`typechecking.cpp`, which today skips enumerations) also
  visits enumerations; without this, none of these checks would run for an enum.

The shape is checked on the declaration, and the binding belongs to the declaration, never to an
instance's payload types:

| Role | Required shape | Binding |
|---|---|---|
| `optional` | one type parameter; exactly two cases: one with no payload, one whose sole payload is exactly the first parameter | the unary case is *present*, the empty case *absent* |
| `result` | two type parameters; exactly two cases, each with one payload: one exactly the first parameter, the other exactly the second | the case carrying the first parameter is *success*, the one carrying the second *failure* |

Payloads must be unlabelled and exactly a parameter (not `[T]` or `T?`). `indirect` cases, parameter
constraints, `deinit`, methods, subscripts and conformances are rejected. So `Result<String, String>`
is unambiguous: success and failure are the declaration's cases, whatever the arguments are.
`Optional<Void>` works as today (`.Some(())` and `.None`).

### Type identity and sugar

- `Optional<X>` and `Result<X, Y>` are ordinary generic enum instances (`TypeKind::enumeration`).
  `TypeContext::optionalType` and `resultType` remain as helpers that instantiate the role-bound
  declaration; they no longer create a separate kind.
- **Sugar binds to the role, not the name.** `T?`, `nil`, implicit wrapping, `x!`, `x?.field`,
  `value?(...)`, `?`, and `??` when implemented always use the role-bound declaration. A user
  `enum Optional<T>` in scope shadows the *name* `Optional` (0022, §21.9.1) but never `T?`; this is
  today's behavior.
- **No hidden layer.** 0022 B2's one-`?` look-through of the expected type does not apply when the
  type being constructed is the `optional` role's own declaration. `let x: Int?? = Optional.None`,
  `= .None` and `= nil` are each the outer `None`, as today, and `Optional.Some(nil)` is the inner one
  (§2.6.1: no optional layer is flattened).
- **Propagation** (§8.3) requires the operand and the enclosing result to be instances of the same
  role. For `result`, the failure types must be identical: the same concrete type or the same type
  parameter (§20.7).
- **Display.** Role-bound instances print unqualified as `T?` (parenthesized for a function type) and
  `Result<T, E>` on every path, through a role branch in `TypeContext::displayName` placed before its
  general disambiguation. Today ordinary diagnostics display types without disambiguation. They now
  disambiguate a nominal type whose name collides with a role-bound declaration, so the user type is
  the one qualified. Text changes only in programs with such a collision. When `nil` or `?` meets a
  shadowing user `Optional` or `Result`, the existing diagnostic gains help text naming that
  declaration; no new ID.

### Linear and `Never` arguments

- **Exemption by role.** A role-bound enum is exempt from `type-checking.linear-type-argument` and
  from the `Never` type-argument rejection by a check keyed on the role. It does **not** use
  `@compiler_transfer` metadata, and its parameters are not must-transfer, so a moved `Int?` is
  unaffected. `Any` stays rejected.
- **Substituted payloads everywhere.** Every check that reads a case's payload types uses them
  substituted with the instance's arguments, never the declared parameter. This covers pattern drop
  checks, exhaustiveness, binding types and lowering. Today `payloadTypesOf` returns the declared
  types for any case that has a signature, so a `.Some(_)` on a moved `JoinHandle<Int>?` would stop
  being `linear-result-dropped`. The builtin path is caught today only because it reads the
  instance's arguments by name. The implementation audits every site that relied on 0022 C1 for
  generic enums.
- **Linearity and copyability.** An instance is linear exactly when a payload type is (§4.1.1,
  unchanged), and copyable by 0022's enum rule, which matches today's builtin rule. The declaration
  has no code that could drop a `T`. A user `extension Optional<T>` or `extension Result<T, E>` stays
  rejected by the existing same-module rule (`name-resolution.invalid-extension-target`). User generics
  keep C1: `Box<JoinHandle<Int>?>` and `f<T>` with `T = JoinHandle<Int>?` are still
  `linear-type-argument`. Any future method on these types waits for the Phase 2 ownership capability.
- **`Never`.** A case whose payload is `Never` is uninhabited:
  - `?` on `Result<T, Never>` is not an early exit for the `initializing`-parameter check (§4.10),
    because its failure case is uninhabited;
  - `?` on `Result<Never, E>` or `Never?` has type `Never`, so the code after it is unreachable;
  - the check is keyed on the `result` role's failure argument, not on the kind;
  - the layout is today's: the tag plus the other case's payload;
  - §2.9's rule that `Never` is not a generic *function* type argument is unchanged.

### Behavior that must not change

These sites test for the old kinds today and would silently change if they only saw an enum:

- `containsFunctionType` descends into these types. Without that, an optional function value would
  be accepted at top level, as a dictionary key, and by `print`. The traversal becomes role-aware, so
  these stay rejected.
- `noncopyableReason` keeps its role-specific wording ("'X?' holds the noncopyable type 'Y'").

### Equality is rejected

`==` and `!=` are rejected on role-bound instances, as on user enums and structs. This is the one
intentional change to accepted programs:

- **Today's rule has no basis.** The checker accepts `==` on any copyable type that is not `Void`,
  contains no function type, mentions no type parameter, and is not a user enum or struct. It never
  asks whether the payload has equality, so `Point?` is accepted although `Point == Point` is
  rejected. The language defines no notion of equatable types.
- **It does not compile.** `Builder::comparison` handles only floating point and `joyeer.string`, then
  falls through to an integer comparison that cannot take an enum aggregate. No fixture compares
  optionals or results.
- **The replacement** is a `match`, or equality through a protocol: plan
  [0012](../plan/0012-equatable.md) brings `==` back for these types, for user types, and in generic code.
- **The diagnostic** stays `type-checking.invalid-operator-operands` (the ID today's rejection of
  `Point == Point` uses), with help text: "compare with `match`, for example
  `match x { .Some(_) => …, .None => … }`".
- **Out of scope:** builtin `Array` and `Dict` are accepted by the same rule and also do not lower.
  They are left to plan 0012.

### IR, wire format and backend

- `ir::EnumerationDefinition` gains an optional role with the bound case indices. The backend reads
  the present and absent tags from it instead of comparing case names. IR lowering's builtin
  enumeration collection and `findEnumCase(..., "Ok")` lookups (for `as?`) use the role.
- The verifier checks that:
  - a role is attached only to an enum of the required shape, and to at most one enum;
  - `arrayRemoveLast` and `dictionaryGet` return an `optional`-role instance.
- Removing `TypeKind::optional` and `result` renumbers the serialized `TypeName.kind`, and the
  enumeration record gains a field, so the backend wire version goes up from 18. Add a round-trip test.
- Compiler-created values (`as?`, `removeLast`, `get`) request their instance through the role
  helper, so the existing opcodes keep working until Phases 3 and 4 remove them.

### Instantiation report and limits

Role-bound instances are listed by `--report-instantiations` like other enum instances, displayed with
the sugar (`Int? | enum | 2 cases`, `Result<Int, String> | enum | 2 cases`). They count toward the
existing limits (10,000 instances, nesting depth 64). Reports for existing fixtures gain these lines;
that change is expected, and the affected fixtures are updated together with this change.

### Removed

`TypeKind::optional`, `TypeKind::result`, their generic-builtin registration, the synthesized case
symbols, and every comparison against a case's spelling.

## Spec changes

- §2.4, §2.5, §2.6: these are prelude enums bound by a role, not types "understood directly by the
  compiler"; state the sugar binding and the no-look-through rule.
- §2.9: `Never` as an argument of a role-bound enum.
- §4.1.1: the copyability and linearity paragraphs name role-bound enums instead of builtins.
- §5.2: `==` and `!=` are not defined on `Optional` or `Result` (until [plan 0012](../plan/0012-equatable.md)).
- §5.4, §5.4.1, §5.6.1, §7.1.2, §8.3, §8.5: refer to the `optional` and `result` roles.
- §19.4.7: core units still cannot use these types; the stated reason changes.
- §20.4.2, §20.5: `T?` is a role-bound enum instance, not a builtin container. It still satisfies no
  user protocol, since conformances are rejected on these declarations.
- §20.12.3: role-bound enums are a second exception to the linear-argument rule, keyed on the role.
- §20.12.5: amend "never layout or executable lowering". The `optional` and `result` roles bind syntax
  and propagation lowering to their declarations; they still select no layout.
- Appendix C:
  - add `| prelude_enum` to `top_decl`, marked "compiler-bundled source only", next to `prelude_struct`;
  - add `prelude_enum ::= prelude_metadata+ [ visibility ] 'enum' identifier [ generic_params ]
    '{' enum_case ( ',' enum_case )* ','? ( func_decl | subscript_decl )* '}'`.
    It uses the full enum body, so methods, `indirect` cases and labels parse, and the shape check
    rejects them with one diagnostic path;
  - the prelude parser accepts an `enumDecl` where it accepts a `structDecl` today and attaches its
    attributes; the ordinary parser keeps rejecting it.
- `docs/impl/supported-features.md`, `ir.md`, `backend.md`.

## Consequences

- The compiler no longer contains a second enum implementation, and Phases 3 and 4 can produce and
  match `Optional` through the same path as user enums.
- The role set grows by two. Unlike 0023's contracts, these roles change how syntax is lowered, not
  only how calls are validated; this is the narrow price of keeping `T?` and `?` independent of names.
- Implementation starts with an inventory of every kind use and case-name comparison, checked into
  the plan's progress log, so none is missed.
- Diagnostics: no new IDs. Prelude errors use `type-checking.invalid-prelude`; user-unit roles keep
  `parser.unsupported-syntax` and `type-checking.invalid-annotation`.

### Tests (failing first)

- **Linear:**
  - `.Some(_)` and `.Some(.Ok(_))` on a moved `JoinHandle<Int>?` and `Result<JoinHandle<Int>, E>?` are
    diagnosed; `.Some(_)` on a moved `Int?` is accepted;
  - `JoinHandle<Int>?` and a written `Optional<JoinHandle<Int>>` are both accepted;
  - `Box<JoinHandle<Int>?>` and a generic function given `JoinHandle<Int>?` are still rejected;
  - `[JoinHandle<Int>?]`, and `removeLast` on `[JoinHandle<Int>]`, matched and joined.
- **`Never`:** every form in `tests/e2e/propagation.joyeer` checked at the type-checking stage, and the
  existing uninitialized-parameter case.
- **Nesting:** `Int??` with `Optional.None`, `.None`, `nil` and `Optional.Some(nil)`, each matched to
  show which layer is absent.
- **Function types:** optional function values at top level, as dictionary keys, and with `print` stay
  rejected.
- **Equality:** `==` and `!=` on `Int?`, `Point?`, `Result<Int, String>` and against `nil` are
  `invalid-operator-operands` with the `match` help.
- **Shadowing:**
  - a user `enum Optional<T>` coexisting with `T?`;
  - a user `Result` that `?` does not propagate through;
  - user enums with their own `None` or `Ok` cases in contextual and exhaustive matches;
  - a mismatch message with both a user and a prelude `Result`.
- **Prelude:**
  - a role in a user unit and in a runtime module (`parser.unsupported-syntax`), and in a
    parser-mode non-prelude source (`invalid-annotation`);
  - each malformed shape (labelled, `indirect`, non-parameter payload, extra case, method,
    conformance, `@compiler_transfer()`);
  - the existing "each role exactly once" test extended with `optional` and `result`.
- **IR:** wire round trip; verifier rejection of a misshaped role, a duplicated role, and a
  non-role `removeLast`/`get` result.
- **Concurrency:** send a `Result<Int, String>` through a channel, and return `JoinHandle<Int>?` from
  a task.
- **Existing behavior:** all other Optional/Result fixtures pass with unchanged diagnostic IDs and
  text; `--report-instantiations` fixtures are updated for the new lines.

### Measurement

Diff `--emit-ir` across the e2e corpus at `-O0` and `-O2`; apart from symbol names it should be
identical. Measure compile time on a representative program in both profiles.

## Review

A language-designer review of the first draft (2026-11-14) asked for revision. The changes above
address its findings:

- **Linear soundness:** the role-keyed exemption and substituted payload types.
- **`Never` arguments** are now covered.
- **Silent behavior changes:** function-type checks, `==`, and nested `.None`.
- **Diagnostics:** the correct stages for prelude and annotation errors.
- **Spec:** the §20.12.5 conflict and the missing spec and grammar edits.
- **Shape:** positional binding, and the forbidden case forms.
- **IR:** the wire version.
- **Other:** instantiation report and limits, display with a same-named user type, load order, the
  propagation wording, and `as?`.

The reviewer kept Option 3 over Option 4.

A second review (2026-11-14) found both blockers resolved and gave "accept with changes". The changes
are applied:

- **Equality:** `==` is now rejected; it was misstated as a payload-equatability rule.
- **Display:** the rule is stated in terms of `displayName`.
- **Diagnostics:** the runtime-module, missing-prelude and invalid-annotation paths are corrected.
- **Load order:** the `buildPrelude` split and role-based lookup are named.
- **Prelude:** the enum validation pass, the full enum body in the grammar, `top_decl`, and the
  `Never` wording.

## Evidence

- 0023 showed that closed prelude roles can replace compiler-known library types without keying on
  names. It was validated on Windows x64 and Linux x86_64.
- Prior art: Swift's `Optional` is a library enum that the compiler finds through a known-declaration
  table rather than by user-visible lookup; Rust ties `Option` and the `?` desugaring to designated
  lang items. Both bind sugar to a designated declaration, not to a name in scope.
