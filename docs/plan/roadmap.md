# Joyeer Implementation Roadmap

## Compiler pipeline

```text
Joyeer source
  -> lexer
  -> syntax parser
  -> name resolution
  -> type checking
  -> control-flow semantic analysis
  -> verified Joyeer IR
  -> textual LLVM IR
  -> Clang optimization/code generation/link
  -> native executable + JoyeerNativeRuntime
```

This is the only compiler pipeline. The old parser passes, bytecode backend,
runtime, VM, golden corpus, and language-mode CLI options were removed in July
2026.

## Current status

| Stage | Status | Main implementation |
|---|---|---|
| Lexer | JSON-parser surface complete; explicit terminals, spans, recovery | `lib/compiler/lexparser.cpp` |
| Parser | Syntax-only AST, precedence, payload enums/match, recovery | `lib/compiler/parser.cpp`, `syntax.cpp` |
| Name resolution | Stable symbols/scopes and deferred type-directed references | `lib/compiler/nameresolution.cpp`, `semantic.cpp` |
| Type checking | Canonical types, calls, access conventions, patterns, exclusivity | `lib/compiler/typechecking.cpp` |
| Semantic analysis | all-paths return, reachability, initialization, unused bindings | `lib/compiler/semanticanalysis.cpp` |
| Joyeer IR | verified CFG, aggregates, ownership operations, source/debug scopes | `lib/compiler/irlowering.cpp`, `lib/ir/ir.cpp` |
| LLVM backend | textual LLVM IR, checked operations, ownership helpers, debug metadata | `lib/backend/llvm.cpp` |
| Native linking | explicit `-O0` through `-O3`, artifact collision policy | `lib/backend/linker.cpp` |
| Native runtime | strings, arrays, dictionaries, file input, checked arithmetic, allocation balance | `lib/native/runtime.c` |
| Diagnostics | stable stage IDs, excerpts, fix-its, help, secondary notes | `lib/diagnostic/diagnostic.cpp` |

The complete JSON parser fixture builds and runs natively with recursive value
clone/destroy, deterministic scope cleanup, whole-binding and projection
consumption, and call-site exclusivity. Debug emission supports line tables and
full source variable/type/scope metadata with PDB/DWARF/dSYM artifact handling.

## Completed migration

- The typed pipeline is unconditional and default.
- CLI parsing is independent of compiler and native runtime internals.
- `LexParser` depends only on diagnostics and source data.
- Compiler tests do not link a VM or obsolete runtime.
- The legacy source directories, tests, and documentation were removed.
- Unfiltered CTest is safe and is the required final gate.

## Next milestones

1. Reconcile remaining normative memory-model questions around copying,
   deinitialization, and heap-backed value representation before expanding the
   language surface.
2. Broaden the standard library beyond the JSON-parser acceptance surface,
   with typed I/O errors and incremental/streaming APIs.
3. Define a Joyeer-specific optimization and LTO policy while preserving
   checked arithmetic, bounds, ownership, and debug semantics.
4. Expand modules, generics, error propagation, and contracts only after their
   syntax/semantic ownership is specified and tested end to end.
5. Improve debugger inspection for optimized values and aggregate projections.

## Validation

The normal acceptance sequence is:

```pwsh
cmake -S . -B build -G Ninja `
  -DJOYEER_BUILD_UNITTESTS=ON `
  -DJOYEER_CLANG_EXECUTABLE='C:/Program Files/LLVM/bin/clang.exe'
cmake --build build
ctest --test-dir build --output-on-failure
```

Use labels such as `lexer`, `parser`, `type-checking`, `ir-lowering`,
`llvm-backend`, `native-runtime`, `native`, and `debug-info` only for focused
iteration.