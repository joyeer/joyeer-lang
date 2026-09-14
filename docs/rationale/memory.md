# Joyeer Language — Memory Management

> **Normative rules live in [spec.md](../spec.md) §4 (Memory Model).** This
> document is *rationale*: why Mutable Value Semantics was chosen, how it
> compares to GC / ARC / Rust, and which alternatives were rejected. Where
> this text and the spec disagree, the spec wins.

## Design Goal

Zero-overhead memory management — easier to learn than Rust, safer than C++,
with nearly identical performance.

These are goals, not measured conclusions. See
[runtime-overhead.md](runtime-overhead.md) for the current baseline and the
evidence still needed.

---

## Core Model and Future Mechanisms

Joyeer's memory model is **Mutable Value Semantics + RAII**, in the style of
Hylo / Val. The current MVP implements compiler-known owned values and access
conventions. User-defined destruction, regions, raw-pointer/unsafe facilities,
and dedicated escape-analysis promotion are not implemented capabilities.

| Piece | Defined in |
|---|---|
| Value semantics and explicit ownership | this doc §1 |
| Deterministic destruction; user-defined `deinit` remains future work | this doc §2 |
| Region allocator candidate, not implemented | this doc §3 |
| Escape-analysis optimization candidate, not guaranteed | this doc §4 |
| Ownership transfer via `consuming` parameters | [parameter-passing.md](parameter-passing.md) |
| Exclusive mutable borrow via `inout` | [parameter-passing.md](parameter-passing.md) |
| **No** GC | [runtime cost goals](runtime-overhead.md#1-what-zero-cost-means-here) |
| **No** ARC / refcounting | [runtime cost goals](runtime-overhead.md#1-what-zero-cost-means-here) |
| **No** first-class references (`&T`) | [parameter-passing.md](parameter-passing.md) |
| **No** work unrelated to the specified operation | [ownership operation costs](runtime-overhead.md#3-ownership-operations-have-real-costs) |

---

## 1. Value Semantics by Default

All types have value semantics. Ordinary initialization and assignment copy
the value and leave the source initialized; an explicit consuming boundary
transfers ownership.

```
let a = Vec3(x: 1, y: 2, z: 3) // value construction
var b = a                 // semantic copy; a remains usable
```

Scalar and fixed-layout aggregate storage is stack-allocated by default.
Heap-owning standard-library values such as `String`, `Array`, and `Dict`
allocate as part of construction, concatenation, growth, or copying. The
source operation and concrete type make that cost predictable; Joyeer does not
insert allocations unrelated to the operation's specified value semantics.

## 2. Deterministic Destruction (RAII)

The language model gives owned values deterministic destruction on normal
scope exit and return. In the current MVP the compiler emits cleanup for
builtin heap-backed values and aggregates; user-defined `init`/`deinit`
bodies remain future work. There is no GC, reference counting, or finalizer
queue.

```
func example() {
    let bytes = "data".utf8()
    print(value: bytes.count)
    // compiler-inserted cleanup destroys the owned byte array
}
```

`consuming` parameters transfer the obligation to `deinit` to the callee. See
[parameter-passing.md](parameter-passing.md) for the full call-convention rules.

### Ownership is a tree, not a graph

Because there are no reference types and no shared ownership, the lifetime
graph of any program is a tree. Retain cycles (the Achilles heel of Swift /
Python RC) are **structurally unrepresentable**.

## 3. Region Allocator Candidate

Regions are a possible design for request-response, per-frame, or
per-compiler-pass workloads. There is currently no `Region` API, region syntax,
or region-lifetime checker.

A future design must track region membership and prevent use after region
destruction. Bulk memory reclamation does not automatically remove per-object
destructor obligations; those may be erased only when semantics permit it.

References: Cyclone, MLKit, Rust's `bumpalo`.

## 4. Escape Analysis Candidate

Promoting non-escaping heap-backed values to stack storage is a possible
optimization, not a current Joyeer guarantee. There is no dedicated
container-to-stack promotion pass in the MVP.

```
func compute(): Int {
    let xs = [1, 2]
    return xs[0] + xs[1]
}
```

Any such optimization must preserve copying, destruction, and observable
behavior. References for related techniques: Go, JVM JIT, GraalVM.

---

## What Is *Not* in the Model (and Why)

| Mechanism | Why rejected |
|---|---|
| Garbage collection | Conflicts with the deterministic ownership and [runtime cost goals](runtime-overhead.md). |
| Automatic reference counting (ARC) | Retain/release work and shared ownership are outside the [chosen runtime model](runtime-overhead.md). |
| First-class references (`&T`, `&mut T`) | Would require an escaping-reference lifetime model. Current access conventions still require static exclusivity and ownership analysis. |
| User-defined linear-resource constraints | Not part of the current subset. `consuming` still requires move/use analysis; it does not eliminate type-system or data-flow work. |
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

This table records availability, not measured runtime or compiler-complexity
rankings.

| Mechanism | Current status |
|---|---|
| Value semantics | Core model; heap-backed ordinary copies can allocate and recursively copy. |
| Deterministic destruction | Compiler-generated cleanup for the MVP; user-defined `deinit` is future work. |
| Access conventions | All four conventions exist, with known analysis/lowering gaps still to close. |
| Regions | Future design; no region API or checker is implemented. |
| Container escape-analysis promotion | Potential optimization, not a current guarantee. |
| GC / ARC | Excluded from the core model. |
| Ownership/exclusivity analysis | Required despite the absence of first-class reference types. |
| Custom allocators, pools, and stronger resource constraints | Follow-on design work, not current APIs. |

---

## Long-Term Priorities

1. Preserve and test MVS/RAII invariants before expanding the ownership surface.
2. Define a source-language FFI and stable layout/calling rules before claiming
   C interoperability; the existing compiler-backend C ABI is a different boundary.
3. Measure compilation and runtime costs rather than assuming they disappear.
4. Design modules, incremental compilation, allocator control, and any unsafe
   interoperability surface explicitly before promising them as capabilities.
