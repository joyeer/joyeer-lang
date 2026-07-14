# Semantic Analysis — Control Flow, Initialization, and Lints

> **Status:** Implemented for the `--lang=v0.1` language surface and wired
> between type checking and Joyeer IR lowering.
> **Input:** `typing::TypeCheckedModel`.
> **Output:** source-spanned errors and warnings; the typed model is unchanged.

---

## 1. Boundary

The Stage 5 pass lives in
`include/joyeer/compiler/semanticanalysis.h` and
`lib/compiler/semanticanalysis.cpp`. It consumes resolved symbols and canonical
types from `TypeCheckedModel`; it does not mutate the syntax tree or route work
through legacy `TypeGen`, bytecode, or the VM.

The compiler runs the pass after successful type checking and before Joyeer IR
lowering. Errors stop the pipeline. Warnings are printed but do not prevent IR,
LLVM, or native output.

---

## 2. Implemented diagnostics

| Diagnostic | Severity | Rule |
|---|---|---|
| `semantic-analysis.missing-return` | error | A non-`Void` function must terminate or produce an assignable trailing expression on every path. |
| `semantic-analysis.unreachable-code` | warning | Statements following a terminating statement in the same block cannot execute. |
| `semantic-analysis.use-before-initialization` | error | A local must be initialized on every continuing path before it is read, projected, subscripted, or passed `inout`. |
| `semantic-analysis.unused-binding` | warning | A local or pattern binding is never read; names beginning with `_` explicitly suppress this warning. |

`let`/field immutability and `inout` access-marker checks remain type-checker
responsibilities because they depend directly on typed storage access.

---

## 3. Control-flow analysis

The pass models normal continuation versus termination for blocks and
expressions:

- `return` and expressions of type `Never` terminate a path;
- `if` terminates only when both branches terminate;
- an exhaustive `match` terminates when every arm terminates;
- `while` is conservatively assumed to fall through, even when its condition
  is a literal `true`;
- a final expression assignable to the declared result type is a valid implicit
  return.

IR lowering implements the same trailing-expression rule and transfers an
owned result to the caller before cleaning the function scope.

---

## 4. Definite initialization

Parameters start initialized. A local with an initializer becomes initialized
after that initializer is evaluated; a typed `var` without an initializer
starts uninitialized. Direct assignment initializes it after the right-hand
side succeeds.

At control-flow joins, the initialized set is the intersection of all
continuing paths. Terminated paths do not constrain the join. A loop body may
execute zero times, so assignments made only in the body do not initialize a
value after the loop.

Assignment to a plain local does not read its old value. Assignment through a
member or subscript does read/project the base storage. The right-hand side and
`inout` arguments are ordinary reads and therefore require prior
initialization.

Joyeer IR represents deferred nontrivial storage with `alloc_stack` followed by
`zero_init`. The zero state is an implementation detail that makes cleanup safe;
semantic analysis still rejects every source-level read before initialization.

---

## 5. Unused-binding analysis

The lint tracks function-local `let`/`var` declarations and payload pattern
bindings. Initializing or assigning a binding does not count as reading it.
Reads, aggregate projections, subscripts, and `inout` uses do count. Parameters
and top-level declarations are not currently linted.

Diagnostics are emitted in declaration order so test output and editor output
remain deterministic.

---

## 6. Remaining work

This pass does not yet implement the complete ownership language from the
specification. In particular, source-level `borrowing`, `consuming`,
`initializing`, and `consume` flow states remain future work. Loop analysis has
no fixed-point refinement because v0.1 has no `break` or `continue`, and the
current conservative rule is sufficient for safety.

Diagnostic presentation also remains basic: stable diagnostic IDs exist, but
file names, source excerpts, one-based locations, and fix-it hints are still
pending.

---

## 7. Validation

```pwsh
ctest --test-dir build -L semantic-analysis --output-on-failure
```

The suite covers all-paths-return, implicit returns, `Never`, unreachable
warnings, branch/loop initialization joins, aggregate and `inout` reads,
unused locals/patterns, CLI error/warning behavior, and native execution of
deferred initialization.
