# 0001: Joyeer Package Manager

- Status: in progress (updated 2026-11-12)
- Decisions: none yet
- Spec: [§12](../../specs/12-modules.md), [§18](../../specs/18-host.md)

## Goal

fern grows from workspace builds to external local modules, rebuild avoidance and remote packages, and
the toolchain passes native platform and release gates.

## Definition of done

- Phases P4, P6 and P7 meet their exit criteria below.
- Every claimed platform has recorded native validation.
- The release gates pass for the supported platforms.

## Current state

Current behavior and validation evidence are in [fern](../impl/fern.md). macOS ARM64 functional
acceptance is recorded in [native validation evidence](../impl/c-interop.md#macos-arm64).

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

- [ ] Filesystem identity (needed by P4).
- [ ] Publication and cleanup: reliable, crash-atomic, concurrent-safe replacement, and anchored
  non-link-following deletion (needed by P6 and P7).
- [ ] Compiler discovery: executable-location, current-directory and environment lookup, before adding
  sibling or `PATH` discovery.
- [ ] Dependency visibility: if strict direct-import allowlists are adopted, add a compiler interface;
  fern must not scan source text.

### P4: external local modules

Identity is canonical filesystem identity, not a display name or lexical path. Logical module names are
unique across the closure; no implicit renaming or side-by-side versions initially. Source paths stay
relative to the owning manifest. Local paths are mutable, not locked.

- [ ] Design record for the identity rules above.
- [ ] Path dependencies outside workspace members, with bounded recursive loading and dedup.
- [ ] Reject cycles and conflicting logical identities.

### P6: rebuild avoidance

A whole-target cache, not compiler incremental compilation. Fingerprint inputs: source bytes, manifests
and resolution graph, compiler/backend/runtime/SDK identity, profile and options, target. Current APIs
do not supply all of these; add tested contracts first.

- [ ] Contracts for every fingerprint input.
- [ ] Whole-module fingerprints and concurrent publication and cache checks.
- [ ] Anchored `clean`.

### P7: remote packages

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

- [x] macOS ARM64. Evidence: [native validation evidence](../impl/c-interop.md#macos-arm64).
- [ ] Intel macOS.
- [ ] Standalone Homebrew dependency packaging.
- [ ] Each other claimed platform without recorded evidence.

### Release gates

Readiness requires the native gates above and [release staging](../building.md#release-staging), not a
version string. Release scripting uses standard-library Python 3.9+, never required to build or run
Joyeer.

- [ ] Third-party license and notice assembly for LLVM/LLD and platform libraries before binary
  redistribution.
- [ ] Per-platform audit of SDK and runtime dependencies and product inventory; exclude test SDKs and
  LLVM tools; verify relocation.
- [ ] Repeatable native CI and release staging for the supported platforms.

## Open questions

None beyond the design items in each phase.

## Out of scope

Streaming, process capture, cancellation and timeouts, and Unicode-aware diagnostics are separately
scoped, not prerequisites.

## Progress log

- 2026-11-12: converted to the plan template; macOS ARM64 validation was already recorded and is checked.
