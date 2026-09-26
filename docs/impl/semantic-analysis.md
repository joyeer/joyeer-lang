# Semantic Analysis — Control Flow, Initialization, and Lints

> **Status:** Implemented for the current language surface and wired between
> type checking and Joyeer IR lowering.
> **Input:** `typing::TypeCheckedModel`.
> **Output:** source-spanned errors and warnings; the typed model is unchanged.

---

## 1. Boundary

The Stage 5 pass lives in
`include/joyeer/compiler/semanticanalysis.h` and
`lib/compiler/semanticanalysis.cpp`. It consumes resolved symbols and canonical
types from `TypeCheckedModel` and does not mutate the syntax tree.

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
| `semantic-analysis.use-after-consume` | error | A binding transferred with `consume` cannot be read or consumed again until direct assignment reinitializes it. |
| `semantic-analysis.initializing-initialized-storage` | error | An `initializing` call requires uninitialized or consumed destination storage. |
| `semantic-analysis.initializing-parameter-not-initialized` | error | Every normal return path must initialize each `initializing` parameter. |
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

Parameters other than `initializing` parameters start initialized. A local
with an initializer becomes initialized after that initializer is evaluated;
a typed `var` without an initializer starts uninitialized. Direct assignment
initializes it after the right-hand side succeeds.

At control-flow joins, the initialized set is the intersection of all
continuing paths. Terminated paths do not constrain the join. A loop body may
execute zero times, so assignments made only in the body do not initialize a
value after the loop.

`consume` moves an owning local or consuming parameter into the callee and
marks its source storage consumed. Branch joins retain consumed state if any
continuing path consumed the binding. The current loop analysis compares body
and condition states at a fixed point, retaining the zero-iteration path and
every reachable back edge. The condition's entry requirements are therefore
checked again for subsequent iterations. Direct assignment reinitializes
consumed storage, including before a later consume in each loop iteration.

Stored-field and collection-subscript consumption is path-sensitive. Distinct
struct fields retain independent initialization state; reading the whole
aggregate while any field is moved out is rejected. Collection indices use one
conservative may-overlap relation for reads, separately from proven identity
for restoration. The same literal or unchanged index binding can identify a
restored slot; writes, mutable calls, consumption, and scope exit invalidate
binding identity. A different or unknown index does not restore a consumed
element. Index expressions are evaluated before applying storage effects.

An `initializing` parameter starts uninitialized and may only be read after a
direct write or a forwarded initializing call. Every normal return/fallthrough
path must establish initialization. At the caller, `&x` is accepted only while
`x` is definitely uninitialized or consumed on every reaching path. The
analysis tracks possible initialization separately from definite
initialization. It checks destination eligibility during argument evaluation
but commits initialization only after all arguments and a normally returning
call; a later argument's early return cannot fulfill an output obligation.

Assignment to a plain local does not read its old value. Assignment through a
member or subscript does read/project the base storage. The right-hand side and
`inout` arguments are ordinary reads and therefore require prior
initialization.

Joyeer IR represents deferred nontrivial storage with `alloc_stack` followed by
`zero_init`. The zero state is an implementation detail that makes cleanup safe;
the source-level contract still requires rejection of reads before
initialization.

Logical `&&` joins the skipped-right and evaluated-right paths. Effects that
occur only on its right cannot establish unconditional initialization or
termination, but possible consumption and initialization remain visible.

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
specification. Call-site and argument-evaluation access-path exclusivity is
enforced by the type checker. Future method/subscript `yield` syntax will need
additional lifetime analysis, but the current language surface has no
first-class escaping projection. Explicit `borrowing` uses the existing
immutable projection behavior. The absence of `break` and `continue` does not
make loop checking complete: conditions are re-evaluated and each back edge
must satisfy their entry requirements.

Diagnostics use the shared [structured source renderer](diagnostics.md), with
stable IDs, file names, one-based locations, source excerpts, and caret ranges.
Ownership-flow diagnostics already carry structured help. Pass-specific
fix-it edits and secondary source notes remain future work.

### Precision limits

Index identity does not prove arbitrary arithmetic expressions equivalent.
When restoration cannot be proved, the consumed state remains. Constant
conditions do not remove paths from the conservative initialization join.
The analysis is not interprocedural and does not model future escaping
references or user-defined accessors.

---

## 7. Validation

```pwsh
ctest --test-dir build -L semantic-analysis --output-on-failure
```

The suite covers all-paths-return, implicit returns, `Never`, unreachable
warnings, branch/loop initialization joins, aggregate and `inout` reads,
unused locals/patterns, CLI error/warning behavior, and native execution of
deferred initialization.
