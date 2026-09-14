# Joyeer Language — Parameter Passing Design

> **Normative rules live in [spec.md](../spec.md) §4.2–§4.3 (access effects &
> call-site markers).** This document is *rationale*: the survey of how other
> languages pass parameters and why Joyeer adopts Hylo-style Mutable Value
> Semantics. The final keyword spelling (`borrowing` / `inout` / `consuming` /
> `initializing`) is defined by the spec; older Hylo-style `let`/`sink`/`set`
> names below describe Hylo, not Joyeer.

This document records the design discussion around how function parameters convey ownership and mutability intent. It covers the `&T` vs `T` distinction, a survey of how major languages approach this problem, and a recommendation for Joyeer.

---

## The Core Question: `peek(p: &Parser)` vs `peek(p: Parser)`

The following contrasts conventional value and reference passing; it does
**not** describe Joyeer's default. In Joyeer, `p: Parser` means `borrowing`
unless another access convention is written. It does not implicitly consume
or deep-copy the caller's value merely because `&` is absent.

### Conventional `peek(p: Parser)` — Pass by Value

- The entire `Parser` is **copied or moved** into the function.
- Changes inside the function do **not** affect the caller's copy.
- With move semantics the caller **cannot use `p` afterwards**.
- `Parser` typically holds a lexer, token buffer, and position state — copying it on every `peek` call is expensive.

### Conventional `peek(p: &Parser)` — Pass by (Immutable) Reference

- Only a **pointer** (8 bytes) is passed — zero copy.
- The function can **only read** `p`; it cannot modify it.
- The caller **retains ownership** and can continue using `p` after the call.
- The compiler can statically guarantee that nobody mutates `p` while `peek` is running.

A "read-only observer" function like `peek` fits the reference model precisely:

```
peek(p: &Parser)      -> Token   // borrow, read-only
advance(p: &mut Parser) -> Token // borrow, mutable
consume(p: Parser)    -> Ast     // take ownership
```

---

## Survey of Approaches in Existing Languages

### Rust — Ownership + Borrow Checker (Strictest)

```rust
fn peek(p: &Parser)      -> Token  // shared borrow, read-only
fn advance(p: &mut Parser) -> Token // exclusive borrow, writable
fn consume(p: Parser)    -> Ast    // ownership transfer
```

**Rule:** at any moment, any number of `&T` may coexist, or exactly one `&mut T` — never both. Enforced by the borrow checker at compile time.

✅ Prevents data races and dangling pointers at compile time  
✅ Zero-cost (just pointers)  
❌ Steep learning curve; lifetime annotations get complex; borrow checker is a large compiler component

### C++ — References + `const` (Convention-based)

```cpp
Token peek(const Parser& p);  // read-only reference
Token advance(Parser& p);     // mutable reference
Ast   consume(Parser&& p);    // rvalue reference (move)
```

✅ Flexible, zero-cost  
`const` does not provide lifetime or exclusive-borrow checking; ordinary C++
references can still dangle. Optimizer alias analysis is a separate mechanism.

Joyeer's stated goal is to **replace C++ for new code**. This is the baseline to surpass.

### Swift — Value Types + `inout`

```swift
func peek(_ p: Parser) -> Token            // value semantics; normally borrowing
func advance(_ p: inout Parser) -> Token   // explicit mutable
```

✅ Default value semantics; easy to reason about  
Copy-on-Write can defer copying storage until mutation, but retaining and
eventual unique-storage copies still have costs.
❌ Implicit copies are invisible in performance-sensitive code  
Joyeer's syntax is intentionally Swift-like, making this the most natural starting point.

### Go — Value vs Pointer (Manual Choice)

```go
func Peek(p Parser) Token   // copy
func Peek(p *Parser) Token  // pointer, readable and writable, may be nil
```

✅ Extremely simple  
❌ No read-only pointer; `*T` can always be mutated; nil safety is runtime-only  
Too loose for Joyeer's "strong types as AI guardrails" direction.

### Hylo / Val — Mutable Value Semantics (Newest Direction)

```
fun peek(p: Parser) -> Token            // let-parameter: read-only
fun advance(inout p: Parser) -> Token   // inout: exclusive mutable
fun consume(sink p: Parser) -> Ast      // sink: ownership transfer
```

Four parameter conventions: `let` / `inout` / `sink` / `set`.

✅ No first-class reference type simplifies escaping-lifetime rules\
✅ Value semantics limit shared ownership; projections still require checking\
✅ Compiler automatically passes large objects by reference as an optimisation  
✅ No lifetime annotations  
✅ Intent is fully visible in the signature  
❌ Cannot return references (can only return values)  
❌ Interop with C/C++ is more limited than Rust

### Carbon — Similar Exploration

Google's Carbon uses `var` / `let` to distinguish mutability and is researching Hylo-style ownership conventions. Worth watching but still in flux.

---

## Memory Safety and Deallocation: Hylo vs Rust

Since the Hylo model is the leading candidate for Joyeer, it is worth understanding how its memory safety compares to Rust's.

### How Hylo Releases Memory

Hylo uses **RAII**: when a variable goes out of scope its `deinit` is called automatically, exactly like C++ destructors.

```hylo
fun example() {
    var buf = Buffer(size: 1024)  // heap allocation inside Buffer.init
    process(buf)
    // scope ends → buf.deinit() called → heap memory freed
}
```

There is no GC, no reference counting, no manual `free`.

### Deinitializer

```hylo
type Buffer {
    var data: Pointer<UInt8>
    var size: Int

    init(size: Int) {
        self.size = size
        self.data = Pointer.allocate(size)
    }

    deinit {
        self.data.deallocate()  // guaranteed to run on scope exit
    }
}
```

### `sink` Prevents Forgetting to Free

When a value is passed as `sink`, ownership transfers unconditionally. The callee is responsible for the `deinit`. The caller cannot access the variable afterwards — so "forget to free" is structurally impossible.

```hylo
fun consume(sink b: Buffer) -> ProcessedData {
    let result = process(b.data)
    return result
    // b.deinit() called here automatically
}
```

### No Cycle Problem (Unlike Reference Counting)

Reference counting alone cannot reclaim strong-reference cycles:

```
// Reference-counting-only model
let a = Node(); let b = Node()
a.next = b   // RC +1
b.prev = a   // RC +1
// Neither reaches zero without an additional cycle-breaking mechanism
```

Swift ARC relies on avoiding/breaking strong cycles; Python also has a cyclic
garbage collector. The example illustrates reference counting alone, not a
claim that Python necessarily leaks such a cycle.

Hylo has **no reference types**. Ownership is a tree, never a graph. Cycles are **structurally unrepresentable** — the compiler cannot express them.

### Model-Level Comparison: Hylo and Rust

This is a comparison of design approaches, not a measured ranking of compiler
quality, complexity, performance, or AI-generated-code correctness.

| Dimension | Rust | Hylo |
|---|---|---|
| Core mechanism | Ownership + **borrow checker** | Ownership + **value semantics** (no reference type) |
| Reference type | Yes (`&T` / `&mut T` with lifetimes) | **No** |
| Lifetime annotations | Often inferred; explicit annotations are sometimes needed | No first-class-reference lifetime annotations |
| Alias analysis | Borrow and lifetime checking | Projection/exclusivity checks still needed |
| Compiler complexity | Ownership and reference-lifetime analysis | Ownership, initialization, projection, and control-flow analysis |
| Can return references? | Yes (with lifetime annotations) | **No** (return values only) |
| Ownership cycles | Exclusive ownership is acyclic; `Rc`/`Arc` can form cycles | The exclusive value-ownership model avoids reference-count cycles |
| C/C++ interop | Excellent | Limited (`remote` variables) |

---

## Recommendation for Joyeer

Given Joyeer's goals — **no GC, zero-cost abstractions, AI-era language, Swift-like syntax, replace C++ for new code** — the Hylo-style Mutable Value Semantics model is the strongest candidate:

```
func peek(p: Parser): Token                // borrowing (read-only, compiler passes by ref)
func advance(p: inout Parser): Token       // inout (exclusive mutable)
func consume(p: consuming Parser): Ast     // consuming (ownership transfer)
```

> **Keyword note.** The spec (§4) renames Hylo's effect keywords to read more
> naturally for the Swift-trained audience: Hylo `let`/`sink`/`set` become
> Joyeer `borrowing` (the default) / `consuming` / `initializing`; method
> receivers use `mutating` / `consuming`. Ownership transfer is marked at the
> call site with `consume x` (mandatory). The return type is written with `:`
> (spec §2.3), not `->`. The conventions below are unchanged; only the
> spelling differs.

### Why This Fits Joyeer

1. **Reviewable intent** — signatures expose "read / mutate / take / initialize", but compiler analysis must still enforce that intent.
2. **No GC** — all value semantics; ownership is a tree; `deinit` fires deterministically.
3. **Narrower lifetime surface** — omitting first-class escaping references removes some rules, not the need for move, initialization, exclusivity, and loop data-flow analysis.
4. **Related syntax** — Swift's [SE-0377](https://github.com/swiftlang/swift-evolution/blob/main/proposals/0377-parameter-ownership-modifiers.md) provides `borrowing`/`consuming` alongside `inout`; Joyeer's `initializing` contract is its own design, not a verbatim Swift feature.
5. **Cost-aware conventions** — borrowing can avoid semantic deep copies, but concrete calling conventions and optimizations still need measurement.

### What Is Sacrificed

- Cannot return references (only values). This restricts some iterator patterns but is acceptable for most systems code.
- C/C++ interop requires an explicit `remote`/unsafe boundary.

### Further Reading

- Hylo language: <https://www.hylo-lang.org/>
- Racordon et al., *Implementation Strategies for Mutable Value Semantics* (ECOOP 2022)
- Dave Abrahams, "Value Semantics: Safety, Independence, Projection, and Future of Programming" (CppNow 2022)

---

## Related Documents

- [memory.md](memory.md) — overall memory management strategy (value semantics, RAII, regions)
- [runtime-overhead.md](runtime-overhead.md) — zero-cost contract and runtime size budget
- [ai-era-design.md](ai-era-design.md) — strong types, explicit ownership, and contracts as AI guardrails
