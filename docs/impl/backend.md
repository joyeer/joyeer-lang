# Compiler Backend Implementation

> **Status:** The supported language surface emits textual LLVM IR;
> the platform backend library validates it, generates machine code, and links
> native executables with in-process LLVM/LLD on Windows, macOS, and Linux.
> The current implementation covers the native acceptance workload and
> regression cases for conditional evaluation, ownership flow, guarded
> payloads, and loop storage. Verifier, native ABI, and library gaps remain.
> User-defined `deinit` and explicit copy initializers are outside the
> [implemented language surface](supported-features.md).

For the C runtime and process startup, see [Runtime](runtime.md).

---

## 1. Pipeline

The compiler pipeline is:

```text
source
  -> lexer / parser
  -> name resolution
  -> type checking
  -> control-flow semantic analysis
  -> verified Joyeer IR
  -> textual LLVM IR
  -> platform native backend object generation + link
  -> native executable
```

The LLVM emitter lives in `include/joyeer/backend/llvm.h` and
`lib/backend/llvm.cpp`. It emits text so verified Joyeer IR remains separated
from LLVM's C++ model by an inspectable format.

For module compilation, one verified Joyeer IR module contains the reachable
graph of named explicit source-file sets. This whole-graph code-generation
container does not merge logical modules or their visibility boundaries, and
does not imply separately linkable library artifacts or a library ABI.
LLVM function linkage uses `joyeer_fn_<FunctionId>`;
aggregate types and ownership helpers likewise use numeric type IDs rather
than source names. Equal names in different modules or file-private scopes
therefore do not collide, and source declarations are not renamed to enforce
entry-point rules.

The native linker lives in `include/joyeer/backend/linker.h` and
`lib/backend/linker.cpp`. It calls the versioned C ABI in
`include/joyeer/backend/native_backend.h` on every platform. The
native-backend library parses and verifies textual IR, runs the selected LLVM
optimization pipeline, emits a COFF, Mach-O, or ELF object, and invokes the
corresponding LLD driver in-process. A mutex serializes LLD because its library
entry point has global process state. LLVM/LLD remain private backend
dependencies; the compiler never launches Clang or an LLD executable for
native output.

The DLL discovers MSVC, the Universal CRT, and Windows SDK library directories
through LLVM's Visual Studio Setup Configuration and registry helpers. It then
links the generated object with the sibling `JoyeerNativeRuntime.lib`. The
release package keeps the DLL, runtime archive, and `joyeer.exe` together.

Windows supports native x64 and ARM64 builds. The compiled backend architecture
selects the MSVC/UCRT/Windows SDK library directories and the COFF machine
argument. Object emission rejects an LLVM default target that disagrees with
the backend architecture. CMake selects the matching DIA SDK library and
checks the LLVM SDK's native architecture. This is native compilation, not an
architecture-switching or cross-compilation API; the versioned C ABI is
unchanged.

On macOS, CMake resolves the active SDK path and version with `xcrun`. The
backend passes them to the embedded Mach-O LLD driver together with the host
architecture and `libSystem`. Debug builds run the system `dsymutil` only to
materialize a sibling dSYM after code generation and linking are complete.
More precisely, this occurs whenever `-g` enables line tables, regardless of
the compiler optimization level; ordinary native output has no tool
subprocess. On Linux, the private Clang
Driver library discovers the host ABI inputs and returns an ELF linker command
that embedded LLD executes without spawning a process. Supported build SDK
versions and platform prerequisites are documented in
[Building Joyeer](../building.md).

---

## 2. CLI

### Argument encoding and filesystem boundary

On Windows the compiler uses `wmain`, not narrow `main`. Its CLI constructor
converts each UTF-16 argument once to UTF-8 with `WideCharToMultiByte(CP_UTF8,
WC_ERR_INVALID_CHARS)`, using a sizing pass followed by the conversion. This
preserves empty arguments, spaces, and supplementary Unicode. Invalid surrogate
sequences are rejected before option parsing, without replacement characters.
The existing `source-file.read-failed`, `driver.output-file-error`, and
`module.read-source` diagnostic IDs identify invalid source/general, output,
and dependency-source arguments respectively. No console code page is changed.

The shared narrow parser treats executable, positional input, output, and
`--module-source name=file` paths as UTF-8 via `std::filesystem::u8path`, not the
Windows active code page. CLI missing/duplicate-source diagnostics encode
their paths with `u8string`. Logical module names retain their ASCII syntax.
Native path objects are passed unchanged through `CompileOptions`; `SourceFile`
opens them with the filesystem-path stream overload and already uses UTF-8
for source/debug identities. Successful validation and textual-IR output
therefore do not require narrowing the CLI source/output paths.

The compiler service uses UTF-8 for module/output path diagnostics. Native
artifact and temporary-path construction preserves filesystem path objects;
suffixes are appended without narrowing, and backend/linker path strings use
UTF-8 at the C ABI boundary. These changes are integrated and compiled in the
Windows x64 Debug build. The observed joypm project smoke workflow succeeded,
but the registered Unicode and temporary-path/failure regressions have not run;
this does not establish general Unicode-complete linking or recovery.
Detailed smoke evidence belongs in [joypm](joypm.md#5-added-tests-versus-validation).
Diagnostic argument/path text is UTF-8, but OS-provided error messages and
console rendering remain host-dependent.
Legacy source-language `readFile` narrow CRT behavior is unchanged.

Validation only:

```pwsh
joyeer source.joyeer
```

Write LLVM IR:

```pwsh
joyeer --emit-llvm output.ll source.joyeer
joyeer -O0 -gfull --emit-llvm output.debug.ll source.joyeer
```

Build a native executable:

```pwsh
joyeer -o output.exe source.joyeer
```

Compile a named root source set and explicitly supply its dependency files
(example paths):

```pwsh
joyeer --module-name acme.app root1.joyeer nested\root2.joyeer --module-source acme.config=path\config.joyeer --module-source acme.config=other\parser.joyeer -o app.exe
```

`--module-name <logical.name>` selects named compilation mode and names the
root unit. It requires a valid, nonempty module name and at least one
positional source file. `--module-source <logical.name>=<source-file>` requires
named mode and supplies one dependency file. Repeating it with the same name
**adds files to one dependency module**; it does not declare duplicate modules.
A dependency name must also be valid and nonempty and cannot use the root
module's name. Names use the existing ASCII dotted-name syntax. Each complete
name is a logical identity, not a directory path or parent/submodule relationship.

Without `--module-name`, the legacy `joyeer input.joyeer` form accepts exactly
one source file and no dependencies. Multiple positional files or any
`--module-source` without a root name are errors. The old `--module-root` and
`--module` directory options have been removed as an intentional breaking
early-development interface change. They produce migration errors: replace
the root directory with `--module-name` plus explicit positional source files,
and each dependency directory with one `--module-source name=file` per file.
They are not aliases for the new options.

CLI source paths resolve against the process working directory, including
dependency paths, not against the first source file or another module.
In the service API, `CompileOptions.moduleName` names the root unit,
`CompileOptions.sourceFiles` lists its explicit source files, and
`CompileOptions.modules` contains dependency `ModuleSources { name, files }`
records. The former `ModuleMapping` and `moduleRoot` API are removed, not
directory-discovery alternatives. Relative source files resolve against
`options.workingDirectory`, falling back to the process working directory when
it is empty. Source paths are canonicalized and sorted for deterministic
ordering. All supplied root and dependency sets must be nonempty, and every
path must exist and name a regular file. Duplicate source identities within
one module or across any supplied modules are errors, including aliases
through symlinks or hard links. Validation covers **all supplied sets** before
compilation of reachable dependencies, including unused dependency inputs.
Source files do not require a `.joyeer` extension, and their paths may contain
spaces; there is no extension-based filtering. Quote a CLI path containing spaces, or
the complete dependency argument, for example
`--module-source "acme.config=path with spaces\config.joyeer"`. Process APIs
should pass each path or `name=file` value as one argument without shell quotes.

Files in one module may be nested or unrelated on disk, and different modules
may select distinct files in the same directory. The compiler does not scan
directories, recursively or otherwise, add sibling files, read manifests, or
fetch dependencies. A build or package tool owns source-root and include/exclude
policy and supplies the selected file paths.

Files retain separate syntax trees, spans, and debug identities. The compiler
resolves file-local imports through the exact supplied names and compiles only
the reachable graph, rejecting unknown imports and cycles with diagnostics.
Unused dependency sets are validated but not parsed. Output must not alias any
supplied source, including unused dependency inputs; on Windows this also
applies to the native output's sibling PDB cleanup path. Use `--emit-llvm`
instead of `-o` for textual IR, or omit both for validation and lowering only.

A native executable requires one root-module `func main()` returning `Void` or
`func main(args: [String]): Int` with a borrowing `args` parameter. The emitter
does not select a dependency module's `main` as the executable entry point. It
rejects multiple root-module `main` candidates, including distinct file-private
declarations, with an entry diagnostic instead of selecting the first.
Dependency functions named `main` are ordinary functions and need not have an
entry-compatible signature. The emitter
provides C-callable `joyeer_main_uses_arguments` and `joyeer_main` trampolines;
the latter receives a pointer to runtime-owned argument storage and returns
an `int64_t` status (zero for the parameterless form). No collection is passed
by value across the generated-module/runtime boundary. Missing or invalid
entry signatures are diagnosed rather than delegated to the linker. The
[runtime entry](runtime.md#process-entry-and-exit) owns argument
storage, cleanup, and exit-status validation.

Native executable linking has an explicit optimization policy: `-O2` is the
default, and `-O0`, `-O1`, `-O2`, or `-O3` may override it on the CLI. The
selected level controls the backend library's LLVM optimization and
code-generation pipelines on every platform. `--emit-llvm`
intentionally writes the pre-optimization backend IR so it remains
deterministic and inspectable.

Checked `Int` add/subtract/multiply lower to LLVM's signed
`*.with.overflow.i64` intrinsics, with a branch to the existing panic routine
on overflow. Checked `Int` divide/remainder first branch to panic for a zero
divisor, then for the pair `INT64_MIN` and `-1`, before executing LLVM `sdiv`
or `srem`. Division truncates toward zero; a nonzero remainder has the
dividend's sign. Both operations trap on the overflow pair, including
remainder, so neither can evaluate undefined arithmetic or produce LLVM
poison on these inputs. The panic messages match the checked C runtime
entry points.

Array reads and mutable projections lower to null/negative/
upper-bound checks followed by a typed `getelementptr`, rather than a
per-access runtime call. The IR verifier checks the array element/result type;
construction and growth validate the allocation size using that same element
layout. An in-range access therefore needs neither a header stride lookup nor
a repeated offset-overflow check. This also handles byte, Boolean, padded
aggregate, nested, and zero-sized element layouts.

These checks are emitted at every optimization level. LLVM may eliminate
only checks proven redundant, not replace trapping arithmetic with unchecked
`nsw` operations. Expanded branches, panic calls, and accesses retain the
source operation's debug location. Array ownership, operand evaluation order,
append/growth, and destruction remain unchanged.

Postfix `?` uses the existing enum-tag branch, payload extraction, and return
instructions from Joyeer IR. The backend guards payload reads by the tag;
no separate runtime propagation operation is required.

Debug information defaults to off (`-g0`). `-g` and `-gline-tables-only`
enable line tables in the host format; `-gfull` adds lexical scopes, source
variables, and physical type metadata. `-gdwarf` selects DWARF 4, and
`-gcodeview` selects CodeView on Windows. Format selection and enable/disable
options compose in command-line order; for example `-gdwarf -g0 -g` produces
DWARF line tables. Debug and optimization are orthogonal: `-O0` marks the
compile unit unoptimized, while `-O1`…`-O3` carry optimized debug flags without
changing the emitted pre-optimization instructions.

The graph's `Module::sourceFiles` table supplies one `DIFile` per real input
file. The compile unit uses the first root source; each source function,
lexical block, variable, and declared aggregate type selects its own file
from `SourceSpan::sourceId`. Line and column lookup uses that file's line-start
table and unshifted byte offsets. If an instruction location names a different
file from its enclosing scope, a `DILexicalBlockFile` preserves the scope's
function while selecting the location's file. This applies to both line-table
and full-debug emission, in DWARF and CodeView formats. The legacy single-file
`sourceInfo` form remains supported as source ID zero.

Native artifacts follow the host format. Windows CodeView keeps a sibling PDB
with the executable and embeds a CodeView debug-directory reference. Windows
DWARF is emitted by the same backend DLL and keeps DWARF sections in the PE
executable. ELF keeps DWARF sections in the executable; macOS runs `dsymutil`
after the in-process Mach-O link to create a sibling dSYM bundle. `-O0`
disables reference elimination
and identical-code folding for Windows debug links; optimized levels retain
them. Paths cross the DLL as UTF-8 C strings and LLD receives an argument
array, so user paths are not subject to shell expansion.

UTF-8 at the backend boundary does not imply end-to-end Windows Unicode path
support for every path or failure mode. The wide compiler entry and native
artifact/temporary-path preservation are integrated and compiled, with scoped
Windows x64 joypm smoke evidence. Registered source/output/temporary-path and
backend regressions still need execution; the smoke workflow is not full
Unicode, native-platform, or release acceptance.

### C ABI lifetimes and failure contract

Options and their input buffers are borrowed for the duration of a call.
Diagnostic callbacks are synchronous; their message storage must be copied
if retained beyond the callback. The version string has static ownership and
must not be freed by a caller. Callbacks must not throw or recursively invoke
linking while the serialized LLD operation is active.

The intended boundary translates failures into status codes and diagnostics,
without exposing C++ exceptions or allocator ownership. The failure-path
gaps below mean it is not yet a complete recovery boundary.

---

## 3. LLVM type and ABI mapping

| Joyeer type | LLVM representation |
|---|---|
| `Int` | `i64` |
| `Bool` | `i1` |
| `UInt8` | `i8` |
| `String` | `{ ptr, i64 }` |
| `Array<T>` | `{ ptr, i64, i64 }` |
| `Dict<K,V>` | `{ ptr, i64, i64 }` |
| user `struct` | named LLVM struct with declaration-order fields |
| enum / `Optional` / `Result` | named `{ i32 tag, [N x i64] payload }` |
| `inout T` / `initializing T` | opaque `ptr` |

Enum payload size is the maximum aligned case payload. Pattern lowering emits
source-ordered tag/literal tests and an unreachable miss, assuming frontend
exhaustiveness. Tag branches precede payload interpretation, including nested
patterns; the type checker accounts for refutable payload coverage.

Internal Joyeer calls pass value parameters/results as their LLVM types;
address parameters use pointers. Runtime C calls follow a separate convention:
LLVM decomposes strings and collection handles into pointers/counts, or uses
out-pointers for aggregate results rather than passing C structs by value.
This avoids target-specific C aggregate calling-convention drift.

These layouts are private implementation choices, not source-language C FFI
or a stable Joyeer library ABI. Value semantics alone do not promise a C struct
layout, a one-byte `Optional<Bool>`, a register-only `Result`, or the absence of
platform unwind/startup metadata. Interoperability requires a separate layout,
calling-convention, and initialization contract.

---

## 4. Runtime boundary

The C11 [runtime](runtime.md) owns process startup, checked
arithmetic and bounds helpers, value storage, collections, file input, and
allocation-balance checking. It is a static archive linked into generated
programs, not a dependency on LLVM/LLD.

Descriptor-driven host calls include
`writeStderr(contents: String): Result<Void, StderrError>`, where
`StderrError` has `WriteFailed(Int)`. The emitter declares/calls
`i32 @joyeer_write_stderr_abi(ptr, ptr, i64)` with the error-code out-pointer
first and borrowed contents data/count afterward. ABI status `0` constructs
`Ok(())`; status `1` constructs `Err(.WriteFailed(code))`. Error tags are mapped
by enum case name, and invalid runtime tags trap. Unit successes use the
existing zero-sized representation, not a runtime result out-pointer. See the
[standard-error boundary](runtime.md#standard-error-boundary) for byte/flush
semantics and CRT error reporting.

---

## 5. Known limitations

The native pipeline does not yet provide its final layout, optimization, or
verification guarantees:

- all four parameter effects are accepted; `consuming` supports owning locals,
  consuming parameters, temporaries, and field/subscript projections, while
  `initializing` supports whole mutable local/forwarded storage. Call-site
  and argument-evaluation exclusivity traverse the supported expression
  surface; [type-checking](type-checking.md#8-known-correctness-gaps) and
  [flow-analysis precision limits](semantic-analysis.md#precision-limits)
  remain documented separately;
- aggregate layout has no niche optimization and uses an `i32` tag plus an
  aligned payload buffer;
- all platforms use LLVM's per-module default optimization pipelines;
  no Joyeer-specific pass pipeline or LTO policy is configured;
- the textual emitter can generate source/function/instruction line tables or
  full lexical-scope/variable/type metadata with DWARF 4 or CodeView module
  flags; compiler-generated cleanup/plumbing is suppressed from line rows;
- aggregate debug metadata currently supplies names and sizes with empty
  member lists, not field/payload debugger structure or optimized-value
  location tracking.

Conditional evaluation, guarded payload comparisons, prepared-operand cleanup,
and one-time stack allocation are covered by native regressions at O0 and O2.
See the [IR invariants](ir.md#7-evaluation-and-storage-invariants) and remaining
structural verifier limitations. Runtime and library limitations are described
in [runtime.md](runtime.md#limits-and-validation).

These gaps must be addressed in Joyeer IR or LLVM lowering without creating a
second execution pipeline.

### Native ABI hardening gaps

The following failure paths were identified from source and SDK contracts;
they are separate from the reproduced Unicode-path issue:

- Object output checks stream errors before closing and returns without
  clearing an observed error. LLVM 22.1.8's `raw_fd_ostream` can then terminate
  through its fatal-error destructor path instead of returning `FILE_ERROR`.
  Close-time failures also need explicit handling.
- Linux ELF argument/Clang Driver preparation occurs outside the exception
  guard used by the later linking call. The whole exported operation needs
  exception translation.
- The current IR parser path expects an accessible trailing NUL even though
  the C options describe a pointer/length pair. The internal `std::string`
  caller satisfies this; arbitrary C buffers need a defensive terminated
  copy inside the backend rather than an undocumented extra-byte requirement.
- Invalid C-ABI optimization values currently fall back to O2 instead of
  producing `INVALID_ARGUMENT`.
- Some unrecoverable LLD paths terminate the process; callbacks and fatal
  failures must not be described as universally recoverable status returns.

The existing C ABI smoke test covers version/platform queries and invalid
zero-initialized options. Successful object emission, malformed IR, output
failures, structure-size boundaries, and callback contracts need dedicated
coverage. Disk exhaustion and Linux exception paths were not exercised by
the Windows review.

---

## 6. Validation

Focused tests:

```pwsh
ctest --test-dir build -L llvm-backend --output-on-failure
ctest --test-dir build -L native --output-on-failure
ctest --test-dir build -L optimization --output-on-failure
ctest --test-dir build -L debug-info --output-on-failure
```

The existing host compiler unit file checks stderr's concrete signature,
error payload/matches, type/error-conversion rejections, and flat ABI emission
with full debug locations. The CLI unit file checks UTF-8 and Windows wide
input/output/dependency paths containing supplementary Unicode and spaces,
UTF-8 path diagnostics, empty wide arguments, and unpaired surrogate rejection
with existing diagnostic IDs. These CLI tests inspect parsing/path ownership;
they do not assert that native linking is Unicode-complete.

The Clang executable is an optional test tool, not a product linker. The
independent textual-IR oracle and many native-executable regression tests are
registered only when `JOYEER_CLANG_EXECUTABLE` is found at configuration time.
Check the configure message and discovered tests before treating a label run
as the full native acceptance suite.

When available, those tests use Clang to independently validate textual IR
and the host's `joyeer-backend` shared library to compile and link native
Joyeer programs. They verify compiler output and debug-info metadata in both
DWARF/CodeView modes and make the configured Clang emit objects containing the
corresponding DWARF `.debug_line` and Windows CodeView `.debug$S` sections.
`NativeBackendAbiTests` is compiled as C and checks the shared library's C
ABI/version without exposing C++ types. Native artifact tests additionally
validate no-debug output, Windows PDB source and line records, embedded
Windows/ELF DWARF sections, platform artifact retention, safe metacharacter
paths, and refusal to overwrite output/PDB directories.

`NativeExecutableCompilationUnitsO0`, `NativeExecutableCompilationUnitsO1`,
`NativeExecutableCompilationUnitsO2`, and `NativeExecutableCompilationUnitsO3`
exercise the explicit source-set graph at each optimization level.
`NativeExecutableCompilationUnitsDebug` covers the same compilation-unit
boundary with debug information. The fixtures include nested source paths and
a directory containing spaces while retaining per-file debug identities.

Optimization tests verify the default and all accepted CLI levels, then
compile at `-O2` and prove checked integer overflow and array bounds still
terminate with their runtime diagnostics. Runtime traps and ownership-heavy
native tests are detailed in [runtime validation](runtime.md#limits-and-validation).

---

## 7. Performance and footprint measurement

The [language cost goals](../spec/00-preamble.md#012-cost-goals) are not
measured performance parity or a fixed binary-size budget. Before adopting
numerical acceptance thresholds, establish reproducible workloads and record:

1. Target architecture, OS/SDK, compiler revision, optimization level, debug
   mode, C-runtime linkage, and whether LTO or stripping was used.
2. Generated-program sizes separately from the compiler/backend package;
   embedding LLVM in the compiler does not make it a program runtime dependency.
3. Allocation count, peak live storage, copied bytes, and final cleanup
   balance. Zero outstanding allocations at exit demonstrates cleanup for
   that workload, not allocation-free execution or low peak memory.
4. Equivalent ownership, error-handling, overflow, and bounds semantics in
   comparison implementations.
5. Ordinary and consuming copies, nested collections, mutation-heavy
   workloads, and failure paths at multiple optimization levels.
6. Results on supported platforms rather than extrapolating from one host.

Account for atomic allocation-balance updates and indirect collection
clone/destroy callbacks as real costs even without GC/ARC. Repeated string
concatenation can copy the growing prefix many times. Copy elision, check
elimination, and any future container-to-stack promotion must preserve
ownership, destruction, and observable behavior; none is a blanket guarantee
that every allocation, copy, or check disappears.
