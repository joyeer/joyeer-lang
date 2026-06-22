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

### 4.2 Access effects on parameters

A function parameter declares **how** the function will access the
argument's storage. The effect is part of the function signature; callers
must match it explicitly at the call site (§4.3).

| Effect | Meaning | Caller obligation | In-body usage |
|--------|---------|-------------------|----------------|
| `borrowing` (default) | Read-only projection. Multiple `borrowing` projections of the same value may coexist. | Pass without marker (may write `borrowing x` for emphasis). | Use as immutable value. |
| `inout` | **Exclusive** mutable projection. While held, the original storage is inaccessible to anyone else. | Mark with `&x` at call site. | Use and mutate like a local `var`. |
| `consuming` | **Consume** the argument. Caller's binding becomes uninitialized after the call. | Mark with `consume x` at call site; caller must own the value. | Owned outright; may be moved into return value, into another `consuming` parameter, or destroyed. |
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
func store(s: consuming String) { /* s is mine; printable, destroyable, returnable */ }

var greeting = "hello"
store(s: consume greeting)
// print(value: greeting)    // ❌ error: use of consumed value 'greeting'
```

After the call, `greeting` is in an **uninitialized state**. The compiler
rejects any subsequent read. A subsequent assignment (`greeting = "world"`)
re-initializes the storage and re-enables reads.

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

---

