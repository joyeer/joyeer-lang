# 0034: Filesystem identity for fern local dependencies

- Status: implemented on Windows ARM64 and Linux AArch64 (2026-11-28);
  pending macOS ARM64 acceptance
- Plan: [0001](../plan/0001-package-manager.md#p4-external-local-modules)
- Contract: [fern](../impl/fern.md) and [host operations §18](../../specs/18-host.md)

## Context

Workspace dependencies currently select named, enumerated members. A path
dependency outside those members can enter the graph twice through different
spellings, symlinks, or hard links. Lexical `joinPath` and `fileKind` cannot
establish that two such paths name the same file.

## Decision

- Resolve a relative dependency path against its declaring manifest's
  directory, not the invocation directory. Load its `project.toml` through
  the bounded regular-file reader. Use the filesystem identity of the
  opened manifest and its resolved containing directory, not the displayed
  path, as the local identity.
- Resolve each manifest's source paths from its owning directory spelling.
  Two aliases to the same physical manifest and directory resolve once.
  Explicit roots and workspace members keep their spellings; otherwise the
  first deterministic discovery supplies the spelling for outgoing paths.
  If a manifest is hard-linked into another directory, reject the ambiguous
  installation instead of silently choosing which relative source set to
  use. Do not treat lexical path checks as confinement.
- The closure contains one identity per logical module name. Reject two
  different identities declaring the same name and report both paths. Detect
  cycles by identity, including cycles reached through aliases. Apply bounds
  to the file reads, graph size and dependency traversal before building.
- Path dependencies are mutable local inputs: there are no version
  aliases, remote fetches, or lockfile claims in this phase.

## Consequences

The host API must report identity for the same opened file the loader
reads. On POSIX, device and inode identify an open file; on Windows, use
the volume identifier and file ID from the handle. Failure to acquire a
reliable identity is an error, not a reason to compare path strings instead.
This identifies inputs during one graph load, not a stable cache key or a
race-free sandbox.

## Evidence

`Fern.LocalDependencies` passed on Windows ARM64 and Linux AArch64 under
WSL2. It builds and runs a transitive dependency closure, verifies binary
prefix and identity behavior, and rejects alias cycles, ambiguous hard links,
duplicate module names, wrong module kinds, and oversized graphs. The fixture
uses directory links when supported and exercises FIFO rejection on POSIX.
The full validator passed on both hosts: Windows ARM64 CTest 2274/2274 in
Debug and 2273/2273 in Release; WSL2 Linux AArch64 2249/2249 and 2248/2248
(one Release CTest passed on rerun). All four root `fern test` runs passed
7/7. `Fern.LocalDependencies` also passed on macOS ARM64 in Debug and Release
at `513aeed1`; [full macOS validation](../impl/fern.md#macos-arm64-toolchain-validation)
remains blocked.
