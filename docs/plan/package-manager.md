# Joyeer Project and Package Manager Plan

> **Status:** the first single-package local-workflow version, `0.1.0`, is
> implemented and accepted on native Windows ARM64. Debug and Release default
> `ALL` builds and unfiltered CTest pass 789/789 each, including all 14 Windows
> tool gates. Self-build, concurrency, failure paths, product/Debug installation,
> and compiler-only builds are verified. This is not a published release or
> evidence for other native platforms/architectures.
> Current CLI, manifest, execution contracts, and evidence belong in
> [joypm](../impl/joypm.md); this plan tracks remaining native rollout and
> future package contracts, not completed implementation PRs.

## 1. Goal and Scope

Deliver a Cargo-like **local project workflow first**, then grow it into a
package manager. Do not make registry infrastructure, separate binary
libraries, or compiler incrementality prerequisites for a useful first release.

The accepted first **v0 scope** covers one package, explicit source modules,
executable and executable-test targets, debug/release profiles, and
`init`, `check`, `build`, `run`, and `test`. It always rebuilds selected targets.
Local path dependencies and workspaces follow as separate releases. Tool
version `0.1.0` is early development and independent of compiler `0.0.1`;
neither a version string nor added fixtures alone establish release acceptance.

### Frozen local-workflow decisions

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

### Remaining host and validation boundaries

- **Diagnostics:** the focused `writeStderr(contents: String)` addition returns
  `Result<Void, StderrError>` with `.WriteFailed(Int)` CRT `errno` and exact
  bytes plus flush. Native ARM64 regressions pass. General streaming and
  Unicode-aware diagnostic rendering remain separate contracts.
- **Bounded acquisition validation:** the loader now uses
  `readFilePrefix(path: String, maximumBytes: Int): Result<String, FileSystemError>`
  with 65537 to reject input above the parser's 65536-byte maximum, after
  `fileKind` accepts only `.File` and rejects final symlinks and other
  nonregular inputs. Host-descriptor `Int` name/type resolution and scalar
  LLVM emission and exact-boundary/error regressions pass on native Windows
  ARM64, including final links and devices. Execute the POSIX FIFO/device
  cases on their native platforms.
  Classification is not race-free confinement, and this is not streaming.
- **Tool discovery:** program arguments exclude the executable name; current
  host operations do not expose executable location, current-directory lookup,
  or environment lookup. The implemented policy requires an explicit
  `--compiler <path>` for compiler-using commands. The bootstrap/test harness
  supplies it.
  Installed sibling discovery is a later host/API task, not string guessing.
- **Windows encoding:** compiler `wmain` UTF-8 conversion and native/linker-
  path regressions, including temporary paths and product relocation, pass in
  native ARM64 Debug/Release. Other native targets need their own gates;
  this does not establish Unicode-aware diagnostic rendering. Legacy narrow
  `readFile` is unchanged.
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

The source-owned [commands and options](../impl/joypm.md#2-commands-and-options)
define the strict CLI, selections, forwarding, profiles, streams, and exact
0/1/2 status policy. Those implementation tasks are no longer proposed PRs;
their native ARM64 acceptance is complete. Preserve the explicit compiler path, no
upward manifest search/PATH lookup/shell, debug `-O0 -gfull`, release `-O2 -g0`,
and no arbitrary compiler-flag passthrough when extending the tool. `check`
does not establish native linkability or all executable-entry diagnostics.

## 4. Manifest and Build Planning

The [strict manifest profile](../impl/joypm.md#3-strict-manifest-profile) owns
the schema/example, required fields, scalar/UTF-8 escapes, portable names,
version metadata, DAG checks, and inclusive limits: 65536 input bytes, 4096
decoded bytes/string, 256 modules, 256 targets, and 1024 strings/array.
Exact-boundary and bounded-acquisition gates pass on native ARM64; other
native platforms still require execution.

Keep package, target, and logical module identities distinct. Source paths
resolve relative to the owning manifest. Compiler physical-source validation
remains authoritative for regular files and symlink/hard-link duplicates.
All records receive schema/DAG validation, but compiler input checks cover
selected supplied source sets, not every unselected target; unimported supplied
modules' bodies need not be parsed.

The [planner boundary](../impl/joypm.md#4-paths-plans-and-execution) is
implemented: pure plans precede effects; targets/module/file sets are sorted;
one explicit compiler argv is emitted per target. Future strict direct-import
allowlists would need a compiler interface, not a joypm source-text scan:
manifest edges currently assemble available sets and do not prohibit imports
from transitively supplied modules. No separate binary-library ABI or compiler
incrementality is implied.

## 5. Execution Policy and Host Safety

Current [execution policy](../impl/joypm.md#4-paths-plans-and-execution) owns
create-new initialization, 128 exclusive generation claims per target/profile,
artifact validation, stale-output rejection, serial tests and summaries. These
pass the full native ARM64 gate, including concurrency, partial init,
native statuses, and stale-output rejection. This is not a general confinement
or universal platform safety claim.

Preserve the trusted-local-project threat model: programs/tests execute with
the user's permissions and no sandbox. Lexical path checks and ordinary
directory inspection do not provide race-free confinement. Generations and
partial initialization accumulate without deletion or rollback. Future
`clean` requires anchored, non-link-following deletion, not check-then-recurse;
replacement/crash-atomic publication, capture, cancellation and timeouts need
their own host contracts first.

## 6. Implementation Layout and Bootstrap

The six sources and generated platform/manifest templates are under
[src/tools/joypm/](../../src/tools/joypm/); durable fixtures and the acceptance gates
are under [tests/joypm/](../../tests/joypm/). Source responsibilities and the
explicit CMake compilation set are documented in
[bootstrap](../impl/joypm.md#1-source-layout-and-bootstrap).

`JOYEER_BUILD_JOYPM=ON` defaults to a compiler-driven `ALL` bootstrap; `OFF`
is compiler-only. Debug uses `-O0 -gfull` (macOS uses `-g0` without
`dsymutil`), and Release uses `-O2 -g0`; produced Debug PDB/dSYM sidecars are
CMake byproducts. The gates require `BUILD_TESTING` and the tool option,
independently of `JOYEER_BUILD_UNITTESTS`. Configure stages a project from
the same explicit source set for actual joypm self-build; Debug/Release tools
and an executed second-stage build pass. Native ARM64 default bootstrap and
Windows Debug-PDB/Release-executable regeneration are verified. Other native
platforms still need validation.
No preinstalled joypm or Python is required.

Install inventory is updated in [building](../building.md#release-staging):
compiler/backend/runtime/current license use `JoyeerRuntime`; optional joypm
uses `PRODUCT`, including its Windows Debug PDB. The Windows Debug installer
includes tool/PDB when enabled.
Native ARM64 product/installer inventories, relocated workflows, and pinned
third-party notice staging are verified. Audit extra dependencies in custom
SDKs and execute installation gates on other native platforms. Never
bundle test SDKs or LLVM tools or install external compilers/SDKs through joypm.
If release automation needs scripts, use the repository's cross-platform
standard-library Python policy without making Python a source-build/tool
runtime prerequisite.

## 7. Dependency-Ordered Milestones

The language/host foundation is owned by the
[implemented surface](../impl/supported-features.md),
[compiler input contract](../impl/backend.md#2-cli), and
[host specification](../spec/18-host.md), not a completed milestone plan.
Acceptance evidence, not source presence or elapsed time, determines platform
readiness. P0-P3 local-workflow implementation and native ARM64 acceptance
are complete in [joypm](../impl/joypm.md); the next package phases remain
separate contracts.

| Phase | Current status / prerequisites | Remaining deliverables and exit criteria |
|---|---|---|
| P4: Local path packages | Deferred; validated v0 plus physical package-identity contract | Recursive loading/source exports/package DAG; deduplicate diamonds and reject cycles/conflicting logical identities without remote access |
| P5: Workspaces | Deferred; P4 | Root/member schema, explicit selection/`--workspace`, shared plans and member-qualified outputs without target collisions |
| P6: Rebuild avoidance and artifact lifecycle | Deferred; P5 plus hashing/identity/publication/cleanup contracts | Whole-target fingerprints, concurrent publication/cache checks and anchored `clean`; changed inputs invalidate, corrupt entries miss, deletion cannot follow links outside owned outputs |
| P7: Remote package ecosystem | Deferred; P4 plus verified acquisition/publication contracts | Git/registry resolution, lockfiles/offline/frozen modes, integrity/publishing policy; pin/verify inputs and diagnose interrupted fetches/conflicts |

P6 and P7 are separate later workstreams; a registry need not wait for compiler
incrementality, and workspace support need not fetch remote packages.

### Remaining native rollout work

- Execute fresh native Windows x64, Linux, and macOS Debug/Release default
  builds and unfiltered CTest with tool and unit-test options enabled. Keep
  hardware/process/SDK target identities distinct from emulation.
- Run the conditional POSIX FIFO/signal/execute-permission and macOS
  dSYM/fallback cases on their own platforms; validate relocated product
  components there. Preserve legacy `readFile` compatibility and the explicit
  non-confinement boundary.
- Add repeatable native CI/release staging and audit custom SDK dependency
  notices before publishing a broader platform release.

Do not restore completed local-workflow implementation checklists.

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

**Current evidence:** native Windows ARM64 Debug and Release default `ALL`
builds and unfiltered CTest pass 789/789 each. All 14 Windows joypm gates,
exact limits, self-build, concurrent generations, native failure injection,
product relocation, Debug installer regressions, and compiler-only builds
pass; details and provenance belong in
[joypm](../impl/joypm.md#5-validation-evidence). Follow
[building](../building.md#configure-build-and-test) for matching external SDKs
and full gates on another native platform.

The table remains the acceptance contract. Windows ARM64 results do not cover
POSIX-only cases, macOS debug sidecars/fallback, or other native architectures.
Those executions, native CI, and release publication remain open.

| Layer | Required cases |
|---|---|
| CLI | Help without compiler; unknown command/flag; missing/duplicate values; `--`; ambiguous/unknown target; exact 0/1/2 statuses |
| Manifest | Valid single/multi-module packages; Unicode/scalar escapes and invalid UTF-8; comments/CRLF; duplicate/unknown fields; wrong types/schema; malformed input; 65536/65537-byte bounded acquisition and regular-file-only rejection; exact 4096-byte decoded-string, 256-module/target and 1024-entry array limits; no panic |
| Planner | Unknown module; empty sources; cycles with cycle path; shared dependency; deterministic ordering; exact debug/release argv; target selection; different invocation directory |
| Compiler integration | Cross-file declarations; public/private imports; invalid source; physical source aliases; missing compiler; native link failure; validation-only mode leaves no executable |
| Artifacts | Concurrent generation claims; collision-retry exhaustion; output creation failure; missing expected executable; failed rebuild with an old executable present |
| Init/run | Nonempty destination; create-new collision; partial write failure; empty/spaced/Unicode/metacharacter arguments; package child cwd; nonzero and full native child status |
| Tests | No test targets; all-pass; compile failure; runtime failure; deterministic summary; selection/forwarding; signal termination where supported |
| Platform/release | Native Windows, Linux, macOS runs; spaces/Unicode source/output/temp/argv; stderr exact bytes/CRT errors; optional PRODUCT tool and Windows Debug PDB install; third-party notices; packaged compiler/backend/runtime lookup; external SDK prerequisites; Debug/Release bootstrap without preinstalled `joypm`; macOS Debug fallback without dsymutil and produced PDB/dSYM byproducts; joypm self-build and second-stage execution |

Use Joyeer executable tests for pure tool logic and fixture projects for
end-to-end behavior; CMake/CTest can capture outputs and enforce test timeouts
without requiring those facilities in the released tool. Use existing
GoogleTests for C++/C runtime changes. Add controlled failing-child fixtures
rather than executing arbitrary shell commands in tests.

During development, run the narrow affected tests. Reconfigure CMake when
registering new targets/fixtures. For each release gate, build with
`BUILD_TESTING`, `JOYEER_BUILD_JOYPM`, and `JOYEER_BUILD_UNITTESTS` enabled and
run unfiltered CTest on each claimed platform/architecture. The tool
gates are independent of the unit-test option, but a tool-only run does not
replace the full gate. A Windows-only pass is not evidence of POSIX behavior.

**The first local-workflow implementation and native Windows ARM64 acceptance
are complete:** fresh projects initialize, check, build in both profiles, run
with preserved arguments, and execute declared tests. Failures have actionable
diagnostics and the specified statuses; no stale executable runs after
failure. Other native platforms still need their gates before broader release
claims, and all later package features above remain deferred.
