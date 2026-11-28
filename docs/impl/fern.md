# Fern Module and Workspace Manager

`fern 0.1.0` is written in Joyeer. It resolves manifests to explicit source sets
and invokes the compiler; it does not change the
[language module definition](../../specs/12-modules.md). The compiler CMake
version remains `0.0.1`, independently of the tool's `0.1.0`. The module and
workspace schemas both remain version 1 during unreleased development;
the numeric version alone is not sufficient build provenance.

## 1. Canonical organization

Each `project.toml` describes exactly one named source compilation unit.
It owns its sources, direct declared dependencies, kind, and optional
executable basename. There is no package/target-to-module mapping layer.

```toml
schema-version = 1

[module]
name = "example.core"
kind = "source"
sources = ["src/core.joyeer"]
dependencies = []
```

An executable or test module requires `output`:

```toml
schema-version = 1

[module]
name = "example.app"
kind = "bin"
output = "example"
sources = ["src/main.joyeer"]
dependencies = ["example.core"]
```

`kind` is `source`, `bin`, or `test`. A `source` module is supplied as
dependency source; it has no standalone library artifact or stable binary
ABI and must not declare `output`. Test programs are separate modules with
their own manifests and entries, not extra units hidden inside one manifest.

Standard modules that ship with the compiler (`Foundation`, `System`, and
`Compression` when built) need no workspace member: fern reads
`<compiler directory>/lib/index.toml` when an external logical name must be resolved, lets a module list them in
`dependencies`, and the compiler resolves them when imported. A workspace
member with a standard module's name takes precedence, which is how the library
modules in this repository's root workspace are developed. fern does not honor a compiler-side
`--sysroot` override. Compiler/runtime construction
and the first fern bootstrap remain CMake responsibilities, not new fern
module kinds.
Native-backed source modules can additionally declare:

```toml
native-bindings = true
native-libraries = ["native/library.a"]
```

Both fields are optional, defaulting to `false` and `[]`. `native-bindings`
must be a Boolean and explicitly authorizes only that module's restricted
C declarations/calls via `--runtime-module`. Importing a module never grants
the caller these privileges. `native-libraries` is an ordered array of
relative file paths without parent traversal, resolved against the module's
directory and forwarded as explicit `--link-library` inputs. The same
1024-entry, duplicate-path, and decoded-string limits as source lists apply.
These files must already exist, including during `check`; they are validated
by the compiler. Only the selected dependency closure contributes bindings
and libraries. Unknown/repeated keys and malformed values are errors.

Executable and test modules can add ordered `native-search` and `native-libs`
arrays within `[module]`, plus optional additive platform tables:

```toml
[native.windows]
libs = ["ws2_32"]

[native.linux]
search = ["native/lib"]
libs = ["m"]
```

The only platform table names are `native.windows`, `native.linux`, and
`native.macos`, with `search` and `libs` as their only keys. Relative search
paths are based at the owning manifest, reject parent traversal, and can be
`.`. Names follow the [compiler's logical library syntax](c-interop.md#3-static-and-dynamic-providers).
`check` validates the manifest but does not pass executable-only options to the
compiler. A build passes common inputs before the active platform inputs;
other platform tables are parsed but not used. These fields do not propagate
through dependencies and are rejected on `source` modules.

Fern does not compile C, fetch dependencies, discover native providers itself,
or deploy shared libraries. CMake can stage
platform-specific manifests, as the optional [Compression](compression.md)
package does. Treat manifests with native privileges and libraries as trusted
build inputs, not a sandbox.

A workspace owns only its explicit member directories:

```toml
schema-version = 1

[workspace]
members = [".", "core", "tests"]
```

A member directory that contains a `workspace.toml` is a nested workspace: it
expands to that file's members, whose paths are relative to its directory and
must not leave it. Any other member directory is one module. The member `"."`
is always the workspace's own module, so a workspace can include the module that
sits beside its `workspace.toml`. Nesting is limited to depth 8, and the whole
project to 256 modules. Logical names must stay unique across all nested
workspaces. The repository's root `workspace.toml` lists `tools/moss`, which is a
workspace of its own.

Each member has a `project.toml`. `"."` includes a root module; directory
nesting does not imply logical submodules or automatic membership.
The workspace has no own sources, module name, or merged dependency list.
There is no upward discovery; nesting is only through listed member directories.

Member dependencies resolve by exact logical module name within the loaded
workspace. Membership does not make all members available to every compiler
invocation: only the selected module's declared dependency closure is passed.
Transitive modules in that closure remain available to the compiler; strict
direct-import allowlists are not implemented. The compiler determines actual
imports and preserves separate visibility/source-location boundaries.

An optional `path-dependencies = ["../shared"]` array in `[module]` selects
other local **source** modules by directory. Each path is relative to the
declaring `project.toml`; `..` is allowed for this field because dependencies
can be outside the workspace. Each directory must contain an ordinary
`project.toml`. Source and native-library path rules are unchanged. Path
dependencies are added to the module graph, not to the list of buildable
workspace members. Existing logical-name dependencies still resolve exactly.

Physical manifest and directory identities deduplicate aliases. A hard-linked
manifest used from different directories is rejected because relative source
paths could denote different files. Different manifests with the same logical
name and cycles (including those reached through an alias) are errors. The
explicit root or workspace member spelling takes precedence; otherwise, the
first spelling discovered in deterministic traversal determines the lexical
base for outgoing paths. The operating system resolves that spelling. The
Windows loader uses lexical Win32 full-path normalization between hops so
repeated `..` components do not inflate an otherwise short path. POSIX keeps
the spelling rather than folding `..` across symlinks. The
project limit of 256 modules includes distinct local dependencies after
deduplication; the loader also bounds physical probes to 4096 path references.
Repeated decoded entries in an array are errors; different spellings of one
identity contribute one edge. Path-only modules cannot be selected as check or
build roots. Exact logical-name lookup uses the entire loaded graph before
falling back to the compiler's standard module list. These are mutable local
inputs, not locked snapshots, remote packages, or confined paths.

## 2. Commands and selection

| Command | Behavior |
|---|---|
| `--help`, `--version` | Used alone; no manifest or compiler required |
| `init <directory> --name <module>` | Create one module in a new/empty directory with an existing parent |
| `check` | Check/lower all selected module kinds without native outputs |
| `build` | Build selected `bin` modules |
| `run` | Build/run exactly one selected `bin` module |
| `test` | Build/run selected `test` modules serially |

Project commands accept:

- `--manifest-path <file>`: one root module; defaults to current-directory
  `project.toml`.
- **Or** `--workspace <file>`: an explicit workspace manifest.
- `--compiler <path>`: override executable discovery.
- `--module <logical-name>`: select one member by exact name, not output
  basename. This option also works with `check`.
- `--release`, `--verbose`.
- `run/test` alone accept `--` followed by unchanged child arguments.

The two manifest-selection options are mutually exclusive, including repeated
uses. No workspace is inferred from a filename, parent directory, or adjacent
file. A standalone module can load explicitly listed path dependencies without
a workspace; a logical-name dependency must already be in the loaded graph
or in the selected toolchain's standard module list.

Without `--module`, `check` selects all members, `build` selects bins, and
`test` selects tests. `run` requires exactly one bin. Unknown modules or
ineligible kinds are status 2; no native artifacts are created by validation
or selection failures. An empty test selection prints `0 tests` and succeeds.
Source-only workspaces can be checked; their default bin build has no outputs.

`init` accepts `--kind source|bin|test` (default bin) and `--output <name>`.
For bin/test, omitted output defaults to the last logical name component if
it is a portable artifact name. Logical names are never inferred from paths
or converted from package-name spellings. Source modules reject `--output`.
Initialization uses exclusive writes, refuses nonempty destinations, creates
no missing ancestors, and retains partial state after failures without
recursive rollback.

Fern chooses the compiler in this order:

1. `--compiler <path>`, if supplied.
2. `JOYEER_COMPILER`, if set. An empty value is an error.
3. `joyeer` (`joyeer.exe` on Windows) beside the running fern executable.
4. The first matching candidate in the caller's `PATH`, left to right.

Explicit and environment paths are interpreted relative to the caller, not
the project, and are never replaced when they fail. Automatic discovery
ignores empty and relative `PATH` entries, scans `:` on POSIX and `;` on
Windows (respecting enclosing quotes), and accepts only ordinary executable
candidates (no final links).
It does not search the current directory implicitly, use `PATHEXT`, invoke
a shell, or retry with another compiler after a selected one fails.
`lib/index.toml` is read beside the selected path, following final POSIX
compiler links for index lookup only. If the manifest graph needs no standard
module lookup, a compiler is not needed until there is a module to build or
check. In environments where the
process's image path cannot be queried (for example Linux without `/proc`),
use `--compiler` or `JOYEER_COMPILER`.

`fern --module` is a tool selector, not the compiler's removed
directory-based `--module` option. Compiler inputs still use `--module-name`
and `--module-source`.

```powershell
$compiler = (Resolve-Path '.\out\build\arm64-debug\bin\joyeer.exe').Path
$fern = (Resolve-Path '.\out\build\arm64-debug\bin\fern.exe').Path
& $fern init .\hello --name hello.app --output hello
& $fern check --compiler $compiler --manifest-path .\hello\project.toml
& $fern run --compiler $compiler --manifest-path .\hello\project.toml

& $fern check --compiler $compiler --workspace C:\Source\Moss\workspace.toml
& $fern run --compiler $compiler --workspace C:\Source\Moss\workspace.toml `
    --module moss.app -- --help
& $fern test --compiler $compiler --workspace C:\Source\Moss\workspace.toml --release
```

Check each exit status before proceeding. Programs/tests run with user
permissions and inherited environment/stdin/stdout/stderr.

## 3. Manifest validation and diagnostics

The shared schema-directed TOML 1.0 profile supports bare keys, comments,
LF/CRLF, single-line basic/literal strings, and nonnested string arrays with
comments, multiline layout, and trailing commas. Basic strings decode the
TOML escape set, including Unicode scalar escapes. Invalid UTF-8, forbidden
control bytes, surrogate escapes, invalid Unicode, lone CR, duplicate keys,
unknown fields/tables, wrong types, or trailing input reject.

Root `schema-version` precedes the single table and accepts legal TOML integer
spellings of its expected value. Both module and workspace schemas are 1.
Quoted/dotted keys, array-of-table headers, inline tables, nested arrays,
multiline strings, dates, and floating-point values are unsupported.

| Bound | Inclusive maximum |
|---|---:|
| Each manifest | 65536 original bytes |
| Decoded string | 4096 bytes |
| Workspace members | 256 |
| Sources/dependencies/native libraries per module array | 1024 strings |

Every module requires name/kind/nonempty sources/dependencies. Bin/test output
names are portable ASCII 1..64 bytes; logical names follow the compiler's
dot-qualified identifier grammar. Bin/test names must also be valid artifact
directory components on the host. Windows rejects case-colliding output
module names; equal executable basenames in different modules are allowed.

Source paths and member directories are relative, not absolute or
parent-traversing. Members may use `"."`; source entries cannot be directories.
Windows rejects device, ADS, and trimming-alias spellings. Duplicate normalized
lexical entries reject; these checks are not canonical filesystem identity.
Physical source aliases within a supplied compilation graph are checked by
the compiler. The tool does not provide a workspace-wide physical identity
service for unrelated source units.

All member and path dependency manifests are loaded and their entire declared
dependency DAG validated before selection, including unrelated members and cycles.
Unknown dependencies, duplicate logical names, and cycles reject before build
effects. Traversal is iterative and bounded by the loaded project limits.

Module documents are classified and read with `readIdentifiedFilePrefix`;
the bytes and file identity come from the same opened ordinary file.
Workspace documents and the compiler index are not graph vertices and still
use `readFilePrefix` after classification. Both reads limit input to 65537
bytes so the parser can reject oversized input. Final symlinks, devices, and
FIFOs reject. Ancestor resolution and later lookups remain host behavior;
this is not confinement.

Parse/graph diagnostics identify the offending member manifest and its
original byte-based line/column. Original member text is retained during
loading and graph validation, not replaced with workspace coordinates or
carried into a successfully resolved project. Graph errors identify their
owning module separately from the diagnostic byte offset; graph diagnostics
currently point to that member's `[module]` header. Host/selection/progress
diagnostics use stderr. Help/version/test summaries use stdout.

Statuses are 0 for success, 1 for manifest/build/host/nonzero-child failures,
and 2 for usage/selection errors. Failed diagnostic writes return 1. Full
native child/compiler statuses and signals remain in messages.

## 4. Paths, execution, and bootstrap

Sources resolve relative to **their owning module manifest**, not the
workspace, caller, or first source file. Module/source sets and selections
are sorted deterministically. Diamonds visit each shared dependency once.
The compiler receives one root name, explicit dependency source mappings,
profile flags, native-only `-o`, and explicit root files after `--`.

Compiler cwd remains caller `"."`. Child cwd is the workspace directory for
workspace commands or the module directory for standalone commands. This
preserves workspace-level test resources without adding a per-module run-cwd
configuration.

`build --bin-dir <directory>` instead writes each built executable to
`<directory>/<output><host suffix>`, creating one missing directory level. There
is no generation: the file is replaced in place by the compiler, so a failed
build can leave a partial file, and the repository build uses it to place `moss`
beside the compiler. It is valid once and only for `build`; `run` and `test`
keep their generations.

Debug uses `-O0 -gfull` except for macOS native builds, where `-O0 -gauto`
lets the selected compiler omit debug information when its configured
`dsymutil` is missing. A compiler override used for macOS native Debug
must support `-gauto`. Release uses `-O2 -g0`. Native generations are exclusively
claimed at `target/<profile>/<logical-module>/build-1` through `build-128`.
The artifact uses the module's output basename and host suffix. Generation
directories therefore do not collide when two modules both output `app`.
Existing generations are never reused, populated, deleted, or silently run
after a failed rebuild. A successful compiler completion still requires a
regular-file artifact.

All selected plans are assembled before effects. Tests continue after
ordinary compile/nonzero-child failures and report pass/build-fail/run-fail
counts; filesystem, stderr, launch/wait/missing-artifact failures and a signaled
compiler abort the suite.

The [manifest parser](../../tools/fern/manifest.joyeer) owns single-document
syntax and field validation. The
[project loader](../../tools/fern/project.joyeer) owns cross-member name
resolution and whole-graph validation. A resolved project stores one record
per module containing its definition, owning directory, manifest path, and
validated dependency indexes, rather than parallel metadata arrays.
The [planner](../../tools/fern/planner.joyeer) traverses those indexes
without resolving names again; the
[executor](../../tools/fern/executor.joyeer) owns filesystem/process
effects. This separation does not add a public library ABI or alter compiler
module semantics.

Fern uses [Foundation](../../lib/Foundation/text.joyeer) for byte slicing,
UTF-8 validation, and decimal formatting. Slices clamp out-of-range endpoints,
return an empty string for empty or inverted ranges, and use bounded chunks
and pairwise concatenation to avoid quadratic prefix copying. UTF-8 validation
scans the bytes and accepts embedded NUL as valid UTF-8; fern separately
rejects NUL in host text. Decimal formatting handles all `Int` values,
including the minimum value.

[System path helpers](../../lib/System/path.joyeer) recognize separators and
extract or trim directory paths by scanning their bytes, using the explicit
syntax selected by `System.isWindows()`. The selection uses the compilation
target, not a runtime host probe. These helpers do not validate paths, resolve
them against the filesystem, or establish confinement. Fern keeps its own
manifest and path validation rules and uses
[System formatters](../../lib/System/messages.joyeer) for host errors and
process outcomes, preserving the numeric platform codes.

The seven maintained sources plus generated platform source compile in
`fern.app`: common, CLI, manifest, **[project loader](../../tools/fern/project.joyeer)**,
planner, executor, and entry. The explicit
[CMake bootstrap](../../tools/fern/CMakeLists.txt) remains an `ALL` target,
enabled by default, without a preinstalled fern or Python requirement. The
build-tree compiler resolves `Foundation` and `System` from its staged
standard library. Fern's build output depends on the staging stamp so
restaging rebuilds fern. Configure stages the same source files with a
schema-1 [self-build manifest](../../tools/fern/project.toml.in) that declares
both direct dependencies.
Debug PDB/dSYM byproducts, platform fallback, and install components are
unchanged; see [building](../building.md#fern-bootstrap-and-acceptance).

Registries/version resolution, lockfiles, cache/clean, process capture/timeouts,
and remote acquisition are not implemented. Atomic publication, release
notices, and general Unicode diagnostic rendering remain separate
contracts in the [remaining plan](../plan/0001-package-manager.md).

### Standard-library adoption validation

The helper migration passed [full macOS ARM64 validation](supported-features.md#standard-library-validation)
in Debug and Release. This included `FoundationTests`, `SystemTests`, and the
bootstrap, self-build, relocated-install, and test-runner gates;
`Fern.TestRunner` verified that a failing suite returns nonzero. Removing
the generated standard-library stamp restaged the library and rebuilt fern
and its fixtures in each configuration. Repeating the build without
invalidating the stamp did not recompile them.

On Windows ARM64 and Linux AArch64 (WSL2), the
[toolchain layout gates](../building.md#toolchain-library-layout) passed in
Debug and Release. The local Debug installer separately staged the archive,
library index, and module packages, and the relocated compiler compiled
and ran a program that imported `System`.

`ModuleServiceTest.TheSystemLibraryChecksForEveryTargetPlatform` resolved
and type-checked `System.isWindows()` for Windows, Linux, and macOS targets.
`SystemTests` exercised native path selection on macOS ARM64. The migration
was not run natively on Windows or Linux in this validation.

### macOS ARM64 toolchain validation

At `5c21924e` with LLVM/LLD 22.1.8 and macOS SDK and deployment target 26.5,
Debug and Release each passed unfiltered CTest 2230/2230 and the root
`fern test` 7/7 without reruns. This included every fern-labeled CTest gate
(16/16), including `Fern.Bootstrap`, `Fern.SelfBuild`, and `Fern.Installed`.
The regular Debug build produced `fern.dSYM`.

The Debug installer and Release CMake components were staged into temporary
prefixes. After relocation, fern ran and the relocated compiler compiled and
ran `hello`. The installed `lib/` contained the runtime archive, the standard
modules, and their index. The backend still loaded LLD, Z3, and zstd from
Homebrew, so these checks do not establish standalone
[release packaging](../building.md#release-staging).

With `JOYEER_DSYMUTIL_EXECUTABLE` empty, that revision regenerated and ran
fern without a dSYM, but `Fern.SelfBuild`, `Fern.Installed`, and the default
Debug build of `moss.app` failed with `linker.missing-tool`.

On 2026-11-28, `513aeed179c565d82322dab2d920e029bdd58a65` was tested on a
native Apple M3 Pro (`Mac15,6`, macOS 27.0.1 build 26A434, no Rosetta) using
the Command Line Tools macOS 26.5 SDK and deployment target and Homebrew
LLVM and LLD 22.1.8. Both regular configurations passed the focused
`JoyeerCInterop.NamedLibrary` and `Fern.Plans` checks, followed by
`Fern.LocalDependencies`, `Fern.CompilerDiscovery`, and `Fern.Installed`.

A second run used the same host, SDK, and toolchain with the uncommitted
Mach-O fixes applied in isolated checkouts of `513aeed1`. Unfiltered
`scripts/validate.py` passed in all three configurations with the fixes,
without retries or skips:

| Configuration | CTest at `513aeed1` | CTest with fixes | Root `fern test` in both runs |
|---|---:|---:|---:|
| Debug | 2275/2284 | 2284/2284 | 7/7 |
| Release | 2274/2283 | 2283/2283 | 7/7 |
| Debug without `dsymutil` | 2272/2281 | 2281/2281 | 7/7 |

The same nine CTests failed in each run of the unmodified commit.
`JoyeerCInterop.NamedLibraryReservedSpellingAndCollision` failed to link:
Mach-O lowering turned `to_library` into `-lto_library`, which is also an
LLD option. Eight `RuntimeSystemProgramHasNoStdio_*` cases failed because
the verifier stripped all leading underscores. The fixes avoid conflicting
linker options, strip only the Mach-O prefix, and allow the memory primitive
`bzero` without allowing stdio. The passing runs do not establish acceptance
of `513aeed1` without the fixes.

The regular Debug build produced `fern.dSYM`. With
`JOYEER_DSYMUTIL_EXECUTABLE=` in an independent checkout, Debug produced no
dSYM and passed `Fern.Bootstrap`, `Fern.SelfBuild`, `Fern.Installed`, and
`Moss.Serve`, including the root workspace's `moss.app` build. Explicit
`-gfull` exited 1 with `linker.missing-tool` naming `dsymutil`. Three tests
that require `dsymutil` were not registered in that configuration.
