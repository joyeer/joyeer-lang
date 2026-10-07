# joypm Local Project Manager

> **Status:** the first local-workflow version, `0.1.0`, is implemented and
> accepted on native Windows ARM64 in Debug and Release. Both default `ALL`
> builds and unfiltered CTest runs pass **789/789**, including all 14 Windows
> joypm gates. Self-build, concurrent generations, failure paths, relocated
> product installs, Debug installation, and compiler-only builds are verified.
> This is not a published release or acceptance evidence for Windows x64,
> Linux, or macOS; see the scoped evidence below and the
> [remaining platform/future-package plan](../plan/package-manager.md).

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

CMake also stages a self-build project at
`out/build/<preset>/src/tools/joypm/project/joyeer.toml`, generated from
[joyeer.toml.in](../../src/tools/joypm/joyeer.toml.in). It copies the same
explicit compilation set, including the generated platform source; it does
not discover another source set. Configure dependencies refresh these copies
when their originals change. `Joypm.SelfBuild` checks this project, builds
Debug and Release tools, runs their complete workflows with declared test
targets, and uses a self-built tool for another self-build.

From the checkout after building, using an explicit compiler path:

```powershell
$preset = 'arm64-debug'
$compiler = (Resolve-Path ".\out\build\$preset\bin\joyeer.exe").Path
$joypm = (Resolve-Path ".\out\build\$preset\bin\joypm.exe").Path
$manifest = ".\out\build\$preset\src\tools\joypm\project\joyeer.toml"
& $joypm build --compiler $compiler --manifest-path $manifest
if ($LASTEXITCODE -ne 0) { throw 'Debug self-build failed' }
& $joypm build --compiler $compiler --manifest-path $manifest --release
if ($LASTEXITCODE -ne 0) { throw 'Release self-build failed' }
```

Install component
`JoyeerRuntime` contains compiler/backend/runtime/current license; `PRODUCT`
contains optional `joypm` and its PDB for Windows Debug installations.
See [building](../building.md#joypm-bootstrap-and-acceptance)
for options, installation, and platform prerequisites.

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
compiler/runtime tests pass in the recorded native ARM64 gate; exact host rules belong to
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
validated in the native Windows ARM64 Debug/Release gates; it is not generic streaming.

Classification is not an atomic open or race-free confinement: ancestor
links and a replacement between classification and reading remain host
concerns. Exact-boundary, final-link, and Windows device rejection regressions
pass; the POSIX FIFO case still needs a native POSIX run. Windows metadata-only
file classification rejects `NUL` without reading it, even though its
attributes resemble a regular file. Legacy narrow
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

## 5. Validation evidence

### Native Windows ARM64 acceptance, 2026-10-02

Source provenance is baseline `0ef8b6e365132dad9ee771efa4c68922fb32e4c3`
plus the first-version completion changes. Hardware was independently
identified as a Snapdragon X1E80100, architecture code 12, running Windows
ARM64. PowerShell 7.6.6, the MSVC host/target tools, LLVM SDK, and product PE
headers were all ARM64; this is not x64 emulation evidence.

The toolchain was MSVC 19.51.36257.0 (toolset 14.51.36231), Windows SDK
10.0.26100.0, LLVM/LLD 22.1.8 at `C:\LLVM-22.1.8-arm64`, CMake
4.3.1-msvc1, and Ninja 1.13.2. Fresh `arm64-debug` and `arm64-release`
trees enabled `BUILD_TESTING`, `JOYEER_BUILD_JOYPM`, and
`JOYEER_BUILD_UNITTESTS`, with `INSTALL_GTEST=OFF`.

| Gate | Recorded result |
|---|---|
| Debug default `ALL` and unfiltered CTest | Passed, 789/789 |
| Release default `ALL` and unfiltered CTest | Passed, 789/789 |
| Tool acceptance in each configuration | Passed, all 14 gates |
| Debug installer regressions | Passed in isolated destinations; tool/PDB/notices, conflict protection, execution, and unchanged PATH |
| Compiler-only Release `ALL` and both install components | Passed; no joypm binary built or installed |
| Unfiltered Release product staging | Passed; exactly four product files and eight license/notice files, without SDK/test payload |

Full CTest logs are under
`out/build/arm64-{debug,release}/ctest-joypm-completion.log`. The earlier
x64 build/smoke handoff is superseded by these native results, not promoted
to x64 full acceptance.

### Executed coverage

[tests/joypm/CMakeLists.txt](../../tests/joypm/CMakeLists.txt) registers
`Cli`, `Init`, `Manifest`, `ManifestLimits`, `Plans`, `Workflow`,
`TestRunner`, `Unicode`, `Logic`, `Concurrent`, `HostFailures`, `SelfBuild`,
and `Installed`; Windows also registers `Bootstrap`. They require
`BUILD_TESTING` and `JOYEER_BUILD_JOYPM`, independently of GoogleTest.

- [ManifestLimits](../../tests/joypm/verifyManifestLimits.cmake) verifies
  65536/65537 original bytes, 4096/4097 decoded bytes, 256/257 modules and
  targets, and 1024/1025 array entries, including original byte locations.
  [The exact-byte helper](../../tests/joypm/helpers.cmake) reverses CMake's
  Windows text-mode newline translation with a Joyeer writer and checks size
  and SHA-256 before publishing fixture input.
- [Concurrent](../../tests/joypm/verifyConcurrent.cmake) executes concurrent
  builders, verifies eight distinct runnable generations, and preserves a
  collided generation. This tests multiple independent tool invocations;
  joypm itself remains serial.
- [HostFailures](../../tests/joypm/verifyHostFailures.cmake) covers final
  symlinks/devices, physical hard-link aliases, missing entries, native
  `4045620583` compiler/child status, invalid/missing/nonregular artifacts,
  suite-aborting host errors, output permissions, and a partial init that
  retains the manifest without overwriting it on retry. A small test-only C11
  helper supplies statuses outside Joyeer entry's `0..255` contract and
  deterministic native permission failures. Package policy remains Joyeer.
- [SelfBuild](../../tests/joypm/verifySelfBuild.cmake) exercises Debug/Release
  tools built through joypm, declared executable tests, and an executed
  second-stage self-build.
- [Installed](../../tests/joypm/verifyInstalled.cmake) checks separate
  `JoyeerRuntime`/`PRODUCT` inventories and license hashes, relocates the
  combined package through a spaced supplementary-plane Unicode path, and
  runs both project profiles using only the staged compiler/backend/runtime.
- [Bootstrap](../../tests/joypm/verifyBootstrap.cmake) regenerates a missing
  Windows Debug PDB or Release executable through the CMake rule. It runs
  serially so other acceptance processes cannot hold the tool open.

Compiler/runtime regressions also pass for scalar prefix inputs, binary
stderr and failure codes, allocation balance, wide compiler argv, Unicode
source/output/temp paths, device classification, and embedded `asInvoker`
manifests. Installer-like valid target names do not request elevation.

### Remaining validation boundary

Windows x64, Linux, and macOS native Debug/Release acceptance are not recorded
for this completion. POSIX FIFO/signal/execute-permission cases and macOS
dSYM/fallback behavior have conditional coverage but still need native runs.
Run the same unfiltered gate on every platform/architecture before claiming
its acceptance. Terminal glyph rendering, general Unicode-aware diagnostics,
legacy narrow `readFile`, race-free confinement, and custom-SDK redistribution
audits remain separate concerns; none is implied by these passing gates.