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
│ 5. Semantic Analysis│  Deep checks (unused vars, reachability, let immutability)
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
│ 8. LLVM Passes    │  LLVM optimization passes (automatic)
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
| 3. Name Resolution | ⚠️ Partial | `symtable.cpp` |
| 4. Type Checking | ⚠️ Partial | `typebinding.cpp` + `typegen.cpp` |
| 5. Semantic Analysis | ❌ Missing | — |
| 6. Own IR | ❌ Missing | Currently AST → bytecode directly |
| 7. LLVM IR Gen | ❌ Missing | Currently generates custom bytecode |
| 8-9. LLVM Optimization + CodeGen | ❌ Missing | Depends on LLVM integration |
| 10. Linker | ❌ Missing | Currently uses VM execution |
| Runtime Library | ⚠️ Partial | `sys.cpp` (VM-level built-ins) |
| Error Diagnostics | ⚠️ Partial | `diagnostic.cpp` |

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

## Immediate Next Milestone: Resolve and Type the Parser MVP AST

The syntax-only [Parser MVP contract](../impl/parser.md) is implemented. The
next frontend work is to consume that stable AST without reviving the legacy
AST's coupling to runtime descriptors:

1. Add name-resolution tables keyed by syntax node IDs.
2. Resolve functions, fields, synthesized struct initializers, enum cases, and
  contextual `.Case` expressions.
3. Type optional/built-in generic uses and minimal match payload bindings.
4. Add enum layout and match-exhaustiveness checks before lowering.
5. Keep `--lang=v0.1` out of the old `TypeGen`/VM until a new semantic path is
  ready end to end.

The legacy parser remains isolated for old tests. Parser MVP completion means
the JSON-parser source has a stable syntax tree; it does not imply enum layout,
exhaustiveness, ownership, or code generation are implemented.

---

## Phase 1: Minimal Native Compilation

**Goal**: Compile `print(42)` to a native executable.

**Language scope**: Int, Bool, functions, if/else, while, print.

### Step 1.1 — Strengthen Name Resolution + Type Checking

- Ensure all identifiers are properly bound to declarations
- Full type inference for local variables
- Type mismatch error reporting with source locations

### Step 1.2 — LLVM Integration (Build System)

- Add LLVM as a dependency in CMake
- Verify LLVM headers and libraries link correctly
- Create a new `LLVMCodeGen` module

### Step 1.3 — AST → LLVM IR (Core Types)

- Map `Int` → `i64`, `Bool` → `i1`
- Local variables → `alloca` + `load`/`store`
- Arithmetic → `CreateAdd`, `CreateSub`, `CreateMul`, `CreateSDiv`, `CreateSRem`
- Comparisons → `CreateICmpSGT`, `CreateICmpSLT`, `CreateICmpEQ`, etc.

### Step 1.4 — AST → LLVM IR (Control Flow)

- `if/else` → `CreateCondBr` + BasicBlocks
- `while` → loop BasicBlocks + `CreateBr`

### Step 1.5 — AST → LLVM IR (Functions)

- Function definitions → `Function::Create`
- Function calls → `CreateCall`
- Return → `CreateRet`

### Step 1.6 — Minimal C Runtime

```
runtime/
├── joyeer_rt.h
├── print.c         // joyeer_print_int(), joyeer_print_bool()
└── panic.c         // joyeer_panic() for runtime errors
```

### Step 1.7 — Link and Output Executable

- LLVM → `.o` object file
- Link with runtime library → executable
- Verify: `./hello` outputs `42`

---

## Phase 2: Language Completeness

**Goal**: A usable minimal language with strings, arrays, and proper safety checks.

### Step 2.1 — Semantic Analysis Pass

- `let` immutability enforcement (assign to `let` → compile error)
- Return value checking (all paths must return in non-void functions)
- Unreachable code warnings
- Uninitialized variable detection
- Unused variable warnings

### Step 2.2 — String Support

- Runtime: `joyeer_string_create()`, `joyeer_string_concat()`, `joyeer_string_print()`
- String representation: pointer + length (no null-terminated)
- String literals compiled as global constants

### Step 2.3 — Array Support

- Runtime: `joyeer_array_create()`, `joyeer_array_get()`, `joyeer_array_set()`, `joyeer_array_size()`
- Bounds checking in debug mode
- Stack-allocated fixed-size arrays (optimization)

### Step 2.4 — Error Diagnostics

- Source location tracking (line, column) in all error messages
- Error recovery in parser (report multiple errors per compilation)
- Suggested fixes in error messages

```
error: type mismatch
  --> main.joyeer:5:12
   |
 5 |   var x: Int = "hello"
   |                ^^^^^^^ expected Int, found String
```

### Step 2.5 — Syntax Conformance and Legacy Removal

- Keep mandatory argument labels: `add(left: 1, right: 2)`; reject positional
  function calls after the legacy lane is removed.
- Migrate the legacy `print(message: x)` spelling to the canonical
  `print(value: x)` label.
- Remove broad legacy token categories once the v0.1 parser consumes explicit
  terminal kinds exclusively.

---

## Phase 3: Maturity

**Goal**: Production-ready language with objects, memory management, and tooling.

### Step 3.1 — Design Own IR (Joyeer IR)

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

- Implement chosen strategy (see `memory.md`)
- Recommended: value semantics by default + compile-time ARC for heap objects
- C runtime functions: `joyeer_alloc()`, `joyeer_retain()`, `joyeer_release()`

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
| Name Resolution | ✅ | ✅ | ✅ | Phase 1 |
| Type Checking | ✅ | ✅ | ✅ | Phase 1 |
| Semantic Analysis | ✅ | ✅ | ✅ | Phase 2 |
| Own IR | MIR | SIL | AIR | Phase 3 |
| LLVM IR Gen | ✅ | ✅ | ❌ (own backend) | Phase 1 |
| Runtime Library | ✅ (C) | ✅ (C++) | ✅ (C) | Phase 1-2 |
| Standard Library | ✅ (Rust) | ✅ (Swift) | ✅ (Zig) | Phase 3 |
| Package Manager | cargo | SPM | zig build | Phase 3 |
| Error Diagnostics | Excellent | Good | Good | Phase 2 |
| Incremental Compilation | ✅ | ✅ | ✅ | Phase 3 |
| Debug Info (DWARF) | ✅ | ✅ | ✅ | Phase 3 |
