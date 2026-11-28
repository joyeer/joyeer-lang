# 0036: Fern remote package identity and lock policy

- Status: proposed; no fetch, lockfile, or build behavior implemented
- Plan: [0001 P7](../plan/0001-package-manager.md#p7-remote-packages)

## Context

Local path dependencies deliberately follow the live filesystem. A remote
dependency must instead select reproducible bytes, whether fetched today or
read from a cache later. The source module's logical name is still the name
used by the compiler; a registry package name or Git URL is not an import.

## Proposed contract

- Use exact stable `MAJOR.MINOR.PATCH` registry versions in the first schema.
  No ranges, prereleases, build metadata, or floating `latest`. A registry
  identity includes its HTTPS origin, package name, and version. A Git
  identity includes its HTTPS URL, full commit object ID, and package
  subdirectory. Branch names, tags, abbreviated IDs, submodules, LFS
  resolution, and automatic URL rewrites are not supported.
- A registry version cannot be republished. Both sources record a
  SHA-256 digest in the lock: exact archive bytes for a registry package and
  a canonical source-tree digest for a Git package. For Git the digest covers
  bytewise-sorted UTF-8 paths, each encoded with its byte length and file
  contents with their byte length; it is independent of timestamps and Git
  object packing. A changed digest for a locked identity is an integrity
  failure, never an update. Define the byte-level encoding and fixed limits
  before using the digest on disk.
- One workspace-root lockfile records a schema version, exact source
  identities, digests, and resolved module edges in deterministic order.
  An unknown schema version fails rather than falling back to unlocked
  resolution. If two locked identities declare the same logical module,
  reject with both origins rather than selecting a winner. Local path
  dependencies retain their existing semantics and are not pinned by a lock.
- Default builds use a matching lock; changing the resolution requires an
  explicit lock update. Offline mode stops before network access and can
  resolve only from a matching lock and verified local contents. Frozen mode
  never changes the lock; it may fetch the exact locked bytes unless also
  offline. Missing or corrupt required content is an error. Neither mode
  accepts an invalid lock, a substituted package, or partial cache contents.
- Use a configured authenticated transport without embedding credentials in
  URLs, manifests, locks, caches, or diagnostics. Do not write credential
  material into logs or forward credentials to a different origin.
- Limit package paths to portable, relative UTF-8 names. Reject dot and
  parent components, absolute and drive/device paths, duplicate names or
  portable-name collisions, links, special files, and entries outside
  resource limits. Check before writing; write only underneath a freshly
  claimed staging root with anchored operations. Do not run package hooks or
  scripts to acquire content. The first format contains source-only modules,
  no native bindings, native libraries, or external path dependencies.
- Downloads, decoding, and verification happen in an exclusive staging area.
  Publish only after all checks pass. Use the same host contract for cache
  entry publication and lock replacement; readers see the previous complete
  state or the next complete state. A competing writer cannot rewrite an
  immutable identity with different bytes. An interrupted writer may leave
  private staging data, not a usable partial package or lock.

## Acceptance before implementation

Specify the on-disk manifest and lock encodings, registry protocol, Git
transport restrictions, digest framing and limits, and native publication
semantics. Fixtures should cover exact-pin reuse, identity changes,
unavailable content, digest mismatches, case collisions, conflicting module
names, concurrent updates, and interrupted publication. A real network or
registry is not needed to exercise the resolver's deterministic rules.
