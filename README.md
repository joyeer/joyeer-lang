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
- Python 3.9 or newer to run the source-checkout bootstrap
- Network access on the first run
- Windows: Visual Studio with the **Desktop development with C++** workload and
  a Windows SDK
- macOS: Xcode command-line tools

The bootstrap obtains a pinned Pixi executable when necessary. Pixi installs
Python, CMake, Ninja, Clang/LLVM 22.1.8, LLD, inspection tools, and Linux GCC
from [pixi.lock](pixi.lock). Windows builds always use the system MSVC compiler;
there is no GCC fallback. Python is not required by released Joyeer binaries or
compiled Joyeer programs.

## Build

The build is out-of-source only. Bootstrap installs or updates the locked Pixi
environment and configures the default CMake + Ninja build:

```text
python3 bootstrap.py
python3 scripts/toolchain.py build
```

On Windows, use `py -3 bootstrap.py` and `py -3 scripts/toolchain.py build`.
Changes to [pixi.toml](pixi.toml) are resolved on the next run and recorded in
the lock file. `--offline` requires both Pixi and all locked packages to be
available locally.

The executable is written to `build/bin/joyeer` on single-config generators.
Manual CMake configurations may omit Clang; in that case, the compiler and
textual backend unit tests still build, but Clang/native integration tests are
not registered. The bootstrap path always supplies or selects Clang.

```text
python3 scripts/toolchain.py status
python3 scripts/toolchain.py build
python3 scripts/toolchain.py test
```

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

The complete acceptance command runs the Python automation tests, builds
Joyeer, and then runs unfiltered CTest:

```text
python3 scripts/toolchain.py test
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