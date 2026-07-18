# Joyeer Language — Runtime Overhead Budget

> Goal: **the cost of every Joyeer feature is what a competent C++ programmer
> would pay to express the same intent — and not a byte more.**
>
> This document defines what the runtime contains, what zero-cost actually
> means in Joyeer, and the rules the compiler must obey to keep that promise.
> It complements [memory.md](memory.md) (ownership model) and
> [parameter-passing.md](parameter-passing.md) (call conventions).

---

## 1. The C++ Baseline We Are Beating (Or Matching)

| Concern | C++ today | Joyeer target |
|---|---|---|
| Heap allocator | optional, opt-in | optional, opt-in |
| RTTI / type-info | off by default | **never** emitted unless asked |
| Exception unwinding | tables + landing pads (often 5–15 % code-size tax) | **none** — error path is `Result<T, E>` |
| Virtual dispatch | one indirect call + cache miss per call | only when programmer writes `dyn`/`protocol` |
| GC | none | **none** |
| Hidden globals (`atexit`, ctors) | yes (`static` init order fiasco) | **forbidden** — see §4 |
| Smallest "hello world" binary (release, stripped) | 14–50 KB | ≤ 16 KB |

The line "replace C++ for new code" in [AGENTS.md](../../AGENTS.md) implies we must be
**within noise** of these numbers, not 2×, not 5×.

---

## 2. Runtime Size Budget

The runtime is split into three concentric layers. A program only pays for the
layers it uses.

```
┌──────────────────────────────────────────────────────────┐
│  std        Optional. Heap, Array, Map, File I/O,        │
│             String formatting, Time, Threads.            │
│             Pulled in only by `import std.*`.            │
├──────────────────────────────────────────────────────────┤
│  core       Always linked. Slices, Optional<T>,          │
│             Result<T,E>, panic(), memcpy/memset,         │
│             integer/float intrinsics. No heap.           │
├──────────────────────────────────────────────────────────┤
│  abi        Always linked. Entry point trampoline,       │
│             stack overflow probe, OS bootstrap.          │
│             A few hundred bytes.                         │
└──────────────────────────────────────────────────────────┘
```

| Layer | Heap? | Syscalls? | OS deps? | Target size (x86_64 release) |
|---|---|---|---|---|
| `abi` | ❌ | start/exit only | minimal | < 1 KB |
| `core` | ❌ | none | none (freestanding) | < 8 KB |
| `std`  | ✅ | yes | libc / OS | grows with use, dead-code-eliminated |

**Rule:** a Joyeer program that uses no heap and no I/O must produce a binary
within 16 KB of the equivalent freestanding C program. If it doesn't, the
runtime is broken.

### Configurable runtime profiles

```
joyeer build --profile=freestanding   # abi + core, no std, no libc
joyeer build --profile=embedded       # abi + core + std-no-alloc
joyeer build --profile=default        # abi + core + std
```

`freestanding` exists so Joyeer can target kernels, bootloaders, MCUs, WASM
without a heap, and unikernels — the same niches C and Zig already serve.

---

## 3. Zero-Cost Abstraction — What We Actually Promise

The phrase "zero-cost abstraction" is overused. Joyeer commits to these
**specific, testable** properties:

### 3.1 Feature → cost table

| Feature | Compile-time cost | Runtime cost | Same as C++? |
|---|---|---|---|
| `struct` (no methods) | layout pass | identical to C `struct` | ✅ |
| Method call on a `struct` | name resolution | direct call, inlinable | ✅ |
| Generic function `func<T>(...)` | one specialization per `T` | direct call, inlinable | ✅ (templates) |
| `enum` (tagged union) | layout + niche analysis | tag load + branch | ✅ (`std::variant` minus padding) |
| `match` on `enum` | exhaustiveness check | jump table or chained branch | ✅ (`switch`) |
| `Optional<T>` | niche analysis | **zero bytes** when niche exists (`Optional<&T>` = `T*`) | better than `std::optional` |
| `Result<T, E>` | layout | tag + branch on `if r is Err` | same as `tl::expected` |
| `inout` parameter | call conv | pointer, no copy | ✅ (`T&`) |
| `consuming` parameter | move analysis | pointer + ownership transfer | ✅ (`T&&`) |
| `borrowing` value parameter | escape analysis | by-ref if large, by-value if small | ✅ (template + `const T&`) |
| `protocol` method via `dyn P` | vtable emission | one indirect call | ✅ (virtual) |
| `protocol` method via generic | monomorphization | direct call | ✅ (concepts) |
| Bounds-checked index `a[i]` | none | compare + branch (predicted) | ❌ (C++ doesn't check by default; see §3.4) |
| Integer overflow check (debug) | none | overflow flag + branch | ❌ in release: same as C++ |
| `defer` block | scope analysis | inlined into epilogue | ✅ (RAII) |

### 3.2 The "no hidden work" rule

Every line of Joyeer source must have a **predictable mapping to machine work**.
The following are **forbidden**, regardless of how convenient they would be:

- Implicit heap allocation. `let s = "abc" + "def"` must not allocate unless
  the programmer can see the allocation (e.g. `String` builder API).
- Implicit copies of types larger than 2 machine words. Large types are
  passed/returned by reference; assignment of `var b = a` for a large `a`
  must be `move`, not `copy`, unless the type opts in to `Copy`.
- Implicit refcount increment. Joyeer has **no ARC**. Period.
- Implicit conversion that allocates (e.g. `Int → String` on `print`).
- Implicit construction of any type (no C++-style converting constructors).
- Hidden global initializers — see §4.

### 3.3 Inlining contract

The compiler **must** inline the following, in every build mode that targets
release:

1. All single-call-site functions in the same module.
2. All accessors (a function whose body is a single field load/store).
3. All generic specializations marked `@inline(always)` or trivially small (≤ 3 IR ops).
4. All `Optional<T>` and `Result<T, E>` constructors / accessors.

If LLVM declines, the compiler should warn at `--Wabstraction-cost`.

### 3.4 Bounds checks: configurable, not "always on" / "never on"

Three modes, per-build-profile:

| Mode | When | Cost |
|---|---|---|
| `safe`     | debug, tests | check every access, panic on fail |
| `release`  | default release | check only when index is not provably in range |
| `unsafe`   | `--no-bounds-checks` or `unsafe { ... }` | none |

The compiler must constant-fold bounds away for proven-in-range accesses
(loop induction variables bounded by `arr.len()`). This is the only way to
compete with hand-written C loops.

---

## 4. No Hidden Globals, No Static Init Order Fiasco

C++'s biggest portability foot-gun is unspecified order of static constructors.
Joyeer bans it structurally:

- Module-level `let` may only be initialized by a **`const` expression**
  (literals, arithmetic on literals, `const fn` calls).
- Anything needing runtime work goes into an explicit `init()` function the
  programmer chooses when to call (typically from `main`).
- No analogue of `atexit`. Use a `defer` in `main`.

Result: a Joyeer binary's `.init_array` / `.ctors` section is **empty**. Startup
latency is dominated by `mmap` + jump-to-`main`, the same as freestanding C.

---

## 5. Memory Layout Rules (the ABI Joyeer Promises)

This is the part C++ programmers will look at first when deciding whether to
port a hot data structure to Joyeer.

### 5.1 Struct layout

- Fields are laid out **in declaration order** (like C, unlike Rust by default).
- Natural alignment, with the minimum trailing padding to satisfy the struct's
  own alignment.
- No vtable pointer unless the struct opts into `dyn`-able conformance.
- `sizeof(S)` and `alignof(S)` are `const fn` and must agree with the C ABI for
  any struct that only contains `extern "C"`-compatible types.

```
struct Point { var x: Int32; var y: Int32 }
// sizeof == 8, alignof == 4, layout identical to C `struct Point`.
```

Reordering is **opt-in only**: `@layout(reorder)` allows the compiler to pack
fields to reduce padding. Default is "what you see is what you get."

### 5.2 Enum (tagged union) layout

```
enum Shape {
    Circle(radius: Float64)
    Square(side: Float64)
    Empty
}
```

- Layout = `{ tag: smallest-int-that-fits, payload: union-of-variants }`.
- Tag uses 1 byte when ≤ 256 variants.
- Total size = `align_up(sizeof(tag) + sizeof(largest_variant), max_align)`.
- **Niche optimization** is mandatory:
  - `Optional<&T>` → exactly `sizeof(&T)`; `None` is the null pointer.
  - `Optional<Bool>` → 1 byte; `None` uses bit pattern `2`.
  - `Optional<enum E>` where `E` has unused tag values → same size as `E`.

### 5.3 Slice / String layout

```
struct Slice<T> { var ptr: *T; var len: USize }   // 16 bytes on 64-bit
String == Slice<UInt8>  with UTF-8 invariant
```

Slices are values. Passing a slice is always pass-by-value of two pointer-sized
words — same cost as passing two ints. **No length prefix in the buffer**, no
null terminator.

### 5.4 Function type layout

- A plain function pointer is one word.
- A closure that captures nothing is **a plain function pointer** (must not
  silently widen to a fat pointer).
- A closure that captures is `{ fn: *(), env: *() }` (two words).
- Generic code over `Fn` is monomorphized — no fat pointer cost unless
  programmer uses `dyn Fn`.

---

## 6. Dispatch Policy

| Construct | Dispatch | Vtable? | Cost |
|---|---|---|---|
| `struct.method()`              | static     | no  | direct call |
| `extension T { ... }`          | static     | no  | direct call |
| `func<T: Protocol>(x: T)`      | static (monomorphized) | no  | direct call per spec |
| `func(x: dyn Protocol)`        | dynamic    | yes | 1 indirect call |
| `enum.variant_method()`        | static (via match) | no | direct call |

**No `override` keyword is needed because there is no implicit virtual.** If a
programmer wants dynamic dispatch they ask for it with `dyn P`. This matches
Rust and is strictly cheaper than Swift's "everything is dynamic in classes."

---

## 7. Generics: Monomorphization with Escape Hatch

- Default: **monomorphize** generic functions and types per concrete `T`.
  Identical machine code to a C++ template.
- Escape hatch: `dyn Protocol` for code-size-sensitive call sites (one copy,
  one indirect call).
- The compiler deduplicates identical monomorphizations across modules via
  LLVM `linkonce_odr`. Generic bloat is real but bounded.

Generics are checked **once at the protocol-bound site**, not per
instantiation (concepts-style, not C++-template-style). AI-generated generic
code therefore produces readable errors, not 800-line template error walls.

---

## 8. `comptime` / Const Evaluation

Following Zig's lead, but narrower in scope to keep the language small:

- `const fn` — a function whose body is restricted to pure operations on
  values known at compile time. Callable at runtime too.
- `const` expressions in array sizes, enum tag layout, generic arguments.
- `static_assert(cond, msg)` for compile-time checks.

What we explicitly **do not** chase in v0.1:

- Full type-level metaprogramming (Zig `comptime` of types).
- Arbitrary code execution at compile time.

Reason: `const fn` covers 90 % of the "do work at compile time" wins (lookup
tables, computed constants, layout validation) at 10 % of the implementation
cost.

---

## 9. C ABI Interop

Joyeer must be a **drop-in replacement for C in a mixed codebase**, otherwise
"replace C++ for new code" is wishful thinking.

- `extern "C" func foo(...)` — declare a C function.
- `@export("foo") func foo(...)` — expose a Joyeer function with no name mangling.
- `extern "C" struct S { ... }` — guarantees C layout (forbids `@layout(reorder)`).
- Joyeer's `Slice<T>` decays to `(T*, size_t)` at the boundary, not a special wrapper.
- No runtime initialization required before calling Joyeer code from C, beyond
  the `abi` layer's bootstrap (which is a no-op if Joyeer is loaded by a
  C `main`).

Joyeer must **never** require a Joyeer runtime call to enter or leave a
function. A C call into Joyeer must be one `call` instruction. This is the line
that disqualifies GC'd or ARC'd languages from this niche.

---

## 10. Error Handling Cost

Already decided in [ai-era-design.md](ai-era-design.md): no exceptions, use
`Result<T, E>`. The runtime cost commitment:

- Returning `Result<T, E>` is **never** more expensive than returning the
  larger of `T` and `E` plus a tag byte (packed into the return registers
  when possible by ABI).
- No landing pads. No unwind tables. `.eh_frame` is empty in release.
- `panic()` aborts the process. No unwinding, no catch. Programs that want
  cleanup on failure must structure recovery via `Result`.

A function that never panics and returns `Result<Int32, ErrCode>` should
compile to the same code as a C function returning `int64_t` with the high
bits encoding tag + payload — i.e. one register, no memory traffic.

---

## 11. Comparison: Runtime Footprint vs. Other Systems Languages

| Language | Min runtime | Heap required? | Unwind tables? | GC? | C ABI without glue? |
|---|---|---|---|---|---|
| C            | < 1 KB | no  | no  | no  | yes |
| C++ (no exc) | ~4 KB  | no  | no  | no  | yes (with `extern "C"`) |
| C++ (exc)    | ~20 KB | no  | **yes** | no | yes |
| Rust         | ~50 KB | no (no_std) | yes (release: small) | no | yes |
| Zig          | < 4 KB | no  | no  | no  | yes |
| Go           | ~1 MB  | yes | yes | yes | requires cgo |
| Swift        | ~5 MB  | yes | yes | ARC | requires bridge |
| **Joyeer (target)** | **< 8 KB** | **no** | **no** | **no** | **yes** |

If at any point Joyeer's `freestanding` profile exceeds 16 KB for `hello world`,
treat that as a P0 bug.

---

## 12. Open Questions

These are decisions still on the table and explicitly **not** answered here.
Track them in their own design docs as they get resolved:

- **Async / coroutines.** Stackful (large per-task overhead) vs. stackless
  (poll-based, complex). Probably stackless if at all — but maybe never,
  if `Result` + threads suffice.
- **Allocator-as-parameter.** Zig passes allocators explicitly to every
  heap-using function. Worth considering for `std`; would prevent hidden allocs.
- **SIMD / vector types.** First-class language support vs. intrinsics only.
- **Atomics and memory ordering.** Need an explicit model before any
  concurrency story.
- **Debug info size.** DWARF is huge. Consider tiered debug info
  (line-only / inlined-frames / full).

---

## 13. Cross-References

- [memory.md](memory.md) — ownership model (value semantics, regions, no GC)
- [parameter-passing.md](parameter-passing.md) — `borrowing` / `inout` / `consuming` conventions
- [ai-era-design.md](ai-era-design.md) — strong types, contracts, error model
- [../plan/roadmap.md](../plan/roadmap.md) — pipeline status (LLVM backend is Phase 1 work)
- [../plan/v0.1.md](../plan/v0.1.md) — what subset is in scope for the first release
