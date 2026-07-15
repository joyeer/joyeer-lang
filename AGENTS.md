# AGENTS.md — Joyeer Language

> Joyeer is an **AI-era systems language**: AI writes most code, humans review and assist.
> Goal: replace C++ for new code. **No GC. Zero-cost abstractions. Swift-like syntax.**
> Initially **no `class`** — use `struct` for aggregates. See [docs/rationale/ai-era-design.md](docs/rationale/ai-era-design.md) and [docs/rationale/memory.md](docs/rationale/memory.md) for the design philosophy, and [docs/spec.md](docs/spec.md) for the normative language spec. The full doc index lives in [docs/README.md](docs/README.md).

## Repository reality vs. vision

The compiler is currently a **C++20 implementation**. Default/legacy mode
lexes/parses Joyeer source and runs it on a custom **stack-based VM** with its
own bytecode. `--lang=v0.1` uses the new lexer, syntax-only Parser MVP,
independent name-resolution semantic model, canonical compile-time type model,
control-flow semantic analysis, verified backend-neutral Joyeer IR, textual
LLVM IR, and Clang-based native codegen/linking. It does not construct the
legacy VM. The native lane is an
MVP with recursive value clone/destroy, deterministic scope cleanup, and
control-flow semantic analysis. Source-level consuming ownership, optimization
work beyond the current Clang `-O0`…`-O3` policy, debug info, and broader
standard-library work remain.

- Current state and pipeline: [docs/plan/roadmap.md](docs/plan/roadmap.md)
- v0.1 target (minimal language able to write a JSON parser): [docs/plan/v0.1.md](docs/plan/v0.1.md)
- Grammar: [docs/spec.md](docs/spec.md) §1 & §17
- Bytecode opcodes: [docs/impl/bytecode.md](docs/impl/bytecode.md)
- Native backend: [docs/impl/native.md](docs/impl/native.md)

> The repository contains a stray `Cargo.lock` from an abandoned Rust experiment. **Ignore it.** Do not propose Rust files or `cargo` commands — the build is CMake + C++.

## Build & test (Windows / macOS)

```pwsh
cmake -B ./build -G Ninja
cmake --build ./build
ctest --test-dir ./build --output-on-failure
```

- Requires: CMake ≥ 3.16, a C++20 compiler (clang ≥ 13 or MSVC), Ninja,
  Python ≥ 3.10. Native output also requires a Clang driver; on Windows the
  official LLVM package is supported.
- The `joyeer` executable is written to `build/bin/joyeer` (or under a config dir on multi-config generators).
- Tests are golden-output: [tests/testRunner.py](tests/testRunner.py) runs the compiled `joyeer` on `tests/**/*.joyeer` and diffs stdout against the sibling `*.result.txt`. To add a test, drop both files in [tests/basis/](tests/basis/) (or `leetcode/`, `errors/`) and re-run CMake configure so they're picked up by [tests/CMakeLists.txt](tests/CMakeLists.txt).

## Source layout

| Area | Headers | Sources |
|---|---|---|
| Driver / `main` | [lib/main/driver.h](lib/main/driver.h) | [lib/main/main.cpp](lib/main/main.cpp), [lib/main/driver.cpp](lib/main/driver.cpp) |
| Lexer / parser | [include/joyeer/compiler/lexparser.h](include/joyeer/compiler/lexparser.h), [include/joyeer/compiler/syntaxparser.h](include/joyeer/compiler/syntaxparser.h) | [lib/compiler/lexparser.cpp](lib/compiler/lexparser.cpp), [lib/compiler/syntaxparser.cpp](lib/compiler/syntaxparser.cpp) |
| v0.1 Parser MVP / syntax AST | [include/joyeer/compiler/parser.h](include/joyeer/compiler/parser.h), [include/joyeer/compiler/syntax.h](include/joyeer/compiler/syntax.h) | [lib/compiler/parser.cpp](lib/compiler/parser.cpp), [lib/compiler/syntax.cpp](lib/compiler/syntax.cpp) |
| v0.1 name resolution / semantic model | [include/joyeer/compiler/nameresolution.h](include/joyeer/compiler/nameresolution.h), [include/joyeer/compiler/semantic.h](include/joyeer/compiler/semantic.h) | [lib/compiler/nameresolution.cpp](lib/compiler/nameresolution.cpp), [lib/compiler/semantic.cpp](lib/compiler/semantic.cpp) |
| v0.1 type checking / typed model | [include/joyeer/compiler/typechecking.h](include/joyeer/compiler/typechecking.h) | [lib/compiler/typechecking.cpp](lib/compiler/typechecking.cpp) |
| v0.1 control-flow semantic analysis | [include/joyeer/compiler/semanticanalysis.h](include/joyeer/compiler/semanticanalysis.h) | [lib/compiler/semanticanalysis.cpp](lib/compiler/semanticanalysis.cpp) |
| v0.1 Joyeer IR / lowering | [include/joyeer/ir/ir.h](include/joyeer/ir/ir.h), [include/joyeer/compiler/irlowering.h](include/joyeer/compiler/irlowering.h) | [lib/ir/ir.cpp](lib/ir/ir.cpp), [lib/compiler/irlowering.cpp](lib/compiler/irlowering.cpp) |
| LLVM/native backend | [include/joyeer/backend/llvm.h](include/joyeer/backend/llvm.h), [include/joyeer/backend/linker.h](include/joyeer/backend/linker.h) | [lib/backend/llvm.cpp](lib/backend/llvm.cpp), [lib/backend/linker.cpp](lib/backend/linker.cpp) |
| Native runtime | [include/joyeer/native/runtime.h](include/joyeer/native/runtime.h) | [lib/native/runtime.c](lib/native/runtime.c), [lib/native/entry.c](lib/native/entry.c) |
| AST | [include/joyeer/compiler/node.h](include/joyeer/compiler/node.h), [include/joyeer/compiler/node+visitor.h](include/joyeer/compiler/node+visitor.h) | [lib/compiler/node.cpp](lib/compiler/node.cpp) |
| Symbol / type binding | [include/joyeer/compiler/symtable.h](include/joyeer/compiler/symtable.h), [include/joyeer/compiler/typegen.h](include/joyeer/compiler/typegen.h), [include/joyeer/compiler/typebinding.h](include/joyeer/compiler/typebinding.h), [include/joyeer/compiler/context.h](include/joyeer/compiler/context.h) | matching `*.cpp` in [lib/compiler/](lib/compiler/) |
| IR / bytecode gen | [include/joyeer/compiler/IRGen.h](include/joyeer/compiler/IRGen.h) | [lib/compiler/IRGen.cpp](lib/compiler/IRGen.cpp) |
| Runtime (types, GC, loader, sys) | [include/joyeer/runtime/](include/joyeer/runtime/) | [lib/runtime/](lib/runtime/) |
| VM interpreter | [include/joyeer/vm/](include/joyeer/vm/) | [lib/vm/interpreter.cpp](lib/vm/interpreter.cpp), [lib/vm/frame.cpp](lib/vm/frame.cpp) |
| Diagnostics | [include/joyeer/diagnostic/diagnostic.h](include/joyeer/diagnostic/diagnostic.h) | [lib/diagnostic/diagnostic.cpp](lib/diagnostic/diagnostic.cpp) |

The compiler pipeline drives stages via `CompileStage` in [include/joyeer/compiler/context.h](include/joyeer/compiler/context.h) — match its existing pattern when adding new stages.

## Conventions when writing **Joyeer source** (`*.joyeer`)

These conventions reflect the **AI-era direction**, not necessarily what every existing test does.

- Prefer **`struct`** for new aggregates. **Do not introduce new `class` declarations** — `class` exists only for legacy tests in [tests/basis/class_*.joyeer](tests/basis/) and will be removed.
- Use `let` for immutable bindings, `var` only when mutation is needed.
- Be explicit about types on public function signatures — strong types are the AI-era guardrail (see [docs/rationale/ai-era-design.md](docs/rationale/ai-era-design.md) §1).
- No exceptions, no `errno`, no nullable-by-default. Use `Optional` ([docs/spec.md](docs/spec.md) §2.4) for absence; future error handling is `Result`.
- **No GC**: assume value semantics and stack allocation by default. Heap allocations should be justified.
- Tests today still write `print(message: x)` (named arg). New tests may use whichever form the parser accepts — verify by running before committing. See `tests/basis/` for current syntax-in-use.

## Conventions when writing **compiler/runtime code** (C++)

- C++20, no exceptions in hot paths; follow the surrounding style (4-space indent, header-pair naming `foo.h` / `foo.cpp` or `foo+suffix.cpp` for partial impls).
- AST changes: update the node type in [node.h](include/joyeer/compiler/node.h), the visitor in [node+visitor.h](include/joyeer/compiler/node+visitor.h), then all three pipeline passes (`typegen`, `typebinding`, `IRGen`) — missing any of these silently breaks codegen.
- `match` remains part of the language. The lexer only recognizes `match`, `where`, `indirect`, `=>`, and ordinary pattern components; pattern structure and exhaustiveness belong to the parser and type checker. The general `performs` / `pure` effect system is removed; ownership access conventions remain.
- New bytecode opcodes: add to [include/joyeer/runtime/bytecode.h](include/joyeer/runtime/bytecode.h), implement in [lib/vm/interpreter.cpp](lib/vm/interpreter.cpp), document in [docs/impl/bytecode.md](docs/impl/bytecode.md).
- Do not add bytecode opcodes for v0.1 native work. Extend Joyeer IR, its
	verifier, LLVM lowering, and native runtime instead.
- Diagnostics: report errors via the `Diagnostics*` carried on `CompileContext` rather than `std::cerr` / exceptions.
- Add at least one end-to-end golden test in [tests/basis/](tests/basis/) for any user-visible feature change.
- New v0.1 frontend work uses the syntax AST, semantic/type models, and direct tests under
	[unittests/compiler/](unittests/compiler/) plus durable sources under
	[tests/parser/](tests/parser/), [tests/type-checking/](tests/type-checking/),
	[tests/ir-lowering/](tests/ir-lowering/), and [tests/native/](tests/native/).
	Do not route it through legacy `TypeGen`,
	bytecode, or VM merely to reuse old tests.

## Pitfalls (don't repeat these)

- The **CMake build is out-of-source only** (`CMAKE_DISABLE_IN_SOURCE_BUILD ON`). Never `cmake .` at the repo root.
- After adding/removing `*.joyeer` test files you **must re-run** `cmake -B ./build ...` because the test list is glob-expanded at configure time.
- The README still says "macOS > 12.0" — that's outdated; CMake + Ninja works on Windows too.
- Don't propose a Rust port — `Cargo.lock` is a leftover artifact (see above).

## When in doubt

- Language design questions → [docs/rationale/ai-era-design.md](docs/rationale/ai-era-design.md), [docs/rationale/memory.md](docs/rationale/memory.md), [docs/rationale/parameter-passing.md](docs/rationale/parameter-passing.md), [docs/rationale/runtime-overhead.md](docs/rationale/runtime-overhead.md)
- "Should this feature be in v0.1?" → [docs/plan/v0.1.md](docs/plan/v0.1.md) checklist
- "What stage of the pipeline owns this?" → [docs/plan/roadmap.md](docs/plan/roadmap.md) §Current Status
