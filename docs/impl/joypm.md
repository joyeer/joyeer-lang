# joypm Local Project Manager

> **Status:** early development, not a released or release-complete v0.
> The Windows x64-target Debug build and local-project smoke workflow succeeded;
> the originating machine is reported as ARM64, so this is not native ARM64
> validation.
> Bounded regular-file manifest acquisition and scalar host-input integration
> are compiled. **CTest has not run for this change set**: the nine registered
> acceptance gates, full native gate, other platforms, package installation,
> and self-build remain pending. See the validation evidence below for the
> scope of the observed x64-target results and the
> [receiving-machine handoff](../plan/joypm-validation-handoff.md).

`joypm` is a separate Joyeer-written executable. It selects explicit files
and invokes `joyeer`; it does not add project commands to the compiler or
change the language's module rules. `joypm --version` reports `joypm 0.1.0`;
that early-development tool version is independent of the compiler CMake
project version `0.0.1`.

## 1. Source layout and bootstrap

All six sources compile in the named root unit `joypm.app`:

| Source | Responsibility |
|---|---|
| [common.joyeer](../../src/tools/joypm/common.joyeer) | Byte/UTF-8 helpers, lexical paths, host-error wrapping, diagnostics, sorting |
| [cli.joyeer](../../src/tools/joypm/cli.joyeer) | Strict command/option parsing, help, profile and status policy |
| [manifest.joyeer](../../src/tools/joypm/manifest.joyeer) | Schema-directed TOML profile, typed records, limits, naming/path/DAG validation |
| [planner.joyeer](../../src/tools/joypm/planner.joyeer) | Target selection, module closures, deterministic plans and compiler argv |
| [executor.joyeer](../../src/tools/joypm/executor.joyeer) | Exclusive generations, compiler/artifact checks, init/run/serial tests |
| [main.joyeer](../../src/tools/joypm/main.joyeer) | Entry, manifest acquisition, command dispatch and error statuses |

[platform.joyeer.in](../../src/tools/joypm/platform.joyeer.in) generates the
host-Windows Boolean and executable suffix in the build tree. The
[bootstrap rule](../../src/tools/joypm/CMakeLists.txt) lists all sources
explicitly; it does not discover files or require an installed `joypm`.

`JOYEER_BUILD_JOYPM` defaults to `ON`. The `joypm` CMake `ALL` target depends
on the compiler, backend, and native runtime, then invokes the build-tree
compiler with `--module-name joypm.app`, `-O0 -gfull` in Debug, and `-O2 -g0`
in Release (and other non-Debug configurations). macOS Debug falls back to
`-g0` when `dsymutil` is unavailable. Debug Windows PDBs and macOS dSYM
bundles, when produced, are declared CMake `BYPRODUCTS`, not Release outputs.
Set the option to `OFF`
for a compiler-only build; cross-compiling configurations must disable it
because bootstrap executes the newly built compiler. Python is not a
source-build or released-tool requirement.

There is **no joypm self-build acceptance yet**: bootstrap and the current
test-tool compilation invoke the compiler directly. Install component
`JoyeerRuntime` contains compiler/backend/runtime/current license; `PRODUCT`
contains optional `joypm` and its PDB for Windows Debug installations.
See [building](../building.md#joypm-bootstrap-and-acceptance)
for options, installation, and pending validation.

## 2. Commands and options

| Command | Source-implemented behavior |
|---|---|
| `joypm --help` / `--version` | Used alone, without manifest or compiler |
| `joypm init <directory> --name <name>` | Scaffold one executable package in a new or empty directory with an existing parent |
| `joypm check --compiler <path>` | Check/lower every declared target, including tests; no native linking or build outputs |
| `joypm build --compiler <path>` | Build all `bin` targets, or one selected by `--target <name>` |
| `joypm run --compiler <path>` | Build/run exactly one `bin`; use `--target` if selection would be ambiguous |
| `joypm test --compiler <path>` | Build/run all explicit `test` targets, or one selected by `--target` |

Project commands accept `--manifest-path <file>` (default `joyeer.toml` in
the invocation directory), `--release`, and `--verbose`. There is no upward
manifest scanning. `--target` is allowed only for `build/run/test`. `init`
does not accept project options. `run/test` forward all arguments after `--`
unchanged; other commands reject that separator. Unknown flags/commands,
missing or empty option values, duplicate singleton flags, and invalid
combinations fail. Prefix option-value paths beginning with `-` with `./`.

The compiler path is mandatory, explicit, and resolved in the caller's
coordinates; no PATH lookup, sibling discovery, environment discovery, or
shell evaluation occurs. Debug is `-O0 -gfull`; release is `-O2 -g0`.
There is no arbitrary compiler-option passthrough. `--verbose` writes an
escaped, one-argument-per-line display to stderr, never an executable shell
command. Compiler and child streams/environment are inherited.

### Example from a Windows build tree

After a successful build, from the checkout with an existing current directory.
This example selects native ARM64; use `x64-debug` with an x64 toolchain when
that is the intended target:

```powershell
$preset = 'arm64-debug'
$compiler = (Resolve-Path ".\out\build\$preset\bin\joyeer.exe").Path
$joypm = (Resolve-Path ".\out\build\$preset\bin\joypm.exe").Path
& $joypm --version
& $joypm init .\hello --name hello
& $joypm check --compiler $compiler --manifest-path .\hello\joyeer.toml
& $joypm build --compiler $compiler --manifest-path .\hello\joyeer.toml --release
& $joypm run --compiler $compiler --manifest-path .\hello\joyeer.toml -- "two words" ""
& $joypm test --compiler $compiler --manifest-path .\hello\joyeer.toml
```

Check every exit status before proceeding. `init` generates schema version 1,
package version `0.1.0`, module `app.main`, one `bin` target, a minimal hello
entry, and a build-output ignore file. It does not create tests, so the last
command reports `0 tests` until the manifest declares them. Linux/macOS use
their native preset and executable suffix. These examples document the CLI;
the validation section records the actual smoke run separately. Example
arguments are not evidence of full forwarding or native acceptance coverage.

### Status and diagnostics

- **0:** successful command or selected children all succeed.
- **1:** manifest, compiler/build, filesystem, stderr, launch/wait, signal,
  missing-artifact, or nonzero child failure.
- **2:** usage or target-selection error (including wrong target kind).

Native child/compiler statuses, signals, and host codes are retained in
diagnostics, not truncated into the tool's three statuses. If writing an error
diagnostic itself fails, the tool returns 1, including for a usage error.
Help/version/test summaries use stdout; tool diagnostics/progress use stderr.
Manifest diagnostics include the path and 1-based line/byte-column derived
from byte offsets; this is not general Unicode-aware source rendering.

The small host addition is
`writeStderr(contents: String): Result<Void, StderrError>`, with
`StderrError.WriteFailed(Int)` carrying CRT `errno` (an I/O fallback is used
when the CRT supplies no code). It writes exact bytes without adding a newline
or validating text, then flushes. The Windows implementation uses binary
mode and restores the previous mode rather than translating LF or changing
the console code page. Failure may follow a partial write. Its source and
compiler/runtime tests are added, not executed; exact host rules belong to
[portable host operations](../spec/18-host.md) and the [runtime](runtime.md).

## 3. Strict manifest profile

This is a schema-directed **TOML 1.0 profile**, not a general TOML library:

```toml
schema-version = 1

[package]
name = "hello"
version = "0.1.0"

[[modules]]
name = "hello.core"
sources = ["src/core.joyeer"]
dependencies = []

[[modules]]
name = "hello.app"
sources = ["src/main.joyeer"]
dependencies = ["hello.core"]

[[modules]]
name = "hello.tests"
sources = ["tests/main.joyeer"]
dependencies = ["hello.core"]

[[targets]]
name = "hello"
kind = "bin"
module = "hello.app"

[[targets]]
name = "hello-tests"
kind = "test"
module = "hello.tests"
```

Every shown field is required. Root `schema-version` must precede tables and
be the TOML integer 1 (including accepted legal representations such as `+1`
or `0x1`). Only one `[package]` table and the `[[modules]]`/`[[targets]]`
arrays of tables are permitted. At least one target is required.

Supported syntax is bare keys, spaces/tabs, comments, LF/CRLF, single-line
basic or literal strings, and nonnested string arrays with comments/newlines
and optional trailing commas. Basic strings decode `\b`, `\t`, `\n`, `\f`,
`\r`, `\"`, `\\`, `\uXXXX`, and `\UXXXXXXXX` to bytes. Unicode escapes must
denote scalars: surrogate values and values above U+10FFFF are rejected;
surrogate-pair escapes are not a substitute for a scalar. Literal strings
do not decode escapes. Raw invalid UTF-8, forbidden control bytes, lone CR,
unknown/duplicate keys, wrong types, malformed/trailing input, quoted/dotted
keys, nested/mixed arrays, inline tables, multiline strings, dates, and floats
are rejected. Manifest escapes do not expand Joyeer source-literal syntax.

| Bound | Inclusive maximum |
|---|---:|
| Manifest bytes | 65536 |
| Decoded bytes per string (not character count) | 4096 |
| Modules | 256 |
| Targets | 256 |
| Strings per array | 1024 |

Package/target names are ASCII, 1..64 bytes, starting with a letter and
continuing with letters/digits/`-`/`_`. Windows rejects reserved device names
and ASCII-case-colliding target names. Logical module names instead follow
the compiler's dot-qualified ASCII identifier grammar and remain
case-sensitive. Version metadata requires three decimal components without
leading zeroes except `0`; no prerelease/range/resolution semantics exist.

Modules require unique names, nonempty explicit source arrays, and dependency
arrays. Repeated entries, unknown dependencies/target roots, and cycles in
**any** declared module fail before planning; cycle diagnostics show the path.

### Bounded regular-file acquisition

The entry first calls `fileKind` and accepts only `.File`, rejecting
directories, final-component symlinks, devices/FIFOs, and other nonregular
entries before reading. It then uses
`readFilePrefix(path: String, maximumBytes: Int): Result<String, FileSystemError>`.
The loader requests a **65537-byte** prefix so one sentinel byte
distinguishes an oversized manifest from a valid 65536-byte input without
reading/allocating the entire file. The parser rejects a count above 65536
and validates UTF-8/scalar content after acquisition. Name resolution and
type checking map the host descriptor's `ValueKind::integer` to `Int`; LLVM
passes `maximumBytes` as one `i64` scalar, not a data/count pair. This path is
integrated and compiled in the Windows x64 build; it is not generic streaming.

Classification is not an atomic open or race-free confinement: ancestor
links and a replacement between classification and reading remain host
concerns. Exact-boundary and nonregular-input regressions still require
execution before treating this as a validated safety guarantee. Legacy narrow
`readFile(path:)` and the whole-file `readFileUtf8` contract remain unchanged.

## 4. Paths, plans, and execution

Source paths are relative to the manifest's directory, not the first source
or child's working directory. They cannot be absolute, drive-prefixed,
parent-traversing, or directory spellings. Windows additionally rejects device,
alternate-stream, and trimming-alias forms. Lexical duplicate checks do not
replace compiler physical-file validation for symlink/hard-link aliases.
Path rules and directory checks are **not a sandbox**: ancestor links and
filesystem races remain host behavior.

All selected plans are assembled before side effects. Targets, dependency
module names, and source paths are sorted. One invocation per target contains:
root `--module-name`, sorted dependency `--module-source name=file` pairs,
profile flags, native-only `-o <output>`, `--`, then sorted root files.
Compiler cwd is `.` with manifest-prefixed sources/outputs in caller
coordinates. The built executable resolves in those same coordinates while
its child cwd is the package directory. Manifest edges assemble available
source sets; they do not enforce direct-import allowlists. The compiler owns
actual import checking, source identity/regularity, entries, linking, and
platform debug artifacts.

Native builds exclusively claim `target/<profile>/<target>/build-1` through
`build-128`, trying at most 128 names per target/profile. A collision is never
populated, reused, or deleted. Existing output parents must be ordinary
directories. Only a zero compiler completion followed by a regular-file
artifact yields a `BuildResult`. Failed/incomplete generations remain visible;
`run/test` never fall back to an older executable. There is no cache, latest
marker, automatic deletion, overwrite transaction, or crash-atomic publication.

Initialization uses exclusive writes and refuses nonempty destinations or
missing parents. Failures leave partial initialization visible, without
recursive rollback or `--force`. Tests are explicit native executable targets,
not discovered test functions. They always rebuild and run serially in target
name order. Ordinary nonzero compiler completions count as `build-fail`;
nonzero/signaled tests count as `run-fail`, and remaining tests continue.
Host/stderr/launch/wait/missing-artifact failures or a signaled compiler abort
the suite. Summaries report pass/build-fail/run-fail counts; no tests prints
`0 tests` and succeeds. Selection of an unknown test is still an error.

Only **trusted local projects** are in scope. Built programs/tests run with
the user's permissions and inherited environment/streams. Remote fetching,
registry/version resolution, local path packages, workspaces, lockfiles,
build scripts, plugins, parallelism, process capture/timeouts, `clean`, and
automatic toolchain installation are not provided.

## 5. Added tests versus validation

### Observed Windows x64-target validation

The owner identifies the originating machine as Windows ARM64. The recorded
commands used `vcvars64.bat`, `x64-debug`, and an x64 LLVM SDK. These observations
therefore establish x64-target build/smoke evidence only, not native ARM64
execution or native-x64 hardware provenance. An emulated shell's architecture
string must not be used to infer the machine's native architecture.

- The full x64 Debug default `ALL` build completed successfully in 121 steps
    with `JOYEER_BUILD_UNITTESTS=ON`. The Joyeer-written joypm bootstrap and
    logic/acceptance helper compilation succeeded; compiling helpers and unit
    tests does not mean their tests were executed.
- `joypm --help` and `joypm --version` succeeded; the latter reported
    `joypm 0.1.0`.
- A freshly initialized project under a pathname containing spaces and
    `雪🦀` (including supplementary-plane Unicode) passed `init`, `check`,
    Debug build, Release build, `run`, and `test`, all with exit status 0.
    `run` printed the scaffold's Hello output; `test` reported `0 tests`.
    This did not execute a declared test target.
- A new Debug generation `build-2` confirmed rebuilding rather than reusing
    the prior generation. The Release project build used the Debug-built
    compiler/tool; it does not validate a Release compiler/tool bootstrap.

These are observations for a Windows x64 target only, not general Unicode support,
temporary-path/failure-path coverage, or evidence for Windows ARM64, Linux,
or macOS. **CTest has not run**; the full native acceptance gate remains
pending, and joypm is not released or release-complete. Terminal glyph rendering
was not validated independently of filesystem path preservation.

### Registered but unexecuted coverage

[tests/joypm/CMakeLists.txt](../../tests/joypm/CMakeLists.txt) registers nine gates:
`Joypm.Cli`, `Joypm.Init`, `Joypm.Manifest`, `Joypm.ManifestLimits`,
`Joypm.Plans`, `Joypm.Workflow`, `Joypm.TestRunner`, `Joypm.Unicode`, and
`Joypm.Logic`. They require
`BUILD_TESTING` and `JOYEER_BUILD_JOYPM`, independently of GoogleTest and
`JOYEER_BUILD_UNITTESTS`. Native helper programs are compiled by CMake with
the build-tree compiler. The gates cover CLI/schema/plans, profiles,
generations/stale outputs, init/run/test behavior, and Unicode paths/argv;
their presence is not proof of passing or complete boundary coverage.

[ManifestLimits](../../tests/joypm/verifyManifestLimits.cmake) adds direct
parser acceptance/rejection at 65536/65537 original bytes, 4096/4097 decoded
bytes (literal and Unicode-escape forms), 256/257 modules and targets, and
1024/1025 array entries. Oversized CLI cases also check diagnostic byte
offsets before planning or compiler invocation. Prefix host/compiler
regressions cover the bounded-read and scalar-input contracts. These tests
are added and compiled where applicable, but have not been executed.

Windows compiler `wmain` UTF-8 argument conversion and native/UTF-8
source/output/temp/linker-path preservation are integrated and compiled.
The smoke result exercises the observed project pathname, but the registered
Unicode gate and compiler/backend/runtime regressions still need execution;
do not advertise Unicode-complete native support from that smoke run.
Legacy narrow `readFile` and general Unicode-aware diagnostic rendering remain
separate limitations.

Run unfiltered CTest with both tool and unit-test options enabled on every
claimed native platform/architecture. Packaging/installation, Release
toolchain bootstrap, self-build, concurrency, failure injection, and
exact-boundary acceptance
are tracked in the [active plan](../plan/package-manager.md#8-validation-and-release-gates).

### Continue on another machine

Use the [validation handoff](../plan/joypm-validation-handoff.md) for native
architecture/SDK selection, fresh Debug/Release configuration, unfiltered CTest,
package checks, and the evidence to return. No originating-machine build cache
or artifact is required. Record the receiving machine's native architecture
separately from the MSVC/LLVM target and any emulated process architecture.