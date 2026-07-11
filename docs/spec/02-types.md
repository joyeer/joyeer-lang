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

> **📌 Decision.** *`String` is indexed by byte offset and its element
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
function_type   ::= '(' [ param_type , ... ] ')' ':' type
param_type      ::= [ access_effect ] type        // access_effect defined in §3.2
optional_type   ::= type '?'
generic_args    ::= '<' type , ... '>'
```

`function_type` parameters carry effect annotations exactly like declared
functions (§3.2):

```joyeer
let f: (inout Int): Int = ...
let g: (consuming String): ()  = ...
```

The inner `:` is the function type's return separator (§3.2 uses the same `:`
for declared functions); the outer `:` in `let f: ...` is the binding's type
annotation.

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

> **📌 Decision.** *v0.1 has **no user-defined generics**.* The only generic
> types are the built-in types `Array<T>` / `[T]`, `Dict<K, V>` /
> `[K: V]`, `Optional<T>` / `T?`, and `Result<T, E>`. Their angle brackets are
> **type arguments understood directly by the compiler**, not a general
> type-parameter mechanism. User code may *use* these containers but may not
> declare new generic `func` / `struct` / `enum`, type parameters, or
> constraints.

```joyeer
let xs: [Int] = [1, 2, 3]               // built-in Array<Int>
let m: [String: Int] = [:]             // built-in Dict<String, Int>
let r: Result<Int, ParseError> = .Ok(1)
```

> **📌 Decision.** *When generics are reintroduced they will use `<T>`*
> (not Hylo's `[T]`), for familiarity with Swift / Rust / C# / TypeScript
> users. User-defined generics, constraints, associated types, and
> monomorphization are reserved for a future version (§15); see
> [../plan/v0.1.md](../plan/v0.1.md) for the milestone rationale.

### 2.7 Type aliases

```
typealias_decl ::= 'typealias' identifier '=' type
```

```joyeer
typealias Bytes = [UInt8]
typealias IntMap = [String: Int]
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

### 2.9 The `Never` type

`Never` is the **uninhabited bottom type**: it has no values. A *diverging*
expression — `return ...`, `fatalError(...)`, or any call to a function whose
return type is `Never` — has type `Never`, which is a subtype of every type.
A diverging expression is therefore well-typed in any position that expects a
value. This is what lets the right operand of `??` be `return .Err(e)` or
`fatalError(...)` (§5.1, §8.5, §9.3).

`Never` may appear in a signature as an explicit "this function never returns"
marker:

```joyeer
func fatalError(message: String): Never { ... }
```

---

