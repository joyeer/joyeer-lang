# Joyeer Spec Implementation Plan

> Companion to [docs/spec.md](../spec.md). This document is the roadmap for
> landing the spec in the compiler. It is **orthogonal** to
> [docs/plan/v0.1.md](v0.1.md) (which tracks the LLVM/native-codegen
> migration). The two tracks can run in parallel.

---

## TL;DR

```
┌───────────────────────────────────────────────────────────────────┐
│  Spec scope  =  A (memory model)                                  │
│                + B (struct / enum / built-in containers / Result)  │
│                + C-syntax-only (property/spec annotations)         │
└───────────────────────────────────────────────────────────────────┘

Implementation tracks (each track has phases that align to spec sections):

  T1 Lexer/Parser  ─┐
  T2 AST/Name-res  ─┤  Roll out per phase together
  T3 Type checker  ─┤
  T4 IR/Bytecode   ─┘
  T5 Tests + legacy migration  (cross-cutting, runs alongside every phase)
  T6 Diagnostics                (cross-cutting; improved per phase)

Phases:

  Phase 0  Scaffolding & legacy compat carve-out          ~ 1 week
  Phase L  Minimal C++ lexer for the JSON-parser profile  ✅ complete
  Phase P  Parser foundation + JSON-parser grammar        ✅ complete
  Phase A  Memory model: inout / consuming / initializing / subscript   ~ 4 weeks
  Phase B  Data model: struct / enum / built-in containers / Result  ~ 6 weeks
  Phase C  Syntax-only property/spec annotations          ~ 1 week
  Phase D  Legacy removal: class out, `print(message:)` out, etc.
                                                            ~ 2 weeks
```

Target: from end of Phase 0 to end of Phase C, **~14 weeks of focused
work**; the full Phase 0 through Phase D sequence is **~17 weeks** for one
engineer or one well-prompted AI agent loop.

---

## Conventions used in this plan

- **Each phase has a "Definition of Done" (DoD).** Phase is not closed
  until the DoD example compiles and the listed golden tests pass.
- **Each phase has an exit criterion test file** in `tests/spec/PhaseX/`.
- File paths refer to current C++ implementation. The same plan applies
  if the compiler is rewritten in another language; only the file names
  change.

---

## Phase 0 — Scaffolding & legacy compat carve-out

### Goal
Set up infrastructure to evolve the language without breaking existing
tests; introduce a deprecation lane.

### Work items

1. **Add `--lang=v0.1-legacy` and `--lang=v0.1` compiler flags.**
   - Default: `v0.1-legacy` (current behavior).
   - `--lang=v0.1` enables the new spec; rejects deprecated syntax that
     is otherwise tolerated.
   - File: `lib/main/driver.cpp`.

2. **Move existing tests under `tests/legacy/` mirror.**
   - Keep them passing under `--lang=v0.1-legacy`.
   - New spec features get new tests under `tests/spec/`.
   - Files: `tests/CMakeLists.txt`, `tests/testRunner.py`.

3. **Add deprecation diagnostic category** to
   `include/joyeer/diagnostic/diagnostic.h`.
   - One enum entry per legacy item from spec §14.
   - All emit as **warnings** in `v0.1-legacy`, **errors** in `v0.1`.

4. **Add `tests/spec/Phase0_smoke/` with a trivial `let x = 1` followed by
  `print(value: x)`
   example** under the new flag.

### DoD

```bash
# All existing tests pass under default (legacy) flag:
ctest --test-dir ./build --output-on-failure -R 'legacy_'

# New spec mode runs a hello-world:
./build/bin/joyeer --lang=v0.1 tests/spec/Phase0_smoke/001.joyeer
```

---

## Phase L — JSON-Parser Lexer MVP ✅

### Goal

Replace the legacy tokenization contract with the deliberately small C++ lexer
defined in [../impl/lexer.md](../impl/lexer.md). This phase recognizes exactly
the terminals needed by the first Joyeer JSON parser, including payload enums,
minimal `match`, byte literals, `=>`, `?`, and `&`. It does not implement
unrelated syntax sugar.

### Work items

1. [x] Add explicit `TokenKind`, absolute `SourceSpan`, `Invalid`, and EOF.
2. [x] Implement trivia, ASCII identifiers, MVP/deferred keyword classification,
   decimal integers, strings, and strict byte literals.
3. [x] Implement the longest-match operator table for the MVP.
4. [x] Diagnose every invalid or intentionally deferred source form.
5. [x] Add direct in-memory lexer unit tests and deterministic malformed-byte
  property coverage.
6. [x] Adapt the existing parser to the new token contract without adding the
   later parser productions yet.

### DoD

- [x] Token classes and diagnostics in the lexer test matrix are covered by 24
  direct tests.
- [x] Spans and LF/CR/CRLF line starts are correct.
- [x] Re-tokenization is deterministic and does not duplicate tokens.
- [x] The focused JSON-parser acceptance source reaches the parser boundary
  with no lexical error.
- [x] Legacy behavior is isolated behind legacy mode rather than leaking into the
  MVP token contract.

Validation uses `ctest --test-dir build --output-on-failure -L lexer`. The
forward-looking source under `tests/target/` is a design fixture, not an
executable golden test. Byte-literal, `enum`, and `match` consumers remain work
for later parser/type/IR phases.

---

## Phase P — JSON-Parser Parser MVP ✅

### Goal

Replace the legacy parser contract with the syntax-only, spanned AST and
focused grammar defined in [../impl/parser.md](../impl/parser.md). Phase P
crosses the later feature phases only at the **syntax** boundary: it parses
`inout`/`&`, payload enums, and minimal match patterns, while Phase A and Phase
B still own their semantic checking, representation, and lowering.

### Work items

1. [x] Add a bounded token cursor, syntax-only AST, node spans, stable parser
   diagnostic IDs, and error nodes.
2. [x] Parse MVP types and declarations: bindings, functions/labels/`inout`,
   field-only structs, payload-only enums, and built-in generic type uses.
3. [x] Replace flat expressions and `TypeGen` precedence repair with Pratt parsing.
4. [x] Parse byte literals, postfix chains, labeled/enum-payload arguments, `&`,
   `if`, `while`, `return`, contextual cases, and minimal `match` patterns.
5. [x] Add synchronization-based recovery and direct AST/diagnostic snapshots.
6. [x] Keep parser tests out of name resolution, type checking, IR, VM, and
   runtime; retain the old parser only for the legacy lane.

### DoD

- [x] Core positive/negative categories have direct cases; deterministic token
  deletion exercises malformed-stream progress and bounds.
- [x] Precedence and associativity are encoded in the parser AST itself.
- [x] The focused JSON-parser acceptance source reaches EOF with no lexical or
  parser diagnostic.
- [x] Malformed token streams terminate, remain in bounds, and recover to later
  independent declarations where possible.
- [x] `ctest --test-dir build --output-on-failure -L parser` passes without
  launching the old VM/runtime.

Successful parsing does not close Phase A or B: ownership checking, enum
layout, name resolution, match exhaustiveness, and lowering remain there.

---

## Phase A — Memory Model

> The chapter that makes Joyeer ≠ Swift-clone. Implements spec §4.

### A.1 Lexer additions

| Token | Notes |
|-------|-------|
| `inout` | Already tokenized and parsed syntactically by Phase P |
| `borrowing` | Keyword (default access effect; may be written explicitly) |
| `consuming` | Keyword |
| `initializing` | Keyword (when in subscript/parameter context) |
| `mutating` | Keyword (method receiver effect) |
| `consume` | Keyword (call-site ownership-transfer marker) |
| `subscript` | Keyword |
| `yield` | Keyword |
| `&` as prefix | Already tokenized and stored as an access marker by Phase P |

Files: `lib/compiler/lexparser.cpp`,
`include/joyeer/compiler/lexparser.h`.

### A.2 AST additions

Add to `include/joyeer/compiler/node.h` and matching visitors:

| Node | Purpose |
|------|---------|
| `AccessEffect` enum (`borrowing`, `inout`, `consuming`, `initializing`) | Field on `ParameterDecl` |
| `MethodEffect` enum (`borrowing`, `mutating`, `consuming`) | Field on `FuncDecl` for the `self` receiver |
| `InoutArgExpr` | Wraps `&expr` at call sites |
| `ConsumeArgExpr` | Wraps `consume expr` at call sites |
| `SubscriptDecl` | New top-level / extension member |
| `AccessorDecl` (one per `borrowing`/`inout`/`initializing`/`consuming` block in a subscript) | |
| `YieldStmt` | With optional `&` for inout yields |
| `ConsumeBindingMark` (data-flow only) | Marks consumed bindings in the type-checked AST |

Update `node+visitor.h` accordingly. **Every visitor in the codebase
must be updated** — leaving any out silently miscompiles (per
[AGENTS.md](../../AGENTS.md) pitfall list).

### A.3 Parser additions

- Extend Phase P's `label [name] : [inout] type` parameter syntax to all
  access effects and method receiver effects.
- Extend Phase P's `&` access marker with the `consume` call-site/receiver
  marker.
- Top-level + member `subscript` declarations with
  `{ borrowing { yield ... } inout { yield &... } ... }` accessor blocks.
- Top-level `yield` only valid inside accessor blocks (parse error otherwise).

Files: `lib/compiler/syntaxparser.cpp`.

### A.4 Name resolution

- New scope kind: **accessor scope** (between subscript and its body),
  binds `self` and subscript parameters.
- `yield`'s lookup: must resolve to a storage path of an accessible value.

Files: `lib/compiler/symtable.cpp`.

### A.5 Type system additions

- `Type` already carries field types; no change in `typegen.cpp`.
- `typebinding.cpp`: enforce that
  - `inout`/`consuming`/`initializing` arguments at the call site match the corresponding parameter effect.
  - `&` at call site iff parameter is `inout` or `initializing`; `consume` at call site iff parameter is `consuming`.
  - `yield` argument's type matches the subscript's declared element type.

### A.6 The Exclusivity Checker (new module)

This is the headline implementation effort.

- New file: `lib/compiler/exclusivity.cpp` + header.
- Pass placement: between `typebinding` and `IRGen`.
- Algorithm:
  1. For each statement, collect the set of *access path projections* it
     establishes (read, mutate, consume, init).
  2. Detect overlaps using §4.4 rules:
     - `borrowing` × `borrowing` over same path → OK
     - any `borrowing` × `inout`/`consuming`/`initializing` over overlapping paths → ERROR
     - two `inout` over overlapping paths → ERROR
  3. Path overlap: two paths overlap unless they differ at a `.field` step.
     Index steps are conservatively assumed to overlap.
  4. Report errors via `Diagnostics`.
- Use `CompileContext` plumbing per existing pipeline patterns
  ([context.h](../../include/joyeer/compiler/context.h)).

### A.7 IR / Bytecode codegen

- `inout`/`initializing` params: pass by pointer; no aliasing checks at runtime (already guaranteed statically).
- `consuming` params: move semantics; emit `MOVE` instead of `COPY`.
- Subscript accessors compile as **two-phase coroutines**:
  - The accessor runs up to `yield`, the caller executes, then control returns to the accessor for cleanup.
  - For the VM, lower as a function pair: prologue (up to yield) + epilogue (after yield), called around the use site.
  - For future LLVM backend, this maps cleanly to coroutine intrinsics or to inline expansion when the accessor is small.
- Add bytecode opcodes if needed; document in [docs/impl/bytecode.md](../impl/bytecode.md).

Files: `lib/compiler/IRGen.cpp`, `lib/vm/interpreter.cpp`,
`include/joyeer/runtime/bytecode.h`.

### A.8 Tests (DoD)

`tests/spec/PhaseA/`:

| File | Tests |
|------|-------|
| `01_inout_basic.joyeer` | `increment(n: &n)`; expect `6` printed |
| `02_swap.joyeer` | Concrete Int swap; round-trip |
| `03_exclusivity_self_alias.joyeer` (errors/) | `add(&n, n)` must produce diagnostic |
| `04_subscript_let_inout.joyeer` | `&a[0] += 10`; verify result |
| `05_subscript_disjoint_paths.joyeer` | `&p.x += p.y`; verify result |
| `06_subscript_overlap_index.joyeer` (errors/) | `&a[0] += a[1]` must error |
| `07_consume.joyeer` (errors/) | use-after-consume must error |
| `08_consume_reinit.joyeer` | consume then re-assign, then use; passes |
| `09_initializing_emplace.joyeer` | `initializing` into uninitialized storage; verify content |
| `10_subscript_yield_inout_in_loop.joyeer` | `for &x in a { &x *= 2 }` |

### A.9 Risks & mitigations

| Risk | Mitigation |
|------|-----------|
| Exclusivity false positives blocking valid code | Provide clear errors + standard-library escape hatches (e.g., `swap_at`) before locking the rule |
| Subscript codegen complexity | First version: only allow `borrowing`/`inout` accessors; defer `initializing`/`consuming` accessors to A-tail |
| Breaking existing tests | All exclusivity checking gated on `--lang=v0.1`; legacy mode unaffected |

---

## Phase B — Data Model

> Implements spec §2 (composite and built-in generic type uses), §3.3–§3.6 (struct, enum,
> extension, subscript already in A), §3.7 (init/deinit), §7 (patterns
> for match), §5.9 (match expression), §2.6 (built-in containers), §8 (Result).
> User-defined generic declarations and monomorphization remain deferred.

### B.1 Lexer additions

| Token | Notes |
|-------|-------|
| `struct` | Keyword (already reserved, currently unused) |
| `enum`   | Already lexed; ensure parser path |
| `match`  | Keyword |
| `indirect` | Contextual keyword inside `enum` body |
| `init`, `deinit` | Keywords |
| `where` | Match-guard keyword |
| `Self`   | Type-name keyword |
| `=>`     | Match-arm separator |
| `..<`, `...` | Range operators |

Patterns are not lexer tokens as a category. The lexer recognizes the
keywords/punctuation above plus ordinary identifiers, literals, `_`, `.`, and
parentheses; `PatternNode` structure is produced by the parser and checked by
the type checker. `as` remains contextual for import aliases, while type-test
`is` / `as` are deferred (§15).

### B.2 AST additions

| Node | Purpose |
|------|---------|
| `StructDecl` | Stored fields, init, deinit, methods, subscripts, invariants |
| `EnumDecl`, `EnumCaseDecl` | Variants with associated types; `indirect` flag |
| `InitDecl`, `DeinitDecl` | Special methods |
| `GenericArgList` | Built-in generic type uses only; no user-declared type parameters |
| `MatchExpr`, `MatchArm`, `PatternNode` | All pattern variants from §7 |
| `IfExpr` | Allow `if` in expression position (per §5.10) |
| `IndirectMarker` | On `EnumCaseDecl` |

### B.3 Parser additions

- Extend Phase P's field/payload-only struct and enum syntax with the remaining
  members required by §3.3–§3.7.
- Extend Phase P's minimal `match` syntax only when guards, alternatives, or
  additional patterns acquire a concrete v0.1 consumer.
- `if` as expression — adjust expression vs statement disambiguation.
- Range literal `0..<n`, `0...n`.
- Function-type syntax `(Int): Int`.

### B.4 Name resolution

- Module-level type table populated with struct/enum names.
- Member lookup through extensions: extension methods join the type's
  member table.
- `Self` resolves to the enclosing type.

### B.5 Type system additions

- Validate built-in generic container arguments; reject user-defined generic
  declarations and generic bases outside the compiler-provided set.
- Pattern type-checking: enum case constructor must match scrutinee
  type; bindings carry the inferred types.
- Exhaustiveness checker for `match` over `enum` and `Bool`.
  - Use a simple "missing constructors" algorithm (Maranget-style is
    overkill for v0.1).

### B.6 Standard library minimum

Add to a new `lib/stdlib/` directory (or inline as built-ins for v0.1):

```joyeer
public enum Optional<T> { None, Some(T) }
public enum Result<T, E> { Ok(T), Err(E) }

public struct Array<T> {
  var storage: <opaque>
  public func count(): Int { ... }
  public init() { ... }
  public subscript(i: Int): T { borrowing { ... } inout { ... } }
  public mutating func append(element: consuming T) { ... }
}

public struct Dict<K, V> { ... }
public struct String { ... }
```

In v0.1 these are still **runtime-provided** (existing `runtime/` in the
VM); the declarations above are interfaces/façades understood as compiler
built-ins, not evidence that user-defined generic declarations are accepted.

### B.7 Memory model integration with structs/enums

- Field-level disjointness analysis (already in A.6) extends naturally
  to struct fields.
- Enum match bindings: each arm's bindings are `let` projections of the
  matched payload by default; `inout` matching reserved for B-tail.
- `deinit` ordering: reverse declaration order for stored fields.

### B.8 Tests (DoD)

`tests/spec/PhaseB/`:

| File | Tests |
|------|-------|
| `01_struct_basic.joyeer` | Point with init/method/print |
| `02_struct_memberwise_init.joyeer` | Synthesized init |
| `03_struct_deinit_order.joyeer` | Verify deterministic deinit order via prints |
| `04_enum_payload.joyeer` | Number(42), match arms |
| `05_enum_indirect_list.joyeer` | `IntList` with `.Cons` indirect |
| `06_enum_exhaustive.joyeer` (errors/) | non-exhaustive match must error |
| `07_match_where_guard.joyeer` | Guard on `.Number(n) where n > 0` |
| `08_result_builtin.joyeer` | Construct and match compiler-provided `Result<Int, E>` |
| `09_optional_builtin.joyeer` | Construct and match `Optional<Int>` / `Int?` |
| `10_if_expression.joyeer` | `let sign = if x > 0 { ... } else { ... }` |
| `11_indirect_list_traverse.joyeer` | Recursive count on `IntList` without user generics |
| `12_subscript_on_enum.joyeer` | Subscript declared in extension on enum |
| `13_quicksort.joyeer` | The canonical demo from spec §16.1; full sort + verify output |
| `14_json_minimal.joyeer` | Parse `{"n":42}`; assert AST shape |

### B.9 Risks

| Risk | Mitigation |
|------|-----------|
| Built-in container special cases leak into general syntax | Keep generic declaration syntax rejected and centralize the allowed built-in bases |
| Enum exhaustiveness false positives on `..` ranges | Restrict to enum + Bool in v0.1; range exhaustiveness in v0.2 |
| Recursive enum (indirect) interactions with deinit | Cycle detection via use-site rules; document non-collected cycles as user responsibility |

---

## Phase C — Syntax-only Property/Spec Annotations

> Implements spec §11 (`@spec` and `@property`) at the parse-and-store level.
> Runtime checks from §9 are ordinary prelude calls and need no declaration-
> level contract grammar. The general effect system in §10 is removed.

### C.1 Lexer additions

| Token |
|-------|
| `@` (attribute marker) |

The `forall` form inside `@property` is an annotation-local DSL recognized by
the annotation parser, not a general lexer keyword (§11.2).

### C.2 AST additions

| Node | Purpose |
|------|---------|
| `AttributeNode` (`@spec`, `@property`, etc.) | Attached to any decl |
| `PropertyQuantifier` | Annotation-local `forall` form |

### C.3 Parser additions

- Attributes (`@spec("...")`, `@property expr`) precede any declaration.

### C.4 Type checking additions

- `@property` bodies must type-check as `Bool` when they use ordinary
  expressions. Annotation-local quantified forms are parsed and stored.

### C.5 No codegen impact

- v0.1: `@spec` / `@property` metadata is not lowered to runtime IR.

### C.6 Tests (DoD)

`tests/spec/PhaseC/`:

| File | Tests |
|------|-------|
| `01_spec_attribute_parse.joyeer` | `@spec` parses and is preserved on the declaration |
| `02_property_attribute_parse.joyeer` | `@property` parses and is preserved on the declaration |
| `03_property_typecheck_bad.joyeer` (errors/) | string-valued property → type error |
| `04_property_forall_parse.joyeer` | annotation-local `forall` form parses and is preserved |

### C.7 Future work (not in scope of this plan)

- v0.3: property-based test runner.
- v1.0: SMT-backed static contract verification (target: Z3 or CVC5
  bridge in IR pass).

---

## Phase D — Legacy Removal

> Implements spec §14. Run after all of A, B, C are merged.

### D.1 Work items

1. Remove `class` parser path. Move existing class tests to a
   `tests/legacy-archive/` for reference; rewrite into struct form
   where they're still relevant.
2. Drop `--lang=v0.1-legacy` flag. Single language mode only.
3. Remove deprecation warnings; the rejected forms now produce
   permanent parse errors with rewrite suggestions.
4. Rewrite remaining `tests/basis/*` and `tests/leetcode/*` to spec
   syntax. Verify expected output unchanged.
5. Update [AGENTS.md](../../AGENTS.md):
   - Drop "legacy `class` exists" note.
   - Update Joyeer conventions to match spec.
   - Update test instructions for the new `tests/spec/` layout.

### D.2 DoD

- All tests under `tests/spec/` and the migrated `tests/basis/`,
  `tests/leetcode/` pass.
- No reference to `class`, `print(message:)`, or legacy positional/unlabeled
  call syntax in production source.
- Spec §14 (Deprecated) becomes an empty section or is removed.

---

## Cross-cutting tracks

### T5 — Tests & legacy migration

Run continuously. Migration strategy:

```
For every test under tests/basis/ and tests/leetcode/:
  1. Copy to tests/spec/migrated/ with the same filename.
  2. Rewrite to spec syntax.
  3. Verify output identical to legacy version.
  4. Delete legacy file once Phase D ships.
```

### T6 — Diagnostics

Each phase should improve at least one diagnostic category. Target
shape:

```
error: cannot establish inout projection while let projections are active
  --> main.joyeer:7:11
   |
 5 |     let snapshot = n
   |                    - 'n' is read here
 7 |     increment(n: &n)
   |                  ^^ inout projection of 'n' conflicts with the read above
   |
help: end the snapshot's use before the inout call, or copy first
   |
 7 |     let n2 = snapshot
 8 |     increment(n: &n)
   |
```

File: `lib/diagnostic/diagnostic.cpp`.

---

## Out of scope (handled elsewhere)

| Item | Tracked in |
|------|------------|
| LLVM backend / native codegen | [docs/plan/v0.1.md](v0.1.md), [docs/plan/roadmap.md](roadmap.md) |
| Multi-file modules / build tool | roadmap Phase 3 |
| Concurrency / async-await | spec §15 (reserved) |
| FFI / C interop | spec §4.9, §15 |
| Macros / metaprogramming | spec §15 |
| Trait/protocol system | spec §15 |

---

## Suggested AI-autonomy strategy

Because Joyeer is an AI-era language, this plan should be executable
mostly autonomously:

1. **One AI agent per phase**. Phases P → A → B → C → D are sequentially
   dependent.
2. **Within a phase**, the cross-cutting tracks (T5, T6) can run in
   parallel agent fleets.
3. **Gating**: each phase ships only when its `tests/spec/PhaseX/`
   directory is green under `--lang=v0.1`.
4. **PR cadence**: 1 PR per AST node addition, 1 PR per checker
   addition, 1 PR per stdlib type. Small PRs keep human review
   tractable.

---

## Open questions (defer until needed)

- **Q1.** Should subscript accessors compile as coroutines (clean) or as
  inline-expanded callee fragments (faster, more codegen work)?
  → Decide at start of A.7 based on bytecode complexity.

- **Q2.** Standard-library home: pure Joyeer with `__builtin` hooks, or
  inline C++? → Per-type decision; start with C++ for the v0.1 stretch.

---

## Appendix: dependency graph

```
Phase 0  ──┐
           │
           ▼
    Phase L ✅ ── ★ minimal JSON-parser lexer
       │
       ▼
      Phase P ✅ ── ★ syntax-only Parser MVP
       │
       ▼
       Phase A ── ★ memory model (4 weeks)
           │
           ▼
       Phase B ── ★ data model (6 weeks)
           │      ├── uses A's exclusivity checker for struct fields
           │      └── uses A's subscript machinery for Array/Dict/String
           ▼
      Phase C ── ★ syntax-only annotations (1 week)
           │
           ▼
       Phase D ── ★ legacy removal (2 weeks)
           │
           ▼
       v0.1 ships
```
