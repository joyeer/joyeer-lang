# Runtime Implementation

The runtime is declared in `src/runtime/runtime.h`. Almost all of it is Joyeer
(`src/runtime/joyeer/`): UTF-8 validation, string equality and ordering, owned strings,
arrays, dictionaries, the native buffer helpers, integer/`Bool`/string `print`,
`joyeer_panic`, the allocator with its leak counter, and the program entry. The
build-tree compiler compiles it into two objects (the core, and the entry with the C
`main`/`wmain`) in the same archive; `Float` and `Double` printing is still C in
`runtime.c` ([decision 0008](../decisions/0008-runtime-core-build-shape.md)). The
`JoyeerRuntime` static archive is linked into generated programs;
LLVM and LLD belong to the [compiler backend](backend.md), not the
program runtime. See [building](../building.md) for platform prerequisites.
The runtime is not offered as separately selectable `abi`, `core`, or `std`
profiles. Programs depend on the target's C runtime and startup/link inputs;
freestanding, kernel, embedded, and no-libc profiles are not supported.

The [runtime plan](../plan/runtime-in-joyeer.md) describes how the whole runtime moves
into Joyeer behind the existing runtime interfaces; this document
describes the current C implementation. Public library modules and their
separate fern workflow are tracked in the [library plan](../plan/library.md).
The [C binding implementation](c-interop.md) supports the restricted
by-value scalar runtime-source boundary of [§19](../spec/19-c-interop.md).
It does not replace any production C algorithm or compiler-known runtime
interface described here.

It supplies checked `Int` add/subtract/multiply/divide/remainder, scalar/string printing,
string storage and operations, arrays and dictionaries, scalar and string
printing, panic and bounds traps, and the process entry and allocation-balance
check. File, filesystem, process and standard-stream operations are in the
`System` module instead (see Host operations below).
Generated Joyeer arithmetic and typed array accesses use inline LLVM checks;
the runtime has no checked-arithmetic or array-indexing entry points of its own
(the former ones were removed in runtime plan stage 0). String and dictionary accesses use runtime
operations. The LLVM emitter generates `joyeer_main` and implements
`String.utf8()` using the runtime's owned-array creation API; neither is a
dedicated C runtime operation.

Scalar printing includes all integer widths, `Float`, and `Double`. Signed
integers use the existing 64-bit printer after sign extension; unsigned
integers use an unsigned printer after zero extension. Floating printers use
9 and 17 significant decimal digits, respectively. These internal printing
adaptations do not introduce implicit conversions at explicit C bindings.

Checked `Int` division truncates toward zero; a nonzero remainder has the
dividend's sign. The generated checks
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

The entry (`src/runtime/joyeer/entry.joyeer`, Joyeer) omits the executable name and preserves argument boundaries,
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

The runtime allocates through `joyeer_alloc`, `joyeer_realloc` and `joyeer_free`
(Joyeer, `allocator.joyeer`), which call libc and keep the leak counter. Collection allocations retain element
layout and clone/destroy callbacks. On the current 64-bit layout, private
headers occupy 24 bytes for arrays and 72 bytes for dictionaries, before
payload and allocator overhead. These callbacks can introduce indirect
calls without source-level protocol dispatch. Allocation/free updates a
relaxed atomic balance counter; this is accounting, not reference counting.

Joyeer IR `copy`, `take`, and `destroy` operations lower to generated per-type
LLVM helpers, which recurse through structs and tagged payloads and call
runtime string/collection operations. Scope lowering destroys owned storage
and temporaries in reverse order on normal and early-return paths. The
Joyeer entry checks the allocation count after the generated function returns and
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

## Host operations

File input, the portable filesystem, synchronous processes, standard-error
writing and standard-input line reading are not part of the C runtime. They are
ordinary Joyeer functions in the `System` standard module
([spec §18](../spec/18-host.md)): `readFile` (legacy), `readFileUtf8`,
`readFilePrefix`, `writeFileNew`, `createDirectory`, `listDirectory`,
`fileKind`, `removeFile`, `removeDirectory`, `joinPath`, `runProcess`,
`writeStderr` and `readStdinLine`. A program writes `import System`. The
compiler knows no host function; it declares only the error and status
enumerations they return (`IOError`, `FileSystemError`, `FileKind`,
`ProcessStatus`, `ProcessError`, `StderrError`, `StdinError`) as prelude types,
so user code and `System` share one definition.

`System` reaches the operating system through `@c_extern` bindings, authorized
by the module's `native-bindings = true` ([§19](../spec/19-c-interop.md)): the
Win32 API on Windows and libc on POSIX, selected per function by `@platform`.
Per-platform files hold the primitives and one portable file holds the logic;
see [decision 0003](../decisions/0003-pure-joyeer-filesystem.md),
[0006](../decisions/0006-runprocess-in-joyeer.md) and
[0007](../decisions/0007-stdio-in-joyeer.md). Paths are validated for UTF-8, an
embedded NUL and emptiness before the host is asked, and become UTF-16 on
Windows and UTF-8 on POSIX. Contents are never validated and remain arbitrary
bytes.

What the runtime still provides to them:

| Symbol | Purpose |
|---|---|
| `joyeer_native_errno()` | The calling thread's `errno`, which a binding cannot dereference |
| `joyeer_native_environment()` | The address of `environ`, for `posix_spawn` |
| `joyeer_native_stdio(which)` | The `FILE*` for standard input, output or error |
| `joyeer_string_clone_abi` and the array API | Construct owned `String` and `[String]` results |
| `joyeer_host_valid_utf8` | Strict UTF-8 validation, shared with the Compression native unit |

Results use the ordinary tracked runtime allocation and recursive destruction.
Failures carry the original platform code; categories are mapped to the enum
cases in Joyeer. `readStdinLine` holds at most `maximumBytes + 1` bytes of a
line, flushes standard output before it blocks, and on Windows switches
standard input to binary mode. `writeStderr` writes exactly its bytes without a
trailing newline and flushes. Both use the C library's own streams, so
buffering, read-ahead and `errno` codes are the program's. The Windows guard
against a closed descriptor checks the Win32 standard handle first, because
Joyeer cannot install the CRT's invalid-parameter handler.

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


Focused test labels include `runtime`, `native`, `file-io`, `filesystem`,
`process`, `stdin` and `host`.
Native tests verify string/collection ownership, runtime traps, argument
boundaries, return statuses, and zero final allocation balance. Entry tests
cover both `main` forms, empty and spaced arguments, Unicode arguments,
POSIX raw bytes, normal and out-of-range statuses, and missing, duplicate,
or invalid signatures. Windows-only entry tests reject unpaired high and low
UTF-16 surrogates and verify supplementary Unicode conversion and cleanup.
The [native executable test helper](../../scripts/cmake/verifyNativeExecutable.cmake)
captures Joyeer compiler and program output with explicit UTF-8 encoding.
On Windows this avoids CMake's policy-dependent `AUTO` decoding through the
console code page; it does not change the program's output bytes or the
console code page.

The host operations are tested through the language. `SystemTests`
([tests/library/System](../../tests/library/System)) runs under `fern test` and
states the expected result of each scenario, rendered by error case rather than
platform code, with the Windows and POSIX values or alternatives where the
platforms genuinely differ. It covers Unicode and malformed paths, create-new
preservation, classification and enumeration, empty-directory removal, argument
quoting, completion statuses, standard-error bytes and mode restoration, line
splitting and limits, the stdout flush before a read, and closed descriptors.
Native fixtures (`tests/native/filesystem.joyeer`, `process.joyeer`,
`read_file.joyeer`, `stdin_lines.joyeer`) and the module fixtures under
`tests/modules/host_*` exercise concrete result types, early propagation, owned
outputs, unit successes, optimization levels and full debug emission. Compiler
unit tests check that the host enumerations are prelude types and that no host
function is.
Many native executable tests are registered only when `JOYEER_CLANG_EXECUTABLE`
was found during CMake configuration; see
[backend validation](backend.md#6-validation) before treating a label
run as the complete acceptance suite.

`NativeExecutableStdinLinesO0`/`O2` feed standard input to a compiled program.
Moss's `Moss.Serve` test and the live-pipe check in
[the serve contract](../../tools/moss/docs/jvm.md#persistent-session-moss-serve)
depend on `readStdinLine` flushing standard output before it blocks.
