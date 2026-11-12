# Library Modules and fern: Plan

- Status: in progress (updated 2026-11-12)
- Decisions: [0004](../decisions/0004-remove-host-builtins.md), [0015](../decisions/0015-noncopyable-and-linear-types.md)
- Spec: [§9](../../specs/09-contracts.md), [§12.5](../../specs/12-modules.md#125-standard-modules),
  [§18](../../specs/18-host.md)

## Goal

A small set of standard modules with clear boundaries, used by fern in place of its own duplicated
helpers.

## Definition of done

- Contract APIs exist and `System` covers host detection, environment, time and owned OS resources,
  each with a §18 contract and tests that run in Debug and Release.
- fern's duplicated helpers are replaced with library calls.
- `Collections` and `Testing` exist only if a concrete consumer needs them.

## Current state

Done (see [fern](../impl/fern.md), [Compression](../impl/compression.md),
[runtime](../impl/runtime.md#host-operations)): `Foundation` (text helpers, UTF-8 validation and
`crc32`, with `FoundationTests`); `System` with every host operation of §18 in Joyeer, lexical path
helpers and message formatters (with `SystemTests`); the optional `Compression` module; the root
`workspace.toml`; standard-module resolution from the toolchain ([toolchain libraries](toolchain-libraries.md));
and the removal of the compiler's host built-ins (decision 0004).

### Module plan

| Module | Responsibility | Boundary |
|---|---|---|
| `Foundation` | Text, encoding validation, numeric parsing/formatting, checksums | No dependency on `System` |
| `System` | Paths, filesystem, processes, standard streams, environment, time | May use `Foundation`; keep the §18 contracts |
| `Collections` | Extra containers and algorithms | Independent; builtin arrays/dicts do not need it |
| `Testing` | Assertions, failure reporting, helpers | May use `Foundation` |

Rules (the language-level ones are in §12.5):

- Each module is one real source unit with its own `project.toml`; no umbrella or re-exports. Keep the
  dependency graph acyclic.
- Prelude types (`String`, `Array`, `Dict`, `Optional`, `Result`, and the host error and status enums)
  need no import.
- Native bindings are authorized by the toolchain (`native-bindings = true`), never by location or module
  name. Public APIs wrap internal bindings.
- Add a module or workspace member only when it has real sources; no empty modules.
- Layout: `lib/<Module>/`, tests in `tests/lib/<Module>/`, both listed in the root `workspace.toml`.
- Use ordinary public functions until protocols exist ([generics plan](generics.md)).

Acceptance for each new module: source and dependency validation, visibility, case-sensitive names,
ambiguity, prelude collisions, duplicate names and cycles. Run real test executables in Debug and
Release, and confirm a failing test returns nonzero. A source-only build or `0 tests` is not acceptance.
Follow [building](../building.md) and record evidence in the implementation notes.

## Todo

### Library

- [ ] Contract APIs: `assert`, `precondition` and `fatalError` (§9; `fatalError` returns `Never`). They
  are prelude names in the spec, so decide between compiler declarations lowered to the runtime panic
  and Joyeer source the compiler loads implicitly. Decide before `Testing`, which builds on them.
- [ ] System host detection: add `System.isWindows()` or a host separator function using `@platform`
  (§11.5), keeping the explicit `windows: Bool` path helpers for cross-host path work.
- [ ] System environment: environment variables and the current directory, each with a §18 contract
  first.
- [ ] System time: monotonic and wall-clock time, with a §18 contract first.
- [ ] System owned OS resources: open files, directory handles and child processes as values. `deinit`
  and noncopyable types now exist (decision 0015); design the handles. Until then every operation opens
  and closes within one call.
- [ ] Verify the POSIX variants of `System` ([filesystem plan](filesystem.md)).
- [ ] `Collections`, when a concrete consumer needs it.
- [ ] `Testing`, when a concrete consumer needs it; then use it in test modules.

### Fern adoption

Replace duplicated helpers in [fern common.joyeer](../../tools/fern/common.joyeer) with library calls,
after defining public input, error and complexity contracts. Update bootstrap source mappings and staged
self-build manifests together. Fern must not depend on a previously installed tool.

- [ ] Path helpers.
- [ ] Text helpers.
- [ ] Number helpers.

## Open questions

- How contract APIs reach the prelude (see the first todo item).

## Out of scope

Installed-library follow-ups (the runtime archive in `lib/`, fern and `--sysroot`, Compression by
default, precompiled modules, cross-compilation) are in [toolchain libraries](toolchain-libraries.md).
Registry packages and a binary-library ABI are in [package manager](package-manager.md).

## Progress log

- 2026-11-12: converted to the plan template; the owned-resources item now notes that `deinit` and
  noncopyable types are implemented.
