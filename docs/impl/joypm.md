# joypm Module and Workspace Manager

> **Status:** `joypm 0.1.0` implements one source module per `joyeer.toml` and
> explicit multi-module `workspace.toml` files. Native Windows ARM64 Debug
> default build and unfiltered CTest pass 809/809, including all 15 joypm
> gates. Debug/optimized self-builds, workspace workflows, concurrent
> generations, failures, and relocated installs are verified. Other native
> platforms, a new full Release build, and publication remain open.

The tool is written in Joyeer. It resolves manifests to explicit source sets
and invokes the compiler; it does not change the
[language module definition](../spec/12-modules.md). The compiler CMake
version remains `0.0.1`, independently of the tool's `0.1.0`. The module and
workspace schemas both remain version 1 during unreleased development;
the numeric version alone is not sufficient build provenance.

## 1. Canonical organization

Each `joyeer.toml` describes exactly one named source compilation unit.
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

A workspace owns only its explicit member directories:

```toml
schema-version = 1

[workspace]
members = [".", "core", "tests"]
```

Each member has a `joyeer.toml`. `"."` includes a root module; directory
nesting does not imply logical submodules or automatic membership.
The workspace has no own sources, module name, or merged dependency list.
There is no nested-workspace traversal or upward discovery.

Member dependencies resolve by exact logical module name within the loaded
workspace. Membership does not make all members available to every compiler
invocation: only the selected module's declared dependency closure is passed.
Transitive modules in that closure remain available to the compiler; strict
direct-import allowlists are not implemented. The compiler determines actual
imports and preserves separate visibility/source-location boundaries.

### Migration from the earlier unreleased layout

The earlier `[package]`, `[[modules]]`, and `[[targets]]` project layout
is rejected at its table with an explicit migration diagnostic. `--target` is rejected
with help directing callers to `--module <logical-name>`. There is no silent
legacy compatibility path.

Keep `schema-version = 1` and split each old module into its own manifest.
Move its old target
kind/name into `kind`/`output`; modules without targets become `source`.
Create a workspace listing the member directories, and make source paths
relative to each member's manifest. No implicit module renaming occurs.

## 2. Commands and selection

| Command | Behavior |
|---|---|
| `--help`, `--version` | Used alone; no manifest or compiler required |
| `init <directory> --name <module>` | Create one module in a new/empty directory with an existing parent |
| `check --compiler <path>` | Check/lower all selected module kinds without native outputs |
| `build --compiler <path>` | Build selected `bin` modules |
| `run --compiler <path>` | Build/run exactly one selected `bin` module |
| `test --compiler <path>` | Build/run selected `test` modules serially |

Project commands accept:

- `--manifest-path <file>`: one module; defaults to current-directory
  `joyeer.toml`.
- **Or** `--workspace <file>`: an explicit workspace manifest.
- `--module <logical-name>`: select one member by exact name, not output
  basename. This option also works with `check`.
- `--release`, `--verbose`.
- `run/test` alone accept `--` followed by unchanged child arguments.

The two manifest-selection options are mutually exclusive, including repeated
uses. No workspace is inferred from a filename, parent directory, or adjacent
file. A module with dependencies needs an explicitly loaded workspace;
a standalone module can resolve only itself.

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

The compiler path is explicit and interpreted in the caller's coordinates;
no PATH/sibling lookup or shell evaluation is performed. `joypm --module`
is a tool selector, not the compiler's removed directory-based `--module`
option. Compiler inputs still use `--module-name` and `--module-source`.

```powershell
$compiler = (Resolve-Path '.\out\build\arm64-debug\bin\joyeer.exe').Path
$joypm = (Resolve-Path '.\out\build\arm64-debug\bin\joypm.exe').Path
& $joypm init .\hello --name hello.app --output hello
& $joypm check --compiler $compiler --manifest-path .\hello\joyeer.toml
& $joypm run --compiler $compiler --manifest-path .\hello\joyeer.toml

& $joypm check --compiler $compiler --workspace C:\Source\Moss\workspace.toml
& $joypm run --compiler $compiler --workspace C:\Source\Moss\workspace.toml `
    --module moss.app -- --help
& $joypm test --compiler $compiler --workspace C:\Source\Moss\workspace.toml --release
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
| Sources/dependencies per module array | 1024 strings |

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

All member manifests are loaded and their entire declared dependency DAG
validated before selection, including unrelated members and cycles.
Unknown dependencies, duplicate logical names, and cycles reject before build
effects. Traversal is iterative and bounded by the member count.

Each document is classified as an ordinary regular file, then acquired with
`readFilePrefix(..., maximumBytes: 65537)` so its parser can reject oversized
input without reading the remainder. This includes workspace and member
manifests. Final symlinks/devices/FIFOs reject. Ancestor resolution and
classification/read races remain host behavior; this is not confinement.

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

Debug is `-O0 -gfull`; release is `-O2 -g0`. Native generations are exclusively
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

The [manifest parser](../../src/tools/joypm/manifest.joyeer) owns single-document
syntax and field validation. The
[project loader](../../src/tools/joypm/project.joyeer) owns cross-member name
resolution and whole-graph validation. A resolved project stores one record
per module containing its definition, owning directory, manifest path, and
validated dependency indexes, rather than parallel metadata arrays.
The [planner](../../src/tools/joypm/planner.joyeer) traverses those indexes
without resolving names again; the
[executor](../../src/tools/joypm/executor.joyeer) owns filesystem/process
effects. This separation does not add a public library ABI or alter compiler
module semantics.

The seven maintained sources plus generated platform source compile in
`joypm.app`: common, CLI, manifest, **[project loader](../../src/tools/joypm/project.joyeer)**,
planner, executor, and entry. The explicit
[CMake bootstrap](../../src/tools/joypm/CMakeLists.txt) remains an `ALL` target,
enabled by default, without a preinstalled joypm or Python requirement.
Configure stages the same files and a schema-1 single-module
[self-build manifest](../../src/tools/joypm/joyeer.toml.in).
Debug PDB/dSYM byproducts, platform fallback, and install components are
unchanged; see [building](../building.md#joypm-bootstrap-and-acceptance).

## 5. Validation evidence

A follow-up native Windows ARM64 Debug default build and unfiltered CTest on
2026-10-11 passed **809/809**, including all 15 joypm gates, after adding
public short-name imports to the compiler. All nine unchanged Moss modules
passed workspace checks with that compiler. The original module/workspace
acceptance evidence below remains dated to its own run.

On native Windows ARM64 on 2026-10-10:

- Default Debug build and unfiltered CTest: **790/790 passed**.
- All 15 joypm gates passed, including the new Workspace gate.
- Exact manifest/string/array/member limits, legacy diagnostics, per-module
  paths, diamonds, unselected cycles, kind/selection rules, and equal output
  basenames are tested.
- Resolved dependency indexes, ownership across differing directory/module
  orders, and member-local CRLF graph diagnostic coordinates are covered.
- Native failure paths, Unicode argv/paths, concurrent generations,
  Debug/optimized self-build and second-stage execution, relocated component
  installs, and bootstrap regeneration pass.
- Migrated Moss workspace checks and Debug/Release native suites pass
  673 binary, 1475 JVM, and 10 CLI checks per profile.

The [shared test harness](../../tests/joypm/helpers.cmake) claims run directories
exclusively with process identity and random suffix. Exact fixture writes
use a Joyeer binary writer with size/SHA-256 checks, not CMake text-mode bytes.
Tests never claim ownership of collided directories.

Previous-layout native Debug/Release acceptance recorded 789/789 each.
Despite retaining version 0.1.0, those historical Release results are not
a new full Release CTest run for the module/workspace layout.
The current optimized tool/workspace self-builds are a narrower gate.
Windows x64, Linux, and macOS still need their own native acceptance.

External local dependencies, registries/version resolution, lockfiles,
cache/clean, process capture/timeouts, nested workspaces, and remote acquisition
are not implemented. Physical filesystem identity, atomic publication,
release notices, and general Unicode diagnostic rendering remain separate
contracts in the [remaining plan](../plan/package-manager.md).
