# C Interoperation: Remaining Work

> **Status:** unfinished work only. The scalar `@c_extern`/`@c_export` boundary
> is implemented; see [impl/c-interop](../impl/c-interop.md). The language rules
> are in [§19](../spec/19-c-interop.md). Remove items here when they are done.

## 1. Scope

The annotations are restricted to a toolchain-controlled runtime context. This
plan does not add user FFI. Related plans:

- [hybrid-runtime](hybrid-runtime.md): runtime migration and bootstrap.
- [library](library.md): `Foundation`, `System`, `Collections`, `Testing`.
- [native-link-inputs](native-link-inputs.md): proposal for supplying link inputs.

## 2. Ground Rules for New Work

- One generic native-call model across type checking, IR verification, and
  backend. Adding a binding must not need per-function compiler changes.
- Native privilege comes from the toolchain context, never from a module name,
  `public`, or directory.
- A missing library or symbol is a link error. No stubs or silent fallbacks.
- The compiler cannot prove a linked C function matches its declared signature.
  The binding author owns that.
- Adopt any new memory or alias rule in the spec before implementing it.

## 3. Remaining Work

### 3.1 Platform acceptance

Native scalar acceptance is open on every platform/architecture except those
already measured. For each claimed target, run the scalar fixtures with real C
callers and callees:

- All integer widths, `Int`/`UInt`, `_Bool`, `Float`, `Double`, including
  boundaries, signed zero, infinities, and NaN.
- Narrow argument/result extension, mixed register classes, stack-passed
  arguments, and fixed-prototype `float`.
- LP64/LLP64 `long`, plain `char` signedness, `size_t`, `ptrdiff_t`, and
  `_Bool` versus Windows `BOOL`.
- Explicit rejection of unsupported targets and non-scalar signatures.

Do not infer one platform's result from another.

### 3.2 Native memory (design needed, no accepted syntax)

Decide each contract below before adding source syntax:

| Boundary | Must define |
|---|---|
| Borrowed read-only bytes | Valid for the call only; no mutation, retention, or escape |
| Writable bytes | Exclusive access, capacity, initialized length, partial writes |
| Raw/opaque addresses | Source representation, allowed operations, lifetime; never an integer |
| Output parameters | Representation, initialization on success/failure, cleanup |
| Nullable addresses | Explicit null form; zero-length inputs; `Optional` is not pointer layout |
| Owned result storage | Allocator, transfer point, exactly one release |
| Native resource handles | Invalid states, close failures, noncopyability, cleanup |

Also define:

- Native-error capture and cleanup order for partial writes and allocations.
- C-string adapters: encoding, NUL policy, copy versus borrow. `String` is
  byte-oriented, not guaranteed UTF-8 or NUL-terminated.
- Lowering of a region to separate C address/length arguments.
- Callbacks and cross-thread access (separate designs).

Prefer an internal scoped region mechanism over exposing pointer arithmetic.

### 3.3 Open decisions

| Question | Next step |
|---|---|
| C-specific source aliases? | Decide separately; today use fixed-width types or C shims |
| Any public native capability? | Separate user-FFI/unsafe proposal |
| Who supplies link metadata? | CMake for bootstrap; joypm design is in [native-link-inputs](native-link-inputs.md) |
| Complex ABI features | Separate gates: aggregates by value, unions/bit-fields, callbacks, varargs, `long double` |
