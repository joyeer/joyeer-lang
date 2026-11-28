# 0033: Names for native link inputs

- Status: accepted (2026-11-28), based on native validation with the fixes applied
- Spec: [§19.7](../../specs/19-c-interop.md#197-linking-and-toolchain-responsibilities)

## Context

The compiler accepts explicit native library files, and fern forwards the
files declared by the selected dependency closure. That works for staged
libraries, but it does not let a project request an SDK or system library
whose concrete filename and location are chosen by the target linker.

## Decision

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
Linux AArch64 under WSL2.

On macOS, the native host was an Apple M3 Pro (`Mac15,6`, arm64 without
Rosetta, macOS 27.0.1 build 26A434), with macOS SDK and deployment target
26.5 and Homebrew LLVM and LLD 22.1.8. The unmodified base commit
`513aeed179c565d82322dab2d920e029bdd58a65` passed both focused tests in Debug
and Release but failed nine CTests in each configuration.

Acceptance is based on isolated checkouts of that commit with the Mach-O
and string comparison fixes applied, not on the unmodified commit. The
tested source patch has SHA-256
`75c50100d859ec14b1cf7960d8fb9f271ebcbc8abc59a6a80e705172a18502f3`.
Both regular configurations passed the focused tests and
`JoyeerCInterop.NamedLibraryReservedSpellingAndCollision`. The full validator
passed without retries or skips:

| Configuration | Unfiltered CTest | Root `fern test` |
|---|---:|---:|
| Debug | 2284/2284 | 7/7 |
| Release | 2283/2283 | 7/7 |
| Debug without `dsymutil` | 2281/2281 | 7/7 |

With `dsymutil` disabled, the three dSYM-dependent tests were not registered.
Explicit `-gfull` exited 1 with `linker.missing-tool` naming `dsymutil`.
See the [macOS validation evidence](../impl/fern.md#macos-arm64-toolchain-validation).
