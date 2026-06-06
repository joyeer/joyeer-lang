## §2 Type System

### 2.1 Primitive types

| Type | Bits | Range / domain |
|------|------|----------------|
| `Bool` | 1 (stored as byte) | `true` / `false` |
| `Int` | 64 | signed two's complement |
| `Int8` `Int16` `Int32` `Int64` | as named | signed |
| `UInt` | 64 | unsigned |
| `UInt8` `UInt16` `UInt32` `UInt64` | as named | unsigned |
| `Float` | 32 | IEEE 754 single |
| `Double` | 64 | IEEE 754 double |
| `Char` | 32 | Unicode scalar |
| `String` | (pointer, length) | UTF-8 |
| `Void` / `()` | 0 | unit type |

`Int` and `UInt` are platform-pointer-sized but always 64-bit on the targets
Joyeer supports. (No 32-bit target in v0.1.)

#### 2.1.1 `String` is byte-indexed 📌

> **📌 Decision D13.** *`String` is indexed by byte offset and its element
> type is `UInt8`, not `Char`.*  A `String` is UTF-8 bytes; `s[i]` returns the
> `UInt8` byte at offset `i` in **O(1)**, and `s.count` is the **byte length**.
> Code-point (`Char`) indexing is *not* O(1) over UTF-8, so Joyeer does not
> offer `String[Int] -> Char` (the same reason Swift forbids integer
> subscripts on `String`). This is the Go model: `s[i]` is a byte; iterate
> code points with `.chars()`.

```joyeer
let s = "héllo"      // 'é' is 2 UTF-8 bytes
s.count               // 6 (bytes), not 5
let b: UInt8 = s[0]   // 0x68 ('h'), O(1)
```

| Accessor | Result | Cost | Notes |
|----------|--------|------|-------|
| `s[i]` | `UInt8` | O(1) | byte at offset `i`; traps if out of range |
| `s.count` | `Int` | O(1) | number of **bytes** |
| `s.utf8()` | `[UInt8]` view | O(1) | the underlying bytes |
| `s.chars()` 🔬 | iterator of `Char` | O(n) | decodes Unicode scalars; v0.2 |

For ASCII-oriented work (parsing JSON, tokenizing source, protocol framing)
byte indexing is exactly what is wanted: ASCII structural characters compare
as their byte value, and multi-byte UTF-8 sequences are copied through
verbatim. `Char` (a 32-bit Unicode scalar, §2.1) remains the element type
produced by `.chars()` and written by character literals like `'a'`.

### 2.2 Nominal types

A nominal type is introduced by a `struct` or `enum` declaration (§3.3,
§3.4). Names are PascalCase (§13).

### 2.3 Composite types

```
type            ::= primitive_type
                 |  nominal_type [ generic_args ]
                 |  array_type
                 |  dict_type
                 |  tuple_type
                 |  function_type
                 |  optional_type

array_type      ::= '[' type ']'
dict_type       ::= '[' type ':' type ']'
tuple_type      ::= '(' type , type , ... ')'
function_type   ::= '(' [ param_type , ... ] ')' '->' type
optional_type   ::= type '?'
generic_args    ::= '<' type , ... '>'
```

`function_type` parameters carry effect annotations exactly like declared
functions (§3.2):

```joyeer
let f: (inout Int) -> Int = ...
let g: (consuming String) -> ()  = ...
```

### 2.4 Optional<T>

`T?` is sugar for `Optional<T>`:

```joyeer
enum Optional<T> {
  None,
  Some(T),
}
```

Construction: `nil` for `None`, `T` is implicitly coerced to `Some(T)` in
positions expecting `T?`. Force-unwrap with `x!` (traps on `None`).
Optional chaining with `x?.field` (see §5.4).

### 2.5 Result<T, E>

`Result<T, E>` is a standard-library enum (no special syntax):

```joyeer
enum Result<T, E> {
  Ok(T),
  Err(E),
}
```

See §8 for error-handling semantics.

### 2.6 Generics 📌

> **📌 Decision D2.** *Generic syntax uses `<T>`*  Alternative: Hylo's `[T]`.
> Chosen `<T>` for familiarity with Swift, Rust, C#, TypeScript users.

```joyeer
func swap<T>(a: inout T, b: inout T) { ... }
struct Pair<A, B> { var first: A; var second: B }
extension Array<T> { ... }
```

Constraints via `where` clauses (§3.2.3). v0.1 supports parametric generics
only (no associated types, no higher-kinded types).

### 2.7 Type aliases

```
typealias_decl ::= 'typealias' identifier [ generic_params ] '=' type
```

```joyeer
typealias StringMap<V> = [String: V]
typealias Bytes = [UInt8]
```

### 2.8 Type inference

Local bindings infer their type from the initializer when not annotated:

```joyeer
let x = 42          // inferred Int
let y: Double = 1   // explicit Double (literal coerced)
var a: [Int] = []   // annotation required when initializer is ambiguous
```

Function parameters and return types **must** be annotated explicitly.
This is a deliberate AI-era choice (§0.1): signatures are contracts.

```joyeer
func add(a: Int, b: Int): Int { return a + b }  // ✅
func add(a, b) { return a + b }                  // ❌ error: missing types
```

---

