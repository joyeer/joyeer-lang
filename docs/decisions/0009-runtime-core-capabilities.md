# 0009: Capabilities the Joyeer runtime core needs from the compiler

- Status: accepted (decided 2026-11-08). Indirect calls (D2), core units (D3) and the
  constant-data address (part of D7) are implemented and
  measured; D1, D4, D5 and the rest of D7 are accepted designs for runtime plan stage 3.
- Spec: [§19.4.5](../spec/19-c-interop.md#1945-compiler-intrinsics) (implemented
  intrinsics) and [§19.4.6](../spec/19-c-interop.md#1946-accepted-runtime-capabilities-not-yet-implemented-)
  (accepted, not yet implemented).

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
- On Windows the runtime archive is linked whole, so `entry.c`'s `wmain` collides
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
single table. Slot ranges are assigned in the runtime plan (core 0 to 15,
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
startup symbols and `joyeer_main*`. The Joyeer entry replaces `entry.c` in one
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
writes `Joyeer runtime error: <message>\n` with one `write` call and calls
`_Exit(1)`; it neither allocates nor touches a managed value. `@c_export` of a
function with result type `Never` is allowed (core units) and emits `noreturn`.
Rejected: passing a `[UInt8]` constant through `pointer:` (the Debug finding
above), and message codes with a table in C.

## Consequences

- Stage 3 builds D1, D3, D4, D5, D7 and the bulk-memory intrinsics, each with
  lowering, misuse and tampered-IR unit tests and a native fixture, as decision
  0005 did for the memory intrinsics. Indirect calls need no further work.
- Core units become a real, enforced subset, so the `RuntimeCoreObjectSymbols`
  gate shrinks to a check of exports and the allowed C library imports.
- Nothing here reaches ordinary programs: every capability is available only in
  units the toolchain authorizes.
- **Not verified:** the entry spike on Linux and macOS, and on Windows ARM64.
