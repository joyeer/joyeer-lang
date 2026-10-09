# Joyeer

Joyeer is an **AI-era systems programming language** with explicit, concise
syntax. It is designed for a workflow where AI writes most code and humans
review and assist, with the long-term goal of supporting production systems.

> **Status:** early development. The language and toolchain are not released
> and may change without compatibility guarantees.

## Design goals

- **AI-first, strongly typed:** explicit types and strong guarantees act as
  guardrails for generated code.
- **No garbage collector:** value semantics, stack allocation, and RAII are
  the default model.
- **Zero-cost abstractions:** unused features should not impose runtime cost.
- **Readable syntax:** explicit declarations and concise expressions.
- **`struct`-first:** aggregates are value types; `class` is not part of the
  implemented language surface.

The normative language definition is [docs/spec.md](docs/spec.md). Design
trade-offs are explained alongside the rules, starting with the
[design philosophy](docs/spec/00-preamble.md) and
[memory model](docs/spec/04-memory.md).

## Implementation status

The compiler is implemented in C++20 and has one pipeline:

```text
source -> lexer -> parser -> name resolution -> type checking
       -> semantic analysis -> verified Joyeer IR -> textual LLVM IR
  -> LLVM code generation + LLD + native C runtime -> executable
```

Invoking `joyeer` without an output option checks and lowers the
source. `--emit-llvm` writes text after the frontend and Joyeer IR checks;
LLVM parsing/verification runs in the native backend when `-o` builds an
executable. On Windows, LLVM and LLD run inside the bundled
`joyeer-backend.dll`; no LLVM executable is launched at runtime.

Implemented features include integers, booleans, bytes, strings, `let`/`var`,
checked arithmetic, `if`/`else`, `while`, typed functions, all four parameter access
conventions, structs, payload enums, exhaustive `match`, arrays, dictionaries,
`Optional`, `Result`, postfix `?` propagation, named compilation units with explicit
qualified imports and visibility, portable filesystem operations, synchronous
subprocesses, deterministic ownership cleanup, projection consumption,
exclusivity checking, and structured diagnostics.

Debug support includes line tables, lexical scopes, source variables, physical
types, and native PDB/DWARF/dSYM artifact handling. The standard library,
optimization policy, and broader language surface remain incomplete.

The source tree now includes the Joyeer-written `joypm` local project manager:
`init`, `check`, `build`, `run`, and `test`, with a strict single-package
manifest and explicit compiler path. The first local-workflow version,
`0.1.0`, passes native Windows ARM64 Debug and Release default `ALL` builds
and unfiltered CTest, **789/789 in each configuration**, including all 14
Windows tool gates. Bounded acquisition, exact limits, failure paths,
concurrent generations, self-build, and relocated installation are verified.
This is not a published release or native acceptance evidence for other
platforms/architectures. See
[joypm](docs/impl/joypm.md) for the exact boundary and validation evidence.

See
[docs/impl/supported-features.md](docs/impl/supported-features.md) for the
current implementation boundary.

## Example

```joyeer
func add(left: Int, right: Int): Int {
    return left + right
}

func main() {
    var sum = add(left: 3, right: 4)
    var i = 1
    while i <= 5 {
        sum = sum + i
        i = i + 1
    }
    print(value: sum)
}
```

## Getting Started

For AI-assisted Joyeer programming, the [portable Joyeer skill](skills/README.md)
includes current language guidance, CLI instructions, and runnable examples for
VS Code Copilot, Copilot CLI, Claude Code, and Codex. The Windows Debug installer
includes the skill by default; it can also be installed separately.

Contributors provide the host compiler, platform SDK, and required LLVM
toolchain. Every supported platform performs LLVM code generation and LLD
linking in-process behind a Joyeer-owned C ABI backend. CMake does not build
LLVM from source.

| Tool | Requirement |
|---|---|
| [CMake](https://cmake.org/download/) | 3.20 or newer |
| [Ninja](https://github.com/ninja-build/ninja/releases) | Required build generator |
| Host compiler | C and C++ compiler with C++20 support |
| [LLVM/LLD 22.1.8](https://github.com/llvm/llvm-project/releases/tag/llvmorg-22.1.8) | Full development libraries on every platform; Linux also uses the Clang Driver library |
| Platform SDK | Windows SDK, macOS SDK, or Linux libc development files |
| [Git](https://git-scm.com/downloads) and network access | Required when CMake first fetches LibXml2 and GoogleTest |

On Windows x64, download
[`clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz`](https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/clang%2Bllvm-22.1.8-x86_64-pc-windows-msvc.tar.xz),
extract it to a stable location such as `C:\LLVM-22.1.8`, and set `LLVM_HOME`
to that directory. The smaller `LLVM-22.1.8-win64.exe` tool-only installer is not
sufficient because building the backend DLL requires LLVM headers, `.lib`
files, and `LLVMConfig.cmake`/`LLDConfig.cmake`.

Platform requirements are:

| Platform | Required environment |
|---|---|
| Windows | [Visual Studio](https://visualstudio.microsoft.com/downloads/) with **Desktop development with C++**, the MSVC x64 toolset, and a Windows SDK |
| Linux | GCC or Clang with C++20 support, libc development files and startup objects, plus the LLVM/LLD/Clang development SDK |
| macOS | Xcode Command Line Tools, the active macOS SDK, and LLVM/LLD development libraries |

Windows builds use MSVC exclusively. Run CMake from a Visual Studio Developer
PowerShell or Developer Command Prompt configured for an **x64 target** so
`cl.exe`, the standard library, and the Windows SDK are available. Ninja and
the preset's external architecture hint do not activate a compiler toolchain;
this also matters on an ARM64 Windows host.

Verify the common tools before configuring:

```text
cmake --version
ninja --version
git --version
```

On macOS, configure, build, and test with the checked-in presets:

```text
brew install cmake ninja llvm lld
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

The compiler is written to `out/build/macos-debug/bin/joyeer`. Use
`macos-release` in the same commands for a release build.

On Linux, point `LLVM_HOME` at the matching full development SDK or pass its
root explicitly:

```text
cmake --preset linux-debug -DJOYEER_LLVM_ROOT=/opt/llvm
cmake --build --preset linux-debug
ctest --preset linux-debug
```

Use `linux-release` for a release build.

On Windows, verify the SDK root in Developer PowerShell:

```powershell
& "$env:LLVM_HOME\bin\clang.exe" --version
& "$env:LLVM_HOME\bin\llvm-readobj.exe" --version
Get-Item "$env:LLVM_HOME\lib\LLVMCore.lib"
Get-Item "$env:LLVM_HOME\lib\cmake\llvm\LLVMConfig.cmake"
```

The build is out-of-source only. Windows Debug and Release presets enable
unit tests by default. For x64 development:

```powershell
cmake --preset x64-debug -DJOYEER_BUILD_UNITTESTS=ON
cmake --build --preset x64-debug
ctest --preset x64-debug
```

For native Windows ARM64, use an ARM64 Developer shell, point `LLVM_HOME`
at the ARM64 LLVM SDK, and use `arm64-debug` or `arm64-release` instead.
See [Building Joyeer](docs/building.md) for architecture-specific prerequisites.

To build and stage a Windows release candidate:

```powershell
cmake --preset x64-release -DJOYEER_BUILD_UNITTESTS=ON -DINSTALL_GTEST=OFF
cmake --build --preset x64-release
ctest --test-dir .\out\build\x64-release --output-on-failure
cmake --install .\out\build\x64-release --prefix .\out\package\joyeer
```

Pass the SDK root explicitly if `LLVM_HOME` is unavailable:

```powershell
cmake --preset x64-debug -DJOYEER_LLVM_ROOT=C:\LLVM-22.1.8 -DJOYEER_BUILD_UNITTESTS=ON
```

The staged Windows package contains `joyeer.exe`, `joyeer-backend.dll`,
`JoyeerNativeRuntime.lib`, optional `joypm.exe`, and licenses.
`JOYEER_BUILD_JOYPM=ON` is the default: CMake builds `joypm` with the new
compiler as part of `ALL`; `OFF` selects a compiler-only build. Bootstrap
uses `-O0 -gfull` in Debug and `-O2 -g0` in Release; macOS Debug uses `-g0`
if `dsymutil` is unavailable. It requires neither a preinstalled `joypm` nor
Python. Users of that package
do not install LLVM or Clang. Creating Windows native executables
still requires MSVC Build Tools and a Windows SDK; the backend locates them
through Visual Studio Setup Configuration and the registry. `INSTALL_GTEST=OFF`
prevents test-only headers and libraries from being added to the package.
Current local installs stage only Joyeer's own license. Third-party licenses
and notices must be assembled before redistributing toolchain binaries.
Product-only inventories and relocated workflows are verified on Windows
ARM64, but those checks do not establish redistribution readiness. See
[building.md](docs/building.md#release-staging) for these packaging limitations,
platform details, and troubleshooting.

Linux and macOS use the same install layout with platform suffixes: the
`joyeer` executable, `joyeer-backend` shared library, native runtime archive,
optional `joypm`, and licenses are installed together. The compiler resolves
the backend and runtime relative to its own location; `joypm` still requires
`--compiler <path>` and does not discover its sibling or search PATH.

For a local Windows Debug installation from an existing build:

```powershell
.\scripts\install-debug.ps1
```

The script selects `out/build/arm64-debug` on ARM64 Windows or
`out/build/x64-debug` on x64 Windows, relative to this checkout rather than the
current working directory. That Debug build must already exist; use
`-BuildDir` to override the selection.

This installs the compiler, backend, runtime, their PDBs, and (when enabled)
`joypm.exe` and its PDB to
`$HOME\.joyeer\bin` and adds that directory to the user and current PowerShell
PATH without duplicating existing entries. Use `-SkipPathUpdate` to leave PATH
unchanged. The complete Joyeer skill is also installed to
`$HOME\.agents\skills\joyeer` for VS Code Copilot, Copilot CLI, and Codex.
Use `-SkillDir` to select another destination or `-SkipSkillInstall` to install
only the binaries, including joypm when enabled. Empty skill directories are
accepted, and identical files
are left unchanged. Use `-Force` after reviewing and backing up local skill
edits to overwrite differing files and restore missing ones. Extra files and
other skills are not deleted. Reload your agent session after installation. See
[Debug installation](docs/building.md#local-windows-debug-installation) for
custom destinations and verification.

## CLI

These examples use the Windows `x64-debug` build above. Linux and macOS use
`out/build/<preset>/bin/joyeer` without the `.exe` suffix. A manually configured
`-B build` directory instead places the compiler under `build/bin/`.

Validate and lower a source file:

```pwsh
.\out\build\x64-debug\bin\joyeer.exe .\tests\native\hello.joyeer
```

Alternatively, compile a named root unit with explicit root and dependency
source files:

```pwsh
.\out\build\x64-debug\bin\joyeer.exe --module-name project.app `
    .\tests\modules\root\main.joyeer ".\tests\modules\root\implementation files\helper.joyeer" `
    --module-source project.config=.\tests\modules\config\entry.joyeer `
    --module-source project.config=.\tests\modules\config\types\record.joyeer `
    -o .\out\modules.exe
.\out\modules.exe
```

The module fixture prints `42`. Repeated `--module-source project.config=...`
arguments add files to the **same** dependency unit. The complete logical name
is its identity, not a folder path or an implicit parent/submodule tree. Module
files may span nested or unrelated directories, and separate modules may use
distinct files in one directory. Every source path is relative to the process
working directory unless absolute.
Quote paths containing spaces as shown above. Explicit source files need not
have a `.joyeer` extension; there is no implicit filename filtering.

Named mode requires a valid nonempty root name and at least one positional
source. Multiple root files or dependencies require `--module-name`; a
dependency cannot reuse the root name. All supplied source sets are validated,
canonicalized, sorted, and checked for duplicate physical files before
reachable dependencies are compiled. The compiler never discovers files,
adds siblings, reads manifests, or fetches dependencies; build tools own
source selection.

The legacy `joyeer input.joyeer` form remains supported. The old `--module-root`
and `--module` directory flags are removed and report migration errors: supply
the root name and every selected file explicitly. See the
[compiler interface](docs/impl/backend.md#2-cli) for validation details and
[Modules](docs/spec/12-modules.md) for file-local imports and visibility.

Emit textual LLVM IR:

```pwsh
.\out\build\x64-debug\bin\joyeer.exe -O0 -gfull -gdwarf --emit-llvm .\out\hello.ll .\tests\native\hello.joyeer
```

Build and run a native executable:

```pwsh
.\out\build\x64-debug\bin\joyeer.exe -O2 -o .\out\hello.exe .\tests\native\hello.joyeer
.\out\hello.exe
```

Optimization defaults to `-O2`; `-O0` through `-O3` are supported. Debug
information is off by default. `-g` and `-gline-tables-only` emit line tables,
while `-gfull` also emits source variables, lexical scopes, and physical types.
Use `-gdwarf` or `-gcodeview` to select the format.

## Local projects with joypm

`joypm --version` reports `joypm 0.1.0`, an independent early-development tool
version; the compiler CMake project remains `0.0.1`. Native Windows ARM64
Debug/Release acceptance is complete for this local-workflow scope. After building,
run from this checkout with a new or empty `hello` directory and existing parent:

```powershell
$compiler = (Resolve-Path .\out\build\arm64-debug\bin\joyeer.exe).Path
$joypm = (Resolve-Path .\out\build\arm64-debug\bin\joypm.exe).Path
& $joypm init .\hello --name hello
& $joypm check --compiler $compiler --manifest-path .\hello\joyeer.toml
& $joypm build --compiler $compiler --manifest-path .\hello\joyeer.toml --release
& $joypm run --compiler $compiler --manifest-path .\hello\joyeer.toml -- "an argument"
& $joypm test --compiler $compiler --manifest-path .\hello\joyeer.toml
```

Check each command's exit status before continuing. Use the platform's matching
preset and executable suffix on Linux/macOS. `init` creates one `bin` target;
`test` reports `0 tests` until executable `test` targets are declared.

The default manifest is `joyeer.toml` in the invocation directory, with no
upward scanning. Commands that use the compiler require an explicit path;
there is no PATH lookup or shell execution. Debug uses `-O0 -gfull`, release
uses `-O2 -g0`. Selected native targets always rebuild serially into fresh
generations, with at most 128 claims per target/profile and no automatic
deletion or cache. Use only trusted local projects: compiled programs and
tests run with the user's permissions. Local path dependencies, workspaces,
registries, caching, and cleanup are deferred. A generated project under
`out/build/<preset>/src/tools/joypm/project/` supports building joypm through
joypm itself. See the
[manifest and workflow reference](docs/impl/joypm.md) and
[remaining plan](docs/plan/package-manager.md).

## Test

The required CMake acceptance gate is an unfiltered CTest run with unit tests
enabled. For the Windows development build above:

```text
cmake --build --preset x64-debug
ctest --preset x64-debug
```

Use the matching `linux-debug` or `macos-debug` preset on those platforms.
Focused labels are available when iterating:

```pwsh
ctest --preset x64-debug -L lexer
ctest --preset x64-debug -L type-checking
ctest --preset x64-debug -L native
ctest --preset x64-debug -L debug-info
```

C++ unit tests use GoogleTest. End-to-end compiler and native fixtures live in
stage-specific folders under [tests/](tests/) and are registered in
[unittests/compiler/CMakeLists.txt](unittests/compiler/CMakeLists.txt).
The joypm gates in [tests/joypm/CMakeLists.txt](tests/joypm/CMakeLists.txt),
including exact limits, concurrent builds, native failures, self-build,
installation, and Windows bootstrap regeneration,
are registered when `BUILD_TESTING` and `JOYEER_BUILD_JOYPM` are enabled,
independently of `JOYEER_BUILD_UNITTESTS`. All 14 Windows gates passed with the
full compiler/runtime suite in native ARM64 Debug and Release. Other native
platforms need their own unfiltered acceptance runs.

## Project layout

| Path | Contents |
|---|---|
| [include/joyeer/](include/joyeer/) | Public compiler, IR, backend, CLI, diagnostic, and native runtime headers |
| [lib/](lib/) | C++ compiler/backend and C11 native runtime implementations |
| [src/tools/joypm/](src/tools/joypm/) | Joyeer-written local project manager and CMake bootstrap |
| [unittests/](unittests/) | GoogleTest unit tests and CMake integration-test registration |
| [tests/](tests/) | Durable lexer/parser/semantic/native source fixtures |
| [scripts/](scripts/) | CMake integration-test helpers |
| [docs/](docs/) | Specification and design explanations, implementation notes, and plans |

## Documentation

- [docs/README.md](docs/README.md): documentation index
- [docs/spec.md](docs/spec.md): normative language specification
- [docs/spec/00-preamble.md](docs/spec/00-preamble.md): design philosophy and
  cost goals
- [docs/impl/supported-features.md](docs/impl/supported-features.md): current
  implementation boundary
- [docs/impl/backend.md](docs/impl/backend.md): LLVM backend and linking
- [docs/impl/runtime.md](docs/impl/runtime.md): process entry and C runtime
- [docs/impl/joypm.md](docs/impl/joypm.md): local project workflow, manifest,
  bootstrap, Windows x64 validation evidence, and pending acceptance gates

Build, test, and contribution conventions are in [AGENTS.md](AGENTS.md).

## License

Licensed under the [Apache License 2.0](LICENSE).