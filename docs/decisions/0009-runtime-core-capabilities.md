# 0009: Capabilities the Joyeer runtime core needs from the compiler

- Status: accepted (decided 2026-11-08). Static slots (D1), indirect calls (D2), core units (D3),
  Never results and the core entry export (D4, part of D7) and the
  constant-data address (part of D7) and data symbols (D5) are implemented and
  measured. Bulk memory (D11, added below) is implemented too.
- Spec: [§19.4.5](../spec/19-c-interop.md#1945-compiler-intrinsics) (implemented
  intrinsics) and [§19.4.7](../spec/19-c-interop.md#1947-core-units-never-results-data-symbols-and-the-program-entry-point)
  (Never results, core units, data symbols and the entry point).

## Context

[The runtime plan](../plan/runtime-in-joyeer.md) writes the runtime in Joyeer. The
first functions ([decision 0008](0008-runtime-core-build-shape.md)) needed nothing
the compiler lacked. The rest need five things it does not yet have: mutable
state, calls through a function address, a way to stop the runtime reaching
itself, a Joyeer program entry point, and a way to name constant data. Stage 2
ran the two riskiest spikes before stage 3 builds on them.

## Spike results

**Entry point (D4).** A Joyeer object exporting `wmain` (`@c_export`), linked
ahead of the runtime archive with `--link-library`, was selected by the linker's
default entry choice, called the generated `joyeer_main`, printed its output and
propagated its exit status (90 for a deliberately failing probe, 0 otherwise), on
Windows x64 with the static CRT. Two findings:

- The compiler rejects `@c_export` of `main` and `wmain` today
  (`type-checking.invalid-native-declaration`, `isCEntrySymbol`). The spike
  relaxed that locally and reverted it; stage 3.3 lifts it for core units only.
- On Windows the runtime archive is linked whole, so the former C entry's `wmain` collides
  with a Joyeer one (`lld-link: duplicate symbol: wmain`). The Joyeer entry
  therefore replaces `entry.c` in one change (stage 7.1); the two cannot coexist.

Linux and macOS (`main` through `crt1.o`) were **not run**: only a Windows host
was available. Plan stage 2.1 stays a precondition for 7.1 on those platforms.

**Indirect calls (D2).** `joyeer_intrinsic_call_void1(function, first)` and
`joyeer_intrinsic_call_void2(function, first, second)` are implemented in the
shared intrinsic table, the type checker, the IR verifier and the backend, with a
native fixture (`JoyeerCInterop.Callbacks*`, both optimization profiles). A Joyeer
loop destroying 1000 elements through them costs **1.57 ns per element, the same
as a C loop doing a true indirect call** (Windows x64 Release, three runs; the C
compiler inlines the callee unless the pointer comes from a volatile, so the
comparison is against the non-inlined form).

## Decisions

**D1: Static storage.** `joyeer_intrinsic_static_slot(index: Int) -> Int` returns
the address of the 8-byte, zero-initialized slot `index` in a table of 64 slots.
An index outside `0..<64` traps. The table is one internal global with
`linkonce_odr` linkage in a comdat, so every runtime object in a program shares a
single table. Slot ranges are assigned in the runtime implementation notes (`docs/impl/runtime.md`) (core 0 to 15,
`System` 16 to 31, the rest reserved). Rejected: mutable top-level `var` (a
language change with ownership and initialization-order questions), and a
heap-allocated state block with its address kept in an OS slot (needs a C
global or TLS to find it).

**D2: Indirect calls.** As implemented above. The callee is the target's C
function returning void; nothing checks the address or the signature. Two
arities are enough for the generated clone (destination, source) and destroy
(address) functions; a third is added only for a concrete need. Replace with
compiler-specialized containers once user generics exist.

**D3: Core units.** A new option `--runtime-core-module <name>` (repeatable, and
implying `--runtime-module`) marks a unit as core. Core units may use `Bool`,
the integer and floating types, `Void`, and top-level scalar constants, and
nothing that lowers to a runtime call: `String`, arrays, dictionaries,
`Optional`, `Result`, enums, structs, `print`, and string
literals (except as the argument of the literal-address intrinsic of D7). A
violation is `type-checking.managed-type-in-core-unit`, reported at the use. The
reason is concrete: decision 0008 showed a single `[UInt8]` constant makes a
Debug build import the runtime's own array code.

**D4: Entry point.** In a core unit, `@c_export(symbol: "main")` and
`"wmain"` are allowed with the platform's C signature (`Int32`, then the `argv`
address as `Int`). Everywhere else exporting them stays an error, as do the other
startup symbols and `joyeer_main*`. The Joyeer entry replaced `entry.c` in one
change (stage 7.1) and needs arrays and strings first to build the arguments.

**D5: Data symbols.** `@c_data(symbol: "environ")` on a bodyless function
`func environAddress(): Int` yields the address of that C data symbol
(`ptrtoint`). It is an import-style annotation, so it is authorized like
`@c_extern`, and a repeated declaration of one symbol must agree. Where the
platform offers a function (`_errno`, `__errno_location`, `__error`,
`__acrt_iob_func`), bind the function with `@c_extern` and do not use `@c_data`.
This replaces `joyeer_native_errno`, `joyeer_native_environment` and
`joyeer_native_stdio` (stage 7.2).

**D7: Panic.** `joyeer_panic` becomes a core function taking the message as an
address. A message is named with
`joyeer_intrinsic_literal_address(text: String) -> Int`, whose argument must be a
string literal: it lowers to the address of that literal's NUL-terminated
constant data and creates no `String` value, so D3 allows it. `joyeer_panic`
writes `Joyeer runtime error: `, the message and a newline to standard error
(three `write` calls: a core unit has no buffer to join them in without
allocating) and calls `_Exit(1)`; it neither allocates nor touches a managed
value. `@c_export` of a
function with result type `Never` is allowed in any runtime unit and emits
`noreturn`, and so is `Never` as the result of a `@c_extern` such as `exit`.
Rejected: passing a `[UInt8]` constant through `pointer:` (the Debug finding
above), and message codes with a table in C.

## Consequences

- Stage 3 builds D1, D3, D4, D5, D7 and the bulk-memory intrinsics, each with
  lowering, misuse and tampered-IR unit tests and an end-to-end fixture, as decision
  0005 did for the memory intrinsics. Indirect calls need no further work.
- Core units become a real, enforced subset, so the `RuntimeCoreObjectSymbols`
  gate shrinks to a check of exports and the allowed C library imports.
- Nothing here reaches ordinary programs: every capability is available only in
  units the toolchain authorizes.
- **Not verified:** the entry spike on Linux and macOS, and on Windows ARM64.

## Added during stage 3

**D11: Bulk memory.** `joyeer_intrinsic_memcpy`, `_memmove` and `_memset` lower to
LLVM's memory intrinsics (alignment 1, count taken as unsigned, no range check),
so a small constant count becomes inline moves and a large one a call to the
platform's function. `memcmp` is **not** an intrinsic: LLVM has no inline form of
it, so a core unit keeps importing the C library's `memcmp` with `@c_extern`
until a Tier 2 runtime compares words itself. Rejected: a checking variant
(counts are validated by the runtime function before it copies, as the C was).
In Tier 2 the variable-size forms still call `memcpy`/`memmove`/`memset` by
name, so a libc-free runtime must define those symbols itself, in a unit that
does not use them.

**Panic in the generated code (stage 4).** The backend's own checks (overflow, bounds)
call `joyeer_panic`; the call now passes the message as an `i64` address
(`ptrtoint` of the literal) so it matches the Joyeer export's signature, which is
the same C ABI as a `const char*` on a 64-bit target. A user program that
declares `joyeer_panic` with any other signature is still
`llvm.conflicting-native-symbol`. A runtime object emits no clone or destroy helpers
for types that only core functions mention (the literal-address argument is a
`String` constant that is never materialized); without this a Debug core object
imported `joyeer_string_clone_abi`.

**The entry object (stage 7).** The Joyeer `main`/`wmain` is in an object of its own,
not the core one: the archive is linked whole on Windows, but a test that links the
core object must not also receive a second entry point. The entry logic
(`joyeer_entry_run`) reaches the core through its C ABI, uses static slots 32 to 39,
and is built alone for the Windows entry encoding tests.

**Symbol addresses (stage 6).** The native `[String]` buffers a native library fills
need element clone and destroy callbacks, which are Joyeer functions of the runtime
itself, and a core unit has no way to name a function. `joyeer_intrinsic_symbol_address(name:)`
reads a string literal like the literal-address intrinsic and yields the address of
the function of that name in the same object, or of an external symbol otherwise.
Rejected: a sentinel in the array header that the runtime interprets (it would make
the header mean different things to generated and runtime code), and keeping the
callbacks in C.

**The allocator contract (stage 4).** `joyeer_alloc(size)`,
`joyeer_realloc(address, oldSize, newSize)` and `joyeer_free(address, size)` are
Joyeer exports; sizes are those of the last request, so the Tier 2 allocator (D8)
needs no header. They wrap `malloc`, `realloc` and `free` today and ignore the
sizes. The counter is static slot 0.

**Entry-point export: the IR flag.** The IR function carries `coreContext`, and
the IR wire format is version 10 (core flag, then the data-symbol flag), so the
verifier checks the entry-point and data-symbol rules independently of the type
checker.
