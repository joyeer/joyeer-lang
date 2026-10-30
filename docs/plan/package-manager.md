# Joyeer Package Manager: Remaining Work

> **Status:** unfinished work only. Current behavior and validation evidence
> belong in [joypm](../impl/joypm.md). Remove items when they are done.

## 1. Phases

| Phase | Deliverables and exit criteria |
|---|---|
| P4: External local modules | Path dependencies outside workspace members; bounded recursive loading and dedup; reject cycles and conflicting logical identities; no remote access |
| P6: Rebuild avoidance | Whole-module fingerprints, concurrent publication/cache checks, anchored `clean`; changed inputs invalidate, corrupt entries miss, deletion never follows links outside owned outputs |
| P7: Remote packages | Git/registry resolution, lockfiles, offline/frozen modes, integrity and publishing policy; pinned inputs, diagnosed interrupted fetches and conflicts |

P6 and P7 are independent later workstreams. A registry need not wait for
incrementality; workspaces need not fetch remote packages.

## 2. Design Gates

- **P4:** identity is canonical filesystem identity, not a display name or
  lexical path. Logical module names must be unique across the closure; no
  implicit renaming or side-by-side versions initially. Source paths stay
  relative to the owning manifest. Local paths are mutable, not locked.
- **P6:** this is a whole-target cache, not compiler incremental compilation.
  Fingerprint inputs: source bytes, manifests and resolution graph,
  compiler/backend/runtime/SDK identity, profile and options, target. Current
  APIs do not supply all of these; add tested contracts first.
- **P7:** settle version semantics, source identity, checksums or pinned
  revisions, lockfile versioning, credentials, archive traversal/link policy,
  offline/frozen behavior, and interrupted-download publication before running
  external code. Lockfile writes need reliable replacement and concurrency.

## 3. Host Contracts Still Needed

- **Filesystem identity** for P4.
- **Publication and cleanup:** reliable, crash-atomic, concurrent-safe
  replacement, and anchored non-link-following deletion (before P6/P7).
- **Compiler discovery:** executable-location, current-directory, and
  environment lookup, before adding sibling or PATH discovery.
- **Dependency visibility:** if strict direct-import allowlists are adopted,
  add a compiler interface; joypm must not scan source text.
- Separately scoped, not prerequisites: streaming, process
  capture/cancellation/timeouts, Unicode-aware diagnostics.

New APIs must not present lexical path checks as confinement. Stay aligned with
[supported features](../impl/supported-features.md),
[compiler input contract](../impl/backend.md#2-cli), and
[host operations](../spec/18-host.md).

## 4. Native Platform Validation

On each claimed platform and architecture (Linux, macOS), with
`BUILD_TESTING`, `JOYEER_BUILD_JOYPM`, `JOYEER_BUILD_UNITTESTS` on:

- Fresh Debug and Release builds and unfiltered CTest.
- The [joypm acceptance gates](../../tests/joypm/CMakeLists.txt); another
  platform's or a tool-only run does not count.
- POSIX FIFO/device rejection, signal termination, execute-permission failures;
  keep legacy `readFile` behavior.
- macOS bootstrap with and without `dsymutil`, dSYM byproducts, self-build,
  second-stage run.
- Separate install components and relocated compiler/backend/runtime lookup;
  the [local Debug installer](../building.md#local-debug-installation).

Record provenance and results in
[joypm evidence](../impl/joypm.md#5-validation-evidence), not here.

## 5. Release Gates

- Third-party license/notice assembly for LLVM/LLD and platform libraries
  before binary redistribution.
- Per-platform audit of SDK/runtime dependencies and product inventory;
  exclude test SDKs and LLVM tools; verify relocation.
- Repeatable native CI and release staging for the supported platforms.
- Release scripting uses standard-library Python 3.9+, never required to build
  or run Joyeer.

Readiness requires the native gates above and
[release staging](../building.md#release-staging), not a version string.
