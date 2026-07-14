# Type Checking — Resolved Parser MVP AST

> **Status:** Implemented for the v0.1 JSON-parser frontend and wired into
> `--lang=v0.1`.
> **Input:** `semantic::SemanticModel`.
> **Output:** `typing::TypeCheckedModel` plus stable, spanned diagnostics.

---

## 1. Boundary

The type checker consumes the syntax tree and all bindings produced by name
resolution. It is independent of the legacy runtime `Type`, bytecode, and VM
classes. The new frontend stops after producing a typed model; native IR
lowering is the next pipeline stage.

The public API lives in `include/joyeer/compiler/typechecking.h`; the
implementation is in `lib/compiler/typechecking.cpp`. A successful result is
owned by `SourceFile::typeCheckedModel`.

---

## 2. Compile-time type model

Every concrete type receives a compilation-local canonical `TypeId` from
`TypeContext`. Equal concrete types have the same ID. `TypeRecord` stores the
kind, declaring type symbol, and concrete type arguments.

The model covers:

- `Void`, `Never`, `Any`, `Int`, `Bool`, `String`, and `UInt8`;
- user `struct` and `enum` nominal types;
- `[T]`, `[K: V]`, `T?`, and `Result<T, E>`;
- a dedicated error type that suppresses cascading diagnostics.

Built-in generic containers are compiler-known type constructors with fixed
arity. They do not implement user-defined generics. An application such as
`Result<[Int], Error>` retains both nested concrete arguments; it is not
collapsed to the name resolver's coarse `Result` symbol.

`TypeCheckedModel` records:

- exact types for syntax nodes and symbols;
- exact parameter/result types for functions, synthesized struct
  initializers, and enum cases;
- type-directed reference completions for deferred members and contextual
  enum cases/patterns;
- call targets completed after receiver type inference.

The underlying `SemanticModel` remains immutable. Type-directed completions
are overlay relations queried through `TypeCheckedModel::referencedSymbol`
and `TypeCheckedModel::callTarget`.

---

## 3. Checking order

The checker uses deterministic passes:

1. Register concrete prelude types and built-in signatures.
2. Resolve all top-level declaration signatures, struct fields, enum payloads,
   and synthesized initializers.
3. Check top-level initializers and function bodies in source order.
4. Complete contextual/deferred references while expression and pattern types
   become available.
5. Verify that every `DeferredReference` was visited and either completed or
   diagnosed.

This ordering preserves forward function/type references while allowing local
bindings to infer from previously checked initializers.

---

## 4. Implemented rules

The v0.1 checker currently validates:

- explicit and inferred binding types;
- scalar, byte, array, dictionary, and `nil` literals;
- optional value promotion and `Never` as the bottom type;
- MVP arithmetic, string concatenation, comparison, and `&&` operators;
- `if`/`while` conditions, `if` branch unification, and return values;
- exact function, struct-initializer, enum-case, and `print(value:)` calls;
- inferred and declared member access;
- `String[Int] -> UInt8`, `Array<T>[Int] -> T`, and
  `Dict<K,V>[K] -> V` typing;
- contextual user enum, `Optional`, and `Result` construction;
- enum pattern payload binding and match-arm result unification;
- exhaustive user enum, `Optional`, `Result`, and `Bool` matches; other
  domains require a catch-all arm;
- immutable bindings/fields and explicit `inout` access markers for assignment
  and calls.

Ordinary call labels, ordering, required/default argument presence, and
lexical declaration binding remain name-resolution responsibilities. The type
checker consumes those validated targets and checks concrete argument types.

---

## 5. Deferred-reference contract

Name resolution emits one of four deferred kinds when a unique target needs
an expression type:

- member needing a base type;
- contextual enum-case expression;
- contextual enum-case pattern;
- call whose callee is a deferred member.

The checker marks each entry as handled when visiting its node. Successful
completion is stored in the typed overlay. A source-level failure receives a
specific diagnostic such as missing context, unknown case/member, or
non-callable value. Any entry not visited is reported as
`type-checking.unresolved-reference`, making an incomplete checker walk
observable rather than silent.

---

## 6. Diagnostics and validation

Diagnostics have stable `type-checking.*` IDs and retain syntax spans. The CLI
renders them only after lexing, parsing, and name resolution succeed, avoiding
cross-stage cascades.

Direct validation:

```pwsh
ctest --test-dir build -L type-checking --output-on-failure
```

The suite covers canonical type construction, signatures, inference,
operators, control flow, calls, members, subscripts, contextual cases,
patterns, exhaustiveness, access conventions, deferred-reference closure, the
JSON-parser MVP source, and CLI rejection of a type mismatch.

---

## 7. Deliberately not implemented here

Type checking does not provide data layout, ownership lowering, Joyeer IR,
LLVM IR, object emission, or linking. It also does not implement user-defined
generics, function values, all-paths-return analysis, unreachable/unused
diagnostics, or enum representation. Control-flow and lint checks now live in
the dedicated [semantic-analysis pass](semantic-analysis.md); representation
and ownership decisions live in later lowering stages. None of these may be
routed through the legacy VM to reuse its runtime descriptors.
