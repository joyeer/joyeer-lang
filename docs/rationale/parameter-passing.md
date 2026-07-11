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

### `peek(p: Parser)` — Pass by Value

- The entire `Parser` is **copied or moved** into the function.
- Changes inside the function do **not** affect the caller's copy.
- With move semantics the caller **cannot use `p` afterwards**.
- `Parser` typically holds a lexer, token buffer, and position state — copying it on every `peek` call is expensive.

### `peek(p: &Parser)` — Pass by (Immutable) Reference

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
❌ `const` can be cast away; no alias analysis; dangling references are entirely the programmer's responsibility  

Joyeer's stated goal is to **replace C++ for new code**. This is the baseline to surpass.

### Swift — Value Types + `inout`

```swift
func peek(_ p: Parser) -> Token            // implicit copy (COW-optimised)
func advance(_ p: inout Parser) -> Token   // explicit mutable
```

✅ Default value semantics; easy to reason about  
✅ Copy-on-Write makes `Array`/`String` copies nearly free  
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

✅ No reference type → no borrow checker required  
✅ All value semantics → no aliasing, no dangling  
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

Swift's ARC and Python's RC both suffer from retain cycles:

```
// Retain cycle (Swift/Python)
let a = Node(); let b = Node()
a.next = b   // RC +1
b.prev = a   // RC +1
// Neither ever reaches RC == 0 → leak
```

Hylo has **no reference types**. Ownership is a tree, never a graph. Cycles are **structurally unrepresentable** — the compiler cannot express them.

### Comparison Table: Hylo vs Rust Memory Safety

| Dimension | Rust | Hylo |
|---|---|---|
| Core mechanism | Ownership + **borrow checker** | Ownership + **value semantics** (no reference type) |
| Reference type | Yes (`&T` / `&mut T` with lifetimes) | **No** |
| Lifetime annotations | Required | **None needed** |
| Alias analysis | Borrow checker at compile time | Structurally impossible |
| Compiler complexity | High (borrow checker is the hardest part) | Low |
| Can return references? | Yes (with lifetime annotations) | **No** (return values only) |
| Cycle-free guarantee | Yes (ownership is acyclic) | Yes (same reason) |
| C/C++ interop | Excellent | Limited (`remote` variables) |
| Performance ceiling | Highest (precise alias control) | High (compiler auto-optimises) |
| AI code correctness | Good | **Better** (rules are simpler; AI is less likely to make alias mistakes) |

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

1. **AI-friendly** — the signature fully describes "read / mutate / take". AI cannot accidentally introduce hidden aliasing bugs.
2. **No GC** — all value semantics; ownership is a tree; `deinit` fires deterministically.
3. **No borrow checker to implement** — the biggest engineering cost of Rust's compiler disappears.
4. **Syntax-compatible** — Swift already has `inout`; the ownership effects (`borrowing` / `consuming` / `initializing`, plus `mutating` receivers) line up with Swift 5.9's ownership vocabulary.
5. **Zero-cost** — the compiler silently chooses pass-by-reference for large objects.

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
