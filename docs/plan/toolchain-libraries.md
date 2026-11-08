# Toolchain Standard-Library Resolution

> **Status:** implemented for the compiler, fern, CMake staging, and install.
> This document records the design and lists the remaining work. Remove items
> when they are done.
>
> Related: [Library modules](library.md), [native link inputs](native-link-inputs.md),
> [fern](../impl/fern.md), [Compression](../impl/compression.md).

## 1. Problem

A standard module must be found by the toolchain that ships it, and a
development compiler and an installed compiler must not see each other's
library. Before this work, `Compression` had to be copied into every consuming
workspace, and fern rejected member paths outside the workspace root.

## 2. Design (implemented)

Joyeer finds its standard library relative to the compiler's own executable.

### 2.1 Layout

```text
<root>/
  joyeer, joyeer-backend, JoyeerRuntime.*, fern, moss   executables and runtime, flat as before
  lib/
    index.toml
    Foundation/   project.toml  *.joyeer
    System/       project.toml  *.joyeer
    Compression/  project.toml  compression.joyeer  native/<archive>  licenses/
```

`<root>` is the directory of the running `joyeer`. A development build stages
`lib/` into `out/build/<preset>/bin/lib` (target `JoyeerStandardLibrary`,
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

The compiler rejects an index whose version, target, or configuration differs
from its own (`toolchain.library-mismatch`).

### 2.2 Compiler

- Options: `--sysroot <dir>` (override the root), `--print-sysroot`,
  `--no-stdlib`. No environment variable and no `PATH` search.
- Import resolution order: supplied modules (`--module-name` /
  `--module-source`), then `lib/`. A supplied module with a standard module's
  name takes precedence, which lets the repository's root workspace develop the
  library modules. Unknown names stay `module.unknown-import`.
- Standard modules are read from their `project.toml` and loaded lazily, only
  when imported, with their own imports resolved the same way. A single source
  file can import them too.
- The toolchain, not the importer, authorizes a standard module's
  `native-bindings`, and its `native-libraries` are linked only when the module
  is imported. Paths must be relative with no parent traversal
  (`toolchain.library-invalid`).
- Sources: [toolchain.cpp](../../src/compiler/driver/toolchain.cpp) and the
  module graph in [compiler+service.cpp](../../src/compiler/driver/compiler+service.cpp).

### 2.3 fern

A module may list a standard module in `dependencies` without a workspace
member. fern reads `<compiler directory>/lib/index.toml` for the names and
leaves resolution to the compiler. Workspace validation is unchanged.

## 3. Remaining work

1. **Move `JoyeerRuntime.*` into `lib/`.** The first version leaves the runtime
   flat beside the compiler (`JOYEER_RUNTIME_PATH` is exe-relative); moving it
   makes `lib/` hold everything the compiler links or imports. It changes the
   packaging inventory and `scripts/install-debug.py`. Do it with or before
   R9 of the [runtime plan](runtime-in-joyeer.md), when the archive starts
   being built by the compiler itself.
2. **fern and `--sysroot`.** fern derives the library from the compiler path and
   does not pass or honor `--sysroot`. Either have fern run
   `--print-sysroot`, or accept the limitation permanently.
3. **Compression in the default install.** It is optional because it fetches
   miniz. Decide whether it ships by default now that Moss depends on it.
4. **Precompiled modules.** Standard modules compile from source with each
   program. A stable module format would avoid that.
5. **Cross-compilation.** One root per target is assumed; cross builds may need
   `lib/<target>/native/`.
6. **Platform validation.** Windows x64 passes the unfiltered CTest in Debug and
   Release with Compression enabled (2026-11-07, with `System` as the only
   host provider). Windows ARM64, Linux and macOS are not yet run.
