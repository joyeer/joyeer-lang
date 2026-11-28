# 0001: Joyeer Package Manager

- Status: in progress (updated 2026-11-28)
- Decisions: [0034](../decisions/0034-fern-local-dependency-identity.md),
  [0035](../decisions/0035-fern-compiler-discovery.md) (both pending macOS acceptance),
  [0036](../decisions/0036-fern-remote-package-locking.md) and
  [0037](../decisions/0037-fern-cache-publication.md) (proposed)
- Spec: [§12](../../specs/12-modules.md), [§18](../../specs/18-host.md)

## Goal

fern grows from workspace builds to external local modules, rebuild avoidance and remote packages, and
the toolchain passes native platform and release gates.

## Definition of done

- Phases P4, P6 and P7 meet their exit criteria below.
- Every claimed platform has recorded native validation.
- The release gates pass for the supported platforms.

## Current state

Current behavior and validation evidence are in [fern](../impl/fern.md), including
[macOS ARM64 toolchain validation](../impl/fern.md#macos-arm64-toolchain-validation).

| Phase | Deliverables and exit criteria |
|---|---|
| P4: External local modules | Path dependencies outside workspace members; bounded recursive loading and dedup; reject cycles and conflicting logical identities; no remote access |
| P6: Rebuild avoidance | Whole-module fingerprints, concurrent publication/cache checks, anchored `clean`; changed inputs invalidate, corrupt entries miss, deletion never follows links outside owned outputs |
| P7: Remote packages | Git/registry resolution, lockfiles, offline/frozen modes, integrity and publishing policy; pinned inputs, diagnosed interrupted fetches and conflicts |

P6 and P7 are independent later workstreams. A registry need not wait for incrementality; workspaces
need not fetch remote packages.

New APIs must not present lexical path checks as confinement. Stay aligned with
[supported features](../impl/supported-features.md), the [compiler input contract](../impl/backend.md#2-cli)
and [host operations](../../specs/18-host.md).

## Todo

### Host contracts

- [x] Filesystem identity (needed by P4): identified bounded reads and directory
  identities pass the Windows ARM64 and Linux AArch64 `Fern.LocalDependencies` gate.
- [x] Same-filesystem publication of a completed regular file without a
  copy/delete fallback. `E2EPortableFilesystem*` passes on both ARM64 hosts;
  `System.ConcurrentFilePublication` checks complete-file visibility with
  independent publishers on WSL AArch64. The Windows regression also
  checks that unsupported path namespaces are rejected for both operands.
  This is not a durability guarantee.
- [ ] Publication and cleanup: reliable, crash-atomic, concurrent-safe replacement, and anchored
  non-link-following deletion (needed by P6 and P7). Crash durability, writer
  coordination, and anchored cleanup are not provided by `replaceFile`.
- [x] Compiler discovery: implement executable-location, sibling and `PATH` lookup
  with explicit overrides. `Fern.CompilerDiscovery` and relocated `Fern.Installed`
  passed on Windows ARM64 and Linux AArch64 (WSL2); see [decision 0035](../decisions/0035-fern-compiler-discovery.md).
- [ ] Run `Fern.CompilerDiscovery` and `Fern.Installed` on macOS ARM64.
  blocked: native macOS host.
- [ ] Dependency visibility: if strict direct-import allowlists are adopted, add a compiler interface;
  fern must not scan source text.

### P4: external local modules

The [identity contract](../decisions/0034-fern-local-dependency-identity.md)
deduplicates physical manifests and their source roots. Logical module names are unique
across the closure, and local paths are mutable rather than locked.

- [x] Record the proposed identity and alias rules in
  [decision 0034](../decisions/0034-fern-local-dependency-identity.md).
- [x] Path dependencies outside workspace members, with bounded loading and dedup.
  `Fern.LocalDependencies` builds and runs a transitive closure from another cwd
  on Windows ARM64 and Linux AArch64 (WSL2).
- [x] Reject cycles and conflicting logical identities. The same fixture covers
  directory aliases, hard-linked manifests with different source bases, and a
  graph exceeding the total module limit.
- [ ] Run `Fern.LocalDependencies` in macOS ARM64 Debug and Release.

### P6: rebuild avoidance

A whole-target cache, not compiler incremental compilation. Fingerprint inputs: source bytes, manifests
and resolution graph, compiler/backend/runtime/SDK identity, profile and options, target. Current APIs
do not supply all of these; add tested contracts first.

- [x] Add an incremental SHA-256 primitive that hashes arbitrary bytes without
  materializing the entire input at once. `FoundationTests` passes the one-shot,
  padding and chunking vectors on Windows ARM64 and WSL2 Linux AArch64 at `-O0` and `-O2`.
- [x] Record the proposed input, publication, and cleanup contract in
  [decision 0037](../decisions/0037-fern-cache-publication.md).
- [ ] Contracts for every fingerprint input.
- [ ] Whole-module fingerprints and concurrent publication and cache checks.
- [ ] Anchored `clean`.

### P7: remote packages

- [x] Record a remote identity, lock, and integrity proposal for review:
  [decision 0036](../decisions/0036-fern-remote-package-locking.md).
- [ ] Settle version semantics, source identity, checksums or pinned revisions, lockfile versioning,
  credentials, archive traversal and link policy, offline and frozen behavior, and interrupted-download
  publication, before running external code.
- [ ] Git and registry resolution.
- [ ] Lockfiles with reliable replacement and concurrency.
- [ ] Offline and frozen modes.

### Native platform validation

For each additional or unrecorded platform, with `BUILD_TESTING`, `JOYEER_BUILD_FERN` and
`JOYEER_BUILD_UNITTESTS` on, using the [full-platform validator](../building.md#full-platform-validation):
fresh Debug and Release builds and unfiltered CTest; the [fern acceptance gates](../../tests/fern/CMakeLists.txt)
(another platform's or a tool-only run does not count); POSIX FIFO and device rejection, signal
termination and execute-permission failures, keeping legacy `readFile` behavior; macOS bootstrap with
and without `dsymutil`, dSYM byproducts, self-build and second-stage run; separate install components
and relocated compiler, backend and runtime lookup with the
[local Debug installer](../building.md#local-debug-installation). Record provenance and results in the
implementation notes, not here.

- [x] macOS ARM64 with `dsymutil`. Evidence: [full Debug and Release validation and relocated
  installations](../impl/fern.md#macos-arm64-toolchain-validation) at `5c21924e`.
- [x] Windows ARM64 and Linux AArch64 (WSL2) passed the full Debug and Release validator,
  host-failure regressions, install component inventories, and relocated workflows.
  Evidence: [toolchain layout validation](../building.md#toolchain-library-layout)
  at `a362c88`.
- [x] The local Debug installer stages the bundled standard modules;
  `DebugInstallerRelocatesStandardLibrary` installs into a fresh prefix, moves it,
  and compiles and runs a `System` import. Passed on both ARM64 hosts.
- [ ] Verify the macOS Debug workflow without `dsymutil`: run the full validator with
  `--define JOYEER_DSYMUTIL_EXECUTABLE=`, including bootstrap, self-build, relocated
  install, and `moss.app`. Fern now passes `-gauto`; the self-build fixture also checks
  that explicit `-gfull` still reports `linker.missing-tool`. blocked: native macOS host.
- [ ] Standalone Homebrew dependency packaging. Blocked: the installed backend still links
  external Homebrew libraries; [local relocation](../building.md#release-staging) does not
  establish a standalone distribution.
- [ ] Remaining native workflow evidence: confirm the x86-64 fern host-failure and
  installer evidence against the existing full Debug/Release runs.

Intel and universal macOS builds are not claimed targets; CMake explicitly rejects them.

### Release gates

Readiness requires the native gates above and [release staging](../building.md#release-staging), not a
version string. Local native runs count; CI runners are not required. Release scripting uses
standard-library Python 3.9+, never required to build or run Joyeer.

- [x] Stage a production Linux ARM64 Release with product-only components,
  explicit notices, and a hashed inventory. Relocated fern, compiler, Moss and
  Foundation smoke tests passed; see [Release staging](../building.md#release-staging).
- [ ] Third-party license and notice assembly for LLVM/LLD and platform libraries before binary
  redistribution. `scripts/stage_release.py` assembles an explicitly audited
  notice set and records a per-file digest; platform-specific notice content
  and redistribution terms still require review.
- [ ] Per-platform audit of SDK and runtime dependencies and product inventory; exclude test SDKs and
  LLVM tools; verify relocation. Local ARM64 component inventories and relocation pass;
  the audit must still cover the complete release product and all target dependencies.
- [ ] Verify repeatable release staging on native hosts for the supported platforms.

## Open questions

None beyond the design items in each phase.

## Out of scope

Streaming, process capture, cancellation and timeouts, and Unicode-aware diagnostics are separately
scoped, not prerequisites.

## Progress log

- 2026-11-28: Dropped CI runner provisioning as a release prerequisite.
  Repeatable native release staging and platform validation remain required.
- 2026-11-28: Added the shared SHA-256 primitive for cache and remote content
  fingerprints and added a same-filesystem file replacement primitive.
  Recorded the P6 publication proposal. Added a Release
  staging helper with explicit notices and a hashed inventory. Staged and
  relocated a production Linux ARM64 build; dependency redistribution remains open.
- 2026-11-28: Added compiler discovery and verified precedence, PATH filtering,
  relative and symlink overrides, empty selections, and relocation on the available
  ARM64 hosts. Implemented the macOS Debug fallback and its acceptance checks;
  native macOS execution remains open. Recorded the P7 remote-locking proposal;
  no remote acquisition or lockfile code was added.
- 2026-11-28: Implemented P4 host identities and local source dependencies;
  verified aliases, cycles, bounds, and an actual linked app on Windows ARM64
  and WSL2 Linux AArch64. Native macOS acceptance remains open.
- 2026-11-28: Recorded Windows ARM64 and Linux AArch64 full-suite, native host-failure,
  and relocation evidence. Fixed the Debug installer omitting `StandardLibrary` and
  added a relocated `System` import check. Recorded the proposed P4 identity rules;
  dependency implementation, publication, and distribution work remains open.
- 2026-11-26: Verified macOS ARM64 with `dsymutil` and relocated installs at `5c21924e`.
  Split out the failing no-`dsymutil` Debug workflow, confirmed the Homebrew dependency
  blocker, and removed the unsupported Intel macOS target from acceptance.
- 2026-11-22: `System` now supplies the environment and current-directory queries needed for
  compiler discovery. Executable-location lookup and discovery policy remain open.

- 2026-11-12: converted to the plan template; macOS ARM64 validation was already recorded and is checked.
