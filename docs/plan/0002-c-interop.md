# 0002: C Interoperation

- Status: in progress (updated 2026-11-28); the scalar boundary is implemented and native
  ARM64 acceptance has passed. x86-64 case evidence and memory design remain.
- Decisions: [0005](../decisions/0005-runtime-unsafe-memory.md), [0009](../decisions/0009-runtime-core-capabilities.md),
  [0038](../decisions/0038-scoped-native-memory.md) (proposed)
- Spec: [§19](../../specs/19-c-interop.md)

## Goal

A C boundary for the toolchain-controlled runtime context that is accepted on every claimed platform,
with designed contracts for native memory before any source syntax is added. This plan does not add
user FFI.

Related: [runtime-in-joyeer](0008-runtime-in-joyeer.md),
[standard modules](../../specs/12-modules.md#125-standard-modules), and
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

- [x] List which targets are already measured in [impl/c-interop](../impl/c-interop.md#native-validation-evidence).
- [ ] Windows x64: the historical scalar result covers `Int`/`Void`; the newer full-suite
  result needs the C ABI case evidence.
- [x] Windows ARM64: unfiltered Debug and Release at `a362c88` each included 158/158
  `c-interop` cases.
- [ ] Linux x86-64: the prior full-suite result needs the C ABI case evidence.
- [x] Linux AArch64 (WSL2): unfiltered Debug and Release at `a362c88` each included
  158/158 `c-interop` cases.
- [x] macOS ARM64. At `5c21924e`, Debug and Release each passed all 158 `c-interop`-labeled
  cases within an unfiltered run. See [native evidence](../impl/c-interop.md#macos-arm64)
  for real static/shared C callers and callees, ABI coverage, and rejection tests.

### Native memory: implemented boundary and remaining design

The checked items cover only the existing authorized buffer forms and explicit wrappers.
For extensions, prefer an internal scoped region mechanism over exposing pointer arithmetic.

- [x] Record a proposal for the scoped byte, nullability, ownership, and
  callback boundaries in [decision 0038](../decisions/0038-scoped-native-memory.md).
- [x] Borrowed read-only bytes: [§19.4.3](../../specs/19-c-interop.md#1943-restricted-buffer-imports)
  makes descriptors and backing storage read-only and call-bound. The native buffer round trips
  passed in both ARM64 runs.
- [ ] Writable bytes: exclusive access, capacity, initialized length, partial writes.
  [§19.4.4](../../specs/19-c-interop.md#1944-bare-byte-pointers) covers an existing initialized
  byte array, not a generic writable region or a returned initialized extent.
- [ ] Raw or opaque addresses: source representation, allowed operations, lifetime; never an integer in
  ordinary code (runtime units already carry addresses as `Int`, decision 0005).
- [x] Managed buffer outputs: [§19.4.3](../../specs/19-c-interop.md#1943-restricted-buffer-imports)
  defines `initializing` runtime handles, valid on both success and failure, with caller cleanup.
  Native buffer fixtures passed on both ARM64 hosts; arbitrary output pointers remain rejected.
- [ ] Nullable addresses: explicit null form; zero-length inputs; `Optional` is not pointer layout.
- [ ] Owned result storage: allocator, transfer point, exactly one release.
- [ ] Native resource handles: invalid states, close failures, noncopyability, cleanup.
- [x] Error capture for explicit wrappers: [§19.6](../../specs/19-c-interop.md#196-ownership-errors-and-control-flow)
  requires capture before cleanup can overwrite the error. The `System` filesystem and process
  fixtures pass exact platform-code assertions; annotations never inspect native error state.
- [ ] C-string adapters: encoding, NUL policy, copy versus borrow (`String` is byte-oriented, not
  guaranteed UTF-8 or NUL-terminated).
- [ ] Lowering of a region to separate C address and length arguments.
- [ ] Callbacks and cross-thread access (separate designs).

### Runtime capabilities

- [x] Bulk memory: [`memcpy`, `memmove`, and `memset` intrinsics](../../specs/19-c-interop.md#1945-compiler-intrinsics).
  The runtime builds with them and passes `RuntimeCoreObjectSymbols`.
- [x] Linux syscall and target-architecture intrinsics ([decision 0011](../decisions/0011-runtime-without-the-c-library.md#decision)):
  the native AArch64 `RuntimeSystemProgramNeedsOnlyTheCStartup_*` fixtures pass.

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

- 2026-11-28: Recorded the scoped native-memory proposal; no address
  representation, ABI, or language rule was added. The x86-64 published
  validation report records aggregate totals but does not enumerate the
  required C ABI case results.
- 2026-11-28: Verified full native scalar and buffer coverage on Windows ARM64 and Linux
  AArch64 (WSL2). Reconciled the syscall, bulk-memory, and restricted buffer checklist
  with the existing contracts; no new native-memory syntax was adopted.
- 2026-11-26: Checked off macOS ARM64 scalar acceptance after verifying the native Debug and
  Release runs at `5c21924e`. No native-memory design items changed.
- 2026-11-12: converted to the plan template; no items changed.
