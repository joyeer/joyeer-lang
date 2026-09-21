# AGENTS.md - Joyeer Language

> Joyeer is an AI-era systems language: AI writes most code, humans review and
> assist. Goal: replace C++ for new code. No GC. Zero-cost abstractions.
> Swift-like syntax with value semantics and deterministic destruction.

The normative language definition is [docs/spec.md](docs/spec.md). Design
rationale lives in [docs/rationale/](docs/rationale/), and the documentation
index is [docs/README.md](docs/README.md).

Keep this file focused on current development rules. Remove completed
one-off tasks and migration history instead of retaining them as instructions.

## Architecture

The compiler uses C++20, the native runtime uses C11, and the build uses
CMake + Ninja. The supported pipeline is:

```text
source -> lexer -> parser -> name resolution -> type checking
       -> semantic analysis -> verified Joyeer IR -> textual LLVM IR
  -> LLVM code generation + LLD + JoyeerNativeRuntime -> native executable
```

LLVM code generation and LLD linking run in-process behind the versioned
`joyeer-backend` C ABI. Do not expose LLVM/Clang C++ types, exceptions,
allocators, or ownership across that boundary.

## Build and test

```text
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

- Source builders install CMake 3.20+, Ninja, a C++20 host compiler, and the
  platform SDK before configuring. See [docs/building.md](docs/building.md).
- Native development on every platform requires LLVM/LLD 22.1.8 development
  libraries; Linux also requires the matching Clang Driver library. Set
  `LLVM_HOME` or `JOYEER_LLVM_ROOT` for a nonstandard SDK.
  CMake must not download or build LLVM itself.
- Windows builds require Visual Studio's MSVC compiler, a Windows SDK,
  compatible `/MT` static LLVM/LLD libraries, and the MSVC DIA SDK, all matching
  the x64 or ARM64 target. Configure from a matching Developer shell; Ninja
  does not select the toolchain. Keep separate build trees for each architecture.
- The build is out-of-source only. Never run `cmake .` at the repository root.
- Manual `-B build` configurations place the executable under `build/bin/`;
  presets use `out/build/<preset>/bin/`.
- Enable `JOYEER_BUILD_UNITTESTS` for the full acceptance gate and run
  unfiltered CTest. Label filters are for focused iteration only.
- Re-run CMake configure after adding or removing registered fixtures or test
  targets.
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
- Add focused GoogleTests and at least one durable stage/native fixture for a
  user-visible feature.
- Preserve source locations and debug scopes when adding or expanding IR
  operations.

## Packaging and automation

- Windows packages should contain only `joyeer.exe`, `joyeer-backend.dll`,
  `JoyeerNativeRuntime.lib`, and required licenses/notices, not test SDKs or
  LLVM tools/DLLs. See [docs/building.md](docs/building.md#release-staging).
- Packaged native linking needs MSVC Build Tools and a Windows SDK, but not
  an LLVM installation. Clang executables are test/inspection tools, not
  product linkers.

- Keep external tool prerequisites in [docs/building.md](docs/building.md);
  checked-in automation must not install or resolve the host compiler, platform
  SDK, CMake, Ninja, LLVM, Clang, or LLD.
- CMake may acquire pinned project dependencies such as GoogleTest and the
  static LibXml2 needed by the official Windows LLD libraries. LLVM and
  platform toolchains remain developer-managed external prerequisites.
- Write checked-in packaging and release automation in cross-platform Python
  3.9+ using the standard library when scripting is necessary.
- Do not maintain parallel `.sh` and `.ps1` implementations. Command examples
  may use the host shell, but reusable workflow logic belongs in Python.
  Exception: `scripts/install-debug.ps1` and its tests use PowerShell for
  Windows-only local Debug installation, not release packaging.
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