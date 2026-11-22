# 0006: Toolchain Standard-Library Resolution

- Status: in progress (updated 2026-11-22); implemented for the compiler, fern, CMake staging and
  install. Windows x64 and macOS ARM64 validation are recorded; follow-ups remain.
- Decisions: none
- Spec: [§12](../../specs/12-modules.md)

Related: [Standard modules](../../specs/12-modules.md#125-standard-modules), [native link inputs](0004-native-link-inputs.md),
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
files in `<prefix>/lib` without duplicating the archive.

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

A module may list a standard module in `dependencies` without a workspace member. fern reads
`<compiler directory>/lib/index.toml` for the names and leaves resolution to the compiler. Workspace
validation is unchanged.

## Todo

- [x] Move `JoyeerRuntime.*` into `lib/` (9.3 of the [runtime plan](0008-runtime-in-joyeer.md#11-todo)).
  Evidence: macOS ARM64 Debug and Release passed the full validator and the installed compiler
  and fern packaging tests. A relocated `install-debug.py` install also linked and ran `hello`.
- [ ] fern and `--sysroot`: fern derives the library from the compiler path and does not pass or honor
  `--sysroot`. Have fern run `--print-sysroot`, or accept the limitation permanently.
- [ ] Decide whether Compression ships in the default install (it is optional because it fetches miniz)
  now that moss depends on it.
- [x] Windows x64: the unfiltered CTest passes in Debug and Release with Compression enabled. Evidence:
  run of 2026-11-07, with `System` as the only host provider.
- [ ] Windows ARM64 validation.
- [ ] Linux validation.
- [x] macOS ARM64: clean Debug and Release validation at `9b334ec` with Compression enabled.
  Evidence: unfiltered CTest 2019/2019 and `fern test` 7/7 in each configuration, including
  `StandardLibraryTest`, `Fern.Installed`, and `InstalledCompilerNativeHello`. Relocated Debug
  and Release installs also resolved `System` and linked the import fixtures
  ([runtime plan §9.4 and §10.7](0008-runtime-in-joyeer.md#11-todo)).

## Open questions

- Precompiled modules: standard modules compile from source with each program; a stable module format
  would avoid that.
- Cross-compilation: one root per target is assumed; cross builds may need `lib/<target>/native/`.

## Out of scope

An environment variable or `PATH` search for the library.

## Progress log

- 2026-11-22: Moved the archive into `lib/` for build staging and installs. The macOS ARM64
  validator passed in Debug and Release, with CTest 2019/2019 and `fern test` 7/7 in each.
  Windows and Linux were not rerun for the layout change.
- 2026-11-22: Checked off macOS ARM64 library-resolution validation after the full Debug and
  Release runs. Those builds still installed the archive beside the compiler.
- 2026-11-12: converted to the plan template; the Windows x64 validation was already done and is checked.
