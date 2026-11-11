# Library Modules and fern Plan

> **Status:** unfinished work only. Remove items when they are done.
>
> **Done (see [fern](../impl/fern.md), [Compression](../impl/compression.md),
> [runtime](../impl/runtime.md#host-operations)):** `Foundation` (text helpers,
> UTF-8 validation and `crc32`, + `FoundationTests`); `System` with every host
> operation of §18 in Joyeer, lexical path helpers and message formatters
> (+ `SystemTests`); the optional `Compression` module; the root
> `workspace.toml`; standard-module resolution from the toolchain
> ([toolchain libraries](toolchain-libraries.md)); and the removal of the
> compiler's host built-ins ([decision 0004](../decisions/0004-remove-host-builtins.md)).
>
> **Pending:** the items in §2–§4.

## 1. Module Plan

| Module | Responsibility | Boundary |
|---|---|---|
| `Foundation` | Text, encoding validation, numeric parsing/formatting, checksums | No dependency on `System` |
| `System` | Paths, filesystem, processes, standard streams, environment, time | May use `Foundation`; keep the §18 contracts |
| `Collections` | Extra containers and algorithms | Independent; builtin arrays/dicts do not need it |
| `Testing` | Assertions, failure reporting, helpers | May use `Foundation` |

Rules (the language-level ones are in [§12.5](../../specs/12-modules.md#125-standard-modules)):
- Each module is one real source unit with its own `project.toml`; no umbrella
  or re-exports. Keep the dependency graph acyclic.
- Prelude types (`String`, `Array`, `Dict`, `Optional`, `Result`, and the host
  error and status enums) need no import.
- Native bindings are authorized by the toolchain (`native-bindings = true`),
  never by location or module name. Public APIs wrap internal bindings.
- Add a module or workspace member only when it has real sources; no empty
  modules.
- Layout: `lib/<Module>/`, tests in `tests/lib/<Module>/`, both
  listed in the root `workspace.toml`.

## 2. Remaining Library Work

- **Contract APIs.** `assert`, `precondition` and `fatalError` (§9) are not
  provided. They are prelude names in the spec, so they need either compiler
  declarations lowered to the runtime panic, or a decision to ship them as
  Joyeer source the compiler loads implicitly. `fatalError` returns `Never`.
  Decide this before `Testing`, which builds on them.
- **System: host detection.** Callers pass an explicit `windows: Bool` to the
  path helpers. `@platform` (§11.5) now makes a `System.isWindows()` or a host
  separator function trivial; add one and keep the explicit-flag forms for
  cross-host path work.
- **System: environment and time.** Reading environment variables, the current
  directory, monotonic and wall-clock time. Each needs a §18 contract first.
- **System: owned OS resources.** Open files, directory handles and child
  processes as values need `deinit` and noncopyable types (§3.7, §4.7), which
  are not implemented; until then every operation opens and closes within one
  call.
- **Verification.** The POSIX variants of `System` have never run
  ([filesystem plan](filesystem.md) §2).
- **Collections:** add only for a concrete consumer.
- **Testing:** add only for a concrete consumer; then use it in test modules.
- Use ordinary public functions until methods/protocols exist.

## 3. Fern Adoption

Replace duplicated helpers in
[fern common.joyeer](../../tools/fern/common.joyeer) with library calls,
after defining public input, error, and complexity contracts. Do it
incrementally. fern already imports `System` for host operations; its own
path, text and number helpers are the next candidates. Update bootstrap source
mappings and staged self-build manifests together. Fern must not depend on a
previously installed tool.

## 4. Installed Libraries

Resolution of standard modules from the toolchain is implemented; the remaining
items (moving the runtime archive into `lib/`, fern and `--sysroot`, shipping
Compression by default, precompiled modules, cross-compilation) are tracked in
[toolchain libraries](toolchain-libraries.md) §3. This is distinct from
registry packages and binary-library ABI ([package manager](package-manager.md)).

## 5. Acceptance

For each new module: source/dependency validation, visibility, case-sensitive
names, ambiguity, prelude collisions, duplicate names and cycles. Run real test
executables in Debug and Release, and confirm a failing test returns nonzero.
A source-only build or `0 tests` is not acceptance. Follow
[building](../building.md); record evidence in impl docs.
