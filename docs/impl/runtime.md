# Runtime Implementation

The C11 runtime is declared in `include/joyeer/native/runtime.h` and
implemented in `lib/native/runtime.c` and `lib/native/entry.c`. The
`JoyeerNativeRuntime` static archive is linked into generated programs;
LLVM and LLD belong to the [compiler backend](backend.md), not the
program runtime. See [building](../building.md) for platform prerequisites.
The runtime is not offered as separately selectable `abi`, `core`, or `std`
profiles. Programs depend on the target's C runtime and startup/link inputs;
freestanding, kernel, embedded, and no-libc profiles are not supported.

It supplies checked `Int` add/subtract/multiply, scalar/string printing,
string storage and operations, arrays and dictionaries, binary file input,
panic and bounds traps, and the process entry and allocation-balance check.
Generated Joyeer arithmetic and typed array accesses use inline LLVM checks;
the checked arithmetic and generic array-indexing C entry points remain
available for runtime clients. String and dictionary accesses use runtime
operations. The LLVM emitter generates `joyeer_main` and implements
`String.utf8()` using the runtime's owned-array creation API; neither is a
dedicated C runtime operation.

## Process entry and exit

The compiler accepts exactly one `func main()` returning `Void` or
`func main(args: [String]): Int` with a borrowing `args` parameter. It emits
C-callable `joyeer_main_uses_arguments` and `joyeer_main` wrappers; the latter
receives a pointer to runtime-owned argument storage and returns an `int64_t`
status (zero for parameterless `main`). No collection is passed by value
across the generated-module/runtime boundary. Missing and invalid entry
signatures produce compiler diagnostics, not platform linker errors.

The C entry omits the executable name and preserves argument boundaries,
empty arguments, and spaces. POSIX arguments preserve their original bytes.
Windows uses `wmain` and converts UTF-16 arguments to UTF-8, rejecting invalid
encoding. The runtime owns the array until the Joyeer function has returned
and cleaned its local values and temporaries. It then destroys the argument
storage and checks the runtime-managed allocation balance. Leaks fail the
process; otherwise an exit status in `0..255` is returned. An out-of-range
`Int` is reported as a runtime error and fails instead of being truncated.

## Value storage and ownership

String concatenation, equality, ordering, byte indexing, deep cloning, and
destruction use runtime storage. See the [string implementation](string.md)
for byte representation, UTF-8 limitations, and value semantics. The
prelude's `byteToInt(value:)` zero-extends `UInt8` to signed 64-bit `Int`;
`byteToString(value:)` creates an owned byte using
`joyeer_byte_to_string_abi`, so normal cleanup applies.

The runtime uses libc allocation. Collection allocations retain element
layout and clone/destroy callbacks. On the current 64-bit layout, private
headers occupy 24 bytes for arrays and 72 bytes for dictionaries, before
payload and allocator overhead. These callbacks can introduce indirect
calls without source-level protocol dispatch. Allocation/free updates a
relaxed atomic balance counter; this is accounting, not reference counting.

Joyeer IR `copy`, `take`, and `destroy` operations lower to generated per-type
LLVM helpers, which recurse through structs and tagged payloads and call
runtime string/collection operations. Scope lowering destroys owned storage
and temporaries in reverse order on normal and early-return paths. The C
entry checks the allocation count after the generated function returns and
its borrowed argument storage is destroyed, making integration-test leaks
observable. The balance covers runtime-managed storage, not arbitrary future
unsafe/native allocations; zero outstanding allocations does not mean the
program was allocation-free.

### Collections

Arrays provide construction, checked indexing, mutable element projection,
ownership-transferring append/growth, recursive clone, and reverse-order
element destruction. The built-in
`&values.append(element: value)` requires an addressable mutable `Array<T>`.
Lowering transfers a type-correct `T` to `joyeer_array_append_owned_abi`;
the runtime grows geometrically and retains element clone/destroy callbacks.
Borrowed heap-backed elements are cloned before transfer; owned temporaries
move directly into the array.

Dictionary lookup is linear, not hashed. Dictionaries provide construction,
count, primitive/string-key lookup, mutable insert/update growth, recursive
key/value clone, and destruction. Mutable `&dictionary[key] = value` lowers
to `joyeer_dictionary_set_owned_abi`. Missing keys take both key and value.
Existing keys retain their original key storage, destroy the incoming
duplicate key and previous value, and take the replacement value without
changing `count`.

`Dict.get(key:)` uses nontrapping `joyeer_dictionary_find_abi`: a borrowed
value address indicates a present key, and null indicates absence. Invalid
handles or key types still trap. This private pointer never escapes to Joyeer
source. Generated code constructs `Optional<V>` and clones the present value
using the existing typed copy helpers; neither the receiver nor an absent
payload is cloned. Zero-sized values have a non-null presence address but
require no payload load. Strict `joyeer_dictionary_at_abi` retains its
missing-key trap and shares the single-pass lookup.

Construction uses the same replacement policy: equal keys retain the first
key storage and the last supplied value, and `count` includes only unique
keys. Construction compacts entries in place and destroys discarded keys
and replaced values exactly once. Cloning a normalized dictionary preserves
its count and independently clones its entries without repeating duplicate
key detection.

### Unit values

Source `Void` values use LLVM's zero-sized `{}` type in value and storage
positions. Ordinary `Void` functions still return LLVM `void`; the separate
C-callable `joyeer_main(ptr)` wrapper returns `i64`, including zero for a
parameterless source `main()`. Unit constants require no instructions; unit
loads, stores, takes, and enum payload reads/writes need no data access.
Logical storage and full-debug metadata still describe unit bindings.

`Result<Void, E>` retains the ordinary enum tag and error storage. Tag-directed
ownership helpers clean an active nontrivial error payload but not a unit
success payload. Arrays and dictionaries can store zero-sized unit values
while retaining their metadata, allocation, count, and bounds behavior. No
new runtime allocation or destruction API is introduced for unit values.

## File input

The current prelude exposes:

```joyeer
readFile(path: String): Result<String, IOError>
```

`Ok` contains an owned, byte-preserving `String`, including embedded NUL
bytes. `Err` contains `.NotFound(code)`, `.PermissionDenied(code)`,
`.InvalidPath(code)`, or `.Other(code)` with a stable category and the nonzero
platform C I/O error code. A function returning `Result<U, IOError>` may use
`readFile(path: path)?` to propagate the same error type; an entry returning
`Int` must handle its final result explicitly.

Byte preservation does not establish valid UTF-8. The reader grows from a
4096-byte buffer and retains spare capacity on success; `count` is the
logical byte length, not allocated capacity. LLVM passes the path as
pointer/count with owned-string and error-code out-pointers to
`joyeer_read_file_abi`. The runtime returns a stable C ABI category; LLVM
constructs the concrete `IOError` and `Result` tags without making the
runtime depend on frontend case ordering or aggregate layout. A path with
embedded NUL produces `InvalidPath`. Windows currently uses the active
narrow-character CRT path encoding; a portable Unicode path API remains
future work.

## Limits and validation

The native pipeline supports compiler-known heap-backed values and recursive
aggregates; user-defined `deinit`, noncopyable user types, and explicit copy
initializers are not implemented. `print` supports primitives and strings,
not arbitrary aggregates. File input is synchronous and whole-file only;
streaming, writing, and metadata APIs are not available. See the
[backend limitations](backend.md#5-known-limitations) and
[IR verification bounds](ir.md#5-verification) for other gaps.

Panic flushes stderr and uses C11 `_Exit` with a nonzero status, avoiding
platform crash dialogs while remaining unrecoverable.

Focused test labels are `native-runtime`, `native`, and `file-io`.
Native tests verify string/collection ownership, runtime traps, argument
boundaries, return statuses, and zero final allocation balance. Entry tests
cover both `main` forms, empty and spaced arguments, Unicode arguments,
POSIX raw bytes, normal and out-of-range statuses, and missing, duplicate,
or invalid signatures. An invalid UTF-16 startup argument still needs a
Windows-only test. File-input tests cover binary data and missing files.
Existing ownership-heavy native tests run at the default `-O2` and retain
the zero-allocation-balance check. Many native executable tests are registered
only when `JOYEER_CLANG_EXECUTABLE` was found during CMake configuration; see
[backend validation](backend.md#6-validation) before treating a label
run as the complete acceptance suite.
