# Joyeer Implementation Roadmap

> **Status:** active work only. The current compiler boundary is documented in
> [Implemented Language Surface](../impl/supported-features.md). Remove items
> from this file when they are complete instead of retaining migration history.

## Current priorities

### 1. Strengthen correctness coverage

- Expand ownership and control-flow edge-case coverage beyond the current
  acceptance workload.
- Fix the native [JSON acceptance fixture](../../tests/native/json_parser.joyeer),
  which is not a conforming JSON library: it accepts leading-zero numbers
  such as `01` and unescaped control characters, and does not implement
  floating-point numbers or Unicode `\uXXXX` escape decoding.
- Define and test integer-boundary behavior in that fixture. Its decimal
  accumulation adds the ASCII byte before subtracting `b'0'`, so the valid
  maximum `Int` can trap on intermediate overflow; out-of-range input lacks
  a deliberate parse-error policy. Also specify contextual handling of
  `-9223372036854775808`.
- Strengthen Joyeer IR ownership, dominance, and opcode-type verification.

### 2. Improve diagnostics and developer feedback

- Add broader type-directed edits without guessing implicit conversions.
- Support multi-line and Unicode-aware diagnostic rendering.
- Define a stable machine-readable diagnostic format such as JSON or SARIF.
- Keep diagnostics, source locations, and ownership guidance consistent as the
  language surface expands.

### 3. Close release and platform gaps

- Stage required third-party licenses and notices.
- Provide a product-only install manifest that excludes GoogleTest and other
  development SDK content.
- Add checked-in CI for supported Windows, macOS, and Linux configurations.
- Exercise ELF and Mach-O behavior on their native platforms in addition to
  Windows release validation.
- Run the added Windows compiler `wmain` UTF-8 and native/linker-path
  regressions, including Unicode source/output/temporary paths. The fixes
  compile and have scoped Windows x64 joypm smoke evidence, not full native
  acceptance or general Unicode coverage; Unicode-aware diagnostic rendering
  remains an independent task above.

### 4. Extend host facilities beyond the first portable interface

- Execute the added regressions for integrated bounded
  `readFilePrefix(path:maximumBytes:)` and scalar host inputs. Validate joypm's
  65537-byte sentinel read and regular-file-only manifest classification,
  including final-symlink/nonregular/device rejection and exact limits,
  without changing legacy narrow `readFile(path:)` compatibility or implying
  race-free confinement.
- Specify streaming and richer filesystem metadata before exposing them.
- Define anchored, non-link-following recursive cleanup and replacement or
  atomic-publication guarantees before adding destructive operations.
- Define cancellation, capture, timeout, and asynchronous process ownership
  before extending the synchronous interface.

The accepted first-scope host contracts are in
[portable host operations](../spec/18-host.md); tool-level requirements and
explicit deferrals are tracked in [package manager plan](package-manager.md).

### 5. Establish performance and footprint evidence

- Add reproducible workloads for runtime performance, allocation behavior, and
  generated-program size.
- Define optimization, stripping, and LTO policies without weakening checked
  arithmetic, bounds, ownership, or debug semantics.
- Measure supported platforms and distinguish generated-program costs from the
  LLVM-based compiler/backend package.

Use the [measurement requirements](../impl/backend.md#7-performance-and-footprint-measurement)
when defining workloads and acceptance thresholds.

### 6. Expand the language deliberately

- Complete the design and end-to-end implementation boundaries for
  user-defined generics and runtime contracts before
  admitting their syntax as supported.
- Keep `Optional`, `Result`, `Array`, and `Dict` on their compiler-known path
  until general generic declarations and monomorphization are specified.
  If a future tool needs dictionary key enumeration, define it as an
  independent owned snapshot with unspecified traversal order; the current
  joypm schema parser does not require it.
- Treat standard-library growth as an API, ownership, portability, and
  diagnostics task rather than exposing runtime helpers ad hoc.

### 7. Improve optimized debugging

- Improve inspection of optimized values and aggregate projections.
- Preserve source locations, variable scopes, and physical type metadata
  through optimization.
- Evaluate an in-process replacement for macOS dSYM post-processing.

### 8. Extend ownership ergonomics safely

- Stress-test exclusivity on broader mutation-heavy workloads before adding
  longer-lived projections.
- Design longer-lived `yield` projections without weakening call-site and
  argument-evaluation exclusivity.
- Specify user-defined destruction and explicit copy initialization while
  keeping resource-owning values noncopyable by default.
- Define a checked generational `Handle<T>` or arena abstraction before
  presenting integer indices as a safe identity mechanism for graph-like
  storage.

### 9. Bound verification claims

- Implement runtime contract APIs with the
  [specified optimization behavior](../spec/09-contracts.md#92-standard-checks):
  elidable `assert` checks do not weaken required precondition, arithmetic,
  or bounds semantics.
- Define any compile-time verification work as a bounded decidable subset or
  optional solver-assisted layer with explicit timeout and unproved-result
  behavior.
- Do not promise automatic proof of arbitrary program properties.

### 10. Validate the first local project-manager workflow

- Run unfiltered CTest with tool and unit tests enabled. The Joyeer-written
  [joypm implementation](../impl/joypm.md) passed a Windows x64-target Debug build
  and local-project smoke run, but the nine registered gates, including
  `Joypm.ManifestLimits`, have not executed; do not mark v0 release or full
  native validation complete.
- Continue using the [receiving-machine handoff](joypm-validation-handoff.md).
  The owner reports an ARM64 originating machine, but recorded commands used
  the x64 target. Validate ARM64 natively with matching MSVC/LLVM inputs and a
  separate build tree; do not infer native coverage from x64 emulation.
- Validate strict parser limits and bounded regular-file acquisition, exact
  compiler plans, both profiles, fresh-generation/stale-artifact safety,
  serial test summaries, argument forwarding, and package child cwd.
- Execute Windows Unicode and stderr byte/error regressions, then native
  Windows/Linux/macOS, Release-bootstrap, and package/Debug-installer gates,
  including Debug PDB/dSYM byproducts and macOS fallback without `dsymutil`.
  Add self-build acceptance; current CMake bootstrap only invokes the compiler
  directly.
- Track remaining cases in the
  [validation and release plan](package-manager.md#8-validation-and-release-gates).
- Add local path dependencies and workspaces after the single-package workflow.
  Keep remote resolution, caching, and anchored cleanup behind their own
  contracts and acceptance gates.

## Planning policy

- Keep completed behavior in [implementation documentation](../impl/) and
  durable tests, not in this roadmap.
- Keep normative language rules and their design explanations together in the
  owning [specification](../spec.md) chapters; distinguish future proposals
  from adopted rules.
- A source-visible feature is not complete until its frontend, Joyeer IR,
  native backend/runtime, diagnostics, and durable fixtures agree.
- Use unfiltered CTest as the final acceptance gate; focused labels are for
  iteration only.
