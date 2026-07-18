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

- Windows or macOS
- CMake 3.16 or newer
- Ninja 1.11 or newer
- A C++20 compiler: Clang, GCC, or MSVC
- A Clang driver for LLVM validation and native `-o` output

On Windows, CodeView/PDB inspection tests also use `llvm-pdbutil` and
`llvm-readobj`; DWARF executable output requires `lld-link` beside Clang.

## Build

The build is out-of-source only:

```pwsh
cmake -S . -B build -G Ninja `
  -DJOYEER_BUILD_UNITTESTS=ON `
  -DJOYEER_CLANG_EXECUTABLE='C:/Program Files/LLVM/bin/clang.exe'
cmake --build build
```

The executable is written to `build/bin/joyeer` on single-config generators.
If Clang is not configured, the compiler and textual backend unit tests still
build, but Clang/native integration tests are not registered.

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

The complete test suite is safe and is the normal acceptance gate:

```pwsh
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