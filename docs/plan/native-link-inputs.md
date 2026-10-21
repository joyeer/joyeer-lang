# Native Link Inputs Design (Draft for Review)

> **Status:** proposal, not implemented. Nothing here changes the current
> contract in [§19](../spec/19-c-interop.md) until accepted and adopted there.

## 1. Problem

`@c_extern(symbol:)` names only a C symbol. Today the only way to supply the
library that defines it is the CMake fixture in `tests/c-interop/`. Neither
`joyeer` nor `joypm` can accept a native library, so a Joyeer project cannot
link against one. [Linker orchestration](../../src/compiler/codegen/linker.cpp)
builds a fixed argument array: the program object, the runtime archive, and
platform libraries.

The annotation must keep naming no library (§19.7). The missing piece is a
build-level way to supply link inputs, in two layers:

1. **Compiler CLI**: the primitive.
2. **`project.toml`**: joypm's declarative layer that lowers to the CLI.

## 2. Prerequisite decision: who may use `@c_extern`

`@c_extern` is currently accepted only in a unit named by `--runtime-module`.
That restriction is a runtime-building gate, not user FFI
([plan §3](c-interop.md)). Native dependencies in a manifest are meaningful for
ordinary application modules only if user FFI is admitted.

Options:

| Option | Effect |
|---|---|
| A. Link inputs only (this proposal) | CLI/manifest can supply libraries, but only runtime-authorized units use `@c_extern`. Useful for the runtime/library plans; no new privilege. |
| B. A + public user FFI | Separate proposal (unsafe spelling, pointers). Out of scope here. |

**Recommendation: A.** Link inputs are orthogonal to who may declare imports,
and A does not widen native privileges. Revisit when B is designed.

## 3. Compiler CLI

Add repeatable options accepted for `-o` (executable) output only:

| Option | Meaning |
|---|---|
| `--link-lib <name>` | Link a library by logical name (`foo` -> `foo.lib` / `libfoo.so` / `libfoo.dylib`) |
| `--link-path <dir>` | Add a search directory for `--link-lib` (repeatable, order preserved) |
| `--link-input <file>` | Link an explicit object, archive, or import library by path |

Rules:

- Rejected with `--emit-llvm`, `--emit-runtime-object`, and validate-only
  mode with a stable diagnostic, rather than silently ignored.
- Paths are UTF-8 decoded with the existing `parseUtf8Path` and absolutized.
  `--link-input` must be a regular file; a missing file fails before linking.
- `--link-lib` names are restricted to `[A-Za-z0-9_.+-]`: no path separators,
  no `-l` style flags, no linker pass-through. This keeps the interface from
  becoming an arbitrary linker-argument channel.
- Order is preserved: user inputs follow the program object and precede the
  runtime archive and platform libraries, so user symbols can be resolved
  before the runtime fallback.
- Missing library or unresolved symbol is a native link failure. No stub or
  fallback (matches plan §7).

Implementation shape:

- `CompileOptions` gains `std::vector<LinkInput> linkInputs` (kind: library
  name / explicit file) and `std::vector<std::filesystem::path> linkSearchPaths`.
- `LinkOptions` carries the same lists into `Linker::link`.
- Per-platform lowering inside `linker.cpp`:
  - COFF: `/LIBPATH:<dir>` and `<name>.lib` / explicit path. An import library
    makes the DLL a load-time dependency.
  - Mach-O: `-L<dir>` and `-l<name>`; explicit path as a plain input.
  - ELF (clang driver): `-L<dir>` and `-l<name>`; explicit path as a plain input.
- No change to the `joyeer-backend` C ABI. Link arguments already cross it as a
  UTF-8 argv array, so only the compiler-side adapter changes.
- Diagnostics get new stable IDs (for example `driver.link-input-invalid`,
  `driver.link-option-context`). Link failures reuse `toolFailure`.

Out of scope: runtime search-path policy (`RPATH`, install names, DLL
copying). The CLI does not make a program relocatable.

## 4. `project.toml`

Native dependencies attach to a `bin` or `test` module, since only
executables link. A `source` module cannot link, but it may need the
dependency to exist. Therefore a `bin` module declares what its closure needs.

```toml
schema-version = 1

[module]
name = "example.app"
kind = "bin"
output = "example"
sources = ["src/main.joyeer"]
dependencies = ["example.core"]

[native]
search = ["native/lib"]          # relative to this manifest
libs = ["sqlite3"]               # logical names
inputs = ["native/glue.obj"]     # explicit files, relative to this manifest

[native.windows]
libs = ["ws2_32"]

[native.linux]
libs = ["m", "pthread"]
```

Rules:

- `[native]` is allowed only when `kind` is `bin` or `test`. On `source` it is
  rejected with an explicit diagnostic. This follows the existing "source
  modules have no artifact" rule.
- Paths are relative to the manifest directory, must stay inside it unless
  `--allow-external-native` style opt-in is later designed (initially:
  contained paths only, consistent with joypm's path containment checks).
  Absolute paths are rejected. This keeps projects relocatable.
- Platform tables (`windows`, `linux`, `macos`) are additive to `[native]`,
  not replacements. Unknown platform keys are errors. Arch-specific tables are
  deferred.
- Names use the same character restriction as `--link-lib`.
- The schema version stays 1 during unreleased development, as is current
  practice. Unknown keys under `[native]` are rejected, consistent with the
  existing strict manifest parsing.
- Workspace dependencies: only the selected `bin`/`test` module's `[native]`
  is used. Native requirements do not propagate through source dependencies in
  this version; the app declares everything its closure needs. Propagation is
  a deliberate later extension, to avoid hidden link inputs.

joypm lowering (`planner.joyeer`): after the existing `--module-name` /
`--module-source` arguments, append `--link-path`, `--link-lib`, and
`--link-input` arguments in manifest order, with the active platform's table
appended after the common one. `joypm` does not resolve or check that files
exist beyond path containment; the compiler diagnoses missing inputs.

Out of scope for joypm: building C code, downloading libraries, copying DLLs
next to the output (a candidate follow-up: an opt-in `deploy` list).

## 5. Test plan

- GoogleTests: argument parsing (valid, repeated, invalid name, wrong output
  mode, missing file), and per-platform linker argument construction.
- Native fixture: reuse `CInteropNativeShared` / `CInteropNativeStatic`,
  passing them via `--link-input` instead of CMake `target_link_libraries`,
  asserting identical results.
- joypm gate: a project with a `[native]` table builds and runs; a `source`
  module with `[native]` fails with the expected diagnostic; an escaping path
  fails.
- Docs to update on acceptance: spec §19.7 (state that build-level link inputs
  exist), [impl/c-interop.md](../impl/c-interop.md) §3,
  [impl/joypm.md](../impl/joypm.md) schema, `supported-features.md`.

## 6. Open questions for review

1. Is Option A (link inputs without widening `@c_extern` access) the right
   first step?
2. Is `--link-lib/--link-path/--link-input` the preferred spelling versus
   `-l/-L`? The long form avoids colliding with the existing `-O`/`-g`
   prefix parsing and reads clearly in AI-generated command lines.
3. Should native settings be per platform table (above) or per target triple?
4. Should native requirements propagate through source dependencies, or stay
   declared only on the executable?
5. Should joypm also copy shared libraries beside the output, or leave
   deployment to the user?
