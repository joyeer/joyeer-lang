# Joyeer

Joyeer is an **AI-era systems programming language** with a Swift-inspired syntax — built for a world where AI writes most of the code and humans review and assist. Its long-term goal is to **replace C++ for new code**.

> ⚠️ **Status: early development.** The language and toolchain are evolving rapidly and not released yet.

## Why Joyeer

- **AI-first, strongly typed** — explicit types and strong guarantees act as guardrails for AI-generated code.
- **No garbage collector** — value semantics, stack allocation, and RAII by default; heap allocations must be justified.
- **Zero-cost abstractions** — you don't pay at runtime for what you don't use.
- **Swift-like syntax** — familiar, readable, and concise.
- **`struct`-first** — aggregates are value types; `class` is legacy and being phased out.

The design philosophy and trade-offs are documented under [docs/rationale/](docs/rationale/) — start with [docs/rationale/ai-era-design.md](docs/rationale/ai-era-design.md) and [docs/rationale/memory.md](docs/rationale/memory.md). The normative language definition lives in [docs/spec.md](docs/spec.md).

## Project status

The "no GC / zero-cost / native" points above describe the **design direction**.
The current implementation is a **C++20 compiler**. Default/legacy mode
executes on a custom stack-based VM. The new `--lang=v0.1` lane has an isolated
lexer/parser, name resolution, type checking, control-flow semantic analysis,
verified Joyeer IR, textual LLVM IR, a minimal C runtime, and Clang-based
native linking. It compiles and runs the JSON-parser MVP language surface
without starting the VM.

The native lane is still experimental. The current heap-backed values have
recursive clone/destroy support, deterministic scope and early-return cleanup,
and a native-entry allocation-balance check. Source-level consuming ownership,
an explicit optimization policy, debug information, and a complete standard
library are not yet complete, so this is not a production release.

Working in the native MVP: integers, booleans, bytes and strings; `let`/`var`;
checked arithmetic, comparisons and `&&`; `if`/`else`; `while`; typed functions
and `inout`; structs; payload enums; exhaustive `match`; arrays, dictionaries,
`Optional`, `Result`, byte indexing, `print(value:)`, all-paths-return,
`readFile(path:)`, mutating `&array.append(element:)`, unreachable-code
warnings, definite initialization, and unused-binding warnings.

See [docs/plan/roadmap.md](docs/plan/roadmap.md) for the full pipeline and current stage, and [docs/plan/v0.1.md](docs/plan/v0.1.md) for the v0.1 goal (a zero-overhead JSON parser written in Joyeer).

## A taste of Joyeer

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

- macOS or Windows
- CMake ≥ 3.16
- Clang ≥ 13, or MSVC (Visual Studio 2022/2026)
- Ninja ≥ 1.11
- Python ≥ 3.10 (for the test runner)
- Clang for `--emit-llvm` validation and native `-o` output

## Getting Started

### Building

The build is **out-of-source only**:

```shell
cmake -B ./build -G Ninja
cmake --build ./build
```

The `joyeer` executable is written to `build/bin/joyeer`.

> On Windows, run the commands from a Visual Studio Developer prompt (or after loading the MSVC environment) so that the compiler and `ninja` are on `PATH`.

### Compiling and running a native program

```shell
./build/bin/joyeer --lang=v0.1 -o ./hello path/to/program.joyeer
./hello
```

Emit textual LLVM IR instead:

```shell
./build/bin/joyeer --lang=v0.1 --emit-llvm ./hello.ll path/to/program.joyeer
```

Without an output option, `--lang=v0.1` validates and lowers the source without
writing an artifact. The default mode remains the legacy VM for compatibility.

### Testing

```shell
ctest --test-dir ./build --output-on-failure
```

Focused frontend validation avoids the legacy VM/runtime:

```shell
ctest --test-dir ./build --output-on-failure -L lexer
ctest --test-dir ./build --output-on-failure -L parser
ctest --test-dir ./build --output-on-failure -L llvm-backend
ctest --test-dir ./build --output-on-failure -L native
```

Tests are golden-output: [tests/testRunner.py](tests/testRunner.py) runs the compiled `joyeer` on `tests/**/*.joyeer` and diffs stdout against the sibling `*.result.txt`. After adding or removing `*.joyeer` test files, re-run `cmake -B ./build -G Ninja` so the test list is regenerated.

## Project Layout

| Path | Contents |
|---|---|
| [include/joyeer/](include/joyeer/) | Public headers: frontend, Joyeer IR, LLVM backend, native/legacy runtimes |
| [lib/](lib/) | Implementation: `compiler/`, `ir/`, `backend/`, `native/`, legacy `runtime/` and `vm/` |
| [docs/](docs/) | Language spec, design rationale, and plans (index: [docs/README.md](docs/README.md)) |
| [tests/](tests/) | Golden end-to-end tests: `basis/`, `errors/`, `leetcode/`, `target/` |
| [unittests/](unittests/) | C++ unit tests (GoogleTest) |

## Documentation

- [docs/README.md](docs/README.md) — documentation index
- [docs/spec.md](docs/spec.md) — normative language specification
- [docs/rationale/](docs/rationale/) — why Joyeer is the way it is
- [docs/plan/roadmap.md](docs/plan/roadmap.md) — implementation roadmap
- [docs/plan/v0.1.md](docs/plan/v0.1.md) — v0.1 scope and status

## Examples

- [Quick Sort in Joyeer](tests/leetcode/quick_sort.joyeer)
- [More language examples](tests/)

## Contributing

Build, test, and contribution conventions are described in [AGENTS.md](AGENTS.md).

## License

Licensed under the [Apache License 2.0](LICENSE).
