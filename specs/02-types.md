## §2 Type System

**Implementation status:** this chapter includes broader type-system design.
The executable subset is listed in
[Implemented Language Surface](../docs/impl/supported-features.md). In particular,
the runtime currently preserves arbitrary string bytes without enforcing the
valid-UTF-8 invariant described below. `Result` and `Optional` are
compiler-known builtins in this subset, not user-declared generic enums. Their
declarations below explain the intended shape, not supported generic
declaration syntax.

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

`Int` and `UInt` are fixed 64-bit types, independent of pointer width and
C's `int` or `long`. There is no 32-bit target in v0.1; that does not make
either type an address type.

#### 2.1.1 `String` is byte-indexed

`String` is indexed by byte offset and its element type is `UInt8`, not
`Char`. A `String` is UTF-8 bytes; `s[i]` returns the `UInt8` byte at offset
`i` in **O(1)**, and `s.count` is the **byte length**. Integer subscripts do
not provide code-point (`Char`) indexing; `.chars()` iterates code points.

```joyeer
let s = "héllo"      // 'é' is 2 UTF-8 bytes
s.count               // 6 (bytes), not 5
let b: UInt8 = s[0]   // 0x68 ('h'), O(1)
```

| Accessor | Result | Cost | Notes |
|----------|--------|------|-------|
| `s[i]` | `UInt8` | O(1) | byte at offset `i`; traps if out of range |
| `s.count` | `Int` | O(1) | number of **bytes** |
| `s.utf8()` | `[UInt8]` | O(n) | independent owned copy of the bytes |
| `s.chars()` 🔬 | iterator of `Char` | O(n) | decodes Unicode scalars; v0.2 |

`utf8()` returns an ordinary owned `Array<UInt8>`. Mutating that array does
not mutate the source string, and the array remains valid after the source is
destroyed. A zero-copy byte view would require a scoped non-escaping projection
type; v0.1 does not expose one as a first-class value (§4.8).

For ASCII-oriented work such as tokenizing source or protocol framing, byte
indexing is exactly what is wanted: ASCII structural characters compare as
their byte value, and multi-byte UTF-8 sequences are copied through verbatim.
`Char` (a 32-bit Unicode scalar, §2.1) remains the element type produced by
`.chars()` and written by character literals like `'a'`.

#### 2.1.2 `Void` and the unit value

`Void` has exactly one value, written `()`. In a type position, `()` is an
alias for the builtin `Void` type. These two uses do not require tuple support.
The unit expression and type alias always refer to the builtin type, even
when a user declaration shadows the name `Void`.

```joyeer
func finish(): Void {
    return ()
}

func accept(value: ()): Void {
    return value
}
```

Unit values may be bound, passed, returned, and stored in struct fields, enum
payloads, arrays, dictionary values, `Optional`, and `Result`. Calls returning
`Void` produce the unit value after their side effects complete. Empty blocks,
blocks ending in a binding or loop, and assignment expressions also have
`Void` results. A bare `return` remains valid in a `Void` function.

Unit values own no resources and have zero-sized data representation.
Initialization, mutability, access exclusivity, and consumption rules still
apply. Containers and enclosing enums can have storage and resources of their
own; a zero-sized element does not make its container allocation-free.

The pattern `()` matches a `Void` value exhaustively. It does not implicitly
unwrap an optional or result. Unit equality, ordering, printing, and dictionary
keys are not part of the implemented primitive operations; use a unit pattern
or wildcard when matching. See §7 and §8.2.2.

#### 2.1.3 C boundary representations

The [C scalar rules](19-c-interop.md#194-c-scalar-representations) define
which primitives may cross a restricted native binding and how they map to
the target C ABI. Ordinary Joyeer storage or calling conventions do not
establish C compatibility. In particular, `Bool` maps to C `_Bool`, not a
platform integer Boolean; `Char` is not C `char`; and `Void` is permitted
only as a native result, not a native parameter.

These rules include the implemented fixed-width integer and floating-point
scalar boundary. They introduce no
pointer types or C-specific source aliases, and do not give `String`,
`Optional`, `Result`, or ordinary aggregates a C-compatible layout.

### 2.2 Nominal types

A nominal type is introduced by a `struct` or `enum` declaration (§3.3,
§3.4). Names are PascalCase (§13).

### 2.3 Composite types

```
type            ::= primary_type [ '?' ]                    // optional_type
primary_type    ::= primitive_type
                 |  '(' ')'                                 // Void type alias
                 |  nominal_type [ generic_args ]
                 |  array_type
                 |  dict_type
                 |  tuple_type
                 |  function_type
                 |  '(' function_type ')'

array_type      ::= '[' type ']'
dict_type       ::= '[' type ':' type ']'
tuple_type      ::= '(' type , type , ... ')'
function_type   ::= '(' [ param_type , ... ] ')' ':' type
param_type      ::= [ access_effect ] type        // access_effect defined in §3.2
generic_args    ::= '<' type , ... '>'
```

#### 2.3.1 Function types

A function type names the parameters' access effects and types and the
result type, without parameter labels ([decision 0020](../docs/decisions/0020-function-values.md)):

```joyeer
let f: (inout Int): Int = bump
let g: (consuming String): () = discard
let h: (): () = tick
```

The inner `:` is the function type's result separator (§3.2 uses the same `:`
for declared functions); the outer `:` in `let f: ...` is the binding's type
annotation.

- Each parameter has an access effect (§4.2): `borrowing` (the default; writing
  it gives the same type), `inout`, `consuming` or `initializing`. A parameter
  type is any type except `Never`.
- The result is mandatory and may be any type, `()` and `Never` included:
  `(Int)` is not a type; write `(Int): ()`.
- A parameter has no label: `(x: Int): Int` is an error
  (`parser.labeled-function-type-parameter`).
- The result type extends as far right as possible: `(Int): Int?` returns
  `Int?`, and `(Int): (Int): Int` returns a function. An optional function is
  written `((Int): Int)?`; parentheses around a type are allowed only around a
  function type.
- Inside `[` `]`, a parenthesized list followed by `:` is a function type, so
  `[(): Int]` and `[(Int): Bool]` are arrays of functions.
- Two function types are the same type when their parameter count, every
  effect, every parameter type and the result are identical. There is no
  subtyping, conversion or variance between function types, `Never` results
  included (§2.9).

A value of a function type is a **function value**: a reference to a top-level
function, with no captured state (closures are deferred, §5.11). How a function
becomes a value and how a value is called are in §5.6.1. Function values are
copyable, never linear, and trivially destroyed (§4.1.1); they cross tasks and
channels freely (§21), because they hold no state. A function type has no
equality: `==` is rejected, it cannot be a dictionary key, and `print` does not
accept it.

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

### 2.6 Generics

v0.1 has **no user-defined generics**. The only generic types are the built-in
types `Array<T>` / `[T]`, `Dict<K, V>` / `[K: V]`, `Optional<T>` / `T?`, and
`Result<T, E>`. Their angle brackets are **type arguments understood directly
by the compiler**, not a general type-parameter mechanism. User code may
*use* these containers but may not declare new generic `func` / `struct` /
`enum`, type parameters, or constraints.

```joyeer
let xs: [Int] = [1, 2, 3]               // built-in Array<Int>
let m: [String: Int] = [:]             // built-in Dict<String, Int>
let r: Result<Int, ParseError> = .Ok(1)
```

User-defined generics will use `<T>`. They, along with constraints,
associated types, and monomorphization, are reserved for a future version
(Appendix A).

#### 2.6.1 Safe dictionary lookup

The builtin `Dict<K, V>.get(key:)` takes a key of type `K` and returns
`Optional<V>`. It borrows both the receiver and key, evaluates the receiver
before the key exactly once each, and does not require `&` or `consume`.
Ordinary access exclusivity and initialization rules still apply.

A present key produces `.Some` containing an independently owned copy of the
stored value. A missing key produces `.None` without trapping or copying a
value. Lookup does not change the dictionary. Later dictionary mutation or
destruction cannot invalidate the returned value.

```joyeer
func lookup(values: [String: Int], key: String): Int? {
    return values.get(key: key)
}
```

No optional layer is flattened. For a dictionary whose value type is `Int?`,
the return type is `Optional<Int?>`: `.None` means the key is missing,
`.Some(.None)` means a present key stores absence, and `.Some(.Some(42))`
means it stores `42`. `Void` values likewise distinguish `.Some(())` from
`.None`. The result supports ordinary matching and postfix `?` propagation.

Strict `dictionary[key]` access retains its missing-key trap. This operation
does not introduce user-defined generic methods or key enumeration. The current
lookup is linear; owned heap-backed values may require recursive copying and
allocation. The receiver dictionary itself is not copied by `get`.

#### 2.6.2 Removing the last array element

The builtin `Array<T>.removeLast()` is a mutating method with no arguments. It
needs a mutable receiver and the `&` marker (§4.3), and returns `Optional<T>`:
`.Some` holding the last element, which is removed from the array, or `.None`
when the array is empty.

```joyeer
var queue: [Int] = [1, 2, 3]
let last = &queue.removeLast()    // .Some(3); queue is now [1, 2]
```

The element moves into the result. It is neither copied nor destroyed by the
call, so `removeLast` works for a noncopyable element (§4.1.1) and is the way to
take one out of an array: for `[TcpConnection]` it returns an
`Optional<TcpConnection>` that owns the handle. The result is an owned
temporary, and a `match` that moves the payload (§7.1.2) can take the element out
of it. **Implementation status:** implemented; the runtime
removes the element in constant time and does not shrink the array's capacity.

#### 2.6.3 Concurrency builtin types (provisional)

The concurrency design ([decision 0013](../docs/decisions/0013-concurrency-model.md)) adds three
compiler-known generic types, in the same manner as `Array`, `Dict`, `Optional` and `Result`:
`JoinHandle<R>`, `ChannelSender<T>` and `ChannelReceiver<T>` (the first draft's `SendError<T>` is dropped from
v1, [decision 0019](../docs/decisions/0019-channels.md)), and the builtin enumeration `ChannelClosed`. Their type
arguments are understood by the compiler, not by a general parameter mechanism, so this does not
introduce user-defined generics. A user declaration of the same name takes precedence over the
builtin in its scope.

- `JoinHandle<R>` is **linear** (§4.1.1): it must be ended, by `join`, on every path (§4.7.1). Tasks are specified in [§21](21-concurrency.md).
- `ChannelSender<T>` and `ChannelReceiver<T>` are **noncopyable** and close their channel when
  destroyed, like a handle with a `deinit` (§3.7.1); an explicit consuming `close` also exists.
  `T` may not be linear, because a value queued when the channel closes would be destroyed; `Any` and
  `Never` are not element types either ([§21.9](21-concurrency.md#219-channels)).
- A channel is created by `channel(capacity:, sender: &s, receiver: &r)`, a builtin returning
  `Void` whose two outputs are `initializing` parameters (§4.10), so both variables are declared,
  uninitialised, with their types, and the element type comes from those types, which must
  agree. A capacity below 1 traps, and a literal below 1 is a compile-time error (§21.9.1).

```joyeer
var sender: ChannelSender<Int>
var receiver: ChannelReceiver<Int>
channel(capacity: 8, sender: &sender, receiver: &receiver)
```

**Implementation status:** `JoinHandle<R>`, `spawn` and `join`, and the channel types, `channel`, `send`,
`receive` and `close`, are implemented ([§21](21-concurrency.md)).

#### 2.6.4 Ending an empty array: `consumeEmpty()`

`Array<T>.consumeEmpty()` is a **consuming** member (spelled `consume a.consumeEmpty()`). If the array
is empty it frees the array's storage and the array is ended (§4.2.3); if it is not empty the program
traps with `array not empty` before any element is touched. It is the only way to end an `Array` of
**linear** elements (§4.7.1), which `removeLast()` drains first, and may be used on any array. It
returns `Void`. In this version the receiver must be a variable, a parameter or a field, not a
temporary.

**Implementation status:** implemented (stage 0 of P5a, [decision 0018](../docs/decisions/0018-tasks.md)): a consuming builtin member, the `array_consume_empty` IR instruction (wire version 13) and `joyeer_array_consume_empty_abi` in the runtime.

### 2.8 Type inference

Local bindings infer their type from the initializer when not annotated:

```joyeer
let x = 42          // inferred Int
let y: Double = 1   // explicit Double (literal coerced)
var a: [Int] = []   // annotation required when initializer is ambiguous
```

Function parameter types **must** be annotated explicitly. An omitted function
result type means `Void`; a value-returning function needs an explicit result
type. This is a deliberate AI-era choice (§0.1): signatures are contracts.

C bindings are stricter: [§19.2](19-c-interop.md#192-declarations-and-annotation-arguments)
requires an explicit result type even for `Void`.

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

The subtyping applies to expressions only. It does not extend to function types
(§2.3.1): a `(Int): Never` value is not a `(Int): Int` value, and `Never` is not
allowed as a function-type parameter.

`Never` may appear in a signature as an explicit "this function never returns"
marker:

```joyeer
func fatalError(message: String): Never { ... }
```

---
