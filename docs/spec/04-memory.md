## §4 Memory Model  ★ CORE ★

This is the chapter that distinguishes Joyeer from "another Swift clone."
Read it carefully.

### 4.1 Value semantics is the only semantics

Every binding holds a **value**. Assignment is conceptually a copy:

```joyeer
var a = [1, 2, 3]
var b = a            // semantically: b is an independent copy of a
&b[0] = 99
print(value: a)       // [1, 2, 3]
print(value: b)       // [99, 2, 3]
```

The compiler is free to implement this as a move, a copy-on-write, or an
in-place reuse, **as long as the observable behavior is value semantics**.
Programmers do not see and do not control this — the compiler picks the
optimal lowering using last-use analysis (§4.6).

There are **no reference types** in Joyeer. No `&T`, no pointers, no
`Box`/`Rc`/`Arc`. The closest equivalents are *access effects* on
parameters (§4.2) and *projections* via subscripts (§4.5), neither of
which is a first-class value.

#### 4.1.1 Ownership state vs. access effect (two axes)

Two **orthogonal** classifications govern every binding. Keeping them apart
removes a common confusion: *owned* is a **state** (who must destroy the
value), whereas `consuming` is an **effect** (how a parameter acquires that
state across a call).

**Axis 1 — ownership state.**

- An **owning** binding is responsible for the value's destruction: its
  `deinit` runs when the binding goes out of scope, is overwritten, or is
  consumed (§4.7). Local `let` / `var` bindings, stored `struct` / `enum`
  fields, and `consuming` parameters (including `consuming self`) are owning
  bindings.
- A **projecting** (borrowing) binding is a temporary view that **never**
  destroys the value; an owner elsewhere stays responsible. `borrowing` and
  `inout` parameters and subscript `yield` projections (§4.5) are projecting
  bindings.

**Axis 2 — mutability.** Independently of Axis 1, a binding is immutable
(`let`, `borrowing`) or mutable (`var`, `inout`, owning `consuming`).

The four parameter **access effects** (§4.2) are *points in this space* — they
describe how a parameter relates to the **caller's** storage, not a separate
kind of thing from ownership:

| Binding | Owns? (runs `deinit`) | Mutable? | Caller's value |
|---------|----------------------|----------|----------------|
| `let x` (local) | ✅ | ❌ | — (no call boundary) |
| `var x` (local) | ✅ | ✅ | — |
| `borrowing` param (default) | ❌ | ❌ | caller keeps |
| `inout` param | ❌ | ✅ (exclusive) | caller keeps |
| `consuming` param / `self` | ✅ | ✅ | **transferred to callee** |
| `initializing` param | writes caller's uninit. storage | ✅ (write) | caller keeps |

> **📌 Decision.** *`consuming` is the only access effect that transfers
> ownership; `borrowing` / `inout` / `initializing` are projections that leave
> ownership with the caller.*  "owned" names the resulting **state**;
> `consuming` names the **effect** that produces it. A local `var` is owned
> without ever being `consuming`; a `consuming` parameter is owned *because*
> the caller relinquished it (the caller's binding becomes uninitialized,
> §4.2.3). Hence every `consuming` binding is owned, but not every owned
> binding is `consuming`.

> **📌 Decision.** *A `consuming` parameter (and `consuming self`) is an
> owning, `var`-like (mutable) binding, and any owning mutable binding may be
> mutated in place by its owner.*  A `consuming` binding is owned outright and
> is therefore mutable — not `let`-like. (A `let` local is owning but immutable,
> per Axis 2.) In-place mutation is still marked with `&` at the mutation site
> (§4.3), so "where is this mutated?" stays greppable. Consequently the
> difference between a `mutating` and a `consuming` receiver is **not** in-body
> mutability — both may mutate `self` — but the **caller's fate**: `mutating`
> returns the receiver to the caller (an exclusive borrow), while `consuming`
> takes it away (§3.2.4).

### 4.2 Access effects on parameters

A function parameter declares **how** the function will access the
argument's storage. The effect is part of the function signature; callers
must match it explicitly at the call site (§4.3).

| Effect | Meaning | Caller obligation | In-body usage |
|--------|---------|-------------------|----------------|
| `borrowing` (default) | Read-only projection. Multiple `borrowing` projections of the same value may coexist. | Pass without marker (may write `borrowing x` for emphasis). | Use as immutable value. |
| `inout` | **Exclusive** mutable projection. While held, the original storage is inaccessible to anyone else. | Mark with `&x` at call site. | Use and mutate like a local `var`. |
| `consuming` | **Consume** the argument. Caller's binding becomes uninitialized after the call. | Mark with `consume x` at call site; caller must own the value. | Owned outright — a `var`-like, mutable binding (§4.1.1); may be **mutated in place**, moved into the return value, passed to another `consuming` parameter, or destroyed. |
| `initializing` | Write-only into uninitialized storage. | Mark with `&x` at call site, where `x` is uninitialized or has been consumed. | Must initialize before the body ends. |

#### 4.2.1 `borrowing` (default)

```joyeer
func max(a: Int, b: Int): Int { if a > b { a } else { b } }

var x = 10
var y = 20
let m = max(a: x, b: y)    // both x and y are borrow-projected; safe to coexist
print(value: x)       // ✅ x still accessible
```

The default may be written explicitly for emphasis:
`func max(a: borrowing Int, b: borrowing Int): Int`.

#### 4.2.2 `inout`

```joyeer
func increment(n: inout Int) { &n += 1 }

var k = 5
increment(n: &k)
print(value: k)       // 6
```

While `increment` holds an `inout` projection of `k`, no other access to
`k` is permitted — checked at compile time by the law of exclusivity
(§4.4).

#### 4.2.3 `consuming`

```joyeer
func store(s: consuming String) { /* s is mine: mutable, printable, destroyable, returnable */ }

var greeting = "hello"
store(s: consume greeting)
// print(value: greeting)    // ❌ error: use of consumed value 'greeting'
```

After the call, `greeting` is in an **uninitialized state**. The compiler
rejects any subsequent read. A subsequent assignment (`greeting = "world"`)
re-initializes the storage and re-enables reads.

Inside `store`, `s` is an owning, `var`-like binding (§4.1.1): the body may
**mutate `s` in place** (e.g. `&s.append(s: "!")`) as well as move or destroy
it. Ownership — not a `mutating` keyword — is what grants in-body mutation.

#### 4.2.4 `initializing`

```joyeer
func produceLargeBuffer(out: initializing [UInt8]) {
  &out = makeBuffer(size: 1_000_000)
}

var buf: [UInt8]                  // declared but uninitialized
produceLargeBuffer(out: &buf)     // 'initializing' writes without destructing prior contents
```

`initializing` is the emplace pattern: the callee promises to initialize the
storage; the caller promises the storage was uninitialized. This avoids a
destruct-then-construct round-trip for large objects.

### 4.3 Call-site markers `&` and `consume` 📌

> **📌 Decision.** *Call-site marker for `inout` / `initializing` is `&x`;
> for `consuming` it is `consume x`.*  Caller readability: any visible `&` or
> `consume` at a call site signals "this argument's storage will be
> exclusively borrowed, or given away, across this call."

```joyeer
swap(a: &a, b: &b)        // both args are inout
produceLargeBuffer(out: &buf)  // 'initializing' is also marked &
store(s: consume s)       // 'consuming' is marked with consume
plain(x: x, y: y)         // no marker → both are borrowing (read-only)
```

The `&` marker is **mandatory** on `inout` and `initializing` arguments,
and `consume` is **mandatory** on `consuming` arguments. Omitting either is
a syntax-level error, not just a type error — this guarantees that mutation
and ownership transfer are always visible at the call site by simple
scanning (§0.1 principle 2).

> **📌 Decision.** *`consume` at the call site is mandatory, not optional.*
> Swift makes its `consume` operator optional, relying on implicit last-use
> analysis to move otherwise. Joyeer requires it explicitly because the bulk
> of code is AI-generated: a mandatory, always-visible marker makes ownership
> transfer trivially greppable for both reviewers and tools, and removes the
> "did the compiler move or copy here?" ambiguity (§0.1 principles 2 and 4).
> Last-use *optimization* still applies to plain `borrowing` bindings (§4.6);
> `consume` is about **semantic** ownership transfer, not the optimization.

### 4.4 The Law of Exclusivity

> At any program point, for any storage location:
> **either** any number of `borrowing` projections coexist,
> **or** exactly one `inout` / `consuming` / `initializing` projection exists.
> The two regimes never overlap in time or in storage.

Checked statically by the compiler. Examples:

```joyeer
var x = 10
let a = x            // ✅ borrowing projection
let b = x            // ✅ another borrowing projection coexists
increment(n: &x)     // ❌ error: cannot establish inout projection while
                     //    borrowing projections 'a' and 'b' are active
```

```joyeer
var x = 10
increment(n: &x)     // ✅ inout for the duration of the call
let a = x            // ✅ inout ended; borrowing now allowed
```

```joyeer
func add(dst: inout Int, src: Int) { &dst += src }

var n = 5
add(dst: &n, src: n)          // ❌ error: 'n' has overlapping inout + borrowing projections
```

#### 4.4.1 Field-level disjointness

The compiler analyzes access paths. For struct fields, distinct paths are
treated as disjoint storage:

```joyeer
struct Point { var x: Int; var y: Int }

var p = Point(x: 1, y: 2)
&p.x += p.y          // ✅ &p.x is inout, p.y is let — disjoint paths
```

#### 4.4.2 Index-level conservatism

The compiler does **not** prove that two array indices are different:

```joyeer
var a = [1, 2, 3]
&a[0] += a[1]        // ❌ error: cannot prove a[0] and a[1] are disjoint
```

Workaround: introduce a `let` snapshot:

```joyeer
let tmp = a[1]
&a[0] += tmp         // ✅
```

Or use a standard-library subscript that takes both indices:

```joyeer
&a.bumpAt(0, by: a[1])      // hypothetical: API takes care of disjointness
```

### 4.5 Subscripts & projection

#### 4.5.1 Accessor blocks

A subscript may expose any combination of `borrowing`, `inout`,
`initializing`, `consuming` accessors (§3.6). Which accessor runs is decided
by the usage context:

| Call-site form | Accessor invoked |
|---------------|-------------------|
| `let v = a[i]` | `borrowing` |
| `&a[i] = expr` or `&a[i] += ...` or `&f(&a[i])` | `inout` |
| `&a[i] = expr` (uninitialized storage) | `initializing` (rare; usually via init helpers) |
| `consume a[i]` | `consuming` |

#### 4.5.2 `yield`

`yield` (and its `yield &` variant for inout) hands control back to the
caller with a projection of the named storage. When the caller's use-site
ends (end of the enclosing statement), control returns to the accessor
and the storage is "ungiven."

```joyeer
extension Buffer {
  public subscript(i: Int): UInt8 {
    borrowing {
      assert(condition: i >= 0 && i < count)
      yield  data[i]
    }
    inout {
      assert(condition: i >= 0 && i < count)
      yield &data[i]
    }
  }
}
```

#### 4.5.3 Lexical lifetime of projections

The lifetime of a yielded projection equals the smallest enclosing
**statement** (not expression). No flow analysis (no NLL-style refinement) —
this is a deliberate simplicity choice for AI tooling.

```joyeer
&a[0] += a[1]        // both projections established at start of this statement
                     //   → conflict detected here
```

#### 4.5.4 Disjoint paths

Paths are sequences of `.field` and `[index]` steps. Two paths are
*disjoint* when they differ at a `.field` step. They are *potentially
overlapping* when they only differ at `[index]` steps (the compiler does
not solve index equality in v0.1).

### 4.6 Last-use optimization (move elision)

When the compiler proves the source of a copy is **not used afterward**,
the copy is downgraded to a move (storage transfer). Example:

```joyeer
var src = makeBigArray()
var dst = src        // last use of src → move, not copy
print(value: dst.count)
```

If `print(value: src.count)` were added between the two lines, the compiler
would instead emit a true copy. Programmers never write `move(x)` — the
compiler infers it.

### 4.7 Deinitialization & destruction order

A value's `deinit` runs when:

1. The owning binding goes out of scope.
2. The binding is **overwritten** by a new value (the previous value's
   `deinit` fires *before* the new value is stored).
3. The value is **consumed** by a `consuming` parameter and the receiver
   finishes with it.

Order within a scope is **reverse declaration order**, for stored fields
of a struct as well. Order is fully deterministic; no finalizer queue,
no GC.

```joyeer
public struct FileHandle {
  var fd: Int32
  public init(path: String) {
    precondition(condition: path.notEmpty())
    fd = sys.open(path: path)
  }
  deinit() {
    sys.close(fd: fd)
  }
}

public func work() {
  var f = FileHandle(path: "a.txt")
  // ...
}  // f.deinit() runs exactly here
```

### 4.8 No reference types

There is no syntax in Joyeer to express "a value that aliases another
value" outside of subscript yields. The following do **not** exist:

- `&T` as a value type
- Pointers (`*T`)
- `Box<T>`, `Rc<T>`, `Arc<T>`
- Weak references

Cycles between values are therefore expressible only via `indirect` enum
cases (§3.4.1) and explicit collection indices. This keeps the memory
model trivially analyzable by both the compiler and AI tools.

### 4.9 Raw memory (FFI)

Out of scope for v0.1. A future `unsafe` block will expose raw pointers
for C interop. Until then, the standard library is the only producer of
heap-backed types (`Array`, `String`, `Dict`).

### 4.10 Return values & ownership escape

A function communicates a result to its caller **only** through its return
value. Because Joyeer has no reference types (§4.8), the return value is the
single mechanism by which a value's ownership may **escape** the frame that
produced it.

> **📌 Decision.** *A function returns **ownership** of its result to the
> caller.*  After the `return`, the callee does not retain, alias, or `deinit`
> the returned value; the destruction obligation (§4.7) transfers to the
> caller. "Who frees the result?" is therefore answerable by simple scanning:
> the value lives until the caller's binding that receives it goes out of
> scope.

Returning a locally-owned binding is a **move** at its last use (§4.6), not a
copy, so the moved-out value's `deinit` does **not** run at the `return` site:

```joyeer
struct FileHandle {
  var fd: Int32
  init(path: String) { fd = sys.open(path: path) }
  deinit() { sys.close(fd: fd) }
}

func openLog(): FileHandle {
  let f = FileHandle(path: "log.txt")
  f                 // last use of f → moved out; f.deinit() does NOT run here
}

func use() {
  let log = openLog()    // caller now owns the handle
  // ...
}                         // log.deinit() runs here — exactly once
```

#### 4.10.1 Returning through `consuming`

A `consuming` parameter or `consuming self` (§4.2.3, §3.2.4) owns its argument
outright. Because an owning binding is mutable in place (§4.1.1), the body may
mutate `self` directly and then move it into the return value — no rebinding to
a local `var` is needed. Ownership flows in at the call site (marked `consume`,
§4.3) and back out through the result:

```joyeer
struct PathBuilder { var buf: String }

extension PathBuilder {
  consuming func join(part: String): PathBuilder {
    &self.buf.append(s: "/")     // self is owned in-body → mutable in place (§4.1.1)
    &self.buf.append(s: part)
    self                         // move out: ownership returns to the caller
  }
}

var p = PathBuilder(buf: "usr")
let full = consume p.join(part: "local").join(part: "bin")
```

Each link consumes its receiver, mutates the owned `self`, and moves it out as
a freshly-owned result; at every point there is exactly one owner (§4.4), and
the transfer is visible at the call site.

#### 4.10.2 In-place result via `initializing`

Returning a large value *by result* would, at an overwrite site, require the
prior value to be destroyed before the new one is stored (§4.7 rule 2). To
avoid that destruct-then-construct round-trip, a function may instead write its
result directly into caller-provided **uninitialized** storage through an
`initializing` parameter (§4.2.4) — the emplace alternative to a return value:

```joyeer
// by-result form
func makeBuffer(size: Int): [UInt8] { ... }

// emplace form — constructs in the caller's storage, no intermediate value
func produceLargeBuffer(out: initializing [UInt8]) {
  &out = makeBuffer(size: 1_000_000)
}

var buf: [UInt8]                  // declared, uninitialized
produceLargeBuffer(out: &buf)     // '&' marks the initializing argument (§4.3)
```

The two forms are observationally equivalent; the `initializing` form exists
for cases where eliding the temporary is required (very large or non-movable
payloads).

#### 4.10.3 No reference or projection may be returned

A function result is always an owned value. The following are **not**
expressible:

- returning a reference (`&T` is not a type, §4.8);
- returning a `borrowing` / `inout` projection of a parameter or local;
- returning the address of a local.

When a caller needs **in-place** access to storage owned by a callee, the
callee exposes a `subscript` whose accessor `yield`s a projection (§4.5.2)
instead of returning it. The projection's lifetime is the enclosing statement
(§4.5.3); it is not a value and cannot be stored:

```joyeer
extension Buffer {
  subscript(i: Int): UInt8 {
    borrowing { yield  data[i] }     // in-place read projection
    inout     { yield &data[i] }     // in-place mutable projection
  }
}

&buf[3] += 1     // projection established and released within this one statement
```

#### 4.10.4 Result, Optional, and diverging returns

Fallible functions return the failure **as a value** — there is no out-of-band
return channel (no exceptions, §8.1). Absence is `Optional<T>` (§2.4);
recoverable failure is `Result<T, E>` (§8.2); both compose with `?` (§8.3) and
`??` (§8.5):

```joyeer
func parseInt(s: String): Result<Int, ParseError> {
  if s.isEmpty() { return .Err(.Empty) }
  // ...
  .Ok(n)                                  // implicit return of the trailing expression
}
```

A function that never returns normally has result type `Never` (§2.9); a
`Never`-typed expression (`return …`, `fatalError(…)`) satisfies any expected
result type and may stand on the right of `??` as an early-exit guard (§8.5).

---

