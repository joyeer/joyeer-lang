# AGENTS.md — Joyeer Language

> Joyeer is an **AI-era systems language**: AI writes most code, humans review and assist.
> Goal: replace C++ for new code. **No GC. Zero-cost abstractions. Swift-like syntax.**
> Initially **no `class`** — use `struct` for aggregates. See [docs/rationale/ai-era-design.md](docs/rationale/ai-era-design.md) and [docs/rationale/memory.md](docs/rationale/memory.md) for the design philosophy, and [docs/spec.md](docs/spec.md) for the normative language spec. The full doc index lives in [docs/README.md](docs/README.md).

## Direction: rebuilding as a Rust bootstrap compiler

> **📌 Current decision.** Joyeer is being **rebuilt from scratch in Rust**. Rust is
> the *bootstrap* implementation language: it hosts the first full compiler until the
> language is ready to self-host. The project layout mirrors the **Rust compiler
> (`rustc`) itself** — a Cargo **workspace** of small, single-responsibility
> `joyeer_*` crates.

- The **new** implementation is a Rust workspace (see **Source layout** below). It is being scaffolded incrementally, front-end first (lexer → parser → AST), with a tree-walking interpreter as the initial backend.
- The **legacy** C++20 implementation (a stack-based VM with custom bytecode) still lives in [lib/](lib/), [include/](include/), and [unittests/](unittests/). Keep it as a **reference** while porting; it will be removed once the Rust bootstrap reaches parity. Do **not** add new features to the C++ tree.
- Language design is unchanged — the spec, grammar, and rationale docs remain the source of truth:
  - Roadmap & pipeline: [docs/plan/roadmap.md](docs/plan/roadmap.md)
  - v0.1 target (minimal language able to write a JSON parser): [docs/plan/v0.1.md](docs/plan/v0.1.md)
  - Grammar: [docs/spec.md](docs/spec.md) §1 & §17, plus [docs/spec/](docs/spec/)
  - Legacy bytecode reference: [docs/impl/bytecode.md](docs/impl/bytecode.md)

## Build & test

The Rust bootstrap builds with Cargo:

```pwsh
cargo build                                    # build the whole workspace
cargo test                                     # unit + integration tests
cargo run -p joyeer -- path/to/file.joyeer     # compile & run a Joyeer program
```

- Requires: a stable **Rust toolchain** (edition 2021, `cargo`/`rustc` ≥ 1.75) and Python ≥ 3.10 for the golden-test runner.
- The `joyeer` binary is produced at `target/debug/joyeer` (`target/release/joyeer` with `--release`).
- Tests are **golden-output** and run under `cargo test`: [compiler/joyeer/tests/golden.rs](compiler/joyeer/tests/golden.rs) runs the built `joyeer` over every `tests/bootstrap/**/*.joyeer` and diffs combined stdout+stderr against the sibling `*.result.txt`. Add a case by dropping both files into [tests/bootstrap/](tests/bootstrap/) — no reconfigure step. Regenerate the expected files with `JOYEER_BLESS=1 cargo test`. (The legacy [tests/testRunner.py](tests/testRunner.py) over `tests/basis/`, `leetcode/`, `errors/` still targets the C++ binary.)

### Legacy C++ build (reference only — do not extend)

```pwsh
cmake -B ./build -G Ninja
cmake --build ./build
ctest --test-dir ./build --output-on-failure
```

Out-of-source only (`CMAKE_DISABLE_IN_SOURCE_BUILD ON`); the binary lands at `build/bin/joyeer`. After adding/removing `*.joyeer` tests the CMake configure step must be re-run because the list is glob-expanded at configure time.

## Source layout (Rust bootstrap)

> **Status:** initial vertical slice in place and green (`cargo build` +
> `cargo test`): lexer → parser → tree-walking interpreter → `joyeer` binary.
> Crates are added incrementally, mirroring `rustc`'s `compiler/rustc_*` split.

```
Cargo.toml                 # workspace manifest (workspace.package + deps)
compiler/
  joyeer_span/             # byte positions, spans, source map     (cf. rustc_span)
  joyeer_errors/           # diagnostics engine / emitter          (cf. rustc_errors)
  joyeer_lexer/            # pure tokenizer over &str, no deps      (cf. rustc_lexer)
  joyeer_ast/              # AST node definitions                  (cf. rustc_ast)
  joyeer_parse/            # recursive-descent parser -> AST       (cf. rustc_parse)
  joyeer_interp/           # tree-walking interpreter (backend)
  joyeer_driver/           # wires the pipeline together           (cf. rustc_driver)
  joyeer/                  # thin binary crate -> `joyeer`         (cf. compiler/rustc)
tests/bootstrap/           # golden `*.joyeer` / `*.result.txt` for the Rust pipeline
library/                   # (future) standard library written in Joyeer
```

- **Deferred crates** (add when they carry real content, splitting out of `joyeer_driver`): `joyeer_session` (options/config, cf. rustc_session), `joyeer_resolve` (name resolution), `joyeer_typeck` (type checking).
- Keep crates **single-responsibility** and acyclic; depend downward (`driver` → `parse` → `lexer`/`ast` → `span`). `joyeer_span` and `joyeer_errors` are the shared leaves; `joyeer_lexer` depends on nothing.
- Each pipeline stage sits behind a crate boundary so passes can be tested in isolation. The current pipeline is lex → parse → interpret; `resolve`/`typecheck` slot in before `joyeer_interp` later.

For the **legacy C++** layout (driver, lexparser, node/AST, typegen/typebinding, IRGen, runtime VM, diagnostics) see [lib/](lib/) and [include/joyeer/](include/joyeer/) — it is frozen for reference.

## Conventions when writing **Joyeer source** (`*.joyeer`)

These conventions reflect the **AI-era direction**, not necessarily what every existing test does.

- Prefer **`struct`** for new aggregates. **Do not introduce new `class` declarations** — `class` exists only for legacy tests in [tests/basis/class_*.joyeer](tests/basis/) and will be removed.
- Use `let` for immutable bindings, `var` only when mutation is needed.
- Be explicit about types on public function signatures — strong types are the AI-era guardrail (see [docs/rationale/ai-era-design.md](docs/rationale/ai-era-design.md) §1).
- No exceptions, no `errno`, no nullable-by-default. Use `Optional` ([docs/spec.md](docs/spec.md) §2.4) for absence; future error handling is `Result`.
- **No GC**: assume value semantics and stack allocation by default. Heap allocations should be justified.
- Tests today still write `print(message: x)` (named arg). New tests may use whichever form the parser accepts — verify by running before committing. See `tests/basis/` for current syntax-in-use.

## Conventions when writing **compiler code** (Rust)

- Rust **edition 2021**; keep `rustfmt` defaults and a `clippy`-clean build (`cargo clippy --all-targets`). 4-space indent, `snake_case` modules, `CamelCase` types.
- One responsibility per crate; name compiler crates `joyeer_*` to match the `rustc_*` convention, and register each in the workspace `members` list.
- **No `unwrap()` / `panic!` on user-reachable paths.** Recoverable problems flow through the `joyeer_errors` diagnostics engine and carry a `joyeer_span::Span`; reserve `panic!` / `unreachable!` for compiler invariants (internal bugs).
- AST changes: update the definitions in `joyeer_ast`, then every consumer (`joyeer_parse`, name resolution, typecheck, `joyeer_interp`). Avoid catch-all `_ =>` arms in passes so a new node surfaces as a non-exhaustive `match` error.
- Prefer borrowing and value types over `Rc`/`RefCell`; reach for interior mutability or arenas only with a reason — this mirrors the language's own no-GC ethos.
- Add at least one end-to-end golden test in [tests/basis/](tests/basis/) for any user-visible change, plus unit tests inside the owning crate.

> Touching the **legacy C++** tree? Follow its local style (C++20, 4-space indent, `foo.h` / `foo+suffix.cpp` pairs, report via the `Diagnostics*` on `CompileContext`, never `std::cerr` / exceptions) — but prefer porting the behaviour into the Rust workspace over extending C++.

## Pitfalls (don't repeat these)

- Don't add new features to the **legacy C++** tree — port them into the Rust workspace instead. The C++ code is a reference, not the build target.
- Keep crate dependencies **acyclic** (Cargo rejects cycles). If two crates want each other, the shared piece belongs in a lower crate (`joyeer_span` / `joyeer_errors`).
- Don't `unwrap()` a `Result` to silence it in a compiler pass — thread a diagnostic instead, or the error reaches users as a panic.
- The README still describes the CMake build and "macOS > 12.0" — that's the **legacy** path; the Rust workspace builds with `cargo` on Windows/macOS/Linux.
- The old advice to "ignore `Cargo.lock`" is **obsolete**: Rust is now the implementation language, so `Cargo.toml` / `Cargo.lock` are real, tracked build files.

## When in doubt

- New Rust implementation vs. legacy C++ → prefer the **Rust workspace**; the C++ tree is frozen reference, ported crate by crate.
- Language design questions → [docs/rationale/ai-era-design.md](docs/rationale/ai-era-design.md), [docs/rationale/memory.md](docs/rationale/memory.md), [docs/rationale/parameter-passing.md](docs/rationale/parameter-passing.md), [docs/rationale/runtime-overhead.md](docs/rationale/runtime-overhead.md)
- "Should this feature be in v0.1?" → [docs/plan/v0.1.md](docs/plan/v0.1.md) checklist
- "What stage of the pipeline owns this?" → [docs/plan/roadmap.md](docs/plan/roadmap.md) §Current Status
