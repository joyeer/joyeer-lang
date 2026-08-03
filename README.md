# Joyeer

Joyeer is an **AI-era systems programming language** with Swift-inspired
syntax. It is designed for a workflow where AI writes most code and humans
review and assist, with the long-term goal of replacing C++ for new code.

> **Status:** early development. The language and toolchain are not released
> and may change without compatibility guarantees.

## Design goals

- **AI-first, strongly typed:** explicit types and strong guarantees act as
  guardrails for generated code.
- **No garbage collector:** value semantics, stack allocation, and RAII are
  the default model.
- **Zero-cost abstractions:** unused features should not impose runtime cost.
- **Swift-like syntax:** familiar, readable, and concise.
- **`struct`-first:** aggregates are value types; `class` is not part of the
  implemented language surface.

The normative language definition is [docs/spec.md](docs/spec.md). Design
trade-offs live under [docs/rationale/](docs/rationale/), starting with
[ai-era-design.md](docs/rationale/ai-era-design.md) and
[memory.md](docs/rationale/memory.md).

## Implementation status

The compiler is implemented in C++20 and has one pipeline:

```text
source -> lexer -> parser -> name resolution -> type checking
       -> semantic analysis -> verified Joyeer IR -> textual LLVM IR
  -> LLVM code generation + LLD + native C runtime -> executable
```

There is no bytecode backend, VM, legacy parser mode, or language-mode CLI
switch. Invoking `joyeer` without an output option validates and lowers the
source. `--emit-llvm` writes verified textual LLVM IR, and `-o` builds a native
executable. On Windows, LLVM and LLD run inside the bundled
`joyeer-native-backend.dll`; no LLVM executable is launched at runtime.

The current MVP can compile and run a Joyeer-written JSON parser. Implemented
features include integers, booleans, bytes, strings, `let`/`var`, checked
arithmetic, `if`/`else`, `while`, typed functions, all four parameter access
conventions, structs, payload enums, exhaustive `match`, arrays, dictionaries,
`Optional`, `Result`, file input, deterministic ownership cleanup, projection
consumption, exclusivity checking, and structured diagnostics.

Debug support includes line tables, lexical scopes, source variables, physical
types, and native PDB/DWARF/dSYM artifact handling. The standard library,
optimization policy, and broader language surface remain incomplete.

See [docs/plan/roadmap.md](docs/plan/roadmap.md) for the current pipeline and
[docs/plan/v0.1.md](docs/plan/v0.1.md) for the JSON-parser milestone.

## Example

```swift
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

Contributors provide the host compiler, platform SDK, and required LLVM
toolchain. Windows builds LLVM/LLD into a Joyeer-owned backend DLL; macOS and
Linux invoke an external Clang driver. CMake does not build LLVM from source.

| Tool | Requirement |
|---|---|
| [CMake](https://cmake.org/download/) | 3.20 or newer |
| [Ninja](https://github.com/ninja-build/ninja/releases) | Required build generator |
| Host compiler | C and C++ compiler with C++20 support |
| [LLVM 22.1.8](https://github.com/llvm/llvm-project/releases/tag/llvmorg-22.1.8) | Full development SDK on Windows; external Clang driver on macOS and Linux |
| Platform SDK | Windows SDK, macOS SDK, or Linux libc development files |
| [Git](https://git-scm.com/downloads) and network access | Required when CMake first fetches LibXml2 and GoogleTest |

On Windows x64, download
[`clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz`](https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/clang%2Bllvm-22.1.8-x86_64-pc-windows-msvc.tar.xz),
extract it to a stable location such as `D:\llvm`, and set `LLVM_HOME` to that
directory. The smaller `LLVM-22.1.8-win64.exe` tool-only installer is not
sufficient because building the backend DLL requires LLVM headers, `.lib`
files, and `LLVMConfig.cmake`/`LLDConfig.cmake`.

Platform requirements are:

| Platform | Required environment |
|---|---|
| Windows | [Visual Studio](https://visualstudio.microsoft.com/downloads/) with **Desktop development with C++**, the MSVC x64 toolset, and a Windows SDK |
| Linux | GCC or Clang with C++20 support, libc development files, and binutils |
| macOS | Xcode Command Line Tools and the active macOS SDK |

Windows builds use MSVC exclusively. Run CMake from a Visual Studio Developer
PowerShell or Developer Command Prompt so `cl.exe`, the standard library, and
the Windows SDK are available. Ninja schedules the build but does not replace
the host compiler.

Verify the common tools before configuring:

```text
cmake --version
ninja --version
git --version
```

On macOS, configure, build, and test with the checked-in presets:

```text
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

The compiler is written to `out/build/macos-debug/bin/joyeer`. Use
`macos-release` in the same commands for a release build.

On Windows, verify the SDK root:

```text
%LLVM_HOME%\bin\clang.exe --version
%LLVM_HOME%\bin\llvm-readobj.exe --version
%LLVM_HOME%\lib\LLVMCore.lib
%LLVM_HOME%\lib\cmake\llvm\LLVMConfig.cmake
```

The build is out-of-source only. Configure, build, test, and stage a Windows
release with:

```text
cmake --preset x64-release
cmake --build --preset x64-release
ctest --test-dir out/build/x64-release --output-on-failure
cmake --install out/build/x64-release --prefix out/package/joyeer
```

Pass the SDK root explicitly if `LLVM_HOME` is unavailable:

```text
cmake --preset x64-release -DJOYEER_LLVM_ROOT=D:/llvm
```

The staged Windows package contains `joyeer.exe`,
`joyeer-native-backend.dll`, `JoyeerNativeRuntime.lib`, and licenses. Users of
that package do not install LLVM or Clang. Creating Windows native executables
still requires MSVC Build Tools and a Windows SDK; the backend locates them
through Visual Studio Setup Configuration and the registry. See
[building.md](docs/building.md) for platform details and troubleshooting.

## CLI

Validate and lower a source file:

```pwsh
build/bin/joyeer path/to/program.joyeer
```

Emit textual LLVM IR:

```pwsh
build/bin/joyeer -O0 -gfull -gdwarf --emit-llvm output.ll path/to/program.joyeer
```

Build and run a native executable:

```pwsh
build/bin/joyeer -O2 -o hello.exe path/to/program.joyeer
./hello.exe
```

Optimization defaults to `-O2`; `-O0` through `-O3` are supported. Debug
information is off by default. `-g` and `-gline-tables-only` emit line tables,
while `-gfull` also emits source variables, lexical scopes, and physical types.
Use `-gdwarf` or `-gcodeview` to select the format.

## Test

The required CMake acceptance gate is an unfiltered CTest run:

```text
cmake --build build
ctest --test-dir build --output-on-failure
```

Focused labels are available when iterating:

```pwsh
ctest --test-dir build -L lexer --output-on-failure
ctest --test-dir build -L type-checking --output-on-failure
ctest --test-dir build -L native --output-on-failure
ctest --test-dir build -L debug-info --output-on-failure
```

C++ unit tests use GoogleTest. End-to-end compiler and native fixtures live in
stage-specific folders under [tests/](tests/) and are registered in
[unittests/compiler/CMakeLists.txt](unittests/compiler/CMakeLists.txt).

## Project layout

| Path | Contents |
|---|---|
| [include/joyeer/](include/joyeer/) | Public compiler, IR, backend, CLI, diagnostic, and native runtime headers |
| [lib/](lib/) | C++ compiler/backend and C11 native runtime implementations |
| [unittests/](unittests/) | GoogleTest unit tests and CMake integration-test registration |
| [tests/](tests/) | Durable lexer/parser/semantic/native source fixtures |
| [scripts/](scripts/) | CMake integration-test helpers |
| [docs/](docs/) | Specification, rationale, implementation notes, and plans |

Useful examples include the
[native JSON parser](tests/native/json_parser.joyeer) and the
[parser MVP fixture](tests/parser/ok/json_mvp.joyeer).

## Documentation

- [docs/README.md](docs/README.md): documentation index
- [docs/spec.md](docs/spec.md): normative language specification
- [docs/rationale/](docs/rationale/): design rationale
- [docs/plan/roadmap.md](docs/plan/roadmap.md): implementation roadmap
- [docs/impl/native.md](docs/impl/native.md): LLVM/native backend details

Build, test, and contribution conventions are in [AGENTS.md](AGENTS.md).

## License

Licensed under the [Apache License 2.0](LICENSE).