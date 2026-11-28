# 0035: Fern compiler discovery and automatic debug output

- Status: implemented; native macOS acceptance pending
- Plan: [0001](../plan/0001-package-manager.md)
- Contract: [Fern commands](../impl/fern.md#2-commands-and-selection),
  [host queries](../../specs/18-host.md#187-process-environment-and-current-directory),
  and [debug information](../impl/backend.md#2-cli)

## Context

A colocated or relocated toolchain should work without requiring a compiler
argument on every Fern command. macOS Debug builds also need to work when the
selected compiler was built without `dsymutil`, without weakening an explicit
request for debug information.

## Decision

- An explicit `--compiler` or `JOYEER_COMPILER` path is authoritative. Otherwise,
  use the compiler beside the running Fern image, then search absolute `PATH`
  entries. Do not invoke a shell or implicitly search the working directory.
  Bind the standard module index to the selected compiler's directory.
- For macOS native Debug output, pass `-gauto`. The selected compiler uses
  full debug information if its configured `dsymutil` is present and omits
  debug information if it is absent. Command-line options requesting explicit
  debug information still require the tool for executable output.

Reading the running image avoids depending on how Fern was invoked. Preferring
the sibling over `PATH` preserves a relocated toolchain as a unit. The compiler
already owns the `dsymutil` path, so an automatic option avoids a separate probe
process or guessing from Fern's bootstrap configuration. A broken or failing
configured symbol tool remains an error.

## Evidence

`Fern.CompilerDiscovery` checks override precedence, sibling selection and
PATH filtering; `Fern.Installed` checks relocation and standard-module
resolution without an explicit compiler override. Both passed in the full
native Debug and Release validators on Windows ARM64 and WSL2 Linux AArch64.
Both checks also passed on macOS ARM64 in Debug and Release at `513aeed1`.
`Fern.SelfBuild` passed with and without `dsymutil`. The independent Debug
build without it also passed the relocated install and confirmed that explicit
`-gfull` exits 1 with `linker.missing-tool`. See
[macOS validation evidence](../impl/fern.md#macos-arm64-toolchain-validation)
for the remaining full-suite failures.
