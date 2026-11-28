# 0004: Native Link Inputs

- Status: blocked (updated 2026-11-28); implemented and exercised on Windows ARM64 and
  Linux AArch64 (WSL2). Native macOS validation is still required.
- Decisions: [0033](../decisions/0033-native-library-names.md) (pending native acceptance)
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

The compiler also accepts executable-only `--link-lib` and `--link-path`.
Fern parses `native-libs`, `native-search`, and per-OS tables.
`@c_extern(symbol:)` still names a symbol only, not a library.

## Implemented design

### Compiler CLI

Repeatable, accepted only for `-o` executable output; rejected with `--emit-llvm`,
`--emit-runtime-object`, and validate-only mode.

| Option | Meaning |
|---|---|
| `--link-lib <name>` | Link by logical name (`foo` -> `foo.lib` / `libfoo.so` / `libfoo.dylib`) |
| `--link-path <dir>` | Add a search directory for `--link-lib`; order preserved |

- Names use `[A-Za-z0-9][A-Za-z0-9_.+-]*`: no separators, leading option marker,
  or linker pass-through.
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
- The new logical-name and search fields apply only to executable modules (`bin`, `test`);
  reject them on `source`. This does not change propagation of explicit `native-libraries`
  from source modules in the selected closure.
- Fern lowers these to `--link-path` / `--link-lib` in manifest order, common table first, then the
  active platform. The compiler diagnoses missing inputs.

## Todo

### Decide

- [x] Record a proposed answer to all four design questions in
  [decision 0033](../decisions/0033-native-library-names.md).
- [ ] Accept the decision after implementation and native validation.

### Compiler

- [x] Parse and verify repeated flags, invalid names/paths, and wrong output
  modes (`ModuleArgumentsTest`).
- [x] Lower named inputs to COFF and ELF. `JoyeerCInterop.NamedLibrary`
  compiles a C provider and runs a Joyeer caller on native Windows ARM64 and
  Linux AArch64 (WSL2). The native fixtures also verify search precedence,
  failure for an absent provider, and preservation of an input archive
  when the requested output would overwrite it.
- [ ] Run the named C provider and the Fern platform-table fixture on macOS ARM64.
  Mach-O argument lowering is implemented but has not been run on a Mac.

### Fern

- [x] Check how the design fits the current `native-libraries` behavior. The
  [manifest contract](../impl/fern.md#1-canonical-organization) already allows
  native-backed source modules to supply explicit files through the selected
  dependency closure. Executable-local logical names must not remove or
  change that behavior.
- [x] Parse and lower `native-search`, `native-libs`, and the active platform table.
- [x] `Fern.Plans` builds and runs a project using platform tables, rejects logical
  link inputs on a `source` module, and rejects an escaping search path.
  Passed on Windows ARM64 and Linux AArch64 (WSL2).

### Documentation

- [x] Update §19.7, [impl/c-interop](../impl/c-interop.md), [impl/fern](../impl/fern.md) and
  [supported features](../impl/supported-features.md).

## Design choices pending acceptance

Decision 0033 proposes long option names, per-OS tables, preserving closure
propagation for explicit files only, and leaving shared-library deployment out.

## Out of scope

RPATH, install names, DLL copying and relocatability; architecture-specific tables; building C code or
downloading libraries.

## Progress log

- 2026-11-28: Verified how closure-provided explicit files interact with the proposed
  executable-local flags. Implemented the compiler and Fern support and verified it on
  Windows ARM64 and Linux AArch64 (WSL2), including full Debug/Release and the
  reviewed search-order and archive-preservation checks. Native macOS acceptance remains open.
- 2026-11-12: converted to the plan template; no items changed.
