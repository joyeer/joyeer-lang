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
       -> Clang + native C runtime -> executable
```

There is no bytecode backend, VM, legacy parser mode, or language-mode CLI
switch. Invoking `joyeer` without an output option validates and lowers the
source. `--emit-llvm` writes verified textual LLVM IR, and `-o` asks Clang to
build a native executable.

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

## Requirements

- Windows, macOS, or Linux
- CMake 3.18 or newer
- Ninja
- A host C and C++ compiler with C++20 support
- Network access on the first dependency population, or a prepared offline
  dependency cache
- Windows: Visual Studio with the **Desktop development with C++** workload and
  a Windows SDK
- Linux: GCC or Clang plus libc development files and binutils
- macOS: Xcode Command Line Tools and the active macOS SDK

Windows builds use the MSVC compiler toolset installed by Visual Studio; CMake
rejects MinGW, GCC, clang-cl, and other non-MSVC compilers. Ninja remains the
build generator and does not replace the compiler. Run CMake from a Visual
Studio Developer PowerShell or Developer Command Prompt so `cl.exe`, the C/C++
headers, libraries, and Windows SDK are active. The current native pipeline
also requires Clang/LLVM 22.1.8, including `lld-link` for Windows DWARF output
and LLVM inspection tools for complete backend test coverage.
This external LLVM requirement is transitional: the planned CMake superbuild
will build a pinned LLVM/LLD SDK and link it privately into the packaged Joyeer
backend. See [building.md](docs/building.md) for dependency ownership, platform
prerequisites, and the migration plan.

## Build

The build is out-of-source only. Run CMake from an environment where the host
compiler and platform SDK are active:

```text
cmake -S . -B build -G Ninja
cmake --build build
```

Set `JOYEER_CLANG_EXECUTABLE` when Clang 22 is not discoverable through `PATH`
or the platform locations checked by CMake:

```text
cmake -S . -B build -G Ninja \
  -DJOYEER_CLANG_EXECUTABLE=/absolute/path/to/clang
```

The executable is written to `build/bin/joyeer` on single-config generators.
Configurations without Clang still build the frontend and textual backend, but
do not register Clang/native integration tests.

The existing Pixi bootstrap remains available as a transitional convenience.
It installs the currently pinned tools and configures the same CMake build:

```text
python3 bootstrap.py
python3 scripts/toolchain.py status
python3 scripts/toolchain.py build
python3 scripts/toolchain.py test
```

On Windows, use `py -3` when `python3` is unavailable. Python and Pixi are not
requirements for a direct CMake build or for released Joyeer binaries.

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

During the Pixi transition, `python3 scripts/toolchain.py test` additionally
runs the Python bootstrap tests before the build and CTest.

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
| [scripts/](scripts/) | Cross-platform Python toolchain automation and CMake test helpers |
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