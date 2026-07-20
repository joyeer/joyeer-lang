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
| Value semantics and explicit ownership | this doc §1 |
| Deterministic destruction via `deinit` (RAII) | this doc §2 |
| Region allocator (opt-in for bulk patterns) | this doc §3 |
| Escape analysis (transparent optimization) | this doc §4 |
| Ownership transfer via `consuming` parameters | [parameter-passing.md](parameter-passing.md) |
| Exclusive mutable borrow via `inout` | [parameter-passing.md](parameter-passing.md) |
| **No** GC | [runtime-overhead.md](runtime-overhead.md) §1 |
| **No** ARC / refcounting | [runtime-overhead.md](runtime-overhead.md) §3.2 |
| **No** first-class references (`&T`) | [parameter-passing.md](parameter-passing.md) |
| **No** work unrelated to the specified operation | [runtime-overhead.md](runtime-overhead.md) §3.2 |

---

## 1. Value Semantics by Default

All types have value semantics. Ordinary initialization and assignment copy
the value and leave the source initialized; an explicit consuming boundary
transfers ownership.

```
let a = Vec3(1, 2, 3)    // stack-allocated
var b = a                 // semantic copy; a remains usable
```

Scalar and fixed-layout aggregate storage is stack-allocated by default.
Heap-owning standard-library values such as `String`, `Array`, and `Dict`
allocate as part of construction, concatenation, growth, or copying. The
source operation and concrete type make that cost predictable; Joyeer does not
insert allocations unrelated to the operation's specified value semantics.

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

## Open Discussion: Concurrency & Cross-Thread Read-Only Sharing

> **Status: not designed, not committed.** The concurrency model is explicitly
> deferred (see the "concurrency model not designed yet" note in the rejected-
> mechanisms table above). This section records candidate approaches for a
> future design discussion; none of it is normative.

### The problem

The model has **no reference types** and **no reference counting** (spec §4.8),
so "who frees the memory when multiple threads read the same value?" cannot be
answered by refcount-reaches-zero. Release is always triggered by the **single
owner** at the end of its scope (spec §4.7). Multiple readers are multiple
`borrowing` projections (spec §4.4), and a reader **never** frees.

The gap: spec §4.5.3 bounds a projection's lifetime to the **enclosing
statement** with *no flow analysis*. That is sufficient for single-threaded
code but does **not** cover handing a `borrowing` projection to a thread that
runs concurrently — such a borrow *escapes* the statement that created it.
Supporting read-only cross-thread sharing therefore requires a new mechanism.

### Candidate approaches

The invariant to preserve in every option: **the single owner's lifetime ≥
every reader thread's lifetime**, and release stays with the owner.

| Option | Idea | Who frees / when | Runtime cost | Fits MVS+RAII? |
|---|---|---|---|---|
| **A. Structured concurrency (scoped threads)** | Child threads are lexically nested in the owner's scope; the compiler proves all borrows end before the owner is dropped. (cf. Rust `thread::scope`, Hylo.) | Owner's scope end, deterministic | Zero | ✅ Best fit |
| **B. Ownership transfer to a holder** | `consume` the data into a container that owns it and outlives all workers; workers `borrowing` from the container. | Holder's `deinit`, deterministic | Zero | ✅ Single owner + RAII |
| **C. Explicit `Shared`/`Arc` library type** | Atomic refcount hidden inside a stdlib type built on an `unsafe` block (spec §4.9). Escape hatch only — contradicts "ownership is a tree". | Runtime: last holder to drop | Atomic refcount | ⚠️ Escape hatch, non-deterministic release |

**Option A** is the preferred default: zero-overhead and the most consistent
with deterministic RAII destruction.

**Option C** is deliberately *not* in the core model (spec §4.8 has no `Rc`/
`Arc`); if ever added it must be a library type behind `unsafe`, because its
non-deterministic release is exactly what [runtime-overhead.md §3.2](runtime-overhead.md)
rejects ARC for.

### Spec work this would require (when picked up)

- Extend spec §4.5.3 with a **cross-thread borrow** rule, making structured
  concurrency (Option A) the only safe way to escape a borrow to another thread,
  and stating explicitly that readers never free.
- Possibly a `Sendable`-style marker for "safe to move/share across threads."
- A new Concurrency chapter (or §10.x) tying Options A/B/C together, with
  `Shared` as the `unsafe` library escape hatch.

---

## Decision: Predictable Copies and Explicit Consumption

> **Status: resolved 2026-07-20.** Ordinary initialization and assignment keep
> value semantics; `consume` remains the only source-visible ownership
> transfer across a call boundary.

`var b = a` is a copy operation. After it completes, `a` and `b` are
independent and `a` remains initialized. For v0.1 heap-backed values, that
means an eager recursive clone into uniquely owned storage. Joyeer does not
use copy-on-write or reference counting to make the operation appear cheaper.

This makes the conservative cost visible from the operation and type: copying
a heap-backed value can be O(n) and can allocate. An already-owned temporary
can transfer directly into its destination. A compiler may also elide a copy
under the as-if rule, but that optimization cannot turn ordinary assignment
into a source-level consume or make later use of the source invalid.

The explicit marker still carries the important semantic distinction:

```joyeer
let copy = value                    // value remains initialized
store(value: consume value)         // value becomes uninitialized
```

Types with custom `deinit` are noncopyable by default. Synthesizing a
fieldwise copy could make two values release the same resource, so such a type
must move through consuming paths until a future explicit copy-initializer
design lets it define an independent copy. Structs and enums without custom
destruction are recursively copyable only when all stored values are
copyable.

Tooling may later report materialized copies for performance review, but
correctness and ownership do not depend on that tooling. The normative rules
are in spec §4.1 and §4.6.

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
