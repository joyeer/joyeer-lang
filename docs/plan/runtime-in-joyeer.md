# Runtime in Joyeer: Plan

> **Status:** proposed. The runtime is still C (`src/runtime/runtime.c`,
> `entry.c`, `host.c`; about 970 lines). The host side is already Joyeer: the
> filesystem, processes, standard streams and `readFile` live in `System`
> ([filesystem plan](filesystem.md)), and the compiler has the pieces this plan
> builds on: `@c_export`, `--emit-runtime-object`, and the memory intrinsics of
> [decision 0005](../decisions/0005-runtime-unsafe-memory.md). Current
> behavior: [runtime](../impl/runtime.md), [strings](../impl/string.md). Remove
> completed work from this plan.

## 1. Goal

**Every line of the runtime is Joyeer.** `JoyeerRuntime` is a static archive built
only from objects that the Joyeer compiler emits from Joyeer source. No C file of
ours is compiled into it, and none remains in `src/runtime/`. The runtime also
stops depending on the *C language's* runtime: it owns its allocator, its output,
its program startup and its argument handling, and talks to the operating system
through the platform's own system interface, written as Joyeer bindings.

The work is split into two tiers, so that each ends in something shippable:

- **Tier 1 (R0 to R9): all runtime code is Joyeer.** Allocation, panic, strings,
  arrays, dictionaries, printing, program entry and the helpers native libraries
  call are Joyeer. For the lowest layer it still calls the platform C library
  (`malloc`/`free`, a write primitive, `_Exit`) and the C startup calls
  `main`/`wmain`. This removes the second language and the second toolchain.
- **Tier 2 (R10 to R12): the runtime no longer needs the C library.** Joyeer owns
  the allocator (over `VirtualAlloc` / `mmap`), output (`WriteFile` / the `write`
  system call), exit, and the process entry point and argument parsing. What is
  left is the operating system's interface alone (§2, "Platform interface").

Done means all of these hold:

- The archive provides every symbol in §3 with its current C ABI and behavior:
  allocation and the leak check, panic, strings, arrays, dictionaries, printing,
  program entry, and the C helpers that native libraries call.
- Programs still link one runtime archive. Artifact names, install components
  and the `runtime.h` / `native.h` headers for native libraries are unchanged.
- After Tier 2, the runtime archive has no undefined symbol except operating
  system interfaces, on the platforms where that is possible (Windows, Linux).
  macOS keeps `libSystem`, which is its system interface and has no stable
  system-call ABI to bypass.

Why: Joyeer is meant to replace legacy software, and a language whose runtime
needs a second language and a second toolchain to build cannot claim that. A
runtime that owns its allocator also makes allocation accounting, deterministic
destruction and the leak check properties of Joyeer code rather than of `malloc`.
It makes the runtime a large, honest test of the compiler and of the unsafe
layer, and removes the `C11` and `/experimental:c11atomics` build special cases.

## 2. Constraints that shape the design

- **No recursion through the runtime.** Joyeer lowers string `==`, `<`, `+`,
  array indexing and append, scope cleanup and `print` to runtime calls. A runtime
  function that used any of them would reach itself. The runtime therefore needs a
  restricted dialect (§4).
- **No mutable global storage.** The language has `let` constants only, and the
  runtime needs an allocation counter. See D1.
- **No function values.** Arrays and dictionaries call per-element-type clone and
  destroy functions that the compiler generates (`joyeer_clone_type_N`,
  `joyeer_destroy_type_N`) and hands over as C function pointers. Joyeer cannot
  call one. See D2.
- **No variadic calls.** `printf("%.17g")` is unavailable, so floating-point
  printing has to be implemented. See R4.
- **No C globals by name.** `errno`, `environ`, `stdin`, `stdout` and `stderr`
  are reached today through three C accessors (`joyeer_native_errno`,
  `joyeer_native_environment`, `joyeer_native_stdio`). See D5.
- **Generated names.** Compiler-generated functions and ownership helpers carry
  numeric ids, so separately compiled runtime and application objects can
  collide. Runtime objects must keep everything but the approved exports internal;
  the object emitter already does this.
- **Bootstrap order.** The runtime must build without the runtime. `joyeer` and
  `JoyeerBackend` already do not wait for `JoyeerRuntime`; the runtime is built
  by the just-built compiler, never an installed one.

### Platform interface

"Not C" is not the same as "no dependencies": every program on every OS ends in
the operating system's interface. This plan fixes what that interface is per
platform, so "written in Joyeer" has a precise end state.

| Target | System interface the runtime binds to | Tier 1 | Tier 2 |
|---|---|---|---|
| Windows x64, ARM64 | `kernel32` (`VirtualAlloc`, `WriteFile`, `GetStdHandle`, `ExitProcess`, `GetCommandLineW`) | CRT `malloc`, `main`/`wmain` startup | Win32 only; own entry point |
| Linux x64, ARM64 | The kernel system calls (`mmap`, `write`, `exit_group`) | libc `malloc`, `_Exit`, crt1 startup | raw system calls through a syscall intrinsic; own `_start` |
| macOS ARM64 | `libSystem` (`mmap`, `write`, `_exit`) | libc `malloc`, `_Exit`, crt1 startup | libSystem calls only; own allocator; startup stays dyld's `main` |

macOS cannot go below `libSystem`: Apple does not support raw system calls, so
for macOS "Tier 2" means no `malloc`, no stdio, and no C startup code of ours, not
no `libSystem`. Linux can be fully libc-free, but a dynamically linked program
still needs the loader; the plan keeps that choice open (D9).

`System` currently uses the C library's stdio (`fopen`, `fgetc`, `fwrite`,
decision 0007) and `errno`. Tier 2 moves it to the same system interface, which
needs mutable state for buffering and `errno`-free error codes; D1 provides the
state, and the change is its own phase (R12).

### Related work outside this plan

| Item | Where it is tracked | How it touches the runtime |
|---|---|---|
| Spec text for the runtime-only capabilities | [§19.4.6](../spec/19-c-interop.md#1946-accepted-runtime-capabilities-not-yet-implemented-) (proposed) | Each capability becomes accepted spec text when its decision lands |
| `assert`, `precondition`, `fatalError` | [library plan](library.md) §2 | Built on `joyeer_panic` (R2); `fatalError` needs a `Never` result |
| `deinit` and noncopyable types | not planned yet (§3.7, §4.7 of the spec) | Needed for owned OS resources in `System`, not by the core runtime |
| User generics | [§20](../spec/20-protocols.md) (proposed) | Lets D2 move to compiler-specialized containers |
| Runtime archive in `lib/` | [toolchain libraries](toolchain-libraries.md) §3 | Do it with R9, when the compiler builds the archive |
| Sanitizer builds | [test audit](test-audit.md) item 3 | The main safety net for unchecked address code |
| POSIX verification of `System` | [filesystem plan](filesystem.md) §2 | Same hosts and gates as this plan's platform acceptance |

## 3. Inventory

Everything the compiler, the tests and native libraries call. "Phase" is where
§7 replaces it.

| Symbols | Called by | Phase |
|---|---|---|
| `joyeer_runtime_active_allocations`, allocation counter | `entry`, tests, fixtures | R2 |
| `joyeer_panic` | every generated trap, all runtime code | R2 |
| `joyeer_checked_{add,sub,mul,div,rem}_int` | removed in stage 0: generated code checks inline | done |
| `joyeer_print_{int,uint,float,double,bool,byte}`, `joyeer_print_string_abi` | generated `print` | R3, R4 |
| `joyeer_string_{equal,compare}_abi` | generated string operators | done (stage 1.3) |
| `joyeer_string_concat_abi` | generated string operators | R3 |
| string byte access | generated `text[index]` | inline in the backend; no runtime function (stage 1.3) |
| `joyeer_string_{clone,destroy}_abi`, `joyeer_byte_to_string_abi` | generated ownership code | R3 |
| `joyeer_array_{create_owned,clone,append_owned,destroy}_abi` | generated array code | R5 |
| `joyeer_dictionary_{create_owned,clone,set_owned,find,at,destroy}_abi` | generated dictionary code | R6 |
| `joyeer_native_{bytes,strings}_init`, `joyeer_native_{bytes,string,strings}_assign` | native libraries (Compression), generated `initializing` outputs | R5 |
| `joyeer_host_valid_utf8` | Compression's C unit | done (stage 1.2) |
| `joyeer_native_errno`, `joyeer_native_environment`, `joyeer_native_stdio` | `System` | R8 |
| `main` / `wmain`, `joyeer_main_uses_arguments` | the C startup | R7 |
| `joyeer_array_create`, `joyeer_array_at`, `joyeer_dictionary_create`, `joyeer_dictionary_at`, non-`_abi` string forms | removed in stage 0 | done |

Layouts that must be preserved: `JoyeerString { data, count }`,
`JoyeerArray { data, count, capacity }` with an `ArrayHeader { elementSize,
cloneElement, destroyElement }` stored just before `data`, and the dictionary
header (key and value sizes, entry size, value offset, key kind, four function
pointers). Strings are not NUL-terminated and may hold arbitrary bytes.

## 4. Architecture

### The core dialect

Runtime units are compiled with `--runtime-module`. A subset of them, the **core**
(everything in this plan), may use only scalars, `Bool`, and addresses held as
`Int`, plus the intrinsics. They may not use `String`, arrays, dictionaries,
`print`, enums with payloads or any construct that lowers to a runtime call. The
compiler enforces this with a diagnostic, because a violation is a link-time
cycle that is hard to read. The core is therefore written in the style of C:
explicit sizes, explicit addresses, no ownership. That is a large but bounded
body of code (the existing C is under a thousand lines).

Everything above the core, including `System`, keeps using the managed types and
calls the core through the existing generated ABI.

### Build shape

```text
compiler + backend -> joyeer --emit-runtime-object (core units) -> runtime objects
runtime objects ----------------------------------------------> JoyeerRuntime archive
runtime archive + compiler/backend -> product staging -> fern / acceptance
```

- The explicit source list is in CMake; no discovery.
- One production provider per symbol. During migration a symbol is provided by
  either `runtime.c` or the Joyeer objects, chosen at configure time, never both;
  a test proves a retained C algorithm cannot mask a missing Joyeer object
  (Windows whole-archive linking, ELF grouping).
- The public C ABI is kept by `@c_export`. `runtime.h` and `native.h` stay as the
  header for native libraries.

## 5. Decisions to settle first

Each gets a record in [decisions](../decisions/README.md) before code.

**D1: Static runtime storage** (decided, decision 0009). The allocator needs a counter, and later a
free-list or arena state. Options: (a) an intrinsic that returns the address of
a zero-initialized slot in a compiler-emitted internal global
(`joyeer_intrinsic_static_slot(index)`); (b) mutable top-level `var` in runtime
units; (c) heap-allocate the state once and stash its address in an OS slot.
Recommend (a): smallest, reserved to runtime units like the other intrinsics,
and needs no language change.

**D2: Calling generated clone and destroy functions.** Arrays and dictionaries
store element-type callbacks. Options: (a) an indirect-call intrinsic
(`joyeer_intrinsic_call_void1`/`_void2(function, ...)`) with the function as an `Int`; (b)
generate per-type array and dictionary helpers in the compiler so the runtime
holds no callbacks; (c) wait for generics. Recommend (a) now, because it keeps
the ABI and the layouts, and (b) once user generics exist. This is the largest
open design question. **Decided and implemented** (decision 0009): option (a), as
`joyeer_intrinsic_call_void1` and `_void2`, measured equal to a C indirect call.

**D3: Enforcing the core dialect** (decided, decision 0009). A diagnostic ID, and what counts as a managed
use. Recommend rejecting any managed type in a core unit's function signatures,
bodies or stored fields.

**D4: The entry point** (decided, decision 0009). Export `main` / `wmain` from Joyeer with
`@c_export(symbol: ...)`, take `argv` as an address, convert arguments (UTF-16 to
UTF-8 on Windows, raw bytes on POSIX) in Joyeer, call the generated
`joyeer_main`, destroy the arguments, run the leak check. Needs an exported
`Int32`-argument signature and an imported `joyeer_main` taking an address. Check
that the platform startup (`mainCRTStartup`, `crt1.o`) accepts an exported
`main` from an archive member.

**D5: Data symbols** (decided, decision 0009). Replace the three C accessors. `errno` is a function on
every target (`_errno`, `__errno_location`, `__error`), `stdin`/`stdout`/`stderr`
are `__acrt_iob_func` on Windows and data symbols elsewhere, and `environ` is a
data symbol. Options: a `@c_extern` form that yields the address of a data symbol
(`@c_data(symbol: "environ")`), or per-platform function bindings where a function
exists. Recommend both: functions where they exist, one data-symbol import for the
rest. This also removes `System`'s dependence on the runtime for them.

**D6: Floating-point printing.** `print` of `Float` and `Double` currently uses
`%.9g` and `%.17g`. Recommend keeping the output byte-identical and implementing
correctly rounded conversion in Joyeer (a Dragon4 or Ryu-style algorithm with
fixed-size limbs in a byte buffer), proven against the C library over a large
sample. Changing the format to shortest round-trip is a language-visible change
and is out of scope here.

**D7: Process termination and diagnostics** (decided, decision 0009). Panic must not allocate or touch
managed values. Recommend: write the message with the C library's `write` or
`WriteFile` equivalent in one call, then `_Exit(1)`; keep the existing text
(`Joyeer runtime error: <message>`) and exit status. It also needs a way to name a
message from Joyeer: see §6 item 9.

**D8: The allocator (Tier 2).** Options: (a) keep libc `malloc`; (b) a Joyeer
allocator over `VirtualAlloc` / `mmap`: size-class free lists for small blocks,
direct mappings for large ones, single-threaded first, the allocation counter in
the same code. Recommend (b), but only after R2 to R9 prove the layers above it,
because every phase until then benefits from `malloc` being a known-good
substrate. Define the contract now so R2 does not paint it into a corner:
`alloc(size)`, `realloc(address, oldSize, newSize)` and `free(address, size)`,
all taking sizes, since Joyeer always knows them.

**D9: Startup and the loader (Tier 2).** Options per OS: keep the platform's C
startup object calling an exported `main`/`wmain` (D4), or supply our own entry
point (`mainCRTStartup` replacement on Windows with `/ENTRY`, `_start` on Linux),
parsing `GetCommandLineW` or the initial stack ourselves. Recommend our own entry
on Windows and Linux in R12 and `main` on macOS. Static linking on Linux is a
separate question and not required.

**D10: System-call intrinsic (Tier 2, Linux).** A reserved
`joyeer_intrinsic_syscall(number, a, b, c, d, e, f)` lowering to inline assembly
per architecture. Windows and macOS need nothing; they call exported functions.

## 6. Compiler capabilities needed

All reserved to runtime units, using the existing `@c_extern` / intrinsic path
(shared descriptor table, type checker and IR verifier checks, inline lowering):

1. Static storage slots (D1).
2. Bulk memory: `memcpy`, `memmove`, `memset`, `memcmp` as intrinsics lowering to
   LLVM, so copies are not byte loops.
3. Indirect calls (D2).
4. Data-symbol import (D5).
5. The core-dialect diagnostic (D3).
6. `@c_export` of a function that never returns, and of `main`/`wmain` with their
   C signatures (D4, D7).
7. A system-call intrinsic on Linux (D10, Tier 2).
8. Build: emit and archive several runtime units, with debug info, for every
   claimed target. Done for the first target in
   [decision 0008](../decisions/0008-runtime-core-build-shape.md).
9. The address of read-only constant data, for panic messages (D7). A `[UInt8]`
   constant passed to a `pointer:` parameter works in an optimized build but, in
   Debug, drags in the runtime's array clone and destroy (decision 0008), so a
   core unit cannot use it. Options: an intrinsic that returns the address of a
   string literal given to it as a compile-time argument, or a core-dialect
   exception for borrowed constant arrays that emits no ownership helpers.

Each gets unit tests (lowering, rejection of misuse, tampered IR) and a native
fixture, as decision 0005 did for the memory intrinsics.

## 7. Phases

| Phase | Deliverable | Exit criteria |
|---|---|---|
| R0: Baseline | Freeze the inventory and ABI. Pin current behavior with black-box tests of the exported C ABI (`runtime_test.cpp` stays valid across providers). Record micro-benchmarks for string compare and concat, array append and dictionary set and find at `-O2`. Delete the symbols that only tests call, or keep them as exports by decision | Baseline numbers and allocation counts recorded; dead symbols decided |
| R1: Capabilities | §6 items 1 to 7, D1 to D5 decided | Each capability tested; a toy core unit builds, archives and links into a program |
| R2: Substrate | Counter, `alloc` / `realloc` / `free` with panic on failure, `joyeer_runtime_active_allocations`, `joyeer_panic`, checked-arithmetic exports if kept | Allocation balance tests, panic text and status identical, no allocation in panic |
| R3: Strings and printing | Equality, ordering, concatenation, byte access, clone and destroy, `byteToString`, `print` for integers, `Bool`, bytes and strings | No allocation for comparison; exactly one for concatenation (including empty); results exact for empty, identical, shared-prefix, embedded NUL, invalid UTF-8, bytes above `0x7f` and large inputs; within the benchmark gate (§8) |
| R4: Floating-point printing | D6: `Float` and `Double` output | Byte-identical to the C library over boundary values, subnormals, extrema, NaN, infinities, signed zero and a large random sample, in both widths |
| R5: Arrays | Create, clone, append (growth policy), destroy, native buffer init and assign | Ownership across structs, enums and nested arrays; zero final allocation balance; overflow checks; native-library tests (Compression) pass |
| R6: Dictionaries | Create, set (duplicates), find, at, clone, destroy for every key kind | Existing dictionary fixtures pass; borrowed lookup does not clone; linear-lookup behavior unchanged |
| R7: Entry | `main` / `wmain`, argument conversion, leak check, exit-status check | Entry tests: both `main` forms, empty and spaced arguments, Unicode, POSIX raw bytes, out-of-range statuses, unpaired surrogates on Windows |
| R8: Native-library API | `joyeer_host_valid_utf8`, the errno, environment and stream accessors or their replacements (D5) | `System` and Compression build and pass without any runtime C |
| R9: Remove the C | Delete `runtime.c`, `entry.c`, `host.c` and the C-only build settings; add a check that the archive holds no object not produced by the Joyeer compiler | Product acceptance on every claimed platform (§8) |
| R10: Allocator | D8: a Joyeer allocator over `VirtualAlloc` / `mmap` replacing `malloc`, behind the R2 `alloc` / `realloc` / `free` contract, counter included | Same allocation-balance and overflow tests; stress and fragmentation tests; within the benchmark gate on allocation-heavy fixtures; sanitizer-style poisoning in Debug |
| R11: Output and exit | Panic, `print` and exit go straight to `WriteFile` / `write` / `ExitProcess` / `_exit` (a syscall intrinsic on Linux, D10) with Joyeer-owned buffering | No stdio dependency left in the runtime archive; output ordering with `System`'s stdout unchanged (§8) |
| R12: Startup and `System` | D9: own entry point and argument parsing on Windows and Linux; macOS keeps `main`. `System` moves from the C library's stdio and `errno` to the system interface, using D1 state for buffering | Runtime archive and `System` have no undefined symbol outside the OS interface on Windows and Linux (checked with `llvm-nm`); entry, stdin and stderr tests unchanged |

R3 to R8 can overlap once R1 and R2 are in. R10 to R12 start only after R9, so Tier 1
ships first. Phases land one provider at a time and keep the suite green. The
phases group the work by subject; the order to implement it in is the checklist
in §11, which starts with functions that need no new compiler capability.

## 8. Acceptance

Extend the existing [tests](../../tests/); the final gate is default builds with
unit tests and fern enabled and the unfiltered CTest ([building](../building.md)).

**Compiler and artifact**
- Accepted and rejected bindings, export names, signatures, malformed IR; ordinary
  diagnostics and ownership checks unweakened.
- One provider per symbol; no numeric-symbol collisions; no recursive or
  unexpected runtime dependencies (`llvm-nm` on objects: only the approved exports
  and imports, as the C interop fixtures do).
- Archive extraction and Windows whole-archive linking; a retained C algorithm
  must not mask a missing Joyeer object.
- Rebuild invalidation, parallel ordering, debug locations, staging freshness,
  relocated lookup. No fallback runtime ships.

**Behavior**
- Every existing `runtime_test.cpp`, native fixture and `-O0` to `-O3`
  optimization-equivalence test passes unchanged against the Joyeer archive.
- Allocation: comparison allocates nothing; concatenation allocates once; zero
  final balance; overflow and allocation-failure paths are checked, never
  success-shaped.
- Performance gate: at `-O2`, each migrated hot path is within 10% of the C
  baseline on the R0 benchmarks, or the gap is recorded with a plan. Linear time
  is preserved.

**Tier 2**
- `llvm-nm` on the runtime archive and on a linked `System` object lists no
  undefined symbol outside the target's system interface (§2), on Windows and
  Linux; on macOS, none from the C library's allocator, stdio or startup code.
- Allocator stress, fragmentation and double-free/overflow tests; the leak check
  and allocation balance keep their meaning; no regression on allocation-heavy
  fixtures against the R9 numbers.

**Product**
- Clean and incremental builds, install and relocation, fern bootstrap,
  Compression and moss, Debug and Release.
- No LLVM or Python dependency in generated programs; packaging unchanged.
- Fresh native results per platform (Windows x64 and ARM64, Linux, macOS arm64);
  one host does not certify another.

## 9. Risks and non-goals

**Risks.**
- *Code quality of the first compiler client that is also its runtime.* Hot paths
  compile through our own backend, with unaligned loads and no vectorized loops;
  the benchmarks in R0 exist to catch this early. Mitigation: bulk-memory
  intrinsics (§6) and a recorded gap.
- *Bugs without a safety net.* The core has no types for addresses. Mitigation:
  small functions, explicit extents, black-box tests of the C ABI, and sanitizer
  builds of the test executables where the toolchain has them.
- *D2.* Indirect-call intrinsics are a stopgap if generics arrive; the array and
  dictionary code should keep the callback use behind two helper functions so it
  can be replaced.
- *Debuggability.* A crash inside the runtime now shows Joyeer frames; full debug
  info for runtime objects must work before R3.
- *Windows startup.* An exported `wmain` from an archive member interacts with
  CRT linking (`/MT`, whole-archive); prove it in R1, not R7.

**Non-goals.** Replacing the operating system's own interface (`libSystem` on
macOS, the kernel on Linux, `kernel32` on Windows); freestanding, kernel and
embedded targets; a dynamic-loader replacement; rewriting the compiler; public
FFI or a user-visible `unsafe`; GC, ARC or copy-on-write; enforcing UTF-8 on
`String`; selectable runtime profiles; a package registry.

## 10. Follow-ups after R12

- More leaf algorithms in Joyeer: UTF-8 validation (also in Foundation), number
  formatting and parsing, lexical paths.
- Inline lowering of string equality, ordering and byte access in the backend,
  removing the call, once the runtime is proven.
- Replace the type-erased array and dictionary helpers with compiler-specialized
  code when generics or an internal type-operation mechanism exist.
- Owned OS resources, after custom destruction and noncopyability.
- Source standard library: see [library](library.md).

Update [runtime](../impl/runtime.md), [strings](../impl/string.md),
[backend](../impl/backend.md), [C interop](../impl/c-interop.md),
[supported features](../impl/supported-features.md) and
[building](../building.md) as phases land.

## 11. Implementation checklist

Work top to bottom. Each box is one change set that leaves the suite green and is
committed on its own. Tick a box (`[x]`) when it lands, with the commit, and
delete a whole stage once every box in it is done and the impl docs describe it.

**Every box is done only when:** the decision, if the box names one, is recorded
in [decisions](../decisions/README.md); a failing test came first; the change
passes focused tests and then the unfiltered CTest in Debug and Release on
Windows x64; the impl docs and §3 here are updated; and the role reviewers in
`.claude/agents/` ran on compiler changes. Other platforms are gated at stage 9,
except where a box says otherwise.

### Why this order

- **Prove the build before the compiler work.** A few runtime functions need only
  what the compiler already has: they read memory, allocate nothing and return a
  scalar. Moving them first puts Joyeer objects into the archive, exercises
  `@c_export` across the real ABI, and checks debug info and `llvm-nm`, before
  any new intrinsic exists.
- **Retire the unknowns early.** An exported `wmain`/`main` and calling a
  compiler-generated function through an address are the two things most likely
  to force a redesign. Spike both before building on them.
- **Allocation before anything that allocates.** Strings, arrays, dictionaries
  and entry all allocate, so the allocator and its counter move first, and the C
  that remains calls the Joyeer allocator, so there is only ever one counter.
- **Leaves before callers.** Strings before arrays (arrays of strings), arrays
  before dictionaries and before entry (the arguments are a `[String]`).
- **Floating-point printing is independent** and algorithm-heavy; run it as a
  parallel track once output works.
- **Delete the C last, then Tier 2.** Only when no symbol is provided by C does
  the archive change shape; the allocator and startup work starts from there.

### Stage 0: Baseline (R0), done

- [x] 0.1 Test-only symbols removed: `joyeer_checked_*`, `joyeer_array_create`,
  `joyeer_array_at`, `joyeer_dictionary_create` and their `_abi` wrappers, and the
  by-value string, array and dictionary forms (now `static` helpers inside
  `runtime.c`; the ABI is the flat `_abi` functions). Their tests were deleted
  where the behavior lives in generated code (checked arithmetic and array bounds
  have native fixtures) and migrated to the `_abi` forms elsewhere.
- [x] 0.2 `runtime_test.cpp` names only symbols that `runtime.h` declares, so it
  runs unchanged against any provider. Added: printed output of every `print_*`,
  panic text and status, exact `-1/0/1` string ordering over unsigned bytes and
  embedded NUL, allocation counts (none for comparison, one for concatenation and
  for an empty result), array growth (capacity 4 then doubling), empty-array
  clone, and the native `errno`, `stdio` and `environment` accessors. Dictionary
  key kinds and clone/set/find were already covered.
- [x] 0.3 `RuntimeBenchmarks` (`tests/unittests/runtime/runtime_benchmark.cpp`),
  run with `cmake --build <dir> --target runtime-benchmark`. It is not registered
  with CTest.

Baseline, C runtime, Windows x64 Release (`/O2 /Ob2 /MT`), AMD EPYC 7763,
2026-11-07, best of 7 repetitions, three runs. Rows under about 10 ns vary by up
to 40% between runs; treat only larger differences as real.

| Benchmark | ns per operation |
|---|---|
| string equal, equal, 16 B | 5.7 to 9.3 |
| string compare, last byte differs, 16 B | 6.3 to 9.4 |
| string concat + destroy, 16 B | 47 to 56 |
| string clone + destroy, 16 B | 41 |
| string equal, equal, 1024 B | 34 |
| string compare, last byte differs, 1024 B | 34 |
| string concat + destroy, 1024 B | 73 to 76 |
| string clone + destroy, 1024 B | 56 to 58 |
| array append, grow from empty (`Int`) | 8.7 to 9.1 |
| array clone + destroy, 1000 elements with callbacks | 5.8 per element |
| dictionary set, 1000 distinct `Int` keys (linear) | 1.4 µs per set |
| dictionary find, last of 1000 `Int` keys | 2.8 µs per find |

The §8 performance gate compares against these rows on the same machine.

### Stage 1: Build shape and pilot, done

- [x] 1.1 The core module `runtime.core` (`src/runtime/joyeer/*.joyeer`, an
  explicit list in `src/runtime/CMakeLists.txt`) is compiled by the just-built
  compiler into one object that `JoyeerRuntime` archives beside the C
  ([decision 0008](../decisions/0008-runtime-core-build-shape.md)). A test
  (`RuntimeCoreObjectSymbols`, `llvm-nm`) allows only the approved exports and
  imports.
- [x] 1.2 `joyeer_host_valid_utf8` is Joyeer (`utf8.joyeer`); its C definition,
  `host.c` and `host_internal.h` are gone. Checked against an independent
  Unicode-table reference over every 1-, 2- and 3-byte sequence and the boundary
  4-byte sequences. Compression links unchanged.
- [x] 1.3 `joyeer_string_equal_abi` and `joyeer_string_compare_abi` are Joyeer
  (`strings.joyeer`, `memcmp` through an ordinary binding until 3.2). String byte
  access is now an inline checked load in the backend, and
  `joyeer_string_byte_at_abi` is removed from the ABI; `text[index]` has native
  trap fixtures for negative, past-end and empty. Dictionary string keys call the
  exported equality. Release benchmarks match the Stage 0 baseline (1 KiB equal
  and compare 34.2 ns, 16 B within noise).

Finding: the symbol gate caught that a `[UInt8]` constant passed to a `pointer:`
parameter makes a Debug build import the runtime's array clone and destroy. Core
units must use scalars and addresses only (D3). The runtime therefore has no way
yet to hand `joyeer_panic` a message from Joyeer; see D7 and §6 item 9.

### Stage 2: Spikes and decisions, done

- [x] 2.1 Entry spike: a Joyeer object exporting `wmain`, linked ahead of the
  runtime archive, is selected by the Windows x64 linker, calls the generated
  `joyeer_main` and propagates its exit status. The compiler rejects exporting
  `main`/`wmain` today and the runtime archive is linked whole, so the Joyeer
  entry must replace `entry.c` in one change (7.1). **Linux and macOS were not
  run (no host); do that before 7.1.** Details in
  [decision 0009](../decisions/0009-runtime-core-capabilities.md).
- [x] 2.2 Indirect-call spike, kept as the implementation:
  `joyeer_intrinsic_call_void1` and `_void2` (table, type checker, verifier,
  backend, native fixture `JoyeerCInterop.Callbacks*`). A Joyeer loop through them
  costs 1.57 ns per element, the same as a true indirect call in C
  (`cinterop-indirect-benchmark`).
- [x] 2.3 Decisions D1, D2, D3, D4, D5 and D7 recorded in decision 0009; the
  indirect calls are in spec §19.4.5 and the other accepted designs in §19.4.6.

### Stage 3: Compiler capabilities (R1)

Each with unit tests for lowering, misuse and tampered IR, and a native fixture.
Designs are in [decision 0009](../decisions/0009-runtime-core-capabilities.md).

- [x] 3.1 Static storage slots (D1).
- [ ] 3.2 Bulk memory intrinsics (needs its own decision first); switch 1.3 from
  `memcmp` to them.
- [ ] 3.3 Non-returning exports (`Never`) and `main`/`wmain` exports in core units
  only (D4, D7).
- [x] 3.4 Indirect calls (D2), done in 2.2.
- [x] 3.5 The core-dialect diagnostic (D3, `--runtime-core-module`), turned on for
  every unit in `src/runtime/joyeer/`.
- [ ] 3.6 Data-symbol imports (D5, `@c_data`).
- [x] 3.7 The constant-data address intrinsic, for panic messages (D7).

### Stage 4: Substrate (R2)

- [ ] 4.1 `joyeer_panic` in Joyeer (D7): message, exit status and the
  no-allocation property unchanged.
- [ ] 4.2 The allocation counter in a static slot, `alloc` / `realloc` / `free`
  taking sizes, and `joyeer_runtime_active_allocations`. The remaining C calls the
  Joyeer allocator through its exports, so there is one counter.

### Stage 5: Strings and printing (R3)

- [ ] 5.1 `joyeer_string_clone_abi`, `joyeer_string_destroy_abi`,
  `joyeer_byte_to_string_abi`.
- [ ] 5.2 `joyeer_string_concat_abi`: one allocation, including for empty input.
- [ ] 5.3 `joyeer_print_string_abi`, and `print` for every integer width, `Bool`
  and `UInt8` (decimal formatting in Joyeer).

### Stage 6: Collections (R5, R6)

- [ ] 6.1 Arrays: create, clone, append with the current growth policy, destroy.
- [ ] 6.2 The native buffer helpers (`joyeer_native_{bytes,strings}_init`,
  `joyeer_native_{bytes,string,strings}_assign`); Compression tests pass.
- [ ] 6.3 Dictionaries: create, set with duplicates, find, at, clone, destroy, for
  every key kind.

### Stage 7: Entry and native-library API (R7, R8)

- [ ] 7.1 `main` / `wmain` in Joyeer: argument conversion, leak check, exit-status
  range check; all entry tests pass.
- [ ] 7.2 Replace `joyeer_native_errno`, `joyeer_native_environment` and
  `joyeer_native_stdio` with direct bindings (D5); update `System`.

### Stage 8: Floating-point printing (R4), parallel track

Start any time after 5.3.

- [ ] 8.1 Decide the algorithm (D6) and build the oracle: a C test that compares
  Joyeer output with the C library over boundary values and a large random sample.
- [ ] 8.2 `Double` output byte-identical to `%.17g`.
- [ ] 8.3 `Float` output byte-identical to `%.9g`.

### Stage 9: Remove the C (R9)

- [ ] 9.1 Delete `runtime.c`, `entry.c`, `host.c` and the C-only build settings
  (`C_STANDARD`, `/experimental:c11atomics`); keep `runtime.h` and `native.h` for
  native libraries.
- [ ] 9.2 Add the archive check: no object not produced by the Joyeer compiler.
- [ ] 9.3 Move `JoyeerRuntime.*` into `lib/`
  ([toolchain libraries](toolchain-libraries.md) §3).
- [ ] 9.4 Platform acceptance (§8) on Windows x64 and ARM64, Linux and macOS,
  together with the POSIX verification of `System` ([filesystem plan](filesystem.md) §2).
  **Tier 1 is done here.**

### Stage 10: No C library (Tier 2: R10 to R12)

- [ ] 10.1 D8, then the Joyeer allocator over `VirtualAlloc` / `mmap`, behind the
  stage 4 contract; stress and fragmentation tests.
- [ ] 10.2 D10, then output and exit straight to the system interface (R11).
- [ ] 10.3 D9, then our own entry point on Windows and Linux (R12).
- [ ] 10.4 `System` off the C library's stdio and `errno`, using static state for
  buffering.
- [ ] 10.5 The Tier 2 `llvm-nm` gate on Windows and Linux, and the macOS
  equivalent. **Tier 2 is done here.**
