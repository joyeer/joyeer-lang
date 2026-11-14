# 0011: Library enums and collections

- Status: proposed (updated 2026-11-14)
- Decisions: [0021](../decisions/0021-protocols-and-generic-functions.md),
  [0022](../decisions/0022-generic-types-and-layout-intrinsics.md),
  [0023](../decisions/0023-library-concurrency.md) (completed concurrency-library migration)
- Spec: [§2](../../specs/02-types.md), [§20](../../specs/20-protocols.md), and the affected
  ownership, expression, and pattern chapters in the [spec index](../../specs/README.md)

## Goal

Replace the remaining compiler-defined generic containers with ordinary Joyeer library declarations:
`Optional` and `Result` first, then `Array` and `Dict`. Preserve their syntax, public APIs, ownership,
diagnostics, and performance while removing container-specific type kinds and executable lowering.

## Definition of done

- `Optional<T>` and `Result<T, E>` are source-library enums; optional sugar, contextual cases,
  exhaustive matching, propagation, and native wrapper signatures remain compatible.
- `Array<T>` and `Dict<K, V>` own their buffers and implement their existing operations in Joyeer.
  An array of linear values can be drained and ended without enabling implicit destruction of them.
- Borrowing an element does not move or clone it, and mutation cannot invalidate a live borrow.
- Removed compiler machinery is replaced by generic capabilities, not dispatch on public names.
- Interleaved benchmarks show no regression outside measured noise in both profiles.
- Separate role reviews and full Debug/Release platform validation are recorded with evidence.

## Current state

[Plan 0010](0010-generics.md) provides generic types, monomorphization, and runtime-only layout
intrinsics. Its concurrency migration is implemented and validated on Windows x64 and Linux
x86_64; 0023 supplies a bundled source prelude, typed stack slots, specialized callback addresses,
and closed task transfer parameters. Native macOS/AArch64 acceptance remains unrun.

`Optional`, `Result`, `Array`, and `Dict` still have compiler type kinds and lowering rules.
Array/dictionary algorithms already live in `lib/Runtime/core/`; the compiler still supplies
layouts, ownership callbacks, construction, and element access. There is no borrowed raw-slot
operation. General user generics still reject linear type arguments, and a raw-address owner
cannot express element-dependent copyability, linearity, and destruction through its fields alone.

## Todo

### Phase 1: Optional and Result

- [ ] Record a decision for canonical library enum identities, optional sugar, and propagation.
  Do not merely replace builtin-name dispatch with enum-name dispatch.
- [ ] Update the normative rules and add failing stage tests and durable fixtures for every
  existing case, pattern, optional wrapping, propagation, and ownership boundary.
- [ ] Declare the enums in the source prelude and preserve source-module visibility/shadowing.
- [ ] Implement generic enum construction, payload access, matching, and propagation through
  concrete typed/IR models; preserve diagnostics and source/debug locations.
- [ ] Remove the dedicated type kinds and cases only after compatible fixtures pass.
- [ ] Measure representative compiler output size and runtime behavior in both profiles; run
  the role reviewers and full supported-platform validation.

### Phase 2: borrowing and linear collection ownership

- [ ] Record a separate decision for borrowed slot access, projection lifetime/exclusivity,
  and element-dependent ownership of a raw-address container.
- [ ] Decide how generic code transfers or ends linear elements without allowing them to be
  silently dropped. The closed task-transfer exception in 0023 is not a general solution.
- [ ] Add failing ownership tests for mutation during a borrow, escaped borrows, partial moves,
  linear elements, early exits, and expanding generic recursion.
- [ ] Implement the minimum runtime-only capability and verification needed by safe library
  collection wrappers; reject unauthorized and malformed uses.
- [ ] Review the type/ownership and IR/ABI changes separately from their author.

### Phase 3: Array

- [ ] Benchmark construction, append/growth, borrowed indexing, removal, nested destruction,
  and draining arrays of linear handles before changing the implementation.
- [ ] Implement the existing array API in Joyeer, including constant-array clone boundaries,
  exact allocation/free sizes, and `consumeEmpty` and its trap.
- [ ] Preserve subscript/storage-path/exclusivity behavior and existing diagnostics/fixtures.
- [ ] Remove array-specific opcodes, type kinds, and backend layout/ownership cases.
- [ ] Benchmark again in both profiles, complete adversarial/role reviews, and run full
  platform validation.

### Phase 4: Dict

- [ ] Record a decision for key constraints and hashing/equality before broadening accepted
  key types or changing the existing linear-key/value prohibition.
- [ ] Benchmark construction, lookup, insertion/replacement, growth, and nested destruction.
- [ ] Implement the existing dictionary API and buffer ownership in Joyeer, preserving the
  documented copy-on-lookup and value-semantics boundaries.
- [ ] Remove dictionary-specific opcodes, type kinds, and backend cases after fixtures pass.
- [ ] Benchmark both profiles, run all role reviewers and full platform validation, and
  update supported features and runtime/IR notes.

## Open questions

- How are syntax sugar and `?` tied to a canonical library declaration without reopening
  arbitrary user enum layouts? Settle in the Phase 1 decision.
- What is the type/IR representation of a borrowed raw-slot projection, and what pins its owner
  while it is used? Settle before Array implementation.
- How can raw-address containers express conditional ownership properties and safely hold
  linear elements? A `deinit` on every instance or an always-linear array would change behavior.
- Should dictionary lookup continue cloning noncopyable values only by rejecting them, or gain a
  separately named borrowing API? Keep current behavior until a distinct decision accepts a change.

## Out of scope

Closures, existential/dynamic protocols, a GC, shared mutable containers, new collection APIs,
multi-sender channels, general explicit user type arguments, and performance-driven allocator
redesign. No prerequisite is considered accepted merely because this plan names it.

## Progress log

- 2026-11-14: Created the Phase 4 follow-up plan requested by 0010. All implementation and
  acceptance items in this plan remain open; 0023 records the completed x86_64 concurrency prerequisite.
