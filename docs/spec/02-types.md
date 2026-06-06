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

