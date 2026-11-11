# Runtime Implementation

The runtime is declared in `lib/Runtime/include/joyeer/runtime.h`. Almost all of it is Joyeer
(`lib/Runtime/core/`): UTF-8 validation, string equality and ordering, owned strings,
arrays, dictionaries, the native buffer helpers, `print` for every scalar,
`joyeer_panic`, the allocator with its leak counter, and the program entry. The
build-tree compiler compiles it into four objects in the same archive (the core; the entry
logic with the Windows `joyeer_start`; the C `main`/`wmain` wrappers; and the thread, mutex and
condition functions, [decision 0016](../decisions/0016-thread-primitives.md)); there is no C source left in the runtime ([decision 0008](../decisions/0008-runtime-core-build-shape.md)). The
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
by-value scalar runtime-source boundary of [§19](../../specs/19-c-interop.md).
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
9 and 17 significant decimal digits, respectively, written exactly like the C
library's `%.9g` and `%.17g` ([decision 0010](../decisions/0010-floating-point-printing.md)). These internal printing
adaptations do not introduce implicit conversions at explicit C bindings.

Checked `Int` division truncates toward zero; a nonzero remainder has the
dividend's sign. The generated checks
panic on a zero divisor or `INT64_MIN` with divisor `-1`, before evaluating
the C arithmetic operation. The generated LLVM checks have the same behavior
at every optimization level. Diagnostics distinguish `integer division by
zero`, `integer remainder by zero`, `integer division overflow`, and `integer
remainder overflow`.

## Threads

`lib/Runtime/core/threads.joyeer` is the fourth archive object (decision 0016): thread, mutex and
condition functions with the C ABI declared in `runtime.h` (`joyeer_thread_start_abi`,
`joyeer_thread_join_abi`, `joyeer_mutex_*_abi`, `joyeer_condition_*_abi`). Windows uses
`CreateThread`, `SRWLOCK` and `CONDITION_VARIABLE` from `kernel32`; Linux and macOS use `pthread`
through the C library the program already links. The caller supplies the storage (128 bytes for a
thread, 64 for a mutex or condition, all zeroed), so the functions never allocate. A thread runs a
`void entry(void *argument)` through a private trampoline with the platform's thread-routine
shape, with an explicit 8 MiB stack on every platform, and at most 256 threads may be unjoined at
once (`JOYEER_THREAD_LIMIT` beyond that). The rest of the runtime is thread safe from P4 on (next section).

On Linux and on the Windows own-entry link the object is linked only into a program that
references it. The Windows C-startup link (`/WHOLEARCHIVE`) and macOS (`-force_load`) load every
member, so those programs also import the thread functions. The tests are
`tests/unittests/runtime/thread_test.cpp` (start and join, many threads, visibility after join,
mutual exclusion, wait and wake, the thread limit, null arguments, and `static_assert`s that the
operating systems' lock types fit the storage) and the `RuntimeThreadsObjectSymbols` gate.

## Thread safety

Decision [0017](../decisions/0017-runtime-thread-safety.md). Every runtime function may be called
from any started thread. Locks are taken only while the count of unjoined threads (slot 11) is
non-zero, so a single-threaded program pays one load and one branch per lock site. The locks
(`core/locks.joyeer`) sleep: a futex mutex on Linux (built on the atomic compare-and-exchange and
exchange intrinsics), `SRWLOCK` on Windows, `os_unfair_lock` on macOS. The allocator has one lock
(slot 12); standard output (slot 13) and standard input (slot 14) have separate locks and state
blocks. A `print` is atomic. The first panic claims slot 38; later panics, and any thread that
reaches a lock afterwards, park, so the message appears once with status 1. Returning from `main`
with an unjoined thread is a runtime error. `RuntimeTestsThreadedMode` reruns the runtime suite with
a parked thread so every locked path executes.

## Static slots

Sixty-four eight-byte, zero-initialised words shared by every unit of a program
(`joyeer_intrinsic_static_slot`). The assignment, in one place:

| Slots | Owner | Use |
|---|---|---|
| 0 | allocator | the leak counter |
| 1 | allocator | the allocator state address |
| 2 | streams | the output state block |
| 3 to 7 | streams | output system-call scratch |
| 8 | streams | input system-call scratch |
| 9 | panic | panic scratch |
| 10 | | free |
| 11 | threads | the number of unjoined threads |
| 12 | allocator | the allocator lock |
| 13 | streams | the output lock |
| 14 | streams | the input lock |
| 15 | streams | the input state block |
| 16 | entry | the process environment block address |
| 17 to 31 | System | reserved for the `System` module |
| 32 to 33 | entry | string scratch |
| 34 to 36 | entry | the argument handle |
| 37 | entry | the byte buffer |
| 38 | panic | the panic flag |
| 39 to 63 | | free (tests use 40, 41, 60 and above) |

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

The entry (`lib/Runtime/core/entry.joyeer`, Joyeer) omits the executable name and preserves argument boundaries,
empty arguments, and spaces. POSIX arguments preserve their original bytes.
Windows converts UTF-16 arguments to UTF-8, rejecting invalid
encoding. A Windows program that calls no C library starts at `joyeer_start`, which splits
`GetCommandLineW` by the C runtime's rules and never initialises the C runtime; one that does,
or that links a native library, keeps the C startup and `wmain` (the linker reads the program
object's undefined symbols to decide, [decision 0011](../decisions/0011-runtime-without-the-c-library.md)). The runtime owns the array until the Joyeer function has returned
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
(Joyeer, `allocator.joyeer`), a size-class allocator over `VirtualAlloc` / `mmap` that keeps the leak counter
(decision [0011](../decisions/0011-runtime-without-the-c-library.md)). Collection allocations retain element
layout and clone/destroy callbacks. On the current 64-bit layout, private
headers occupy 24 bytes for arrays and 72 bytes for dictionaries, before
payload and allocator overhead. These callbacks can introduce indirect
calls without source-level protocol dispatch. Allocation/free updates a
sequentially consistent atomic balance counter; this is accounting, not reference counting.

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
`values.removeLast()` calls `joyeer_array_remove_last_abi`, which moves the last element's bytes into the
result payload and shortens the array without destroying the element.
`consume values.consumeEmpty()` calls `joyeer_array_consume_empty_abi`, which traps (`array not empty`)
unless the count is zero and otherwise frees the storage and zeroes the handle.
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
([spec §18](../../specs/18-host.md)): `readFile` (legacy), `readFileUtf8`,
`readFilePrefix`, `writeFileNew`, `createDirectory`, `listDirectory`,
`fileKind`, `removeFile`, `removeDirectory`, `joinPath`, `runProcess`,
`writeStderr` and `readStdinLine`. A program writes `import System`. The
compiler knows no host function; it declares only the error and status
enumerations they return (`IOError`, `FileSystemError`, `FileKind`,
`ProcessStatus`, `ProcessError`, `StderrError`, `StdinError`) as prelude types,
so user code and `System` share one definition.

`System` reaches the operating system through `@c_extern` bindings, authorized
by the module's `native-bindings = true` ([§19](../../specs/19-c-interop.md)): the
Win32 API on Windows, the kernel on Linux and libSystem on macOS, selected per function by
`@platform`.
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
| `joyeer_stderr_write`, `joyeer_stdout_flush`, `joyeer_stdin_read_byte`, `joyeer_stdin_discard` | The standard streams, written straight over the operating system (`streams.joyeer`) |
| `joyeer_string_clone_abi` and the array API | Construct owned `String` and `[String]` results |
| `joyeer_host_valid_utf8` | Strict UTF-8 validation, shared with the Compression native unit |

Results use the ordinary tracked runtime allocation and recursive destruction.
Failures carry the original platform code; categories are mapped to the enum
cases in Joyeer. `readStdinLine` holds at most `maximumBytes + 1` bytes of a
line, flushes standard output before it blocks, and reads bytes exactly (no
text-mode translation, as nothing translates). `writeStderr` writes exactly its
bytes without a trailing newline, at once. Both use the runtime's own streams, which
talk to the operating system directly (decision
[0011](../decisions/0011-runtime-without-the-c-library.md)), so no C library buffer,
mode or `errno` is involved; failures carry errno-style codes (9, 13, 28, 32, 5). On Windows
the whole of `System` now calls only `kernel32`, including the legacy narrow-path
`readFile` (`CreateFileA`, with the C library's mapping of system errors to `errno`
codes). On Linux the file, directory and process operations are system calls
(`filesystem_linux.joyeer`, `process_linux.joyeer`); macOS calls libSystem.

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

Panic writes any buffered standard output, then its message to standard error, directly
through the system interface (`WriteFile`, the Linux `write` system call, libSystem), and ends the
process with `ExitProcess`, `exit_group` or `_exit` and status 1, avoiding platform crash dialogs
while remaining unrecoverable. `print` and a panic share one standard output buffer
([decision 0011](../decisions/0011-runtime-without-the-c-library.md)).


Focused test labels include `runtime`, `e2e`, `native`, `file-io`, `filesystem`,
`process`, `stdin` and `host`.
End-to-end tests verify string/collection ownership, runtime traps, argument
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
([tests/lib/System](../../tests/lib/System)) runs under `fern test` and
states the expected result of each scenario, rendered by error case rather than
platform code, with the Windows and POSIX values or alternatives where the
platforms genuinely differ. It covers Unicode and malformed paths, create-new
preservation, classification and enumeration, empty-directory removal, argument
quoting, completion statuses, standard-error bytes and mode restoration, line
splitting and limits, the stdout flush before a read, and closed descriptors.
End-to-end fixtures (`tests/e2e/filesystem.joyeer`, `process.joyeer`,
`read_file.joyeer`, `stdin_lines.joyeer`) and the module fixtures under
`tests/modules/host_*` exercise concrete result types, early propagation, owned
outputs, unit successes, optimization levels and full debug emission. Compiler
unit tests check that the host enumerations are prelude types and that no host
function is.
Many end-to-end tests are registered only when `JOYEER_CLANG_EXECUTABLE`
was found during CMake configuration; see
[backend validation](backend.md#6-validation) before treating a label
run as the complete acceptance suite.

`E2EExecutableStdinLinesO0`/`O2` feed standard input to a compiled program.
Moss's `Moss.Serve` test and the live-pipe check in
[the serve contract](../../tools/moss/docs/jvm.md#persistent-session-moss-serve)
depend on `readStdinLine` flushing standard output before it blocks.

## Channels

`lib/Runtime/core/channels.joyeer` is the fifth archive object (decision [0019](../decisions/0019-channels.md),
spec §21.9), linked only into a program that creates a channel (which also links the thread object and
`-lpthread`). One allocation holds the channel block: a mutex, two conditions, the capacity, stride, head,
count, element destroy callback, a closed flag per end and the ring buffer; the receiver handle is the block
address plus one. Everything, including the "last closer frees the block" decision, happens under the mutex;
closing the receiver moves queued values into a private buffer while holding it and destroys them after
unlocking. A wait with no unjoined thread (static slot 11
zero) traps as `channel deadlock`. The tests are `tests/unittests/runtime/channel_test.cpp`.

## Tasks

A task (decision [0018](../decisions/0018-tasks.md), spec §21) is one heap block from `joyeer_alloc`:
a zeroed 128-byte thread record (`JOYEER_THREAD_BYTES`), the block's own size, the result, then the
input. The backend emits a thunk per target that loads the input, calls the function and stores the
result; `joyeer_thread_start_abi` runs it, and `join` calls `joyeer_thread_join_abi`, moves the result
out and frees the block with the stored size. A refused thread traps with `too many tasks` or `cannot
start task`. Because the block is ordinary allocator memory, the leak counter needs no task rule, and
the generated destroy helper for a handle traps (`task dropped without join`) if one is ever dropped
live. Linux links `-lpthread` after the runtime archive group for a program that starts a task.
