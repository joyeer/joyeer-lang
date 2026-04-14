# Joyeer Language - Memory Management Design

## Design Goal

Zero-overhead memory management — easier to learn than Rust, safer than C++, with nearly identical performance.

---

## Recommended Combination

### 1. Value Semantics by Default (Stack-allocated, Zero Overhead)

All types are value types by default, allocated on the stack with copy or move semantics.

```
let a = Vec3(1, 2, 3)    // stack-allocated
var b = a                 // copy (small types) or move (large types)
```

### 2. Region-based Allocator (Bulk Allocation Scenarios)

Allocate in bulk, free in bulk — no need to free individual objects. Ideal for request-response patterns, game frames, and compiler passes.

```
Region r = Region.create()
let a = r.alloc(Point(1,2))
let b = r.alloc(Point(3,4))
// ... use a, b ...
r.destroy()                 // free entire region at once, O(1)
```

- The compiler tracks which references belong to which region via the type system
- Guarantees references are not used after region destruction
- Eliminates per-object free overhead entirely; minimal memory fragmentation
- References: Cyclone, MLKit, Rust's `bumpalo` crate

### 3. Compile-time Reference Counting (Optimized ARC for Shared Ownership)

The compiler inserts retain/release at compile time and optimizes away 90%+ of them:

- **Pair elimination**: consecutive retain + release cancel out
- **Move semantics**: last use transfers ownership without retain
- **Whole-module analysis**: optimizes across function boundaries

```
fn foo(x: Object) {
    bar(x)         // release after bar returns
    baz(x)         // last use — retain/release pair optimized away
}
```

- References: Swift (ARC), Lobster language

### 4. Escape Analysis (Transparent Optimization)

The compiler automatically analyzes whether an object escapes the current scope. Non-escaping objects are stack-allocated:

```
fn compute() -> i32 {
    let p = Point(1, 2)   // does not escape → stack-allocated, zero heap overhead
    return p.x + p.y
}
```

- Fully transparent to the programmer, zero additional programming burden
- References: Go, JVM JIT, Graal

### 5. Second-class References (Simple, Safe Borrowing)

References cannot be stored in data structures — they can only be used as function parameters:

```
fn process(data: &BigData) { ... }   // ✅ reference as parameter

struct Cache {
    data: &BigData    // ❌ compile error: references cannot be stored in structs
}
```

- Reference lifetime equals the call stack frame — inherently safe
- Completely eliminates Rust-style complex lifetime annotations
- Trade-off: reduced flexibility (self-referential structures not possible)
- References: Hylo (formerly Val) language

---

## Alternative Patterns (Future Consideration)

### Linear / Affine Types

Each value must be used exactly once (linear) or at most once (affine). Rust's ownership system is essentially a practical implementation of affine types. Can manage any resource (file handles, network connections, locks), not just memory.

### Pool / Slab Allocator as a Language Feature

Objects of the same type are placed in a contiguous memory pool. O(1) allocation, O(1) deallocation, cache-friendly iteration. Ideal for ECS architectures and game engines.

### Reference Capabilities (Pony Language)

Fine-grained reference permission control (iso/ref/trn) that can express concurrency safety. Relatively high complexity.

### Generational References (Vale Language)

Each object carries a generation counter; access validates version match at runtime. Runtime overhead is one integer comparison per dereference. Simpler than Rust but not zero-overhead.

---

## Comparison Summary

| Pattern | Runtime Overhead | Compile-time Complexity | Programming Burden |
|---------|-----------------|------------------------|--------------------|
| Value semantics (stack) | Zero | Low | Zero |
| Region-based | Near zero | Medium | Low |
| Compile-time RC | Minimal | Medium | Low |
| Escape analysis | Zero (non-escaping) | Medium | Zero (transparent) |
| Second-class references | Zero | Medium | Low |
| Linear/Affine types | Zero | High | Medium-High |
| Pool/Slab | Zero | Low | Medium |
| Capabilities | Zero | High | High |
| Generational Ref | Low | Low | Low |

---

## Key Principles

1. **Zero-overhead C FFI** — the entry ticket to systems programming
2. **Pick one memory safety approach and commit** — do not try to support all approaches
3. **Compilation speed is a feature** — module system + incremental compilation
4. **Gradual migration path** — must interop with existing C/C++ codebases
5. **Explicit unsafe blocks** — allow bypassing safety checks when needed, but require explicit marking
