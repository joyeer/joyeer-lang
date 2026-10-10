# Joyeer Package Manager: Remaining Work

> **Status:** unfinished work only. Current commands, manifest and execution
> contracts, bootstrap behavior, and acceptance evidence belong in
> [joypm](../impl/joypm.md). Remove items from this plan when they are complete.

## 1. Remaining Package Phases

| Phase | Current status / prerequisites | Remaining deliverables and exit criteria |
|---|---|---|
| P4: Local path packages | Deferred; local workflow plus physical package-identity contract | Recursive loading/source exports/package DAG; deduplicate diamonds and reject cycles/conflicting logical identities without remote access |
| P5: Workspaces | Deferred; P4 | Root/member schema, explicit selection/`--workspace`, shared plans and member-qualified outputs without target collisions |
| P6: Rebuild avoidance and artifact lifecycle | Deferred; P5 plus hashing/identity/publication/cleanup contracts | Whole-target fingerprints, concurrent publication/cache checks and anchored `clean`; changed inputs invalidate, corrupt entries miss, deletion cannot follow links outside owned outputs |
| P7: Remote package ecosystem | Deferred; P4 plus verified acquisition/publication contracts | Git/registry resolution, lockfiles/offline/frozen modes, integrity/publishing policy; pin/verify inputs and diagnose interrupted fetches/conflicts |

P6 and P7 are separate later workstreams; a registry need not wait for compiler
incrementality, and workspace support need not fetch remote packages.

## 2. Design and Host/API Gates

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

Supporting host contracts still need specification and implementation:

- **Filesystem identity:** provide physical package identity for P4 rather
  than treating lexical paths or display names as identity.
- **Publication and cleanup:** define reliable replacement, concurrent and
  crash-atomic publication, and anchored, non-link-following deletion before
  P6 caching/`clean` or P7 acquisition/lockfile publication. Deletion must not
  escape owned outputs through links or races.
- **Compiler discovery:** define executable-location, current-directory, and
  environment lookup before adding installed-sibling or PATH discovery.
- **Additional interfaces:** streaming, process capture/cancellation/timeouts,
  and general Unicode-aware diagnostics remain separately scoped work, not
  prerequisites for local path packages.
- **Dependency visibility:** if strict direct-import allowlists are adopted,
  add a compiler interface instead of scanning Joyeer source text in joypm.

Keep these extensions aligned with the
[implemented surface](../impl/supported-features.md),
[compiler input contract](../impl/backend.md#2-cli), and
[portable host operations](../spec/18-host.md). New APIs must not silently
strengthen lexical path checks into confinement claims.

## 3. Native Platform Validation

- Execute fresh native Windows x64, Linux, and macOS Debug/Release default
  builds and unfiltered CTest with `BUILD_TESTING`, `JOYEER_BUILD_JOYPM`, and
  `JOYEER_BUILD_UNITTESTS` enabled. Keep hardware, process, compiler, and SDK
  target identities distinct from emulation.
- Run the existing [acceptance gates](../../tests/joypm/CMakeLists.txt) on
  each claimed platform/architecture. A tool-only run or another platform's
  results do not replace its full gate.
- Execute POSIX FIFO/device rejection, signal termination, and
  execute-permission failures on native POSIX hosts. Preserve legacy
  `readFile` compatibility and the explicit non-confinement boundary.
- Verify macOS bootstrap with and without `dsymutil`, produced dSYM
  byproducts, self-build, and second-stage execution.
- Validate separate install components and relocated compiler/backend/runtime
  lookup on each new platform. Exercise the
  [local Debug installer](../building.md#local-debug-installation) and its
  platform-specific symbols and PATH setup there.

Use the [building guide](../building.md#configure-build-and-test) for matching
external SDKs and commands. Record platform/toolchain provenance and measured
results in [implementation evidence](../impl/joypm.md#5-validation-evidence),
not as completed checklists in this plan.

## 4. Release Gates

- Assemble the required third-party licenses/notices for LLVM/LLD and
  platform-specific support libraries before binary redistribution.
- Audit SDK/runtime dependencies and product inventories on each release
  platform. Exclude test SDKs and LLVM tools, and verify that packaged
  compiler/backend/runtime/tool files work after relocation.
- Add repeatable native CI and release staging for the supported platform
  matrix before publishing a broader release.
- Keep external compilers and SDKs developer-managed. Use cross-platform
  standard-library Python 3.9+ when release scripting is needed, without
  making Python a source-build or released-tool runtime prerequisite.

Release readiness requires the native gates above and the
[release-staging requirements](../building.md#release-staging), not merely
a version string or registered fixtures.
