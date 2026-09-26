# Joyeer Implementation Roadmap

> **Status:** active work only. The current compiler boundary is documented in
> [Implemented Language Surface](../impl/supported-features.md). Remove items
> from this file when they are complete instead of retaining migration history.

## Current priorities

### 1. Strengthen correctness coverage

- Expand ownership and control-flow edge-case coverage beyond the current
  acceptance workload.
- Fix the native JSON parser's malformed-input handling for leading-zero
  numbers and unescaped control characters.
- Define and test integer-boundary behavior, including decimal accumulation,
  out-of-range JSON input, and contextual handling of
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

### 4. Broaden file, path, and process support

- Define portable path encoding and use wide-character Windows host
  boundaries for command-line arguments and filesystem operations.
- Add non-destructive filesystem operations, streaming, writing, metadata, and
  explicit error contracts.
- Design synchronous process execution with argument arrays, working-directory
  control, typed launch/wait failures, and no implicit shell evaluation.
- Preserve the compatibility contract of the existing `readFile(path:)`
  builtin while introducing richer APIs.

The proposed host contracts and acceptance cases are tracked in
[joypm M0](joypm-m0.md).

### 5. Establish performance and footprint evidence

- Add reproducible workloads for runtime performance, allocation behavior, and
  generated-program size.
- Define optimization, stripping, and LTO policies without weakening checked
  arithmetic, bounds, ownership, or debug semantics.
- Measure supported platforms and distinguish generated-program costs from the
  LLVM-based compiler/backend package.

### 6. Expand the language deliberately

- Complete the design and end-to-end implementation boundaries for modules,
  user-defined generics, error propagation, and runtime contracts before
  admitting their syntax as supported.
- Keep `Optional`, `Result`, `Array`, and `Dict` on their compiler-known path
  until general generic declarations and monomorphization are specified.
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

- Decide which runtime contract APIs are required in optimized builds and
  preserve the distinction between elidable `assert` checks and required
  precondition, arithmetic, and bounds semantics.
- Define any compile-time verification work as a bounded decidable subset or
  optional solver-assisted layer with explicit timeout and unproved-result
  behavior.
- Do not promise automatic proof of arbitrary program properties.

## Planning policy

- Keep completed behavior in [implementation documentation](../impl/) and
  durable tests, not in this roadmap.
- Keep normative language rules in the [specification](../spec.md) and design
  motivation in [rationale](../rationale/).
- A source-visible feature is not complete until its frontend, Joyeer IR,
  native backend/runtime, diagnostics, and durable fixtures agree.
- Use unfiltered CTest as the final acceptance gate; focused labels are for
  iteration only.
