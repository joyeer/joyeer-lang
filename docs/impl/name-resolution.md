# Name Resolution — Parser MVP Semantic Model

> **Status:** Implemented for the type-independent portion of the current
> Parser MVP AST.
> **Input:** `syntax::SourceFileSyntax`.
> **Output:** `semantic::SemanticModel` plus stable, spanned diagnostics.

---

## 1. Boundary

The resolver answers which declaration a syntactic name denotes without
mutating the syntax AST.

The semantic model assigns compilation-local identities:

| Identity | Meaning |
|---|---|
| `NodeId` | One syntax-node occurrence |
| `SymbolId` | One declaration or compiler-provided symbol |
| `ScopeId` | One lexical or member scope |

Its principal relation is:

```text
name-use NodeId -> declaration SymbolId
```

Additional relations record declaration symbols, containing/introduced
scopes, qualified-name symbols, resolved call targets, and references that
must wait for type information. The public model is in
`include/joyeer/compiler/semantic.h`; the resolver API is in
`include/joyeer/compiler/nameresolution.h`.

---

## 2. Resolution order

Resolution is deliberately split into three deterministic passes:

1. **Index and collect declarations.** Assign every syntax node a stable
   `NodeId`; collect all top-level functions, bindings, structs, enums,
   fields, and cases.
2. **Resolve signatures.** Resolve all parameter, return, field, and enum
   payload type names before inspecting any function body. This permits
   recursive functions and forward type/member references.
3. **Resolve bodies.** Walk initializers and expressions, create local/block
   and match-arm scopes, and bind each use to the nearest declaration.

Top-level declarations are visible throughout the file. Local bindings become
visible after their initializer. A declaration may shadow one in an outer
scope, but a duplicate in the same scope is diagnosed.

---

## 3. Scopes and symbols

The model contains these scope kinds:

- compiler prelude;
- source file;
- function;
- lexical block;
- struct/enum member table;
- individual match arm.

Value and type lookup are separate inside each scope. Expression lookup checks
the value namespace first and then permits a type as a constructor or qualified
member base. Type syntax checks only the type namespace. A same-scope
cross-namespace collision is rejected because `Foo(...)` would otherwise be
ambiguous.

The current prelude declares:

- types: `Void`, `Never`, `Int`, `Bool`, `String`, `UInt8`, `Array`, `Dict`,
  `Optional`, `Result`, and `IOError`;
- `String.count`, `Array.count`, and `Dict.count`;
- `String.utf8()` and mutating `Array.append(element:)`;
- `Optional.Some` / `Optional.None` and `Result.Ok` / `Result.Err`;
- the builtin `IOError` cases;
- `print(value:)`, `readFile(path:)`, `byteToInt(value:)`, and
  `byteToString(value:)`.

An internal `Any` marker also exists in the semantic prelude; the source
spelling is reserved and is not a supported dynamic-value type.

Struct declarations receive a separate synthesized memberwise-initializer
symbol. Its parameters retain field labels, declaration order, field types,
and whether a field initializer makes the argument optional.

---

## 4. Resolved now

The pass resolves and records:

- value, function, parameter, and local binding references;
- built-in and user-defined nominal type references;
- fields when the base type follows from a declaration signature;
- qualified enum cases such as `JsonValue.Bool`;
- ordinary function calls and synthesized struct initializer calls;
- argument labels/order for known functions, initializers, and enum cases;
- qualified enum patterns and their payload bindings;
- one independent scope for every match arm.

Error nodes produced by parser recovery are skipped without cascaded semantic
diagnostics.

---

## 5. Explicitly deferred references

Some syntax cannot have one correct target before type checking:

| Deferred kind | Why |
|---|---|
| member with unknown base type | the receiver expression still needs a type |
| contextual `.Case` expression | its enum comes from the expected type |
| contextual `.Case` pattern | its enum comes from the match scrutinee type |
| call through unresolved member | the member target depends on receiver type |

These are stored as `DeferredReference` entries with node, name, span, and
reason. They are not errors and are not treated as successfully resolved. The
type checker must consume every deferred entry or issue a diagnostic.

Type compatibility, access-marker/exclusivity checks, consuming flow,
mutability, exhaustiveness, layout, and lowering remain outside this pass.

---

## 6. Diagnostics and validation

Stable diagnostics cover duplicate declarations, undefined values/types,
unknown members/cases, non-callable targets, payload-clause misuse, argument
count, labels, and order. CLI rendering preserves the syntax-node source span.

Direct validation:

```pwsh
ctest --test-dir build -L name-resolution --output-on-failure
```

The test target covers forward references, nested shadowing, type/member
resolution, synthesized initializers, enum cases, labels, match-arm bindings,
deferred contextual cases, diagnostics, and the JSON-parser fixture. A CLI
negative test verifies that the default compiler reports resolver failures.

## 7. Known resolution gaps

- Intrinsic literal/member resolution can use lexical lookup of a builtin
  type's spelling. A user `struct String` can therefore make `"abc".count`
  incorrectly resolve against that struct. Literal and intrinsic-syntax
  identities must remain tied to the compiler's builtin symbols.
- Parenthesizing a direct callee, as in `(identity)(value: 7)`, does not
  currently preserve its recorded target and can produce a later
  `type-checking.not-callable` error. Grouping should remain transparent;
  this does not require general first-class-function support.
- Repeated external parameter labels and receiver-dependent field defaults
  need explicit end-to-end rules. Current declaration acceptance alone does
  not establish that those declarations can be called or lowered.