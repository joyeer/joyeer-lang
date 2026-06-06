# Joyeer Language — Memory Management

> **Normative rules live in [spec.md](../spec.md) §4 (Memory Model).** This
> document is *rationale*: why Mutable Value Semantics was chosen, how it
> compares to GC / ARC / Rust, and which alternatives were rejected. Where
> this text and the spec disagree, the spec wins.

## Design Goal

Zero-overhead memory management — easier to learn than Rust, safer than C++,
with nearly identical performance.

---

## The Model (Committed)

Joyeer's memory model is **Mutable Value Semantics + RAII**, in the style of
Hylo / Val. This document covers the **defaults** that work for 95 % of code.
Opt-in tools (regions, raw pointers, `unsafe`) cover the rest.

| Piece | Defined in |
|---|---|
| Value semantics by default, stack allocation | this doc §1 |
| Deterministic destruction via `deinit` (RAII) | this doc §2 |
| Region allocator (opt-in for bulk patterns) | this doc §3 |
| Escape analysis (transparent optimization) | this doc §4 |
| Ownership transfer via `consuming` parameters | [parameter-passing.md](parameter-passing.md) |
| Exclusive mutable borrow via `inout` | [parameter-passing.md](parameter-passing.md) |
| **No** GC | [runtime-overhead.md](runtime-overhead.md) §1 |
| **No** ARC / refcounting | [runtime-overhead.md](runtime-overhead.md) §3.2 |
| **No** first-class references (`&T`) | [parameter-passing.md](parameter-passing.md) |
| **No** hidden allocations | [runtime-overhead.md](runtime-overhead.md) §3.2 |

---

## 1. Value Semantics by Default (Stack-allocated, Zero Overhead)

All types are value types by default, allocated on the stack with copy or move
semantics.

```
let a = Vec3(1, 2, 3)    // stack-allocated
var b = a                 // move (last use of a) — no copy
                          // or copy if a is used again later
```

Heap allocation is **never implicit**. To put something on the heap, the
programmer uses an explicit container (`Box<T>`, region allocator, or a stdlib
type like `Array<T>` that allocates internally).

## 2. Deterministic Destruction (RAII)

When a value goes out of scope, its `deinit` runs. No GC, no refcount, no
finalizer queue.

```
type Buffer {
    var data: Pointer<UInt8>
    var size: Int

    init(size: Int) {
        self.size = size
        self.data = Pointer.allocate(size)
    }

    deinit {
        self.data.deallocate()   // runs at scope exit, guaranteed
    }
}

func example() {
    let buf = Buffer(size: 1024)
    process(buf)
    // scope ends → buf.deinit() runs → memory freed
}
```

`consuming` parameters transfer the obligation to `deinit` to the callee. See
[parameter-passing.md](parameter-passing.md) for the full call-convention rules.

### Ownership is a tree, not a graph

Because there are no reference types and no shared ownership, the lifetime
graph of any program is a tree. Retain cycles (the Achilles heel of Swift /
Python RC) are **structurally unrepresentable**.

## 3. Region Allocator (Opt-in for Bulk Patterns)

For request-response, per-frame, or per-compiler-pass workloads, the region
allocator provides bulk allocation and O(1) bulk free.

```
region r = Region.create()
let a = r.alloc(Point(1, 2))
let b = r.alloc(Point(3, 4))
// ... use a, b ...
r.destroy()                    // free entire region at once
```

The type system tracks which values belong to which region and prevents
use-after-region-destroy at compile time. The compiler may erase per-object
`deinit` calls for region-allocated objects when proven safe.

References: Cyclone, MLKit, Rust's `bumpalo`.

## 4. Escape Analysis (Transparent Optimization)

The compiler automatically promotes heap-allocated stdlib containers to the
stack when escape analysis proves they do not outlive the current frame.

```
func compute(): Int {
    let xs = Array<Int>()    // logically heap, optimized to stack if non-escaping
    xs.push(1); xs.push(2)
    return xs.sum()
}
```

Fully transparent to the programmer. References: Go, JVM JIT, GraalVM.

---

## What Is *Not* in the Model (and Why)

| Mechanism | Why rejected |
|---|---|
| Garbage collection | Pauses, hidden cost, large runtime ([runtime-overhead.md §2](runtime-overhead.md)) |
| Automatic reference counting (ARC) | Hidden retain/release work, retain cycles, contradicts [runtime-overhead.md §3.2](runtime-overhead.md) |
| First-class references (`&T`, `&mut T`) | Forces a borrow checker; replaced by `borrowing` / `inout` / `consuming` ([parameter-passing.md](parameter-passing.md)) |
| Linear / affine types (Rust-style) | High learning curve; `consuming` captures the "use once" property without the type-system tax |
| Reference capabilities (Pony `iso` / `ref` / `trn`) | Too complex; concurrency model not designed yet |
| Generational references (Vale) | Per-deref runtime check is not zero-cost |

---

## Future Extensions (Not in v0.1)

Valid additions in the future, **not** part of the v0.1 commitment:

- **Pool / Slab allocator** as a language feature for ECS-style hot loops.
- **Custom allocators as explicit parameters** (Zig-style) for `std` containers.
- **Linear types** for resources that must not be dropped silently (file
  handles, locks).

---

## Comparison Summary

| Mechanism | Runtime overhead | Compile-time complexity | Programmer burden | In Joyeer? |
|---|---|---|---|---|
| Value semantics (stack) | Zero | Low | Zero | ✅ Default |
| RAII / `deinit` | Zero | Low | Low | ✅ Default |
| `consuming` ownership transfer | Zero | Medium | Low | ✅ Default ([parameter-passing.md](parameter-passing.md)) |
| Region-based | Near zero | Medium | Low | ✅ Opt-in |
| Escape analysis | Zero (non-escaping) | Medium | Zero | ✅ Optimization |
| Garbage collection | High (pauses) | Low | Zero | ❌ Rejected |
| ARC / refcounting | Low–medium | Medium | Low | ❌ Rejected |
| Borrow checker (Rust) | Zero | **High** | Medium | ❌ Avoided by MVS |
| Linear / affine types | Zero | High | Medium-high | ⏸ Future |
| Pool / Slab | Zero | Low | Medium | ⏸ Future (library) |
| Capabilities (Pony) | Zero | High | High | ❌ |
| Generational refs (Vale) | Low | Low | Low | ❌ |

---

## Key Principles

1. **Zero-overhead C FFI** — the entry ticket to systems programming.
2. **Pick one memory safety approach and commit** — MVS + RAII, no fallbacks.
3. **Compilation speed is a feature** — modules + incremental compilation.
4. **Gradual migration from C / C++** — must interop without runtime overhead.
5. **Explicit `unsafe` blocks** — raw pointers, FFI, low-level work needs an
   explicit marker but is not forbidden.
