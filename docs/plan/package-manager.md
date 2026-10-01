# Joyeer Project and Package Manager Implementation Plan

> **Status:** Proposed implementation plan, not an implemented CLI or manifest
> specification. The provisional tool name is `joypm`; it is written in Joyeer.
> [M0](joypm-m0.md) records the implemented first language/host scope.
> This plan starts from that foundation rather than reopening it. New choices
> below are recommended defaults to freeze before their implementation phase.

## 1. Goal and Scope

Deliver a Cargo-like **local project workflow first**, then grow it into a
package manager. Do not make registry infrastructure, separate binary
libraries, or compiler incrementality prerequisites for a useful first release.

The first usable release, **v0**, covers one package, explicit source modules,
executable and executable-test targets, debug/release profiles, and
`init`, `check`, `build`, `run`, and `test`. It always rebuilds selected targets.
Local path dependencies and workspaces follow as separate releases.

### Recommended decisions

| Area | v0 decision | Reason |
|---|---|---|
| Product boundary | Separate `joypm` executable; keep `joyeer` as the compiler | Preserve the existing compiler CLI and avoid mixing package policy with compilation |
| Implementation | Joyeer tool, C++ compiler, C11 runtime | Exercise the language without rewriting the existing toolchain |
| Manifest | `joyeer.toml`, versioned schema, documented TOML profile | Familiar project configuration without promising a full general-purpose TOML library |
| Source selection | Explicit file lists and explicit logical module names | Matches the implemented compiler contract; no hidden recursive discovery |
| Compilation | One compiler invocation per target, containing its source-module closure | Separate dependency binaries and their ABI do not exist yet |
| Dependencies | None outside the package in v0; local paths next | Separate project usability from package resolution |
| Execution | Synchronous, serial, no shell, inherited streams | Matches existing host operations |
| Outputs | Fresh build directory per invocation; no automatic deletion | Avoid overwrite/publication promises unsupported by current host APIs |

Not in v0: registry/Git fetching, dependency version solving, lockfiles,
`publish`, `install`, `update`, build scripts, plugins, feature flags,
cross-compilation, implicit parent-directory discovery, source globbing,
parallel builds, caching, `clean`, and automatic toolchain installation.
Do not reserve an `edition` value before the language defines edition behavior.

## 2. Host and Language Foundation

### Reuse what exists

| Capability | Existing contract | Consequence for the tool |
|---|---|---|
| Named modules | `--module-name`, explicit root files, repeated `--module-source name=file` | No compiler-side manifest parsing or directory scanning |
| Visibility and imports | File-local qualified imports; public/internal/private; compiler cycle checks | Do not concatenate source text or implement a second Joyeer parser |
| Program entry | Borrowed user arguments and integer status in `0..255` | CLI parsing is possible; native child status needs deliberate mapping |
| Error handling | `Result`, `Optional`, unit values, postfix propagation | Keep manifest, filesystem, process, and compiler failures distinguishable |
| Containers | Concrete structs/enums, arrays, safe dictionary `get` | No dependency on user-defined generics or dictionary key enumeration |
| Filesystem | Read, create-new write, directory creation/listing, lexical joining | Enough for explicit-source projects, not atomic replacement or safe recursive cleanup |
| Processes | Explicit executable path, argument array, child directory, synchronous status | Enough for compiler/program execution; no PATH lookup or output capture |

Sources: [implemented surface](../impl/supported-features.md),
[compiler CLI](../impl/backend.md#2-cli),
[module rules](../spec/12-modules.md), and
[portable host operations](../spec/18-host.md).

### Gaps must not become hidden assumptions

- **Diagnostics:** Joyeer exposes `print`, but no general stderr-writing
  interface. Specify and implement a small fallible stderr operation before
  promising conventional CLI diagnostics. This is a focused host addition,
  not a reason to build a general streaming library.
- **Tool discovery:** program arguments exclude the executable name; current
  host operations do not expose executable location, current-directory lookup,
  or environment lookup. Initially require an explicit `--compiler <path>`
  for compiler-using commands. The bootstrap/test harness supplies it.
  Installed sibling discovery is a later host/API task, not string guessing.
- **Windows encoding:** generated programs have UTF-8 arguments, but the
  compiler CLI still has non-ASCII limitations. End-to-end Unicode project
  paths require fixing that boundary and testing native Windows execution;
  do not infer support from `runProcess` alone.
- **Identity and cleanup:** lexical paths are not physical identities.
  Canonicalization, anchored recursive deletion, atomic publication, and
  process capture/cancellation remain separate contracts.
- **Parsing:** the native JSON acceptance fixture is not a conforming parser.
  Do not reuse it as a production manifest parser or silently fall back to it.

No v0 task requires general generics, closures, async functions, a source FFI,
dictionary key enumeration, or a stable binary-library ABI. If later code
needs dictionary keys, define an independent owned snapshot with unspecified
order and sort explicitly where determinism matters.

## 3. CLI Design and Status Conventions

### Commands and selection

| Command | v0 behavior |
|---|---|
| `joypm --help` / `--version` | Tool usage/version without loading a manifest or compiler |
| `joypm init <directory> --name <name>` | Create an executable project; never overwrite existing files |
| `joypm check` | Validate all declared targets through the compiler without native linking |
| `joypm build` | Build all `bin` targets, or one selected by `--target <name>` |
| `joypm run` | Build and run the sole `bin` target; require `--target` when ambiguous |
| `joypm test` | Build and run all declared `test` targets, or one selected by `--target` |

Project commands accept `--manifest-path <file>`; the default is
`joyeer.toml` in the invocation directory, with no upward search.
`--compiler <path>` is required for `check/build/run/test` in v0, and denotes
an explicit executable, not a name to search on PATH. `--release` selects the
release profile; the default is debug. `--verbose` prints planned compiler
arguments for humans, but that display is never used as a shell command.
`run` and `test` forward arguments after `--` unchanged.

Reject unknown options, missing values, duplicate singleton options, invalid
option/subcommand combinations, and ambiguous selections. An explicit unknown
target is an error. A valid package with no test targets reports "0 tests"
and succeeds; selecting a nonexistent test target fails.

`check` uses the existing compiler validation/lowering mode. It does not
promise native linkability or all executable-entry diagnostics; `build` and
native test execution cover those boundaries.

### Profiles

| Profile | Compiler flags | Notes |
|---|---|---|
| debug | `-O0 -gfull` | Tool default, deliberately different from the compiler's `-O2` default |
| release | `-O2 -g0` | No implicit LTO, stripping, or unchecked arithmetic |

Platform debug artifacts remain compiler-owned. Do not offer arbitrary
compiler-flag passthrough in v0: it could override validated sources, output
paths, module identity, or profile policy.

### Exit status and output

- **0:** successful command or successful child completion.
- **1:** manifest/build/tool failure, launch/wait failure, signal termination,
  or nonzero child completion.
- **2:** invalid CLI syntax, missing required CLI arguments, or invalid selection.

Preserve the original compiler/child status, signal, or platform error in the
diagnostic; never truncate it into the tool's entry status. This intentionally
keeps the previously planned three-state policy rather than copying Cargo's
numeric exit codes. Help/version and requested results use stdout; tool errors
and progress use stderr once the host prerequisite is implemented. Child
stdin/stdout/stderr are inherited, not captured.

## 4. Manifest and Build Planning

### Proposed v0 manifest

```toml
schema-version = 1

[package]
name = "hello"
version = "0.1.0"

[[modules]]
name = "hello.core"
sources = ['src\core.joyeer']
dependencies = []

[[modules]]
name = "hello.app"
sources = ['src\main.joyeer']
dependencies = ["hello.core"]

[[modules]]
name = "hello.tests"
sources = ['tests\main.joyeer']
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

Paths above illustrate Windows; path syntax follows the host. TOML literal
strings avoid interpreting backslashes as escapes. These are explicit files,
not directory-to-module conventions. Packages, targets, and modules are
different concepts: two targets can share source dependency modules, and a
package can own several modules. Test roots have their own program entry.

### Parser and schema contract

- Define a **manifest-specific TOML 1.0 profile**, not a claim to implement all
  TOML. Cover comments, LF/CRLF, bare keys, basic/literal single-line strings,
  string arrays, the schema integer, tables, and arrays of tables used above.
  Specify escapes and Unicode validation, including invalid scalar rejection.
  Reject unsupported constructs explicitly, including inline tables,
  multiline strings, dates, floats, and quoted/dotted keys in the first profile.
- Require UTF-8 manifest contents; file reading itself does not validate UTF-8.
  Reject duplicate keys/tables where forbidden, unknown schema versions,
  unknown fields, invalid value types, and trailing malformed input.
- Parse into typed package/module/target records. Detect keys as they are
  consumed so validation does not depend on dictionary key enumeration or a
  recursive generic TOML value tree.
- Carry manifest file and byte spans into diagnostics, rendering line/column
  and field context. Invalid input returns a tool error, not a bounds or
  checked-arithmetic trap. Bound file size, nesting, and collection counts with
  documented limits and exact-boundary tests.
- Require package metadata and at least one target. Use portable ASCII package
  and target names suitable for output components; reject reserved device
  names and case-folded target collisions on Windows. Keep package names
  separate from compiler logical module names.
- Version is metadata in v0: require a `MAJOR.MINOR.PATCH` string with
  nonnegative decimal components and no leading zeroes except zero itself.
  Do not claim version solving, prereleases, or compatibility ranges yet.
- Every module has a valid unique logical name, a nonempty explicit source
  list, and a dependency list. Every target has a unique name, `bin` or `test`
  kind, and an existing root module. Unknown references, repeated entries,
  and dependency cycles are errors with the relevant declaration/cycle path.

### Path policy and graph semantics

Sources are relative to the owning manifest's directory, not the first source
file or child working directory. For v0, require relative source paths without
parent traversal or Windows drive-relative forms. This is a configuration
rule, **not** a filesystem confinement guarantee; ancestor links may resolve
outside the package.

Keep compiler invocation paths in the caller's coordinate system and run the
compiler with working directory `.`. For example, an explicit manifest in a
different directory contributes that directory as the prefix for every source
and output argument. Run a built program with its package directory as child
working directory; its executable path still resolves against the caller,
as required by `runProcess`. Test invocation from outside the package.

Validate the manifest's declared module DAG and select the closure of each
target root. The compiler separately validates actual source imports and their
cycles. Manifest dependency edges assemble available source sets; the current
compiler interface does **not** enforce per-module direct-import allowlists.
Do not claim it prevents importing a transitively supplied module. Strict
dependency-edge enforcement would need a future compiler interface, not a
source-text scan in `joypm`.

Compiler physical-source validation remains authoritative, including hard-link
and symlink aliases, nonregular inputs, and duplicates across supplied modules.
All manifest records receive schema/graph validation; compiler input checks
cover the selected target's supplied source sets, not every unselected target.
Compiler validation also covers supplied but unimported modules' file sets;
their source bodies are not necessarily parsed.

### Build plan and compiler boundary

Keep planning separate from side effects:

```text
CLI -> manifest parse/validation -> target selection -> module closure
    -> BuildPlan -> filesystem preparation -> compiler invocation -> BuildResult
                                                               -> run/test
```

Use concrete records such as `PackageManifest`, `ModuleSpec`, `TargetSpec`,
`BuildPlan`, `CompilerInvocation`, and `BuildResult`; these are proposed tool
types, not a new compiler ABI. A plan carries the selected profile, ordered
module/file sets, argument array, working directory, and artifact destination.
Sort targets, module names, and source paths explicitly. Equivalent declarations
with reordered arrays must produce equivalent plans, apart from fresh output
directory allocation.

Emit one invocation per target using the existing flags:

```text
joyeer --module-name hello.app --module-source hello.core=src\core.joyeer -O0 -gfull -o target\debug\hello\build-1\hello.exe -- src\main.joyeer
```

This is a display example, not shell execution. Each option/value/path is a
separate argument; `--` protects positional source paths. Executable suffixes
follow the target host. `check` omits `-o` and does not create build outputs.
There is no per-module object cache or independently linked library here.
Preserve legacy single-file compilation and the removal diagnostics for old
directory-based compiler flags.

## 5. Execution Policy and Host Safety

### Output ownership and failure behavior

- Allocate a fresh directory under `target\<profile>\<target>\build-<id>` using
  exclusive directory creation with bounded collision retries. A process may
  populate only the directory it successfully created, never an existing
  generation. Define the retry limit in the CLI contract and test exhaustion.
- Create missing parent directories one component at a time. Accept an
  existing parent only after checking it is an appropriate directory; surface
  other filesystem errors. Such checks do not provide race-free confinement.
- Return an artifact only after successful compiler completion and validation
  that the expected executable exists as a regular file. Preserve compiler
  diagnostics and identify incomplete outputs after failure.
- `run` and `test` consume the current invocation's successful `BuildResult`.
  A failed build must never run a previous generation, even if it exists.
- Always rebuild in v0. Do not use timestamps, stale executable existence, or
  a "latest" marker as a success/cache substitute. Generations accumulate;
  report their location and document that automatic cleanup is unavailable.
- No clean-before-build, replacement transaction, or crash-atomic publication
  is promised. Future `clean` must use anchored, non-link-following deletion,
  not a check-then-recurse wrapper over current path APIs.

The v0 threat model is trusted local projects. There are no package build
scripts or automatic remote execution, but compiled project programs and tests
run with the user's permissions; no sandbox is implied.

### Initialization, running, and tests

`init` requires an existing parent and a new or empty destination directory.
Use exclusive writes for the manifest, a minimal source entry, and an ignore
file for build outputs. Refuse nonempty destinations and existing files; do
not add `--force` in v0. Report any partial initialization and leave it visible
on failure rather than recursively rolling back paths that may have changed.

Test discovery means selecting explicit `kind = "test"` targets, not scanning
function names or inventing a language test attribute. Each is a native
executable with its own root entry. Build and run selected tests serially in
stable name order, forward arguments, report pass/build-fail/run-fail counts,
and return failure if any test fails. Continue after an ordinary test failure;
abort on a tool-internal or host failure that prevents reliable continuation.
No output-capture, per-test timeout, or cancellation guarantee is implied.

## 6. Implementation Layout and Bootstrap

Proposed new areas, to be created only as implementation reaches them:

| Area | Responsibility |
|---|---|
| `tools\joypm\` | Joyeer entry, CLI, manifest parser/schema, planner, executor, and diagnostics |
| `tests\joypm\` | Durable project fixtures, invalid manifests, executable tool tests |
| Existing CMake test helpers | Compile test/tool entry points, invoke them, check status and artifacts |
| Existing compiler/runtime unit tests | Any required host or compiler-boundary changes |

Start with a few cohesive files; do not build a plugin framework or generic
package-resolution abstraction. Keep parser/planner functions testable without
filesystem/process effects. Wrap host errors into tool-level context explicitly;
postfix propagation does not convert different error types.

Bootstrap through CMake: build the existing compiler, then invoke that compiler
with an explicit source list to produce `joypm`. The initial build must not
need an already installed `joypm`. Use the same compiler/runtime/backend
packaging boundaries, and add a later self-build acceptance case after the
tool can describe its own source graph.

Do not introduce Python as a source-build or released-tool requirement. If
release automation eventually needs scripting, follow the repository's
cross-platform standard-library Python policy. Do not install external
compilers/SDKs through the tool. Adding `joypm` to released packages requires
an explicit update to the current product-only packaging inventory and notices;
do not silently bundle test SDKs or LLVM tools.

## 7. Dependency-Ordered Milestones

`P0` onward refers to tool delivery, not the completed first M0 language/host
slices. Estimates are intentionally omitted until P0/P1 expose bootstrap and
parser costs; acceptance gates, not elapsed time, determine completion.

| Phase | Prerequisites | Deliverables | Exit criteria |
|---|---|---|---|
| P0: Freeze contracts and unblock CLI output | Existing M0 foundation | Approve v0 manifest profile, naming/path limits, exit policy; implement the minimal stderr contract with spec/runtime/compiler tests | A native Joyeer CLI can print help and report a typed failure on the correct stream; no assumed host API |
| P1: Bootstrap, CLI, manifest | P0 | CMake-built `joypm`; help/version; CLI parser; TOML-profile parser; typed validation and source-located diagnostics | Valid examples parse deterministically; invalid/duplicate/unsupported input is rejected without traps; tool builds without itself |
| P2: Local `check/build` | P1 | Target selection, DAG validation, pure build plans, compiler adapter, profiles, fresh output generations | Multi-file/multi-module projects check and build; failures never yield usable artifacts; reordered inputs yield equivalent plans |
| P3: `init/run/test`, v0 acceptance | P2 | Safe scaffolding, argument forwarding, explicit executable tests, serial summaries, usage documentation | Initialize a project, build/run it, and pass/fail its tests; failed rebuild cannot run an old binary |
| P4: Local path packages | P3 plus physical package-identity contract | Recursive manifest loading, selected source exports, package DAG, identity/collision diagnostics | Diamond graphs deduplicate a package; cycles and conflicting logical names fail deterministically; no remote access |
| P5: Workspaces | P4 | Workspace root/member schema, explicit member selection and `--workspace`, shared planning, member-qualified artifacts | Members build/test from root or explicit manifest; same-named targets in different members do not collide |
| P6: Rebuild avoidance and artifact lifecycle | P5 plus hashing/identity/publication/cleanup contracts | Whole-target fingerprints, safe concurrent publication, cache validation, anchored `clean` | All changed inputs invalidate correctly; partial/corrupt entries are not hits; cleanup cannot follow links outside owned outputs |
| P7: Remote package ecosystem | P4 and verified acquisition/publication contracts | Git/registry sources, resolution, lockfile and frozen/offline modes, integrity checks, publishing policy | Resolved inputs are pinned and verified; interrupted fetches and conflicting constraints fail explicitly |

P6 and P7 are separate later workstreams; a registry need not wait for compiler
incrementality, and workspace support need not fetch remote packages.

### Concrete first PR sequence

1. **CLI host prerequisite:** small stderr contract through the required
   specification, frontend, IR/backend, C runtime, and acceptance tests.
2. **Bootstrap and CLI skeleton:** CMake integration, help/version/usage errors,
   required compiler option, entry/status tests.
3. **Manifest parser and schema:** profile grammar, strict validation,
   source-located diagnostics, limits, valid/invalid fixtures.
4. **Pure build planner:** module DAG, deterministic target/source ordering,
   profile flags and exact compiler argument arrays.
5. **Executor and `check/build`:** output allocation, process/error mapping,
   successful artifact checks and stale-artifact regressions.
6. **`init/run`:** create-new scaffolding, package working directory,
   argument forwarding and exit reporting.
7. **Executable test runner:** explicit test targets, serial selection/summary,
   passing/failing projects and full v0 acceptance documentation.

Fix the Windows compiler Unicode boundary in a focused prerequisite PR before
claiming Windows Unicode support for v0. If it cannot ship with v0, mark that
platform limitation explicitly and diagnose unsupported inputs rather than
silently mangling paths. Do not change legacy `readFile` behavior incidentally.

### Later-phase design gates

- **P4:** define package identity from canonical filesystem identity, not a
  display name or lexical path. Require a globally unambiguous logical module
  name across the compilation closure; no implicit module renaming or
  side-by-side versions of the same module in the initial implementation.
  Define exported source modules and resolve each source relative to its owning
  manifest. Local paths are mutable inputs, not reproducibly locked content.
- **P5:** define ownership of member manifests, workspace selection defaults,
  and output namespace before adding shared configuration inheritance.
- **P6:** distinguish a whole-target cache from compiler incremental
  compilation. Fingerprints need source bytes, manifests/resolution graph,
  compiler/backend/runtime and host SDK identity, profile/options, and target
  platform. Current APIs do not supply all of these; add tested contracts first.
- **P7:** settle full version semantics, source identity, checksums/pinned
  revisions, lockfile versioning, credentials, archive traversal/link policy,
  offline/frozen behavior, and interrupted-download publication before
  downloading or executing external code. Lockfile publication itself needs
  reliable replacement and concurrency behavior.

## 8. Validation and Release Gates

| Layer | Required cases |
|---|---|
| CLI | Help without compiler; unknown command/flag; missing/duplicate values; `--`; ambiguous/unknown target; exact 0/1/2 statuses |
| Manifest | Valid single/multi-module packages; Unicode/escapes; comments/CRLF; duplicate/unknown fields; wrong types/schema; malformed input; exact parser limits; no panic on malformed input |
| Planner | Unknown module; empty sources; cycles with cycle path; shared dependency; deterministic ordering; exact debug/release argv; target selection; different invocation directory |
| Compiler integration | Cross-file declarations; public/private imports; invalid source; physical source aliases; missing compiler; native link failure; validation-only mode leaves no executable |
| Artifacts | Concurrent generation claims; collision-retry exhaustion; output creation failure; missing expected executable; failed rebuild with an old executable present |
| Init/run | Nonempty destination; create-new collision; partial write failure; empty/spaced/Unicode/metacharacter arguments; package child cwd; nonzero and full native child status |
| Tests | No test targets; all-pass; compile failure; runtime failure; deterministic summary; selection/forwarding; signal termination where supported |
| Platform/release | Native Windows, Linux, macOS runs; spaces/Unicode; packaged compiler/backend/runtime lookup; external SDK prerequisites; bootstrap without preinstalled `joypm` |

Use Joyeer executable tests for pure tool logic and fixture projects for
end-to-end behavior; CMake/CTest can capture outputs and enforce test timeouts
without requiring those facilities in the released tool. Use existing
GoogleTests for C++/C runtime changes. Add controlled failing-child fixtures
rather than executing arbitrary shell commands in tests.

During development, run the narrow affected tests. Reconfigure CMake when
registering new targets/fixtures. For each release gate, build with
`JOYEER_BUILD_UNITTESTS` enabled and run unfiltered CTest on each claimed
platform. A Windows-only pass is not evidence of POSIX behavior.

**v0 is complete when** a fresh local project can be initialized, checked,
built in both profiles, run with preserved arguments, and tested; failures
have actionable diagnostics and the specified statuses; no stale executable
runs after failure; and documentation distinguishes shipped behavior from
every deferred feature above.
