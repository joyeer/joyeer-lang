# 0002: C Interoperation

- Status: in progress (updated 2026-11-12); the scalar boundary is implemented, acceptance and memory
  design remain
- Decisions: [0005](../decisions/0005-runtime-unsafe-memory.md), [0009](../decisions/0009-runtime-core-capabilities.md)
- Spec: [§19](../../specs/19-c-interop.md)

## Goal

A C boundary for the toolchain-controlled runtime context that is accepted on every claimed platform,
with designed contracts for native memory before any source syntax is added. This plan does not add
user FFI.

Related plans: [runtime-in-joyeer](0008-runtime-in-joyeer.md) (the runtime in Joyeer),
[library](0003-library.md) (`Foundation`, `System`, `Collections`, `Testing`),
[native-link-inputs](0004-native-link-inputs.md) (supplying link inputs).

## Definition of done

- Native scalar acceptance has run with real C callers and callees on every claimed target.
- Each native-memory boundary below has an adopted contract in the spec, or is explicitly rejected.
- Every open decision below is settled in a decision record.

## Current state

The scalar `@c_extern`/`@c_export` boundary is implemented ([impl/c-interop](../impl/c-interop.md)).
Runtime-only capabilities are implemented: static storage, indirect calls, data symbols, `Never`
results, core units and the core entry export
([§19.4.5](../../specs/19-c-interop.md#1945-compiler-intrinsics),
[§19.4.7](../../specs/19-c-interop.md#1947-core-units-never-results-data-symbols-and-the-program-entry-point)).

Ground rules for new work:

- One generic native-call model across type checking, IR verification, and backend. Adding a binding
  must not need per-function compiler changes.
- Native privilege comes from the toolchain context, never from a module name, `public`, or directory.
- A missing library or symbol is a link error. No stubs or silent fallbacks.
- The compiler cannot prove a linked C function matches its declared signature; the binding author owns
  that.
- Adopt any new memory or alias rule in the spec before implementing it.

## Todo

### Platform acceptance

For each claimed target, run the scalar fixtures with real C callers and callees. Do not infer one
platform's result from another. Each target covers: all integer widths, `Int`/`UInt`, `_Bool`, `Float`,
`Double`, including boundaries, signed zero, infinities and NaN; narrow argument and result extension,
mixed register classes, stack-passed arguments and fixed-prototype `float`; LP64/LLP64 `long`, plain
`char` signedness, `size_t`, `ptrdiff_t`, and `_Bool` versus Windows `BOOL`; and explicit rejection of
unsupported targets and non-scalar signatures.

- [ ] List which targets are already measured in [impl/c-interop](../impl/c-interop.md).
- [ ] Windows x64.
- [ ] Windows ARM64.
- [ ] Linux x86-64.
- [ ] Linux AArch64.
- [ ] macOS arm64.

### Native memory (design needed, no accepted syntax)

Prefer an internal scoped region mechanism over exposing pointer arithmetic. Decide each contract before
adding source syntax:

- [ ] Borrowed read-only bytes: valid for the call only; no mutation, retention, or escape.
- [ ] Writable bytes: exclusive access, capacity, initialized length, partial writes.
- [ ] Raw or opaque addresses: source representation, allowed operations, lifetime; never an integer in
  ordinary code (runtime units already carry addresses as `Int`, decision 0005).
- [ ] Output parameters: representation, initialization on success and failure, cleanup.
- [ ] Nullable addresses: explicit null form; zero-length inputs; `Optional` is not pointer layout.
- [ ] Owned result storage: allocator, transfer point, exactly one release.
- [ ] Native resource handles: invalid states, close failures, noncopyability, cleanup.
- [ ] Native-error capture and cleanup order for partial writes and allocations.
- [ ] C-string adapters: encoding, NUL policy, copy versus borrow (`String` is byte-oriented, not
  guaranteed UTF-8 or NUL-terminated).
- [ ] Lowering of a region to separate C address and length arguments.
- [ ] Callbacks and cross-thread access (separate designs).

### Runtime capabilities still proposed

- [ ] Bulk memory ([§19.4.6](../../specs/19-c-interop.md#1946-runtime-capabilities-still-proposed-)).
- [ ] A Linux syscall intrinsic (§19.4.6).

## Open questions

| Question | Next step |
|---|---|
| C-specific source aliases? | Decide separately; today use fixed-width types or C shims |
| Any public native capability? | Separate user-FFI/unsafe proposal |
| Who supplies link metadata? | CMake for bootstrap; fern design is in [native-link-inputs](0004-native-link-inputs.md) |
| Complex ABI features | Separate gates: aggregates by value, unions/bit-fields, callbacks, varargs, `long double` |

## Out of scope

User FFI; public native capabilities without their own proposal.

## Progress log

- 2026-11-12: converted to the plan template; no items changed.
