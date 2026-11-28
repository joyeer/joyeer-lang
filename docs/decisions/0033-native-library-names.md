# 0033: Names for native link inputs

- Status: implemented on Windows ARM64 and Linux AArch64 (2026-11-28);
  pending native macOS acceptance
- Spec: [§19.7](../../specs/19-c-interop.md#197-linking-and-toolchain-responsibilities)
- Plan: [0004](../plan/0004-native-link-inputs.md)

## Context

The compiler accepts explicit native library files, and fern forwards the
files declared by the selected dependency closure. That works for staged
libraries, but it does not let a project request an SDK or system library
whose concrete filename and location are chosen by the target linker.

## Proposed decision

- Use `--link-lib` and `--link-path`, not compiler-driver-compatible `-l`
  and `-L` aliases. The names distinguish logical lookup from the existing
  `--link-library` file input and do not imply arbitrary linker passthrough.
  Libraries stay in input order; search directories stay in search order.
- Start with additive `windows`, `linux`, and `macos` manifest tables.
  The first use cases name operating-system libraries. Do not introduce
  target-triple conditions before there is an architecture-specific case.
- Limit the new manifest fields to executable and test modules. Keep
  explicit `native-libraries` on source modules and its existing propagation
  through the selected closure; do not make those modules less portable.
  Defer propagation of logical names and search directories until an
  ordering and conflict policy is needed.
- Do not deploy shared libraries. A successful link does not establish
  that the result's runtime dependencies have been installed.

## Consequences

An application that needs native lookup explicitly owns it, while existing
native-backed modules can still supply an exact artifact. A missing library
must remain a link failure. No change to native binding annotations or the
backend C ABI is proposed.

## Evidence

The existing [fern manifest contract](../impl/fern.md#1-canonical-organization)
and [C binding implementation](../impl/c-interop.md) already separate native
binding authorization from explicit library selection. Native execution
and platform-table tests are the acceptance gate for this extension.
`JoyeerCInterop.NamedLibrary` and `Fern.Plans` passed on Windows ARM64 and
Linux AArch64 under WSL2. macOS ARM64 has not run.
