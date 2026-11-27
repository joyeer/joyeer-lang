# 0008: Runtime in Joyeer

- Status: in progress (updated 2026-11-27). The runtime is Joyeer; what is open is platform
  evidence, the R0 performance gate and the macOS `System` stdio imports.
- Decisions: [0005](../decisions/0005-runtime-unsafe-memory.md),
  [0008](../decisions/0008-runtime-core-build-shape.md),
  [0009](../decisions/0009-runtime-core-capabilities.md),
  [0010](../decisions/0010-floating-point-printing.md),
  [0011](../decisions/0011-runtime-without-the-c-library.md)
- Spec: [§19.4.5 and §19.4.7](../../specs/19-c-interop.md#1947-core-units-never-results-data-symbols-and-the-program-entry-point)

## Goal

Every line of the runtime is Joyeer, and the runtime needs nothing from the C library beyond
what decision 0011 keeps. `JoyeerRuntime` is built only from objects the in-tree compiler emits
from `lib/Runtime/`, it keeps the C ABI that `runtime.h` and `native.h` declare, and on each
claimed platform it is as fast as the C runtime it replaced. A language meant to replace legacy
software cannot need a second language and toolchain for its own runtime, and an allocator in
Joyeer makes the leak check a property of Joyeer code.

## Definition of done

- The archive holds only Joyeer-emitted objects (`RuntimeArchiveMembers`) and provides every
  symbol `runtime.h` and `native.h` declare.
- The runtime archive and linked `System` programs import nothing from the C library except the
  [compiler support symbols](../decisions/0011-runtime-without-the-c-library.md#decision)
  (`memcpy`, `memmove`, `memset`, `memcmp`; `_fltused` and `__chkstk` on Windows) and the
  platform startup decision 0011 keeps:
  - Windows: Win32 only, entering at `joyeer_start` when the program calls no C library.
  - Linux: kernel system calls, plus the glibc startup that calls `main`.
  - macOS: libSystem only; no C heap, stdio or CRT startup symbols.
- Each gate above, and unfiltered CTest and `fern test` in Debug and Release, has a fresh
  native result on Windows x64, Windows ARM64, Linux x86-64, Linux AArch64 and macOS ARM64.
  One host does not certify another.
- At `-O2`, each R0 benchmark row is within 10% of the C runtime on the same host, or an
  accepted exception records the gap and why.

## Current state

All runtime code is Joyeer (`lib/Runtime/core/*.joyeer` and the library preludes); only the C
headers in `lib/Runtime/include/joyeer/` remain. See [runtime](../impl/runtime.md) for the
current layout, allocator, entry, streams and collections, and decisions 0009 to 0011 for the
design. The core dialect that runtime core units follow is D3 in
[decision 0009](../decisions/0009-runtime-core-capabilities.md).

Platform results so far:

- Windows x64 and Linux x86-64 (WSL2): full validation passed in Debug and Release, most
  recently [at `5d75dad`](../decisions/0031-equatable.md#platform-validation). The per-object
  import checks, `RuntimeSystemProgramHasNoCRuntime_*` (Windows) and
  `RuntimeSystemProgramNeedsOnlyTheCStartup_*` (Linux) pass.
- macOS ARM64: at `5c21924e` (LLVM 22.1.8, SDK 26.5), Debug and Release each passed CTest
  2230/2230 and `fern test` 7/7 without reruns, and
  [relocated installs](../impl/fern.md#macos-arm64-toolchain-validation) ran `hello`. The
  runtime archive imports only libSystem. Linked `System` programs still import stdio (below).
- Windows ARM64 and Linux AArch64: not run. The code paths exist
  (`memory_windows_arm64.joyeer`, AArch64 system-call numbers, the AArch64 startup imports in
  `tests/unittests/runtime/CMakeLists.txt`).

R0 benchmark (`cmake --build <dir> --target runtime-benchmark`, method in
[tests/perf](../../tests/perf/README.md#comparing-two-builds)). The C runtime is frozen at
`456b78d9`; compare it with the Joyeer archive using the same harness object on one host.

- macOS ARM64 (`5c21924e`, median of five interleaved runs, seven repetitions each), Joyeer
  against C in ns/op: array append 6.10 against 3.17, dictionary set 752.67 against 323.08,
  dictionary find 1248.98 against 624.29. The 16-byte unequal string compare is 2.49 against
  2.26, near the limit; other rows are within it or faster. The harness measures the legacy
  collection ABI, not the library `Array` and `Dict` APIs.
- Windows x64 (`00b245a3`, AMD EPYC 7763, 2026-11-27): the `456b78d9` harness compiled once
  with `cl /O2 /Ob2 /MT` and linked against the `456b78d9` C runtime (built with the same flags,
  without `entry.c`) and against the Release `JoyeerRuntime.lib`. Fifteen interleaved rounds,
  alternating A/B order, pinned to one core; minimum and median agree within about 1% on every
  row. Median ns/op, Joyeer against C:

  | Benchmark | C | Joyeer | Ratio |
  |---|---|---|---|
  | string equal, equal, 16 B | 5.63 | 5.63 | 1.00 |
  | string compare, last byte differs, 16 B | 5.32 | 6.26 | **1.18** |
  | string concat + destroy, 16 B | 43.50 | 21.97 | 0.51 |
  | string clone + destroy, 16 B | 40.80 | 18.81 | 0.46 |
  | string equal, equal, 1024 B | 33.90 | 33.88 | 1.00 |
  | string compare, last byte differs, 1024 B | 34.35 | 34.56 | 1.01 |
  | string concat + destroy, 1024 B | 74.11 | 40.89 | 0.55 |
  | string clone + destroy, 1024 B | 57.08 | 28.13 | 0.49 |
  | array append, grow from empty (`Int`) | 8.88 | 9.27 | 1.04 |
  | array clone + destroy, 1000 elements | 5.72 | 3.13 | 0.55 |
  | dictionary set, 1000 `Int` keys | 1394.60 | 1370.00 | 0.98 |
  | dictionary find, last of 1000 keys | 2806.00 | 2658.75 | 0.95 |

  The 1.18 row is code placement, not code generation. Both functions call the same
  `libvcruntime` `memcmp` and run nearly the same instructions on the benchmark's path (`memcmp`
  is negative). The C function happened to start on a 64-byte boundary. Relinked with padding so
  it starts 16, 32 or 48 bytes into a line, it takes 5.95 ns/op at every offset, and Joyeer's
  6.26 is 1.05 of that. The equality row shows the same effect in reverse: shifted C equality
  takes 6.26 ns/op, while Joyeer's equality, at a luckier address, takes 5.63. Rows under about
  10 ns can move by about 0.6 ns with placement alone, so compare them across placements before
  calling a gap real.
- Linux: no same-host Joyeer against C comparison recorded.

## Todo

### Phase 1: Performance gate

- [ ] Profile `joyeer_array_append_owned_abi`, `joyeer_dictionary_set_owned_abi` and
  `joyeer_dictionary_find_abi` on macOS ARM64, fix the cause, and remeasure against `456b78d9`
  (§ Current state). Done when each row is within 10% or an exception is accepted.
- [x] Run the same-host R0 comparison on Windows x64 and record every row here. Evidence:
  the Windows x64 table under Current state (2026-11-27, `00b245a3`).
- [x] Explain the 16-byte unequal string compare gap on Windows x64 (6.26 against 5.32 ns/op).
  Evidence: placement sweep under Current state (2026-11-27). Against the C function at offsets
  16, 32 and 48 the ratio is 1.05, within the gate; no code change.
- [ ] Check the macOS ARM64 16-byte unequal compare (2.49 against 2.26, near the limit) across
  function placements the same way. blocked: macOS ARM64 host.
- [ ] Run the same-host R0 comparison on Linux x86-64 and record every row here.

### Phase 2: macOS `System` without stdio

- [ ] Move the macOS file primitives in [filesystem_posix.joyeer](../../lib/System/filesystem_posix.joyeer)
  from stdio streams to libSystem `open`/`read`/`write`/`close` file descriptors
  (`legacy_file.joyeer` reaches stdio only through them). blocked: needs a macOS ARM64 host to verify. At
  `5c21924e` stdio appeared at `-O0` in the `filesystem`, `process`, `read_file` and
  `stdin_lines` import fixtures, and at `-O2` in `filesystem` and `read_file`.
- [ ] Add a macOS `System` import gate that forbids stdio, like the Linux one, and pass it in
  Debug and Release. blocked: macOS ARM64 host.

### Phase 3: Remaining platforms

- [ ] Windows ARM64: full validation in Debug and Release, and the Tier 2 import gates
  (own-entry link through the kernel32 memory wrappers). blocked: no Windows ARM64 host.
- [ ] Linux AArch64: full validation in Debug and Release, and
  `RuntimeSystemProgramNeedsOnlyTheCStartup_*` with the AArch64 startup imports. blocked: no
  Linux AArch64 host.
- [ ] Linux: record checks of the `statx` and `getdents64` layouts against the kernel headers,
  on x86-64 and AArch64.
- [ ] Linux and Windows: record child-environment and exact error-code evidence for `System`
  process and file operations, as the macOS run did.
- [ ] Record the minimum Linux kernel and glibc versions the system-call and startup paths
  need, in [building](../building.md).

## Open questions

- Whether WSL2 counts as native Linux x86-64 evidence. The recorded Linux runs all used it.
- Whether to accept a performance exception for the legacy collection ABI instead of tuning it,
  since generated code now reaches collections through the library `Array` and `Dict`
  ([decision 0026](../decisions/0026-library-array.md)). Record the answer in decision 0011.

## Out of scope

- Replacing the operating system interface (libSystem, the kernel, kernel32) or the dynamic
  loader; an own `_start` on Linux (decision 0011 keeps the glibc startup).
- Freestanding, kernel and embedded targets; selectable runtime profiles.
- Changing `Float`/`Double` print format to shortest round-trip (decision 0010).
- Compiler-specialized collections replacing the type-erased helpers; that waits for generics.

## Progress log

- 2026-11-27: Ran the Windows x64 R0 comparison. Strings, arrays and dictionaries are within
  the gate or faster, except the 16-byte unequal string compare (1.18). A placement sweep showed
  that gap is the C function's lucky 64-byte alignment (1.05 otherwise); split the macOS check
  into its own item.
- 2026-11-27: Rewrote the plan to the template. Removed completed stages 0 to 10 (now in
  decisions 0008 to 0011 and the impl docs). Aligned the definition of done with decision 0011:
  compiler support symbols stay, and Linux keeps the glibc startup. Split the platform-evidence
  item into one item per check, and added Windows x64 and Linux performance comparisons, which
  were never recorded.
- 2026-11-26: Verified macOS ARM64 functional validation at `5c21924e`. Rechecked the four linked
  `System` fixtures; six fixture and profile combinations still import stdio. Recorded the
  same-host R0 benchmark gaps.
- 2026-11-22: Completed the move of `JoyeerRuntime.*` into `lib/`; retired the host-operations
  and standard-library plans.
