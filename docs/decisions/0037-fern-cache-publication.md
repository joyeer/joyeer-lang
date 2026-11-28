# 0037: Fern cache inputs and publication

- Status: proposed; hashing and single-file namespace replacement implemented
- Plan: [0001 P6](../plan/0001-package-manager.md#p6-rebuild-avoidance)

## Context

Fern invokes an independent compiler process for each selected module. A
cache can skip an invocation only when it represents the inputs the
compiler actually reads and the native output it actually produced. The
manifest graph alone omits compiler-discovered standard modules and native
inputs. File identities and timestamps alone are not content fingerprints.

## Proposed contract

- Use a versioned, length-framed SHA-256 input record. Include the command
  arguments, delivered source paths and bytes, manifest bytes, resolved
  edges, target, profile, native libraries and search results, and compiler,
  backend, runtime, standard-library and platform SDK identities. Preserve
  order where it affects the invocation. Unavailable or unstable input
  identity disables reuse rather than guessing. An independent build
  snapshot, or compiler evidence of the bytes it read, is needed before
  storing an entry; a second pathname read alone does not establish that.
- Start with whole-target artifacts, not incremental compilation or a
  cross-machine cache. A cache entry includes a complete file inventory and
  content digests for the executable and applicable debug artifacts.
  Missing, truncated, or mismatched entries do not run. An old generation
  never substitutes for a failed rebuild.
- Claim private sibling staging directories exclusively. Build and verify
  there before publishing a complete entry with a same-filesystem native
  operation; another writer of the same key can reuse a verified winner.
  Scope the host contract precisely: atomic visibility to readers is not a
  durability guarantee without flushing file data and directory metadata.
  `System.replaceFile` supplies only the same-filesystem namespace step.
  Implement and test the required durability separately on each host.
- `clean` removes only recognized owned outputs. It must walk from an
  open root and operate on child names relative to that root, rejecting
  symlinks, junctions, reparse points, and unknown entries. A lexical path
  test and a separate pathname deletion would permit a race. An active
  generation or publisher needs coordination before any deletion.

## Acceptance before enabling reuse

Use compiler-generated evidence for actual read inputs, including the
selected native provider. Verify simultaneous builds, invalidation of
each input class, corrupt cache entries, interrupted publication, and
non-link-following cleanup under renames. Do not enable the cache on a
host where the required native operations cannot establish the contract.
