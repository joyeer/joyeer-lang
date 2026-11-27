# 0006: Toolchain Standard-Library Resolution

- Status: in progress (updated 2026-11-23); compiler lookup, fern, staging, and
  installation are implemented. Native Windows ARM64 and Linux AArch64 validation remain.
- Decisions: none
- Spec: [§12](../../specs/12-modules.md)

Related: [Standard modules](../../specs/12-modules.md#125-standard-modules), [native link inputs](0004-native-link-inputs.md),
[fern](../impl/fern.md), [Compression](../impl/compression.md).

## Goal

By default, each development or installed compiler finds its own bundled standard modules.
Workspaces can use those modules without copying them or adding workspace members.

## Definition of done

- Staged and installed toolchains hold the bundled runtime archive, standard modules, and
  module index in their own `lib/` directory.
- Fern resolves the standard modules bundled with the compiler it selects.
- The layout and resolution pass [full platform validation](../building.md#full-platform-validation)
  in Debug and Release on each supported native target.

## Current state

Joyeer finds its standard library relative to the compiler's own executable.

### Layout

```text
<root>/
  joyeer, joyeer-backend, fern, moss
  lib/
    JoyeerRuntime.lib or libJoyeerRuntime.a
    index.toml
    Foundation/   project.toml  *.joyeer
    System/       project.toml  *.joyeer
    Compression/  project.toml  compression.joyeer  native/<archive>  licenses/
```

`<root>` is the directory of the running `joyeer`. In development builds,
`JoyeerToolchain` stages the runtime archive in `out/build/<preset>/bin/lib`;
`JoyeerStandardLibrary` stages the modules and index there
([stageStandardLibrary.cmake](../../scripts/cmake/stageStandardLibrary.cmake)).
The `JoyeerRuntime` and `StandardLibrary` install components put their respective
files in `<prefix>/lib` without duplicating the archive. `JOYEER_BUILD_COMPRESSION`
defaults to `ON`; `StandardLibrary` installs [Compression](../impl/compression.md) when
built. Disabling it also disables the moss build. `JoyeerRuntime` does not depend on it.

`lib/index.toml` lists the installed modules under a build stamp:

```toml
schema-version = 1
compiler-version = "0.0.1"
target = "windows-amd64"
configuration = "debug"
modules = ["Foundation", "System", "Compression"]
```

The compiler rejects an index whose version, target, or configuration differs from its own
(`toolchain.library-mismatch`).

### Compiler

- Options: `--sysroot <dir>` (override the standard-module root), `--print-sysroot`, `--no-stdlib`.
  The runtime archive stays relative to the compiler executable. No environment variable or
  `PATH` search.
- Import resolution order: supplied modules (`--module-name` / `--module-source`), then `lib/`. A
  supplied module with a standard module's name takes precedence, which lets the repository's root
  workspace develop the library modules. Unknown names stay `module.unknown-import`.
- Standard modules are read from their `project.toml` and loaded lazily, only when imported, with their
  own imports resolved the same way. A single source file can import them too.
- The toolchain, not the importer, authorizes a standard module's `native-bindings`, and its
  `native-libraries` are linked only when the module is imported. Paths must be relative with no parent
  traversal (`toolchain.library-invalid`).
- Sources: [toolchain.cpp](../../compiler/driver/toolchain.cpp) and the module graph in
  [compiler+service.cpp](../../compiler/driver/compiler+service.cpp).

### fern

A module may list a standard module in `dependencies` without a workspace member. Fern reads
`<compiler directory>/lib/index.toml` for the names and leaves resolution to the compiler.
Use fern's `--compiler` option to select a complete toolchain. This plan accepts the
[documented scope](../impl/fern.md#1-canonical-organization): `--sysroot` overrides standard-module
lookup for direct `joyeer` calls, not for fern. Workspace validation is unchanged.

## Todo

### Completed

- [x] Resolve bundled standard modules in the compiler and fern. Evidence:
  [compiler lookup](../impl/backend.md#2-cli) and
  [fern bootstrap, self-build, and restaging validation](../impl/fern.md#standard-library-adoption-validation).
- [x] Move `JoyeerRuntime.*` into `lib/` (9.3 of the [runtime plan](0008-runtime-in-joyeer.md)).
  Evidence: macOS ARM64 Debug and Release passed the full validator and the installed compiler
  and fern packaging tests. A relocated `install-debug.py` install also linked and ran `hello`.
- [x] Windows x64: [post-layout validation at `5d75dad`](../decisions/0031-equatable.md#platform-validation).
  Debug and Release each passed unfiltered CTest 2200/2200 and `fern test` 7/7;
  some CTest cases passed on the validator's rerun.
- [x] Linux x86-64 (WSL): [post-layout validation at `5d75dad`](../decisions/0031-equatable.md#platform-validation).
  Debug and Release each passed unfiltered CTest 2175/2175 and `fern test` 7/7.
- [x] macOS ARM64: the archive move in `8289622` recorded unfiltered CTest 2019/2019 and
  `fern test` 7/7 in Debug and Release, including `Fern.Installed` and
  `InstalledCompilerNativeHello` ([runtime plan](0008-runtime-in-joyeer.md)).
  The earlier run at `9b334ec` still used the old archive location.

### Remaining

Run the [full validator](../building.md#full-platform-validation) in Debug and Release,
with Compression enabled, on each native target. Record the install component and relocation
results alongside the full suite.

- [ ] Windows ARM64: no recorded validation of the current `lib/` layout.
- [ ] Linux AArch64: no recorded validation of the current `lib/` layout.

## Open questions

None.

## Out of scope

Fern custom sysroots, precompiled modules, cross-compilation, and environment variable or `PATH`
search for standard modules.

## Progress log

- 2026-11-23: recorded Windows x64 and Linux x86-64 validation after the archive move.
  Kept the native ARM64 gates open. Removed the resolved Compression install question and
  scoped out fern custom sysroots, precompiled modules, and cross-compilation.
- 2026-11-22: Moved the archive into `lib/` for build staging and installs. The macOS ARM64
  validator passed in Debug and Release, with CTest 2019/2019 and `fern test` 7/7 in each.
  Windows and Linux were not rerun for the layout change.
- 2026-11-22: Checked off macOS ARM64 library-resolution validation after the full Debug and
  Release runs. Those builds still installed the archive beside the compiler.
- 2026-11-12: converted to the plan template; the Windows x64 validation was already done and is checked.
