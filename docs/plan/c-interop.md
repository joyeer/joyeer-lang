# C Interoperation Implementation Plan

> **Status:** the first runtime-only `Int`/`Void` import/export and object-build
> slice is implemented; see [current coverage](../impl/c-interop.md).
> The broader annotation/scalar design is specified in
> [§19 C Interoperation](../spec/19-c-interop.md). This plan tracks remaining
> scalar/platform gates and unresolved memory proposals, not new language
> rules or a completed runtime migration.

## 1. Purpose and Direction

Enable Joyeer to call C functions through a reusable native-binding mechanism,
then use the same ABI model to export Joyeer runtime functions to C callers.
The compiler should understand C ABI signatures, not require
new semantic/code-generation cases for every native function.

The specification uses `@c_extern` and `@c_export`, with C ABI implied.
Other foreign ABIs and a configurable `abi` argument are not part of this
scope. Keep the normative declaration, type, and symbol rules in §19 rather
than maintaining a second version here.

The recommended direction combines:

- Swift-like public modules that hide native implementation details.
- Explicit contracts for ABI, memory ownership, errors, and unsafe operations.
- Enough internal storage control to implement a runtime without GC.

Implement the specified restricted runtime-building context, not unrestricted
user FFI.
The [hybrid runtime plan](hybrid-runtime.md) owns migration and bootstrap
phases; the [library plan](library.md) owns `Foundation`, `System`,
`Collections`, and `Testing`. This plan owns the binding implementation
gates and the memory questions those plans still need to settle.

## 2. Current Behavior and Compiler Dependencies

Joyeer currently calls selected C runtime functions through compiler-known
operations. The [host builtin table](../../src/compiler/frontend/hostbuiltins.h)
describes filesystem/process interfaces; it is not a general external-function
declaration facility. The separate restricted C binding mechanism implements
the first `Int`/`Void` slice. General user FFI and pointers remain outside the
[implemented surface](../impl/supported-features.md) and
[raw-memory scope](../spec/04-memory.md#49-raw-memory-ffi).

Distinguish the following execution, linking, and build/product cases:

| Context | Current relationship to `JoyeerRuntime` |
|---|---|
| Compiler's own execution | The C++ compiler links its compiler core/backend, not the Joyeer program runtime. It still uses system C/C++ libraries and the native backend. |
| Checking source, emitting LLVM IR, or internal runtime-object generation | These modes do not require the runtime archive to be present. Runtime calls can remain unresolved native references until linking. |
| Linking a generated executable | The driver locates the runtime archive and links it into the generated program, together with platform inputs. |
| Compiler construction versus product staging | `joyeer` builds independently of the runtime. `JoyeerToolchain` stages the archive beside it; joypm bootstrap depends on the staged product. |

See [compiler CMake](../../src/compiler/CMakeLists.txt),
[driver output-mode handling](../../src/compiler/driver/driver.cpp), and
[native linking](../../src/compiler/codegen/linker.cpp).
The acyclic object-generation path is available. Assembling actual mixed
runtime providers remains part of the hybrid runtime migration, not a rewrite
of the compiler to remove an execution dependency.

## 3. Implementation Architecture

```text
Application
    -> ordinary Foundation/System APIs
    -> checked Joyeer wrappers
    -> explicit native bindings
    -> C substrate or platform C library
```

A source binding describes a callable, its external symbol, the target ABI,
and parameter/result types. The build system separately supplies the native
link inputs. Joyeer argument labels remain source-level information, not part
of a C symbol.

Implementation obligations under [§19](../spec/19-c-interop.md):

- Use one generic native-call model across type checking, IR verification,
  and backend emission. Adding a binding with an already-supported signature
  should not require changes to those algorithms.
- A toolchain-controlled runtime context permits the C binding annotations.
  A module name, `public` declaration, or source-directory location does not
  grant native privileges. This restriction is not an OS sandbox.
- Separate Joyeer module visibility from C exports. `public` alone does not
  export a C symbol; a native export does not re-export source declarations.
- Keep the ABI private and compiler-version-matched initially. C ABI use does
  not imply identical layouts across targets or a stable public Joyeer ABI.
- Native calls may have side effects. Do not assume purity, thread safety,
  pointer retention rules, or error conventions from a function's name.

## 4. First Slice: Scalar Import and Export

The delivered implementation is deliberately narrower than both general
C and the [specified scalar boundary](../spec/19-c-interop.md#194-c-scalar-representations).
This is the H1-H2 subset, not the long-term language type limit. Its measured
acceptance and supported toolchain inputs belong in
[the implementation reference](../impl/c-interop.md):

| Joyeer position | C representation | Implementation gate |
|---|---|---|
| `Int` parameter or result | `int64_t` | Supported exactly, without implicit narrowing |
| `Void` result | `void` | Supported; no result value crosses the ABI |
| Empty parameter list | C prototype with `(void)` | Supported; not a variadic declaration |
| `Bool`, other integer widths, `UInt`, `Float`, `Double` | Defined in §19.4 | Follow-up scalar implementation after the first round trip |
| `String`, arrays, dictionaries, structs, enums, `Optional`, `Result` | No automatic C-compatible representation | Rejected in native signatures |
| `inout`, `initializing`, `consuming` | Not native pointer or ownership syntax | Rejected |
| Raw addresses, output pointers, callbacks | No accepted source representation | Separate design and implementation gates |

Ordinary C `int`, `long`, and `size_t` must not be guessed to equal Joyeer
`Int`. A C shim can adapt unsupported interfaces when conversions and failure
conditions are explicit. Addresses must not be disguised as integers.
Initially pass scalar values by value; managed-value ownership effects are
not part of this slice.

The compiler can validate a binding's internal consistency, but without a C
header importer it cannot prove that the linked C function actually has the
declared signature. The binding author owns that obligation; a matching
linker symbol does not prove type compatibility.

## 5. Source Contract and Compiler Work

The source contract is defined in
[§19.2](../spec/19-c-interop.md#192-declarations-and-annotation-arguments),
with the shared declaration/attribute grammar in [§17](../spec/17-grammar.md).
The `c_` prefix identifies the ABI, not the implementation language.

The initial implementation accepts these declarations only in an explicitly
authorized named unit. They still need the C definition and native linking
shown below; they are not standalone application programs:

```joyeer
@c_extern(symbol: "joyeer_native_identity")
func nativeIdentity(value: Int): Int

@c_export(symbol: "joyeer_rt_identity")
func runtimeIdentity(value: Int): Int {
    return nativeIdentity(value: value)
}
```

The initial lexer/parser, typed binding, IR/verifier, backend, diagnostic, and
runtime-context plumbing is implemented. Extend that same source-derived
binding model for the remaining scalar representations rather than adding
special cases per native function or a second signature configuration.
Keep this work independent of the proposed `@spec`/`@property` parser/runner;
see [annotation status](../spec/11-annotations.md).

Internal binding descriptions remain a compiler representation, not another
user-maintained file. Header import, C-specific source aliases, a public
unsafe-call spelling, and native dependency fields in `project.toml` remain
separate decisions.

## 6. Example: A Complete C/Joyeer/C Round Trip

This illustrates the implemented call path when the Joyeer unit is authorized
and its object is linked with the C helper and caller. The
[durable fixture](../../tests/c-interop/) also validates static and shared
providers, multiple objects, boundary values, and stack-passed arguments.
The C helper has no allocation, overflow, or managed-value dependencies:

```c
#include <stdint.h>

int64_t joyeer_native_identity(int64_t value) {
    return value;
}
```

Together with the annotated Joyeer declarations in the previous section,
a C caller would validate the exported entry point:

```c
#include <stdint.h>

int64_t joyeer_rt_identity(int64_t value);

int main(void) {
    return joyeer_rt_identity(INT64_MIN) == INT64_MIN &&
           joyeer_rt_identity(INT64_MAX) == INT64_MAX ? 0 : 1;
}
```

The actual path is C caller -> C-callable Joyeer export -> imported C helper.
Export adapters preserve the declared ABI rather than exposing incidental
internal Joyeer function layouts or numeric names. Fixture symbols belong
in tests, not the installed production runtime interface.

## 7. Linking and Build Responsibilities

- Use the existing internal runtime-object path for explicit Joyeer sources,
  without an application `main`, executable link, or completed runtime archive.
- Let CMake compile the C substrate, provide explicit native objects/libraries,
  and assemble the mixed static archive. Do not require preinstalled joypm
  or Joyeer to construct the first toolchain.
- Preserve the independent compiler construction and `JoyeerToolchain`
  product staging when adding mixed-runtime objects. Normal executable output still
  requires its runtime and platform SDK inputs.
- Keep LLVM/Clang C++ types and ownership inside the backend. Any new backend
  entry points must follow its versioned C ABI boundary.
- Match target architecture, calling convention, SDK, and platform runtime
  settings. A source import does not discover libraries or compile C files.
- Keep source-module handling in joypm unchanged initially. Future native
  dependencies and installed SDK binding discovery need explicit contracts;
  they are not automatically provided by the current workspace schema.

Missing libraries or symbols must fail explicitly. Do not substitute a stub,
silently fall back to a C algorithm, or bypass the configured linker.

## 8. Memory, Errors, and the Next Slice

Scalar calls establish the plumbing, not the storage model needed for strings.
**Separate proposal, not accepted source syntax:** the address mechanism,
its runtime-only versus public scope, and any new type/alias spellings are
unresolved. [§19.10](../spec/19-c-interop.md#1910-deferred-memory-and-abi-extensions)
does not admit them in native signatures. Before extending the specification,
decide:

| Boundary | Contract to define |
|---|---|
| Borrowed read-only bytes | Address and extent are valid for the call; no mutation, retention, release, or escaping borrow |
| Writable bytes | Exclusive access, capacity, initialized length, and partial-write behavior |
| Raw or opaque native addresses | Source representation, permitted operations, provenance, and lifetime; never encode an address as an integer |
| Output parameters | Pointee representation/alignment, initialization on success, partial initialization on failure, and cleanup responsibility |
| Nullable addresses | Explicit null representation and validity rules, including zero-length inputs; no assumption that `Optional<T>` has pointer layout |
| Owned result storage | Allocator provenance, initialization, transfer point, and exactly one release obligation |
| Native resource handles | Invalid states, close failures, noncopyability, and deterministic cleanup |

Prefer an internal scoped region or address mechanism over exposing arbitrary
pointer arithmetic to applications. A declaration cannot make an incorrect C
implementation memory-safe. Full user resource types also need destruction
and noncopyability support; they are not a prerequisite for the scalar fixture.
If a region maps to separate C address/length arguments, specify that lowering
explicitly rather than assuming a by-value Joyeer aggregate has C layout.
Neither `inout` nor `initializing` is already a native output-pointer contract.

Preserve the current runtime's byte-oriented `String` behavior. It is neither
a guaranteed UTF-8 text type nor a NUL-terminated `char*`. C-string adapters need an explicit
encoding/NUL policy and lifetime; copying versus temporary borrowing must
not be hidden. Never reinterpret a managed aggregate as a C aggregate.

The accepted [error and control-flow rules](../spec/19-c-interop.md#196-ownership-errors-and-control-flow)
already require explicit wrappers and prohibit nonlocal unwinding. The memory
extension must additionally define native-error capture and cleanup ordering
for partial writes/allocations. Callbacks and cross-thread access need separate
designs. Minimal panic/allocation-failure paths must not depend on managed
formatting or the operation being implemented.

## 9. Acceptance Boundaries

The first-slice unit/native gates are registered; their exact scope and
measured host evidence are recorded in
[implementation acceptance](../impl/c-interop.md#5-build-and-acceptance).
Keep running them as later scalar/storage support lands. Native acceptance
on other platform/architecture combinations remains open until measured.
A scalar fixture does not complete runtime migration, safe pointer support,
or general user FFI.

### 9.1 Follow-up scalar acceptance

Implement the rest of §19.4 as a separate scalar gate, not an implicit change
to the H1-H2 fixture:

- Round-trip every signed/unsigned width, `Int`/`UInt`, `_Bool`, `Float`, and
  `Double` through real C callers and callees. Cover signed boundaries,
  unsigned high bits, zero/nonzero Boolean semantics, floating signed zero,
  infinities, and NaNs without assuming a portable NaN payload convention.
- Exercise narrow argument/result extension, mixed scalar register classes,
  enough arguments to require stack passing, and fixed-prototype `float`
  without variadic promotion.
- Verify the LP64/LLP64 `long` difference, signedness-selected plain `char`,
  `size_t`, and `ptrdiff_t` on every claimed target. Use real C prototypes
  to distinguish `_Bool` from integer conventions such as Windows `BOOL`.
- Reject unsupported target properties and all unimplemented scalar kinds
  explicitly until their gates pass. Do not advertise full C scalar support
  on the strength of the `Int`/`Void` round trip.

## 10. Remaining Decisions

Annotation names, explicit `symbol:`, implied C ABI, body rules, runtime-only
authorization, visibility separation, and scalar mappings are specified in
§19. The following remain separate work:

| Question | Boundary / next decision |
|---|---|
| Are C-specific source aliases needed? | Decide target-selected spelling and semantics separately; the current specification uses existing fixed-width types or C shims |
| What represents native memory? | Decide scoped internal regions/addresses, nullability, output storage, and ownership before adding source syntax |
| Should any native capability become public? | A separate user-FFI/unsafe proposal, not a side effect of this runtime gate |
| Who supplies linking metadata? | CMake for bootstrap; separately design future joypm/native dependency integration |
| When do complex C ABI features enter? | Separate gates for aggregates by value, unions/bit-fields, callbacks, varargs, and `long double` |

Adopt any resulting memory or alias rules in the specification before
implementing those extensions. Remaining phases do not change current support until their implementation
and corresponding acceptance evidence land.
