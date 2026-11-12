# 0004: Native Link Inputs

- Status: proposed (updated 2026-11-12); explicit library files are implemented, the rest is proposed
- Decisions: none yet
- Spec: [§19.7](../../specs/19-c-interop.md)

## Goal

Link native libraries by logical name and search path, per platform, from the compiler command line
and from `project.toml`.

## Definition of done

- `--link-lib` and `--link-path` (or the spelling chosen) work for executable output on Windows, Linux
  and macOS, with stable diagnostics.
- `project.toml` declares names, search paths and platform tables, and fern lowers them to the compiler
  options.
- §19.7, [impl/c-interop](../impl/c-interop.md), [impl/fern](../impl/fern.md) and
  [supported features](../impl/supported-features.md) are updated.

## Current state

Explicit library files are done: compiler `--link-library <file>` ([impl/c-interop](../impl/c-interop.md))
and the manifest fields `native-bindings` / `native-libraries` ([impl/fern](../impl/fern.md)).

Only explicit file paths can be linked. There is no way to link a library by logical name (for example
`ws2_32`, `m`, `pthread`, or a system `sqlite3`), no search directories, and no per-platform inputs.
`@c_extern(symbol:)` keeps naming no library.

## Proposed design

### Compiler CLI

Repeatable, accepted only for `-o` executable output; rejected with `--emit-llvm`,
`--emit-runtime-object`, and validate-only mode.

| Option | Meaning |
|---|---|
| `--link-lib <name>` | Link by logical name (`foo` -> `foo.lib` / `libfoo.so` / `libfoo.dylib`) |
| `--link-path <dir>` | Add a search directory for `--link-lib`; order preserved |

- Names are restricted to `[A-Za-z0-9_.+-]`: no separators, no `-l` style flags, no linker pass-through.
- Paths are UTF-8 decoded and absolutized like `--link-library`.
- Order: program object, user inputs (existing `--link-library` and new ones, in supplied order),
  runtime archive, platform libraries.
- A missing library or unresolved symbol is a link failure; no stub or fallback.
- Per-platform lowering in `linker.cpp`: COFF `/LIBPATH:` and `<name>.lib`; Mach-O and ELF `-L` and `-l`.
- No change to the `joyeer-backend` C ABI. New stable diagnostic IDs for invalid names and wrong output
  mode.

### `project.toml`

```toml
native-libraries = ["native/library.a"]   # exists
native-search = ["native/lib"]            # relative, no parent traversal
native-libs = ["sqlite3"]                 # logical names

[native.windows]
libs = ["ws2_32"]

[native.linux]
libs = ["m", "pthread"]
```

- Paths follow the existing `native-libraries` rules (relative, contained).
- Platform tables (`windows`, `linux`, `macos`) are additive; unknown keys are errors.
- Applies only to modules that produce an executable (`bin`, `test`); rejected on `source`. Native
  requirements do not propagate through dependencies; the executable declares everything its closure
  needs.
- Fern lowers these to `--link-path` / `--link-lib` in manifest order, common table first, then the
  active platform. The compiler diagnoses missing inputs.

## Todo

### Decide

- [ ] Settle the open questions below in a decision record.

### Compiler

- [ ] Unit tests: argument parsing (valid, repeated, bad name, wrong output mode) and per-platform
  linker arguments.
- [ ] Parse `--link-lib` and `--link-path` with the name and path rules and diagnostics above.
- [ ] Per-platform lowering in `linker.cpp`.
- [ ] Native fixture: the existing `CInteropNative*` fixtures linked by name via `--link-path` /
  `--link-lib`, with the same results as today.

### Fern

- [ ] Check how the design fits the current `native-libraries` behavior before changing it.
- [ ] Parse `native-search`, `native-libs` and the platform tables.
- [ ] Lower them to compiler options in manifest order.
- [ ] Tests: a project with platform tables builds and runs; a `source` module with native fields fails;
  an escaping path fails.

### Documentation

- [ ] Update §19.7, [impl/c-interop](../impl/c-interop.md), [impl/fern](../impl/fern.md) and
  [supported features](../impl/supported-features.md).

## Open questions

1. Is `--link-lib` / `--link-path` the right spelling versus `-l` / `-L`?
2. Per-platform tables, or per target triple?
3. Should native requirements ever propagate through source dependencies?
4. Should fern copy shared libraries beside the output (opt-in `deploy` list)?

## Out of scope

RPATH, install names, DLL copying and relocatability; architecture-specific tables; building C code or
downloading libraries.

## Progress log

- 2026-11-12: converted to the plan template; no items changed.
