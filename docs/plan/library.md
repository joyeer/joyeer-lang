# Library Modules and joypm Plan

> **Status:** unfinished work only. Remove items when they are done.
>
> **Done (see [joypm](../impl/joypm.md), [Compression](../impl/compression.md)):**
> `Foundation` (+ `FoundationTests`), root `workspace.toml`, part of `System`
> (lexical path helpers, host error/status formatters, `SystemTests`), and the
> optional `Compression` module.
>
> **Pending:** the items in §2–§4.

## 1. Module Plan

| Module | Responsibility | Boundary |
|---|---|---|
| `Foundation` | Text, encoding validation, numeric parsing/formatting | No dependency on `System` |
| `System` | Paths, filesystem, processes, environment, time | May use `Foundation`; keep existing host contracts |
| `Collections` | Extra containers and algorithms | Independent; builtin arrays/dicts do not need it |
| `Testing` | Assertions, failure reporting, helpers | May use `Foundation` |

Rules:
- Each module is one real source unit with its own `project.toml`; no umbrella
  or re-exports. Keep the dependency graph acyclic.
- Prelude types (`String`, `Array`, `Dict`, `Optional`, `Result`) need no import.
- Runtime-only native bindings are not available to library code because of its
  location or module name. Public APIs wrap internal bindings.
- Add a module or workspace member only when it has real sources; no empty
  modules.
- Layout: `src/library/<Module>/`, tests in `tests/library/<Module>/`, both
  listed in the root `workspace.toml`.

## 2. Remaining Modules

- **System:** filesystem and process wrappers, host detection (no builtin
  exists; callers pass an explicit `windows` flag today), environment, time.
  Do not change current host encoding, error, or ownership semantics.
- **Collections:** add only for a concrete consumer.
- **Testing:** add only for a concrete consumer; then use it in test modules.
- Use ordinary public functions until methods/protocols exist.

## 3. joypm Adoption

Replace duplicated helpers in
[joypm common.joyeer](../../src/tools/joypm/common.joyeer) with library calls,
after defining public input, error, and complexity contracts. Do it
incrementally, using explicit local source mappings. Update bootstrap source
mappings and staged self-build manifests together. joypm must not depend on a
previously installed tool.

## 4. Installed Libraries (separate gate)

joypm resolves only inside an explicitly loaded workspace. Before offering
`import Foundation` from an external project, define:

- A toolchain-matched source-module catalog and relocatable install layout;
  missing sources or version mismatches are explicit errors.
- joypm resolution that combines project dependencies with selected bundled
  modules and passes complete source mappings to the compiler. Do not weaken
  workspace path validation.
- Reserved official names and workspace/SDK collision rules. Reject ambiguity;
  no first-found search order.
- The compiler keeps taking explicit source inputs; it never discovers
  libraries or reads manifests.
- Prelude lookup precedes imported short names, so importing `System` does not
  redirect an existing builtin. Use qualification; specify any builtin
  migration separately.

This is distinct from registry packages and binary-library ABI.

## 5. Acceptance

For each new module: source/dependency validation, visibility, case-sensitive
names, ambiguity, prelude collisions, duplicate names and cycles. Run real test
executables in Debug and Release, and confirm a failing test returns nonzero.
A source-only build or `0 tests` is not acceptance. Follow
[building](../building.md); record evidence in impl docs.
