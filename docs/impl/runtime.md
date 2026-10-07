# Runtime Implementation

The C11 runtime is declared in `include/joyeer/native/runtime.h`, with portable
host ABIs in `include/joyeer/native/filesystem.h` and
`include/joyeer/native/process.h`. Its implementation lives under
`lib/native/`, including shared strict UTF-8 host conversion. The
`JoyeerNativeRuntime` static archive is linked into generated programs;
LLVM and LLD belong to the [compiler backend](backend.md), not the
program runtime. See [building](../building.md) for platform prerequisites.
The runtime is not offered as separately selectable `abi`, `core`, or `std`
profiles. Programs depend on the target's C runtime and startup/link inputs;
freestanding, kernel, embedded, and no-libc profiles are not supported.

It supplies checked `Int` add/subtract/multiply/divide/remainder, scalar/string printing,
string storage and operations, arrays and dictionaries, binary file input,
portable filesystem operations, synchronous processes, fallible byte-exact
standard-error writing, panic and bounds
traps, and the process entry and allocation-balance check.
Generated Joyeer arithmetic and typed array accesses use inline LLVM checks;
the checked arithmetic and generic array-indexing C entry points remain
available for runtime clients. String and dictionary accesses use runtime
operations. The LLVM emitter generates `joyeer_main` and implements
`String.utf8()` using the runtime's owned-array creation API; neither is a
dedicated C runtime operation.

Checked `Int` division truncates toward zero; a nonzero remainder has the
dividend's sign. Both `joyeer_checked_div_int` and `joyeer_checked_rem_int`
panic on a zero divisor or `INT64_MIN` with divisor `-1`, before evaluating
the C arithmetic operation. The generated LLVM checks have the same behavior
at every optimization level. Diagnostics distinguish `integer division by
zero`, `integer remainder by zero`, `integer division overflow`, and `integer
remainder overflow`.

## Process entry and exit

Native executables require exactly one root-module entry: `func main()`
returning `Void` or `func main(args: [String]): Int` with a borrowing `args`
parameter. A single-file input is its own root module; dependency functions
named `main` remain ordinary functions and are not selected as entries. The compiler emits
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
narrow-character CRT path encoding. This compatibility function is unchanged;
new code can use the separate portable filesystem interface below.

## Portable filesystem and process boundaries

The source signatures and compatibility rules are specified in
[portable host operations](../spec/18-host.md). The new functions are
compiler-known borrowing operations, not source-language FFI. Filesystem
operations return `Result<T, FileSystemError>`; `runProcess` returns
`Result<ProcessStatus, ProcessError>`. The existing `IOError` cases and
`readFile` result type are not extended or converted implicitly.

`host.c` validates Unicode scalar UTF-8, excluding overlong sequences,
surrogates, truncated sequences, and code points beyond U+10FFFF. Host string
conversion additionally rejects embedded NUL and empty paths, while allowing
empty process arguments. Windows conversion uses strict UTF-8/UTF-16 APIs.
File contents are not passed through path validation and remain arbitrary
bytes.

The private runtime ABI uses flat data/count inputs, scalar error-kind
returns, and output pointers. No platform-dependent C aggregate is passed or
returned by value. Runtime tags are stable constants in the host headers;
the LLVM emitter maps them by source enum case name, rather than assuming
that a source enum's declaration order is its C ABI representation.

| Operation group | Runtime outputs on success |
|---|---|
| File read and lexical path join | Owned `JoyeerString` |
| Directory listing | Owned `JoyeerArray` of owned `JoyeerString` names |
| File classification | Scalar `JoyeerFileKind` |
| Create-new write, mkdir, file removal, empty-directory removal | No payload; the compiler constructs `Ok(())` |
| Synchronous process | Scalar completion kind and full signed-64-bit status storage |

Failures return a stable category and original platform code, without
publishing a partially owned success value. Returned strings and arrays use
the ordinary tracked runtime allocation and recursive destruction paths.
Host conversion buffers and OS resources have shorter native lifetimes and
are released by the host operation.

### Bounded prefix input

`readFilePrefix(path: String, maximumBytes: Int): Result<String, FileSystemError>`
uses the stable exported C ABI
`int32_t joyeer_fs_read_file_prefix_abi(JoyeerString* result, int64_t* errorCode, const uint8_t* pathData, int64_t pathCount, int64_t maximumBytes)`.
The string output is a pointer to runtime data/count storage, followed by the
error-code pointer, borrowed path data/count, and the scalar signed-64-bit
limit. No string or `Result` aggregate is passed by value. On success the
runtime returns zero, clears the error code, and transfers one owned string;
on failure the output stays empty and the original platform code is retained.

The host descriptor declares concrete `String`/`Int` parameters and a
`Result<String, FileSystemError>` result. `ValueKind::integer` maps to `Int`
in name resolution and type checking. LLVM's flat host-call lowering matches
an integer and emits one `i64` operand, rather than a string/array data-count
pair. Those scalar branches are integrated and compiled in the Windows x64
Debug build. Ordinary Joyeer IR host-call collection consumes the descriptor;
this needs no new IR opcode, source-language FFI, or user-defined generic
support. The added compiler/runtime regressions have not executed under CTest.

The shared native reader keeps `readFileUtf8`'s unbounded growth and final-copy
behavior unchanged. Prefix reads use the same strict UTF-8 path conversion,
wide Windows `CreateFileW`/`ReadFile`, and narrow POSIX `open`/`read` behavior,
including interrupted-read retry and resource cleanup. They read at most the
requested bytes without querying a file size, reading the tail, or probing
another byte after the cap. Contents are not decoded or UTF-8 validated; a
prefix can cut a multibyte sequence. A zero limit still opens and closes the
path before returning an owned empty string. The prefix reader does not itself
require a regular file; joypm's loader classifies the manifest first and
rejects final symlinks and other nonregular entries. Negative limits return the
existing `Other` tag with `ERROR_INVALID_PARAMETER` or `EINVAL` before path
conversion, deliberately bypassing the invalid-path error mapper.

Prefix storage starts as a one-byte tracked empty-string allocation, grows
geometrically from at most 4096 bytes, and clamps every growth to
`maximumBytes`. Reallocation preserves that single tracked libc block, which
is transferred directly to the output without a second content copy. Content
capacity cannot exceed the limit (one byte for an empty result); path
conversion storage and native/allocator bookkeeping are separate. Early EOF
can retain spare capacity within the cap. Read or close failure destroys this
storage without publishing partial success. Ordinary
`joyeer_string_destroy_abi` balances successful output ownership.

For a 65,536-byte manifest limit, the caller requests 65,537 bytes and rejects
a returned count above 65,536. A result exactly as long as the requested cap
does not distinguish an exact-size file from a larger file. The caller should
classify tool inputs using `fileKind` before reading to reject directories,
links, and other nonregular entries; that check is not race-free confinement.
This interface remains synchronous and bounded, not streaming. The legacy
`readFile`/`IOError` boundary and all existing error enum cases are unchanged.

### Other portable operations

Exclusive creation enforces `writeFileNew`'s nonreplacement contract at the
OS operation. A failed write can leave its own partially created file; it
must not remove or replace an unrelated preexisting destination.
`fileKind` examines the final link itself, `removeFile` unlinks the final
entry, and `removeDirectory` handles only empty real directories. There is
no recursive cleanup API or confinement guarantee.
On Windows, ordinary-looking file attributes are not sufficient: DOS devices
such as `NUL` can advertise them. Non-directory, non-reparse entries are
classified through a metadata-only handle and its disk-file information.
Character/pipe devices return `Other` without reading; query/open/close errors
remain fallible host errors. Metadata inspection does not require data access,
including for an exclusively opened ordinary file.

Processes use an explicit executable path, an argument array, and a child
working directory, without shell evaluation or PATH fallback. The runtime
preserves native nonzero completion separately from launch/wait errors.
Windows status values are widened without signed-32-bit truncation; POSIX
signal completion remains distinct. Native standard streams and environment
are inherited, and the parent's working directory is unchanged.

## Standard-error boundary

The exact source signature is
`writeStderr(contents: String): Result<Void, StderrError>`, with the single
`StderrError.WriteFailed(Int)` case. It is a compiler-known borrowing operation
described by the same host builtin table as filesystem/process operations.
Name resolution, concrete result typing, Joyeer IR lowering, and LLVM emission
consume that descriptor; there is no dedicated stderr IR instruction or second
error conversion path. The existing unit-result machinery constructs `Ok(())`
without loading or storing LLVM `void`.

The stable C ABI is
`int32_t joyeer_write_stderr_abi(int64_t* errorCode, const uint8_t* data, int64_t count)`.
It returns `0` with `*errorCode = 0` on success, or `1` with the native CRT
`errno` for `WriteFailed` (falling back to `EIO` when none was supplied). LLVM
maps that stable error kind to the source enum by case name. The runtime never
receives a `Result` aggregate or owns the borrowed contents, and makes no
runtime-tracked allocation.

The implementation uses a byte-counted `fwrite`, appends nothing, and checks
both short writes and `fflush(stderr)`. Empty contents still flush the stream.
The first observed failure is retained even if a subsequent flush also fails.
On Windows, pending CRT output is flushed before switching temporarily to
`_O_BINARY`; the previous mode is restored after the write/flush, including
the failure path. Mode failures are fallible CRT errors too. NUL, invalid UTF-8,
and newline bytes are not transformed. This does not modify global console
code pages or establish how a terminal displays arbitrary bytes.

Null error-code storage, a negative count, a count unrepresentable as `size_t`,
or null data with a nonzero count is ABI misuse and panics, matching the other
host write/owned-value boundaries. Null data with count zero is valid.
Failures may leave partial output; no rollback or message-level atomicity is
guaranteed. Native signals (including default POSIX `SIGPIPE`) and concurrent
foreign CRT mode/stream reconfiguration remain host concerns.

## Limits and validation

The native pipeline supports compiler-known heap-backed values and recursive
aggregates; user-defined `deinit`, noncopyable user types, and explicit copy
initializers are not implemented. `print` supports primitives and strings,
not arbitrary aggregates. File input is synchronous, with whole-file reads and
the bounded `readFilePrefix` interface; streaming is not available. Portable
writing is create-new only; classification
does not expose general metadata. Replacement, recursive cleanup, process
capture, timeouts, and asynchronous handles remain outside this interface.
See the
[backend limitations](backend.md#5-known-limitations) and
[IR verification bounds](ir.md#5-verification) for other gaps.

Panic flushes stderr and uses C11 `_Exit` with a nonzero status, avoiding
platform crash dialogs while remaining unrecoverable.

Focused test labels include `native-runtime`, `native`, `file-io`, `filesystem`,
`process`, and `host`.
Native tests verify string/collection ownership, runtime traps, argument
boundaries, return statuses, and zero final allocation balance. Entry tests
cover both `main` forms, empty and spaced arguments, Unicode arguments,
POSIX raw bytes, normal and out-of-range statuses, and missing, duplicate,
or invalid signatures. Windows-only entry tests reject unpaired high and low
UTF-16 surrogates and verify supplementary Unicode conversion and cleanup.
File-input tests cover binary data and missing files.
Stderr unit coverage in the existing runtime test target checks empty writes,
binary bytes including NUL/invalid UTF-8/newlines, no appended newline, Windows
CRT mode restoration, write and buffered/empty flush failures with the native
CRT error code, and misuse traps. Failure injection is isolated in death-test
subprocesses rather than modifying the test runner's standard-error descriptor.
Portable host tests cover Unicode, malformed encoding, create-new
preservation, classification/enumeration, empty-directory removal, link-target
preservation, argument quoting, completion statuses, inherited streams and
environment, explicit executable resolution, and unchanged parent directories.
Prefix unit coverage includes zero/partial/exact/greater-than-EOF limits,
arbitrary binary bytes and cuts inside UTF-8, clamped growth and the 65,537-byte
manifest sentinel, negative-limit platform errors, opening/validation at zero,
and allocation balances on success and failure. Compiler coverage checks the
descriptor, exact `String`/`Int` signature, unchanged exhaustive filesystem
errors, same-error propagation, rejection of incorrect types/error conversions,
verified IR scalar operands, and the flat prefix ABI with full debug emission.
Source-level host fixtures exercise concrete result types, early propagation,
owned outputs, unit successes, optimization, and full debug emission.
Host compiler unit tests additionally exercise `StderrError` construction and
matching, same-error propagation, rejection of implicit error conversions,
the flat stderr ABI declaration/call, and full-debug unit-result emission.
Existing ownership-heavy native tests run at the default `-O2` and retain
the zero-allocation-balance check. Many native executable tests are registered
only when `JOYEER_CLANG_EXECUTABLE` was found during CMake configuration; see
[backend validation](backend.md#6-validation) before treating a label
run as the complete acceptance suite.
