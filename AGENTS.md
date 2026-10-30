# AGENTS.md - Joyeer Language

> Joyeer is an AI-era systems language: AI writes most code, humans review and
> assist. Goal: replace C++ for new code. No GC. Zero-cost abstractions.
> Swift-like syntax with value semantics and deterministic destruction.

The normative language definition is [docs/spec.md](docs/spec.md). Design
explanations live alongside the rules in its chapters; implementation details
belong in [docs/impl/](docs/impl/). The documentation index is
[docs/README.md](docs/README.md).

Keep this file focused on current development rules. Remove completed
one-off tasks and migration history instead of retaining them as instructions.

Write all documentation in English.

## Architecture

The compiler uses C++20, the runtime uses C11, and the build uses
CMake + Ninja. The supported pipeline is:

```text
source -> lexer -> parser -> name resolution -> type checking
      -> semantic analysis -> verified Joyeer IR -> LLVM IR objects
  -> LLVM code generation + LLD + JoyeerRuntime -> native executable
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

- `python3 scripts/validate.py --llvm-root <sdk>` configures, builds, and runs
  the unfiltered ctest and `joypm test` in Debug and Release, then prints one
  platform x config x suite table. Use it, not ad-hoc shell scripts, to validate
  a platform or to hand a validation to someone else. `--report FILE` saves the
  table and `--tests-list DIR` saves the sorted ctest names so two platforms can
  be diffed. It never installs a toolchain.
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
- `JOYEER_BUILD_JOYPM` defaults to `ON`: CMake bootstraps the Joyeer-written
  `joypm` as an `ALL` target using the build-tree compiler. Set it to `OFF`
  for a compiler-only build. Its acceptance gates require `BUILD_TESTING`
  and `JOYEER_BUILD_JOYPM`, independently of `JOYEER_BUILD_UNITTESTS`.
- Bootstrap uses `-O0 -gfull` in Debug and `-O2 -g0` in Release; macOS Debug
  falls back to `-g0` when `dsymutil` is unavailable. Debug PDB/dSYM sidecars
  are CMake byproducts when produced; Windows Debug installs the joypm PDB
  with component `PRODUCT`. CMake stages an explicit self-build project under
  `out/build/<preset>/src/tools/joypm/project/`.
- Re-run CMake configure after adding or removing registered fixtures or test
  targets.
- C++ unit tests live under [tests/unittests/](tests/unittests/). Durable
  Joyeer sources live under stage-specific directories in [tests/](tests/).

## Source layout

Headers and implementations live together under `src/`; they are internal
interfaces, not an installed C++ SDK. Same-directory includes use the filename;
cross-directory includes use the path relative to `src/`.

| Area | Sources |
|---|---|
| Program entry | [src/compiler/main.cpp](src/compiler/main.cpp) |
| CLI / compilation service / driver | [src/compiler/driver/](src/compiler/driver/) |
| Lexer / parser / AST / name, type, and flow analysis | [src/compiler/frontend/](src/compiler/frontend/) |
| Joyeer IR / verifier / typed AST lowering | [src/compiler/ir/](src/compiler/ir/) |
| Diagnostics | [src/compiler/diagnostics/](src/compiler/diagnostics/) |
| Compiler-side backend adapter / link orchestration | [src/compiler/codegen/](src/compiler/codegen/) |
| LLVM objects / optimization / embedded LLD / C ABI | [src/backend/](src/backend/) |
| C11 program runtime | [src/runtime/](src/runtime/) |
| Local project manager | [src/tools/joypm/](src/tools/joypm/); behavior and validation in [docs/impl/joypm.md](docs/impl/joypm.md) |

`JoyeerCompilerCore` is one non-installed CMake `OBJECT` target reused by
`joyeer` and the compiler unit tests. Directories are responsibility groups,
not separate libraries. Keep `JoyeerBackend` shared and
`JoyeerRuntime` static: LLVM and Windows `/MT` objects stay in the backend,
and generated programs link the runtime plus explicitly selected optional
native libraries. Compile the same IR verifier
source privately in the backend rather than reusing compiler-core objects.

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
  runtime only where the feature crosses those boundaries.
- Report compiler failures through `Diagnostics` with stable IDs and
  `SourceSpan`; do not write ad hoc errors to `std::cerr`.
- Add focused GoogleTests and at least one durable stage/native fixture for a
  user-visible feature.
- Preserve source locations and debug scopes when adding or expanding IR
  operations.

## Packaging and automation

- Windows packages should contain only `joyeer.exe`, `joyeer-backend.dll`,
  `JoyeerRuntime.lib`, optional `joypm.exe` (enabled by default), and
  required licenses/notices, not test SDKs or LLVM tools/DLLs. Compiler,
  backend, runtime, and the current license use install component
  `JoyeerRuntime`; `joypm` uses `PRODUCT`. The Debug installer includes
  `joypm` and its PDB when enabled. Current local installs stage only Joyeer's
  own license. Third-party license/notice assembly is deferred and must be
  completed before redistributing toolchain binaries.
  See [docs/building.md](docs/building.md#release-staging).
- Packaged native linking needs MSVC Build Tools and a Windows SDK, but not
  an LLVM installation. Clang executables are test/inspection tools, not
  product linkers.

- Keep external tool prerequisites in [docs/building.md](docs/building.md);
  checked-in automation must not install or resolve the host compiler, platform
  SDK, CMake, Ninja, LLVM, Clang, or LLD.
- CMake may acquire pinned project dependencies such as GoogleTest and the
  static LibXml2 needed by the official Windows LLD libraries, and miniz when
  `JOYEER_BUILD_COMPRESSION=ON`. The optional `Compression` library remains a
  separate archive/source package, never a core-runtime dependency. Its
  install component includes the miniz license. LLVM and
  platform toolchains remain developer-managed external prerequisites.
- Write checked-in installation, packaging, and release automation in
  cross-platform Python 3.9+ using the standard library when scripting is
  necessary.
- Do not maintain parallel `.sh` and `.ps1` implementations. Command examples
  may use the host shell, but reusable workflow logic belongs in Python.
- Launch tools with argument arrays and `subprocess.run`; do not use
  `shell=True` or construct shell command strings.
- Python is not a source-build requirement. Released Joyeer compiler binaries
  and programs must not require Python.

## Learnings

- Text files are stored and checked out with LF on every platform
  ([.gitattributes](.gitattributes)). Byte-exact fixtures and diagnostics
  snapshots break if a tool rewrites them to CRLF, so move a tree between
  machines with git, never by copying a Windows working tree to Linux.
- On Linux the in-process Clang Driver only checks that an `ld.lld` file exists
  for `-fuse-ld=lld`; put the SDK `bin` on `PATH` (or `ld.lld` in `/usr/bin`) or
  the link step fails with `invalid linker name`. `validate.py` does this for
  its child processes.
- A CMake build tree records its source path. After a checkout moves, reusing
  `out/build/<preset>` fails to regenerate; configure a new tree instead.
- Writing Joyeer library code: `result` and the other words in
  [spec §15](docs/spec/15-reserved.md) are reserved, and the compiler reports one
  used as an identifier with a misleading "JSON-parser MVP" message, so rename
  it. `&` marks assignment through `inout` storage and mutating calls only; using
  it on a plain local `var` is `invalid-access-marker`. There is no Int to Double
  conversion and no `Array.removeLast`; check
  [supported features](docs/impl/supported-features.md) before relying on an
  API.
- Use [jp_write_bytes](tests/joypm/helpers.cmake) for byte-exact manifest
  fixtures, not CMake `file(WRITE)` directly. Windows text-mode writes insert
  CR before LF, changing CRLF and inclusive byte limits; the helper verifies
  size and SHA-256 after a Joyeer binary write.
- Parallel native test directories must include process identity and be
  claimed exclusively. Cleanup must only remove directories the current
  test successfully created, never another process's collided path.

## When in doubt

- Language design and rationale: [docs/spec.md](docs/spec.md), starting with
  [the preamble](docs/spec/00-preamble.md)
- Implemented language surface:
  [docs/impl/supported-features.md](docs/impl/supported-features.md)
- Backend ABI/debug behavior: [docs/impl/backend.md](docs/impl/backend.md)
- Program startup and runtime: [docs/impl/runtime.md](docs/impl/runtime.md)