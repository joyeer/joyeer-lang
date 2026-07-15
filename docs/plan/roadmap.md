# Joyeer Language - Implementation Roadmap

## Compiler Pipeline (Full)

```
Source (.joyeer)
  │
  ▼
┌──────────────────┐
│ 1. Lexer          │  Source → Token stream
└──────────────────┘
  │
  ▼
┌──────────────────┐
│ 2. Parser         │  Token stream → AST
└──────────────────┘
  │
  ▼
┌──────────────────┐
│ 3. Name Resolution│  Bind identifiers to declarations
└──────────────────┘
  │
  ▼
┌──────────────────┐
│ 4. Type Checking  │  Type inference + type checking + error reporting
└──────────────────┘
  │
  ▼
┌──────────────────┐
│ 5. Semantic Analysis│  Returns, reachability, initialization, unused bindings
└──────────────────┘
  │
  ▼
┌──────────────────┐
│ 6. IR Lowering    │  AST → Joyeer IR (own intermediate representation)
└──────────────────┘
  │
  ▼
┌──────────────────┐
│ 7. LLVM IR Gen    │  Joyeer IR → LLVM IR
└──────────────────┘
  │
  ▼
┌──────────────────┐
│ 8. LLVM Passes    │  Explicit Clang -O0/-O1/-O2/-O3 policy (default -O2)
└──────────────────┘
  │
  ▼
┌──────────────────┐
│ 9. Code Gen       │  LLVM IR → machine code (.o)
└──────────────────┘
  │
  ▼
┌──────────────────┐
│ 10. Linker        │  .o + runtime lib → executable
└──────────────────┘
```

---

## Current Status

| Stage | Status | Source |
|-------|--------|--------|
| 1. Lexer | ✅ JSON-parser MVP; legacy profile retained | `lexparser.cpp`, [lexer contract](../impl/lexer.md) |
| 2. Parser | ✅ JSON-parser MVP; legacy parser retained | `parser.cpp`, `syntax.cpp`, [Parser MVP contract](../impl/parser.md) |
| 3. Name Resolution | ✅ Parser-MVP resolver; type-directed references handed to Stage 4 | `semantic.cpp`, `nameresolution.cpp`, [contract](../impl/name-resolution.md) |
| 4. Type Checking | ✅ JSON-parser MVP typed model | `typechecking.cpp`, [contract](../impl/type-checking.md) |
| 5. Semantic Analysis | ✅ all-paths-return, unreachable warnings, definite initialization, unused bindings; access/mutability in Stage 4 | `semanticanalysis.cpp`, [contract](../impl/semantic-analysis.md) |
| 6. Own IR | ✅ JSON-parser MVP lowering, ownership operations, cleanup, verifier | `irlowering.cpp`, `ir.cpp`, [contract](../impl/ir.md) |
| 7. LLVM IR Gen | ✅ JSON-parser MVP textual LLVM IR | `backend/llvm.cpp`, [native contract](../impl/native.md) |
| 8. LLVM Optimization | ✅ Explicit `-O0`…`-O3` policy; `-O2` default; safety traps tested | `backend/linker.cpp`, [native contract](../impl/native.md) |
| 9. Code Gen | ✅ Clang emits native objects/executables | `backend/linker.cpp` |
| 10. Linker | ✅ `-o` links the C runtime and native module | `backend/linker.cpp` |
| Runtime Library | ✅ Primitive/string/collection/file-input MVP with recursive clone/destroy and allocation-balance checks | `native/runtime.c` |
| Error Diagnostics | ✅ v0.1 structured source output; ownership/access marker help and fix-its; broader fix-it coverage pending | `diagnostic.cpp`, [contract](../impl/diagnostics.md) |

The stack VM and bytecode pipeline are now compatibility-only. New v0.1
frontend work must consume `TypeCheckedModel` and must not add dependencies on
legacy runtime descriptors or VM opcodes.

### Currently Working Features (legacy pipeline)

- Integer, Bool, String literals
- Variable declaration and assignment (`var`)
- Arithmetic operators: `+`, `-`, `*`, `/`, `%`
- Comparison operators: `>`, `>=`, `<`, `<=`, `==`, `!=`
- Logical AND: `&&`
- `if/else` conditionals
- `while` loops
- Functions with typed parameters and return types
- Named argument function calls: `add(left: 1, right: 2)`
- `print(message: x)` built-in
- Arrays, Dictionaries, Classes, Optionals

The list above describes behavior exercised by the old parser/VM, not complete
conformance with the v0.1 specification. In particular, the legacy parser has
no enum or match AST, conflates parameters with binding patterns, stops on the
first syntax failure, and delegates a two-level approximation of operator
precedence to `TypeGen`. It must not be marked complete merely because legacy
golden programs execute.

---

## Immediate Next Milestone: Ownership Syntax and Native Quality

The [Parser MVP](../impl/parser.md), [name-resolution model](../impl/name-resolution.md),
[type checker](../impl/type-checking.md), [Joyeer IR lowering](../impl/ir.md),
and [native backend](../impl/native.md) are wired into `--lang=v0.1`. Clang
compiles the complete JSON-parser MVP fixture, and `-o` builds/runs native
programs using strings, arrays, and dictionaries. A Joyeer-written parser now
reads files and handles the complete v0.1 JSON value surface natively. Explicit IR ownership,
recursive runtime clone/destroy, deterministic cleanup, allocation-balance
checks, and Stage 5 control-flow analysis are complete for that surface. The
next work is product completeness and quality:

1. Expand structured fix-it/help beyond ownership/access-marker diagnostics.
2. Add source-level debug information and define Joyeer-specific LLVM passes/
  LTO only when profiling justifies them.
3. Retire the legacy
  VM lane after native golden coverage is equivalent.

---

## Phase 1: Minimal Native Compilation

**Goal**: Compile `print(42)` to a native executable.

**Language scope**: Int, Bool, functions, if/else, while, print.

### Step 1.1 — Strengthen Name Resolution + Type Checking ✅

- Ensure all identifiers are properly bound to declarations
- Full type inference for local variables
- Type mismatch error reporting with source locations

### Step 1.2 — Joyeer IR Foundation ✅

- Typed SSA-like values and basic blocks
- Explicit load/store, calls, branches, aggregate construction, and match
- No references to legacy VM runtime descriptors

### Step 1.3 — LLVM Integration (Build System) ✅

- Discover a Clang driver in CMake
- Emit portable textual LLVM IR without linking LLVM's unstable C++ ABI
- Make Clang parse/verify every tested LLVM module

### Step 1.4 — Joyeer IR → LLVM IR (Core Types) ✅

- Map `Int` → `i64`, `Bool` → `i1`
- Local variables → `alloca` + `load`/`store`
- Arithmetic → `CreateAdd`, `CreateSub`, `CreateMul`, `CreateSDiv`, `CreateSRem`
- Comparisons → `CreateICmpSGT`, `CreateICmpSLT`, `CreateICmpEQ`, etc.

### Step 1.5 — Joyeer IR → LLVM IR (Control Flow) ✅

- `if/else` → `CreateCondBr` + BasicBlocks
- `while` → loop BasicBlocks + `CreateBr`

### Step 1.6 — Joyeer IR → LLVM IR (Functions) ✅

- Function definitions → `Function::Create`
- Function calls → `CreateCall`
- Return → `CreateRet`

### Step 1.7 — Minimal C Runtime ✅

```
runtime/
├── joyeer_rt.h
├── print.c         // joyeer_print_int(), joyeer_print_bool()
└── panic.c         // joyeer_panic() for runtime errors
```

### Step 1.8 — Link and Output Executable ✅

- LLVM → `.o` object file
- Link with runtime library → executable
- Verify: `./hello` outputs `42`

---

## Phase 2: Language Completeness

**Goal**: A usable minimal language with strings, arrays, and proper safety checks.

### Step 2.1 — Semantic Analysis Pass ✅

- ✅ `let`/field immutability enforcement (assign to immutable storage → error)
- ✅ return value checking (all paths return or yield a trailing expression)
- ✅ unreachable code warnings
- ✅ definite-initialization analysis across branches and loops
- ✅ unused local and pattern-binding warnings

### Step 2.2 — String Support

- Runtime: `joyeer_string_create()`, `joyeer_string_concat()`, `joyeer_string_print()`
- String representation: pointer + length (no null-terminated)
- String literals compiled as global constants

### Step 2.3 — Array Support ✅ (native MVP)

- ✅ typed literals, count, checked read/write subscripts, and mutable
  `&array.append(element:)`
- ✅ ownership-aware growth, clone, and reverse-order element destruction
- fixed-size stack allocation remains a future optimization

### Step 2.4 — Error Diagnostics ✅ (v0.1 rendering)

- ✅ stable stage-qualified IDs and error/warning severity
- ✅ file path, one-based line/column, source excerpt, and caret range
- ✅ parser recovery reports multiple errors per compilation
- suggested fixes/help notes remain future quality work

```
error: type mismatch
  --> main.joyeer:5:12
   |
 5 |   var x: Int = "hello"
   |                ^^^^^^^ expected Int, found String
```

### Step 2.5 — File Input ✅

- ✅ `readFile(path: String): Result<String, Int>` in the v0.1 prelude
- ✅ binary-safe owned `String` result and nonzero platform error code
- ✅ pointer/count plus tag/payload out-pointer native ABI
- ✅ success/error, ownership-balance, and native executable tests

### Step 2.6 — Syntax Conformance and Legacy Removal

- Keep mandatory argument labels: `add(left: 1, right: 2)`; reject positional
  function calls after the legacy lane is removed.
- Migrate the legacy `print(message: x)` spelling to the canonical
  `print(value: x)` label.
- Remove broad legacy token categories once the v0.1 parser consumes explicit
  terminal kinds exclusively.

### Step 2.7 — Native JSON Parser ✅

- ✅ reads external input with `readFile(path:)`
- ✅ recursively parses null, Boolean, integer, UTF-8 byte string, array, and
  object values using dynamic collections
- ✅ validates nested values and malformed-input status in a native CTest
- ✅ exits with zero runtime-managed allocations
- floating-point numbers and `\uXXXX` escapes remain outside v0.1

---

## Phase 3: Maturity

**Goal**: Production-ready language with objects, memory management, and tooling.

### Step 3.1 — Optimize and Stabilize Joyeer IR

- Intermediate representation between AST and LLVM IR
- Enables language-specific optimizations that LLVM cannot do
- Decouples frontend from backend (can swap LLVM later)
- References: Swift SIL, Rust MIR

### Step 3.2 — Struct / Enum Data Model

- Predictable value-type layout for `struct`
- Tagged-union layout and payload construction for `enum`
- `init` construction, `deinit`, and `self`
- Member field access, methods, and exhaustive `match`
- No inheritance, object identity, or vtables

### Step 3.3 — Memory Management

- Extend the implemented value-semantics ownership model (see `memory.md`)
- Keep deterministic clone/move/destroy lowering; no implicit ARC or GC
- Extend ownership lifetimes when future method/subscript `yield` syntax adds
  escaping projections; the v0.1 surface is complete

### Step 3.4 — Optional Types

- Runtime representation: tagged union or pointer + flag
- Compile-time null safety checks
- `?` optional chaining, `!` force unwrap

### Step 3.5 — Standard Library

```
std/
├── io.joyeer           // File I/O
├── math.joyeer         // Math functions
├── string.joyeer       // String methods
└── collections.joyeer  // Data structures
```

### Step 3.6 — Debug Information (DWARF)

- Emit DWARF debug info via LLVM
- Source-level debugging with gdb/lldb
- Variable inspection, breakpoints, stack traces

### Step 3.7 — Build Tool & Package Manager

- `joyeer build` — compile project
- `joyeer run` — build and run
- `joyeer test` — run tests
- `joyeer.toml` — project manifest with dependencies

### Step 3.8 — Incremental Compilation

- Module-level dependency tracking
- Only recompile changed modules
- Cache compiled artifacts

---

## Reference: How Real Languages Compare

| Stage | Rust | Swift | Zig | Joyeer (target) |
|-------|------|-------|-----|-----------------|
| Lexer | ✅ | ✅ | ✅ | ✅ |
| Parser → AST | ✅ | ✅ | ✅ | ✅ JSON-parser MVP |
| Name Resolution | ✅ | ✅ | ✅ | ✅ JSON-parser MVP |
| Type Checking | ✅ | ✅ | ✅ | ✅ JSON-parser MVP |
| Semantic Analysis | ✅ | ✅ | ✅ | ✅ JSON-parser MVP |
| Own IR | MIR | SIL | AIR | ✅ JSON-parser MVP |
| LLVM IR Gen | ✅ | ✅ | ❌ (own backend) | ✅ JSON-parser MVP |
| Runtime Library | ✅ (C) | ✅ (C++) | ✅ (C) | ✅ native MVP clone/destroy; stdlib incomplete |
| Standard Library | ✅ (Rust) | ✅ (Swift) | ✅ (Zig) | Phase 3 |
| Package Manager | cargo | SPM | zig build | Phase 3 |
| Error Diagnostics | Excellent | Good | Good | ✅ v0.1 source rendering; fix-its pending |
| Incremental Compilation | ✅ | ✅ | ✅ | Phase 3 |
| Debug Info (DWARF) | ✅ | ✅ | ✅ | Phase 3 |
