## §19 C Interoperation

**Specification status:** this chapter defines the restricted C boundary in
the broader language design. It does not introduce a public pointer or
general-purpose unsafe system.

**Implementation status:** the authorized native implementation supports
both annotations, bodyless imports, C-callable scalar exports, all §19.4 mappings,
and the restricted buffer imports in §19.4.3.
The example in §19.2 requires an explicitly authorized compilation context;
it is not an ordinary application program. Native platform acceptance and
the still-unsupported memory extensions are recorded in the
[C binding implementation](../impl/c-interop.md),
[implemented surface](../impl/supported-features.md) and the separate
[implementation plan](../plan/c-interop.md).

### 19.1 Scope and authorization

`@c_extern` imports a native function; `@c_export` provides a C-callable
definition written in Joyeer. Both select the configured target's ordinary
C calling convention. There is no `abi` argument or selection of another
calling convention.

Only compilation units explicitly selected by the toolchain for native
bindings may declare or directly call these bindings, including runtime
construction and optional native-backed source libraries. Authorization is
an explicit compilation input, not a property inferred from a path, module
name such as `Foundation`, visibility modifier, or annotation itself.
It is not inherited by importing an authorized unit, nor automatically
granted to that unit's dependencies. Ordinary code uses unannotated, checked
Joyeer APIs instead. This restriction does not establish an OS sandbox or
prove the safety of a native implementation.

The build interface that selects authorized units is an implementation
contract; this chapter does not invent a command-line flag, manifest field,
object-output mode, or separately linkable module facility.

### 19.2 Declarations and annotation arguments

```joyeer
@c_extern(symbol: "joyeer_native_identity")
func nativeIdentity(value: Int): Int

@c_export(symbol: "joyeer_rt_identity")
func runtimeIdentity(value: Int): Int {
    return nativeIdentity(value: value)
}
```

| Annotation | Definition provider | Function body |
|---|---|---|
| `@c_extern` | A native link input | Must be absent |
| `@c_export` | The annotated Joyeer function | Required |

These annotations precede any visibility modifier on a top-level free
function declaration (§3.2). They are not permitted on methods, initializers,
accessors, types, fields, or bindings. A declaration may have exactly one C
binding annotation: repeats and combining the two are errors.

Each annotation requires a first named argument, `symbol:`, whose value
is a nonempty string literal. A `@c_extern` may then add `pointer:` arguments,
each a string literal naming one of its `[UInt8]` parameters (§19.4.4). Expressions,
inferred names, omitted arguments, duplicate `symbol:` arguments, and other
arguments, including `abi`, `library`, or a repeated signature, are errors. The decoded symbol must be an ASCII C
identifier: `[A-Za-z_][A-Za-z0-9_]*`. Embedded NUL, whitespace, and platform
linker decorations are not allowed.

Parameter types and the result type must be explicit. A unit result may be
written `: Void` or `: ()`, but it may not be omitted on a C binding. Parameter
names and labels follow §3.2.1. Scalar parameters use default or explicit
`borrowing`; imports may also use the buffer access forms in §19.4.3.
Defaults for argument values are not admitted. The function must satisfy
the signature rules in §19.4 and §19.5.

A bodyless declaration is valid only with `@c_extern`; an empty `{}` is
still a body and is invalid on an import. Ordinary functions and
`@c_export` definitions require bodies. The declaration and attribute
productions are consolidated in [§17](17-grammar.md).

### 19.3 Source calls and native symbols

The function declaration is the sole source signature. Joyeer calls use
its source name, argument labels, access rules, and declaration order.
Labels and internal parameter names are not part of the C ABI. A binding
does not require a second user-maintained signature file.

`symbol` names the C linkage identifier, not a module-qualified Joyeer name
or a raw object-file spelling. The backend applies the target's normal C
symbol decoration. For example, a target's object-file underscore prefix is
not included in `symbol: "joyeer_rt_identity"`. No overload suffix or
Joyeer-specific mangling is added to that C linkage name.

Joyeer visibility and C linkage are independent:

- `public` alone does not create a C export.
- `@c_export` does not make a Joyeer declaration `public`, create an import,
  or re-export source declarations.
- An internal or private Joyeer definition may supply a C entry point.
  Its C symbol is externally linkable even though Joyeer lookup still
  enforces its declared visibility and the authorization in §19.1.

For every native symbol visible in a compilation, all declarations must
agree on a normalized C ABI signature: calling convention, parameter count
and order, parameter representations, and result representation. Source
labels, internal names, and visibility do not affect this comparison.
Repeated compatible imports are allowed; incompatible imports are errors.
An import may refer to a compatible export, but there may be at most one
definition of that symbol in the linked program. Two Joyeer exports with the
same symbol are errors even if their signatures agree. Conflicts with
definitions supplied by native objects must also fail, not select a provider
silently.

Annotations do not change executable entry-point selection (§3.2.6).
They are not permitted on the Joyeer executable entry, and a C export may
not claim `main` or another entry symbol owned by the toolchain. Runtime
object construction does not turn a missing application entry into a valid
executable.

### 19.4 C scalar representations

The following are the complete by-value scalar representations for this
revision. They apply equally to imports and exports. These are ABI
representations, not new source aliases or promises about the ordinary
Joyeer calling convention.

| Joyeer type | C representation | Permitted positions |
|---|---|---|
| `Void` / `()` | `void` | Result only |
| `Bool` | C11 `_Bool` (`bool` from `<stdbool.h>`) | Parameter and result |
| `Int8` | `int8_t` | Parameter and result |
| `UInt8` | `uint8_t` | Parameter and result |
| `Int16` | `int16_t` | Parameter and result |
| `UInt16` | `uint16_t` | Parameter and result |
| `Int32` | `int32_t` | Parameter and result |
| `UInt32` | `uint32_t` | Parameter and result |
| `Int64`, `Int` | `int64_t` | Parameter and result |
| `UInt64`, `UInt` | `uint64_t` | Parameter and result |
| `Float` | C `float`, IEEE 754 binary32 | Parameter and result |
| `Double` | C `double`, IEEE 754 binary64 | Parameter and result |

An empty Joyeer parameter list denotes a fixed, zero-argument C prototype,
written `(void)` in C11. It is neither a `Void` parameter, an unspecified
parameter list, nor a variadic function.

#### 19.4.1 Target C data models

Bindings to C spelling rather than fixed-width typedefs must use the
representation selected for the actual target, not the build host:

| C type | 64-bit LP64 targets (macOS/Linux) | 64-bit LLP64 targets (Windows) |
|---|---|---|
| `signed char` / `unsigned char` | `Int8` / `UInt8` | `Int8` / `UInt8` |
| `short` / `unsigned short` | `Int16` / `UInt16` | `Int16` / `UInt16` |
| `int` / `unsigned int` | `Int32` / `UInt32` | `Int32` / `UInt32` |
| `long` / `unsigned long` | `Int64` / `UInt64` | `Int32` / `UInt32` |
| `long long` / `unsigned long long` | `Int64` / `UInt64` | `Int64` / `UInt64` |
| `size_t` | `UInt64` | `UInt64` |
| `ptrdiff_t` | `Int64` | `Int64` |

`Int` is always signed 64-bit, not C `int` or an address carrier (runtime units
may use it as one; see the last rule of §19.5). `UInt`
is always unsigned 64-bit, not a promise that every C unsigned type has that
width. The table does not add support for a new target or data model.

Plain C `char` maps to `Int8` when the target C implementation makes it
signed, and to `UInt8` when it makes it unsigned. Its signedness must be
established for the configured target and native build options; it must
not be inferred from the host or from the word `char`. Joyeer `Char` is a
Unicode scalar and is never the representation of C `char`.

Windows `BOOL` is a signed 32-bit integer and uses `Int32`, not `Bool`.
An explicit wrapper may interpret its zero/nonzero convention. A C typedef
is admissible only when its underlying type has one of the specified scalar
representations. Similar names or sizes do not establish compatibility for
an enum, aggregate, or pointer.

This revision introduces no C-specific source aliases. Whether to add
target-selected names such as `CInt`, `CLong`, or `CChar` remains a separate
proposal; those names are not reserved here. A binding author must select
the target-correct fixed-width type or use a C shim with an explicit
fixed-width interface. Unknown target properties must be diagnosed rather
than guessed.

#### 19.4.2 Calling convention requirements

The target C ABI governs register/stack passing, result locations, scalar
extension, and floating-point classification. In particular, small integers
and `_Bool` must use the target's required argument and result extension
rules; mapping every scalar to `Int` or assuming a raw LLVM `i1` convention
is not sufficient.

The source `borrowing` effect remains read-only access to the caller's value,
but a C scalar crosses the native boundary by value. It does not pass a
pointer to Joyeer storage or let C mutate the caller's binding. An export
must adapt its incoming C values to the ordinary Joyeer function semantics;
it must not expose incidental internal parameter or result layouts.

No implicit narrowing, sign change, integer/Boolean conversion, or
integer/floating conversion is performed to repair a mismatched signature.
Fixed-prototype calls do not apply variadic default promotions: `Float`
remains C `float`. `_Bool` represents only `false` and `true`; it is not
an arbitrary integer truth convention.

#### 19.4.3 Restricted buffer imports

`@c_extern` additionally admits borrowing `String`, `[UInt8]`, `[String]`,
and `[[UInt8]]`, and initializing `String`, `[UInt8]`, and `[String]`.
These are explicit adapter conventions, not general C aggregate or pointer
types. Results and `@c_export` signatures remain scalar/`Void` only.

A borrowed buffer crosses as a read-only data pointer and signed 64-bit
count. Strings and byte arrays count bytes; lists count elements and expose
read-only runtime string/array descriptors. The native function must not
mutate, retain, or free descriptors or backing storage. No encoding conversion
or NUL terminator is implied. In particular, `String` is not a C string.

An initializing argument is passed by pointer to a caller-owned empty handle.
The caller must provide uninitialized mutable owning storage with `&`.
The adapter constructs a valid empty value before entering C. Native code
may replace that value only with runtime-owned storage through the designated
buffer assignment helpers, and must leave it initialized on every return,
including failure. The caller then owns its ordinary destruction obligation.
Foreign allocator storage must not be installed directly in such handles.

Exclusivity and initialization rules apply exactly as for ordinary Joyeer
calls. Native declarations for one symbol must agree on buffer shape and
access effect, not merely on the size of the flattened pointer arguments.
Descriptor layouts, helper names, and runtime revision matching are defined
in the [buffer ABI implementation contract](../impl/c-interop.md#buffer-imports).
There is no source-level raw-pointer value, asynchronous retention, callback,
or general aggregate FFI in this extension.

#### 19.4.4 Bare byte pointers

An import may pass a `[UInt8]` parameter as a bare data pointer, with no count.
The annotation names it: `@c_extern(symbol: "ReadFile", pointer: "buffer")`
names the parameter whose label is `buffer`. Each `pointer:` argument names a
distinct parameter, and the parameter must be a `[UInt8]` that is borrowing or
`inout`. Without `pointer:`, a borrowed `[UInt8]` keeps the pointer-and-count
form of §19.4.3, and an `inout` `[UInt8]` is rejected.

```joyeer
@c_extern(symbol: "read", pointer: "buffer")
func nativeRead(descriptor: Int32, buffer: inout [UInt8], count: Int): Int
```

A borrowed buffer crosses as `const uint8_t*`; an `inout` buffer as `uint8_t*`
to the array's own storage, which native code may write in place. The count of
bytes valid at that address is the array's `count`; the signature carries any
length the C function needs as an ordinary scalar parameter, and the binding
author must not let it exceed the array's count. The native function must not
retain the pointer, resize the array, or write beyond the count, and the
compiler cannot check that it does not. An empty array may pass a null pointer.
No NUL terminator is implied; a caller that needs a C string appends the `0`
byte itself. Exclusivity and initialization rules for `inout` are those of
ordinary calls. Repeated declarations of one symbol must agree on which
parameters are bare pointers and on their access effects.

This lets a binding fill C structures that are laid out in a `[UInt8]` and read
back with ordinary indexing. It is a deliberately small step toward the
byte address-and-length pairs of §19.10, remains runtime-only, and adds no
source-level pointer value, address arithmetic, or dereference.

#### 19.4.5 Compiler intrinsics

A C import whose symbol begins with `joyeer_intrinsic_` names a compiler
intrinsic. The compiler lowers it to inline instructions and never links a
symbol of that name, so the declaration is the only place the operation exists.
Every other rule for `@c_extern` applies unchanged: the declaration must be in
a unit authorized for native bindings, so ordinary programs cannot use these.
The symbol must be one of the table below, declared with exactly the listed
parameter and result types; anything else with the prefix is an error, and
`@c_export` of such a symbol is rejected. An address is an `Int`.

| Symbol | Declaration | Effect |
|---|---|---|
| `joyeer_intrinsic_address_of` | `(bytes: inout [UInt8]) -> Int`, with `pointer: "bytes"` | The address of the array's first element. The parameter must be `inout`: a borrowed array may be a constant in read-only memory, and a store through its address would be undefined |
| `joyeer_intrinsic_load_u8` / `_u16` / `_u32` / `_u64` | `(address: Int) -> UInt8` / `UInt16` / `UInt32` / `UInt64` | Little-endian load of that width |
| `joyeer_intrinsic_store_u8` / `_u16` / `_u32` / `_u64` | `(address: Int, value: UInt8` / `UInt16` / `UInt32` / `UInt64`) `-> Void` | Little-endian store of that width |
| `joyeer_intrinsic_atomic_add` | `(address: Int, delta: Int) -> Int` | Sequentially consistent add; returns the previous value |
| `joyeer_intrinsic_call_void1` | `(function: Int, first: Int) -> Void` | Calls the C function at `function`, which returns void and takes one address argument, with the target's C calling convention |
| `joyeer_intrinsic_call_void2` | `(function: Int, first: Int, second: Int) -> Void` | The same for a function taking two address arguments |
| `joyeer_intrinsic_literal_address` | `(text: String) -> Int` | The address of the NUL-terminated constant data of the string literal given as `text`, which must be a literal. No `String` value is created and nothing is allocated; it is the one use of a string literal a core unit (§19.4.6) may make |
| `joyeer_intrinsic_static_slot` | `(index: Int) -> Int` | The address of 8-byte, zero-initialized slot `index` of a 64-slot table shared by every unit in the program; an index outside `0..<64` traps (`static slot index out of range`). Slots are 8-byte aligned, so `atomic_add` applies. Slot ranges are assigned in the [runtime plan](../plan/runtime-in-joyeer.md) |

Loads and stores need no alignment. The atomic add requires an 8-byte-aligned
location. The operations assume a little-endian target; the supported targets
are. An address from `address_of` is meaningful only while the array it came
from is alive and unmodified in size, and the language does not track that.
Nothing checks that an address points at memory the program may access: these
operations can read and write anywhere, so a unit that uses them takes on the
obligations of C. They exist so that the runtime itself can be written in
Joyeer ([decision 0005](../decisions/0005-runtime-unsafe-memory.md)); they add
no pointer type, no address arithmetic beyond `Int`, and no dereference syntax.

#### 19.4.6 Accepted runtime capabilities (not yet implemented) ⏳

Writing the whole runtime in Joyeer ([runtime plan](../plan/runtime-in-joyeer.md))
needs operations beyond §19.4.5. The designs below are **accepted**
([decision 0009](../decisions/0009-runtime-core-capabilities.md)) but are **not
implemented**; until each lands, the compiler rejects it. All follow the rules of
the existing intrinsics: reserved symbols, exact signatures, and units authorized
for native bindings only.

| Capability | Declaration | Effect |
|---|---|---|
| Data symbol | `@c_data(symbol: "environ")` on `func environAddress(): Int` | The address of that C data symbol; repeated declarations must agree |
| Non-returning export | `@c_export` with result type `Never` | The export is `noreturn` |
| Program entry | `@c_export(symbol: "main")` or `"wmain"`, `(Int32, Int) -> Int32` | The C entry point; allowed in core units only, all other startup symbols stay rejected |

Still proposed, without a decision: bulk memory (`memcpy`, `memmove`, `memset`,
`memcmp`) and, for a runtime that does not depend on the C library, a Linux
system-call intrinsic.

A **core unit**, selected with `--runtime-core-module <name>` (which implies
`--runtime-module`), may use only `Bool`, the integer and floating types, `Void`
and top-level scalar constants. `String`, arrays, dictionaries, `Optional`,
`Result`, enums, structs, `print` and string literals (other than
as the argument of the constant-data intrinsic) are reported as
`type-checking.managed-type-in-core-unit`, because each lowers to a call into the
runtime that the core unit is itself part of. None of this changes ordinary
programs.

### 19.5 Invalid signatures

All types and parameter effects not admitted by §19.4 are rejected at this
boundary. In particular:

- `Void` parameters and `Never` results have no admitted C representation.
- `consuming` parameters are not substitutes for ownership transfer, and
  `inout` is admitted only for a `[UInt8]` named by `pointer:` (§19.4.4).
  `initializing` is restricted to §19.4.3's outputs.
- Other array forms, dictionaries, tuples, ordinary structs and enums,
  `Optional`, and `Result` have no implicit C ABI representation.
- Neither `T?` nor `Optional<T>` denotes a nullable C value or address.
- Function values, callbacks, variadic or unprototyped functions, C
  aggregates by value, unions, bit-fields, `long double`, complex numbers,
  vectors, and wider integers are not admitted.
- Native addresses and opaque handles that are actually pointers may not
  be disguised as `Int`, `UInt`, or another integer type. A native API whose
  handle is genuinely an integer may use its exact scalar representation;
  that does not supply resource lifetime or close semantics. Runtime units
  (§19.4.5) are the exception: they carry `FILE*`, `DIR*`, and other addresses
  as `Int` because they also own the matching close, and the compiler does not
  enforce this rule. Application code must not follow them.

In particular, `String` is not `char*`: its current implementation preserves
arbitrary bytes, including NUL and invalid UTF-8. Neither a string's storage
layout nor its longer-term text design grants a C-string conversion.
An explicit future C-string adapter must define encoding, terminators,
embedded-NUL handling, ownership, and lifetime.

### 19.6 Ownership, errors, and control flow

Scalar imports and exports do not transfer ownership of managed storage,
allocate on behalf of the binding mechanism, or synthesize copy/destruction
obligations. Their implementations may still allocate, perform I/O, fail,
or have other side effects. No purity, thread-safety, retention, or error
contract is inferred from a symbol's spelling.

Native error conventions remain explicit. A checked Joyeer wrapper translates
status values into `Result` or other ordinary types; an annotation does not
automatically inspect `errno`, Windows last-error state, sentinel values, or
Boolean results. If a wrapper needs native error state, it must capture it
before cleanup or another native call can overwrite it.

Neither direction permits a C++ exception, `longjmp`, or other nonlocal
unwinding across Joyeer frames. Recoverable failures use the declared scalar
protocol and explicit wrappers. A Joyeer panic retains its defined fatal
behavior; it must not become a fabricated C success value or an exception
escaping into C. Callbacks, asynchronous retention, and entry from foreign
threads require separate designs.

The binding author must match the real native prototype and its behavioral
contract. Without a header importer, checking the Joyeer declaration and
finding a linker symbol cannot prove that the native definition agrees.
A native contract violation is not made safe or recoverable by the annotation.

### 19.7 Linking and toolchain responsibilities

Source declarations specify callable signatures and C symbols. The build
system supplies native objects, libraries, search paths, SDKs, target identity,
and any required archive extraction or dynamic-export policy. Neither
annotation locates a library, compiles C, imports a header, or adds a native
dependency to a module manifest.

The selected provider may be static or dynamically linked without changing
the declaration. Library selection and runtime search/deployment policy
belong to the build target. Explicit runtime loading and symbol lookup
(`dlopen` or `LoadLibrary`) are separate, unintroduced capabilities.

Missing required symbols, incompatible target inputs, and duplicate
definitions are failures. No stub, alternate algorithm, or success-shaped
fallback may mask a missing binding. An export creates a C-linkable entry
in its object; it does not by itself force an archive member to be retained
or a symbol to appear in a dynamic library's public export table.

Bindings lower to one typed native-binding model carrying import/export kind,
C ABI, symbol, parameter/result representations, and source/module identity.
Adding a binding with an already-supported signature must not require a new
compiler special case for that particular C function. Preserve source spans
for diagnostics and debugging through semantic checking, IR verification,
and native emission.

C ABI compatibility is target-specific. This chapter does not promise a
stable Joyeer binary-module ABI, a third-party runtime SDK, or identical
layouts across platforms.

### 19.8 Required diagnostics

Implementations must report invalid annotation names/arguments, unauthorized
use, invalid declaration placement or body presence, missing explicit types,
unsupported signatures, conflicting symbol signatures, and duplicate exports
as errors with source locations. Unknown semantic annotations must not be
silently ignored as metadata. Native link failures must also be surfaced.

An incremental implementation may support fewer scalar types than §19.4
while development proceeds, but must diagnose every unsupported type instead
of approximating its ABI. Its implementation documentation must identify that
subset; passing the initial `Int`/`Void` gate does not establish full chapter
support.

### 19.9 Specification examples

The identity example in §19.2 has these C11 prototypes:

```c
#include <stdint.h>

int64_t joyeer_native_identity(int64_t value);
int64_t joyeer_rt_identity(int64_t value);
```

The broader scalar design also admits these declarations in an authorized
runtime context; they are not part of the plan's first implementation slice:

```joyeer
@c_extern(symbol: "joyeer_native_ready")
func nativeReady(): Bool

@c_extern(symbol: "joyeer_native_sample")
func nativeSample(value: Float, scale: Double): Double
```

Their C11 prototypes are:

```c
_Bool joyeer_native_ready(void);
double joyeer_native_sample(float value, double scale);
```

### 19.10 Deferred memory and ABI extensions

No address type, address-taking expression, dereference syntax, nullable
pointer syntax, or public `unsafe` block is introduced here. The runtime-only
`Int`-address intrinsics of §19.4.5 are not a pointer type.
The no-reference model in [§4.8](04-memory.md#48-no-reference-types) remains
unchanged. Raw addresses, general address-and-length pairs (§19.4.4 admits only
bare byte pointers), output parameters, and owned native result storage need a separate accepted
source representation and lifetime/initialization contract before they can
appear in these signatures.

The [memory-boundary proposal](../plan/c-interop.md#32-native-memory-design-needed-no-accepted-syntax)
tracks those decisions separately, including whether a mechanism remains
runtime-only. It is not an implicit approval of `Optional` pointer layouts,
C-compatible ordinary structs, callbacks, variadic calls, or a general
user-facing unsafe system.
