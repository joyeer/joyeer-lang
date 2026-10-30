# Native Link Inputs: Remaining Work

> **Status:** partly implemented. Explicit library files are done:
> compiler `--link-library <file>` ([impl/c-interop](../impl/c-interop.md)) and
> the manifest fields `native-bindings` / `native-libraries`
> ([impl/joypm](../impl/joypm.md)). This plan covers only what is missing.
> Remove items when done.

## 1. Gap

Only explicit file paths can be linked. There is no way to link a library by
logical name (for example `ws2_32`, `m`, `pthread`, or a system `sqlite3`), no
search directories, and no per-platform inputs. `@c_extern(symbol:)` keeps
naming no library ([§19.7](../spec/19-c-interop.md)).

## 2. Compiler CLI (proposed)

Repeatable, accepted only for `-o` executable output; rejected with
`--emit-llvm`, `--emit-runtime-object`, and validate-only mode.

| Option | Meaning |
|---|---|
| `--link-lib <name>` | Link by logical name (`foo` -> `foo.lib` / `libfoo.so` / `libfoo.dylib`) |
| `--link-path <dir>` | Add a search directory for `--link-lib`; order preserved |

- Names are restricted to `[A-Za-z0-9_.+-]`: no separators, no `-l` style
  flags, no linker pass-through.
- Paths are UTF-8 decoded and absolutized like `--link-library`.
- Order: program object, user inputs (existing `--link-library` and new ones,
  in supplied order), runtime archive, platform libraries.
- Missing library or unresolved symbol is a link failure; no stub or fallback.
- Per-platform lowering in `linker.cpp`: COFF `/LIBPATH:` and `<name>.lib`;
  Mach-O and ELF `-L` and `-l`.
- No change to the `joyeer-backend` C ABI. New stable diagnostic IDs for
  invalid names and wrong output mode.
- Out of scope: RPATH, install names, DLL copying, relocatability.

## 3. `project.toml` (proposed)

Extend the existing fields with names, search paths, and platform tables:

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
- Platform tables (`windows`, `linux`, `macos`) are additive; unknown keys are
  errors. Architecture-specific tables are deferred.
- Applies only to modules that produce an executable (`bin`, `test`); reject on
  `source`. Native requirements do not propagate through dependencies; the
  executable declares everything its closure needs. Check how this fits the
  current `native-libraries` behavior before changing it.
- joypm lowers these to `--link-path` / `--link-lib` in manifest order, common
  table first, then the active platform. The compiler diagnoses missing inputs.
- Out of scope: building C code, downloading libraries, copying DLLs.

## 4. Tests

- Unit: argument parsing (valid, repeated, bad name, wrong output mode) and
  per-platform linker arguments.
- Native fixture: the existing `CInteropNative*` fixtures linked by name via
  `--link-path` / `--link-lib`, same results as today.
- joypm: a project with platform tables builds and runs; `source` module with
  native fields fails; an escaping path fails.
- Update spec §19.7, [impl/c-interop](../impl/c-interop.md),
  [impl/joypm](../impl/joypm.md), and `supported-features.md` on acceptance.

## 5. Open Questions

1. Is `--link-lib` / `--link-path` the right spelling versus `-l` / `-L`?
2. Per-platform tables, or per target triple?
3. Should native requirements ever propagate through source dependencies?
4. Should joypm copy shared libraries beside the output (opt-in `deploy` list)?
