# Native-only Migration Completion Record

**Decision date:** 2026-07-16

**Completion date:** 2026-07-18

**Branch:** `feature/init_version`

**Commit identity:** `Qing Xu <xuqing@decompile.io>`

## Outcome

The migration requested by the original handoff is complete. Joyeer now has
one compiler pipeline: typed frontend, verified Joyeer IR, textual LLVM IR,
Clang native code generation, and `JoyeerNativeRuntime`.

Removed:

- legacy parser and AST passes;
- bytecode generation and runtime descriptors;
- stack VM and interpreter;
- language-mode CLI flags;
- legacy golden tests and Python runner;
- obsolete runtime unit tests and bytecode documentation.

The `-gfull` work was preserved and committed before migration. It emits source
variables, lexical scopes, physical types, and `llvm.dbg.declare` metadata.

## Migration commits

| Commit | Purpose |
|---|---|
| `aa34a0a` | make the typed native pipeline unconditional/default |
| `11f4db6` | decouple frontend and CLI from legacy runtime infrastructure |
| `63e5b5f` | remove legacy compiler passes, runtime, VM, and lexer profile |
| `eb6b9df` | remove golden corpus, runner, dead tests, and `--lang` no-op |

All commits use `Qing Xu <xuqing@decompile.io>` and were pushed to
`origin/feature/init_version`.

## Current validation

Unfiltered CTest is safe and is the required final gate:

```pwsh
cmake -S . -B build -G Ninja `
  -DJOYEER_BUILD_UNITTESTS=ON `
  -DJOYEER_CLANG_EXECUTABLE='<path-to-clang>'
cmake --build build
ctest --test-dir build --output-on-failure
```

A clean GCC 16 build without compatibility include flags passed the complete
non-Clang-gated suite on Windows. Clang-dependent native/debug tests require a
toolchain containing `clang`, `lld-link`, `llvm-readobj`, and `llvm-pdbutil`.

## Continuing work

Use [roadmap.md](roadmap.md) for active milestones and
[../impl/native.md](../impl/native.md) for backend/runtime constraints. Do not
recreate a compatibility lane to implement future features.