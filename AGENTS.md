# AGENTS.md - Joyeer Language

> Joyeer is an AI-era systems language: AI writes most code, humans review and
> assist. Goal: replace C++ for new code. No GC. Zero-cost abstractions.
> Swift-like syntax. Use `struct` for aggregates; do not add `class`.

The normative language definition is [docs/spec.md](docs/spec.md). Design
rationale lives in [docs/rationale/](docs/rationale/), and the documentation
index is [docs/README.md](docs/README.md).

## Repository reality

The compiler is a C++20 implementation with one supported pipeline:

```text
source -> lexer -> parser -> name resolution -> type checking
       -> semantic analysis -> verified Joyeer IR -> textual LLVM IR
       -> Clang + JoyeerNativeRuntime -> native executable
```

The old parser/AST passes, bytecode runtime, VM, language-mode switch, and
golden-output corpus were removed in July 2026. Do not reintroduce compatibility
paths, VM opcodes, runtime descriptors, or `--lang` modes.

The repository contains a stray `Cargo.lock` from an abandoned experiment.
Ignore it. The build is CMake + C++20 + the C11 native runtime; do not propose
Rust files or Cargo commands.

## Build and test

```text
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

- Source builders install CMake 3.20+, Ninja, a C++20 host compiler, and the
  platform SDK before configuring. See [docs/building.md](docs/building.md).
- Native development additionally requires a developer-installed Clang/LLVM
  22.1.8 toolchain. Pass a non-default driver as
  `-DJOYEER_CLANG_EXECUTABLE=/path/to/clang`.
- CMake must not download, build, package, or link LLVM/LLD. Do not add an LLVM
  superbuild, package-manager environment, Git submodule, `add_subdirectory`,
  `FetchContent`, or SDK link mode. Joyeer emits textual LLVM IR and invokes
  the external Clang driver.
- Windows builds use Visual Studio's MSVC compiler exclusively. CMake enforces
  this even when Ninja is the generator; do not add MinGW, GCC, or clang-cl
  fallbacks.
- The executable is under `build/bin/` for single-config generators.
- Unfiltered CTest is the required final gate. Label filters are for focused
  iteration only.
- C++ unit tests live under [unittests/](unittests/). Durable Joyeer sources
  live under stage-specific directories in [tests/](tests/).

## Source layout

| Area | Headers | Sources |
|---|---|---|
| CLI / driver | [include/joyeer/main/arguments.h](include/joyeer/main/arguments.h), [lib/main/driver.h](lib/main/driver.h) | [lib/main/](lib/main/) |
| Lexer / parser / syntax AST | [include/joyeer/compiler/lexparser.h](include/joyeer/compiler/lexparser.h), [include/joyeer/compiler/parser.h](include/joyeer/compiler/parser.h), [include/joyeer/compiler/syntax.h](include/joyeer/compiler/syntax.h) | matching files in [lib/compiler/](lib/compiler/) |
| Name resolution / semantic model | [include/joyeer/compiler/nameresolution.h](include/joyeer/compiler/nameresolution.h), [include/joyeer/compiler/semantic.h](include/joyeer/compiler/semantic.h) | matching files in [lib/compiler/](lib/compiler/) |
| Type / control-flow analysis | [include/joyeer/compiler/typechecking.h](include/joyeer/compiler/typechecking.h), [include/joyeer/compiler/semanticanalysis.h](include/joyeer/compiler/semanticanalysis.h) | matching files in [lib/compiler/](lib/compiler/) |
| Joyeer IR / lowering | [include/joyeer/ir/ir.h](include/joyeer/ir/ir.h), [include/joyeer/compiler/irlowering.h](include/joyeer/compiler/irlowering.h) | [lib/ir/](lib/ir/), [lib/compiler/irlowering.cpp](lib/compiler/irlowering.cpp) |
| LLVM/native backend | [include/joyeer/backend/](include/joyeer/backend/) | [lib/backend/](lib/backend/) |
| Native runtime | [include/joyeer/native/runtime.h](include/joyeer/native/runtime.h) | [lib/native/](lib/native/) |
| Diagnostics | [include/joyeer/diagnostic/diagnostic.h](include/joyeer/diagnostic/diagnostic.h) | [lib/diagnostic/diagnostic.cpp](lib/diagnostic/diagnostic.cpp) |

## Joyeer source conventions

- Prefer `let`; use `var` only for mutation.
- Use `struct` for aggregates. `class` is outside the implemented surface.
- Put explicit types on public function signatures.
- Use `Optional` for absence and `Result` for recoverable failure.
- Assume value semantics, deterministic destruction, and no GC.
- Named calls use the syntax accepted by current parser fixtures, for example
  `print(value: result)`.

## Compiler and runtime conventions

- C++20, four-space indentation, and surrounding header/source naming.
- Keep syntax, semantic, type, and IR models separate. Syntax nodes carry
  spans; semantic annotations belong in `SemanticModel`/`TypeCheckedModel`.
- User-visible frontend work normally requires changes through parser or
  semantic ownership, Joyeer IR lowering/verifier, LLVM emission, and the
  native runtime only where the feature crosses those boundaries.
- Report compiler failures through `Diagnostics` with stable IDs and
  `SourceSpan`; do not write ad hoc errors to `std::cerr`.
- Do not add VM or bytecode abstractions. Extend Joyeer IR, its verifier, LLVM
  lowering, and `JoyeerNativeRuntime`.
- Add focused GoogleTests and at least one durable stage/native fixture for a
  user-visible feature.
- Preserve source locations and debug scopes when adding or expanding IR
  operations.

## Pitfalls

- The build is out-of-source only. Never run `cmake .` at the repository root.
- Re-run CMake configure after adding or removing registered fixtures or test
  targets.
- Native output needs a configured Clang driver even when the compiler itself
  is built with MSVC or GCC.
- Do not fix historical behavior by creating a second language mode.

## Repository automation

- Keep external tool prerequisites in [docs/building.md](docs/building.md);
  checked-in automation must not install or resolve the host compiler, platform
  SDK, CMake, Ninja, LLVM, Clang, or LLD.
- CMake may acquire project test dependencies such as GoogleTest, but LLVM and
  platform toolchains remain developer-managed external prerequisites.
- Write checked-in packaging and release automation in cross-platform Python
  3.9+ using the standard library when scripting is necessary.
- Do not maintain parallel `.sh` and `.ps1` implementations. Command examples
  may use the host shell, but reusable workflow logic belongs in Python.
- Launch tools with argument arrays and `subprocess.run`; do not use
  `shell=True` or construct shell command strings.
- Python is not a source-build requirement. Released Joyeer compiler binaries
  and programs must not require Python.

## When in doubt

- Language design: [docs/spec.md](docs/spec.md) and
  [docs/rationale/](docs/rationale/)
- v0.1 scope: [docs/plan/v0.1.md](docs/plan/v0.1.md)
- Pipeline ownership: [docs/plan/roadmap.md](docs/plan/roadmap.md)
- Native ABI/debug behavior: [docs/impl/native.md](docs/impl/native.md)