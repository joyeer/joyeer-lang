# AGENTS.md - Joyeer Language

> Joyeer is a general-purpose programming language for the AI era.
> AI writes code; humans review it. No GC. Zero-cost abstractions.
> Built to create modern replacements for legacy software.

Value semantics and deterministic destruction are core design constraints.
Read any directory-specific `AGENTS.md` before changing that area.

## Essential commands

Install the prerequisites in [docs/building.md](docs/building.md) first.
On Windows, use an MSVC Developer shell matching the target architecture.

```text
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON -DJOYEER_BUILD_UNITTESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

- Build out of source; never run `cmake .` at the repository root. Keep
  separate build trees for each architecture.
- Reconfigure after adding or removing registered fixtures or test targets.
- Toolchains are developer-managed. Automation must not install or resolve
  host compilers, platform SDKs, CMake, Ninja, LLVM, Clang, or LLD. CMake may
  acquire pinned project dependencies under the documented
  [dependency policy](docs/building.md#dependency-policy).

## Architectural constraints

The compiler is C++20, the runtime is C11, and the build uses CMake + Ninja.

```text
source -> lexer -> parser -> name resolution -> type checking
       -> semantic analysis -> verified Joyeer IR -> LLVM IR objects
       -> LLVM code generation + LLD + JoyeerRuntime -> native executable
```

- LLVM code generation and LLD linking run in-process behind the versioned
  `joyeer-backend` C ABI. Never expose LLVM/Clang C++ types, exceptions,
  allocators, or ownership across that boundary.
- Keep `JoyeerCompilerCore` a private `OBJECT` target shared by the compiler
  and unit tests, `JoyeerBackend` shared, and `JoyeerRuntime` static. Compile
  the same IR verifier privately in the backend; do not reuse compiler-core
  objects there. LLVM and Windows `/MT` objects stay behind the backend ABI.
- Keep syntax, semantic, type, and IR models separate. Syntax nodes carry
  spans; semantic annotations belong in `SemanticModel`/`TypeCheckedModel`.
- Preserve source locations and debug scopes when changing IR operations.
- Keep `Compression` a separate archive/standard module, not a core-runtime
  dependency. Generated programs link only explicitly selected optional
  native libraries.

## Coding rules

- Use four-space indentation and surrounding C++ naming conventions. Headers
  and implementations live together under `src/`; same-directory includes use
  the filename, cross-directory includes use paths relative to `src/`.
- Report compiler failures through `Diagnostics` with stable IDs and
  `SourceSpan`; do not write ad hoc errors to `std::cerr`.
- In Joyeer, prefer `let`, use `var` for mutation, and use `struct` for
  aggregates. Put explicit types on public signatures. Use `Optional` for
  absence and `Result` for recoverable failure; use named calls such as
  `print(value: answer)`. Check the
  [implemented surface](docs/impl/supported-features.md) before assuming an API.
- Write documentation in English. Follow [.gitattributes](.gitattributes):
  text uses LF on every platform. Transfer checkouts with Git rather than
  copying platform-modified working trees.
- Write reusable installation, packaging, and release scripts in
  cross-platform Python 3.9+ with the standard library. Use `subprocess.run`
  with argument arrays, never `shell=True` or shell command strings; do not
  maintain parallel `.sh` and `.ps1` implementations. Python must not become
  a source-build or released-program requirement.
- Keep test SDKs and LLVM tools out of product packages. Required third-party
  notices must be assembled before redistribution; follow
  [release staging](docs/building.md#release-staging).

## Verification workflow

- **Documentation only:** check wording, links, examples, and consistency.
  Builds and runtime tests are unnecessary unless executable behavior changes
  or a documentation test covers the edit.
- **Implementation changes:** start with focused tests. C++ unit tests live in
  [tests/unittests/](tests/unittests/); durable Joyeer fixtures live in the
  stage-specific [tests/](tests/) directories. Label filters are for iteration,
  not full acceptance.
- **User-visible features:** follow the
  [feature workflow](docs/decisions/README.md). Record contested decisions,
  update the spec, add failing GoogleTests and a durable stage/native fixture,
  implement through every affected pipeline boundary, and run the role
  reviewers separately from the author before full acceptance.
- **Full feature/platform acceptance:** use
  `python3 scripts/validate.py --llvm-root <sdk>`, not ad-hoc scripts. It runs
  unfiltered CTest and `fern test` in Debug and Release. Keep `BUILD_TESTING`,
  `JOYEER_BUILD_UNITTESTS`, and `JOYEER_BUILD_FERN` enabled; the full root
  workspace also needs `JOYEER_BUILD_COMPRESSION`. Report unrun or blocked
  gates explicitly. See [validation details](docs/building.md#full-platform-validation).
- Use [jp_write_bytes](tests/fern/helpers.cmake) for byte-exact manifest
  fixtures, not CMake `file(WRITE)`: Windows text writes can change byte counts.
- Claim parallel test directories exclusively with process-specific names.
  Cleanup may remove only paths the current test successfully created.

## Documentation map

| Topic | Source of truth |
|---|---|
| Language rules and design rationale | [Spec](docs/spec.md), starting with the [preamble](docs/spec/00-preamble.md) |
| Current compiler support and authoring pitfalls | [Implemented surface](docs/impl/supported-features.md) |
| Toolchains, bootstrap, packaging, troubleshooting | [Building](docs/building.md) |
| Compiler layout, backend ABI, debug behavior | [Backend](docs/impl/backend.md), [IR](docs/impl/ir.md), [runtime](docs/impl/runtime.md) |
| Local project manager | [Fern](docs/impl/fern.md) |
| JVM inspection tool | [Moss README](tools/moss/README.md) and [instructions](tools/moss/AGENTS.md) |
| All documentation | [Index](docs/README.md) |

Keep this file focused on current development rules. Put implementation detail
in the linked docs; remove completed tasks and migration history rather than
accumulating them here.