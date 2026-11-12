# Toolchain Standard-Library Resolution: Plan

- Status: in progress (updated 2026-11-12); implemented for the compiler, fern, CMake staging and
  install, follow-ups remain
- Decisions: none
- Spec: [§12](../../specs/12-modules.md)

Related: [Library modules](library.md), [native link inputs](native-link-inputs.md),
[fern](../impl/fern.md), [Compression](../impl/compression.md).

## Goal

A standard module is found by the toolchain that ships it, and a development compiler and an installed
compiler never see each other's library. Before this work, `Compression` had to be copied into every
consuming workspace, and fern rejected member paths outside the workspace root.

## Definition of done

- `lib/` holds everything the compiler links or imports, including the runtime archive.
- fern and `--sysroot` agree, or the limitation is accepted on record.
- The resolution passes the unfiltered CTest on every supported platform.

## Current state

Joyeer finds its standard library relative to the compiler's own executable.

### Layout

```text
<root>/
  joyeer, joyeer-backend, JoyeerRuntime.*, fern, moss   executables and runtime, flat as before
  lib/
    index.toml
    Foundation/   project.toml  *.joyeer
    System/       project.toml  *.joyeer
    Compression/  project.toml  compression.joyeer  native/<archive>  licenses/
```

`<root>` is the directory of the running `joyeer`. A development build stages `lib/` into
`out/build/<preset>/bin/lib` (target `JoyeerStandardLibrary`,
[stageStandardLibrary.cmake](../../scripts/cmake/stageStandardLibrary.cmake));
`cmake --install --component StandardLibrary` installs it to `<prefix>/lib`.

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

- Options: `--sysroot <dir>` (override the root), `--print-sysroot`, `--no-stdlib`. No environment
  variable and no `PATH` search.
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

A module may list a standard module in `dependencies` without a workspace member. fern reads
`<compiler directory>/lib/index.toml` for the names and leaves resolution to the compiler. Workspace
validation is unchanged.

## Todo

- [ ] Move `JoyeerRuntime.*` into `lib/`. Today the runtime sits flat beside the compiler
  (`JOYEER_RUNTIME_PATH` is exe-relative). This changes the packaging inventory and
  `scripts/install-debug.py`; do it with or before 9.3 of the [runtime plan](runtime-in-joyeer.md).
- [ ] fern and `--sysroot`: fern derives the library from the compiler path and does not pass or honor
  `--sysroot`. Have fern run `--print-sysroot`, or accept the limitation permanently.
- [ ] Decide whether Compression ships in the default install (it is optional because it fetches miniz)
  now that moss depends on it.
- [x] Windows x64: the unfiltered CTest passes in Debug and Release with Compression enabled. Evidence:
  run of 2026-11-07, with `System` as the only host provider.
- [ ] Windows ARM64 validation.
- [ ] Linux validation.
- [ ] macOS validation.

## Open questions

- Precompiled modules: standard modules compile from source with each program; a stable module format
  would avoid that.
- Cross-compilation: one root per target is assumed; cross builds may need `lib/<target>/native/`.

## Out of scope

An environment variable or `PATH` search for the library.

## Progress log

- 2026-11-12: converted to the plan template; the Windows x64 validation was already done and is checked.
