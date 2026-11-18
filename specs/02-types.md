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

A byte is read-only, whatever the binding: no subscript writes into a
`String`, so `&s[i] = b` and a compound assignment to `s[i]` are
`type-checking.assignment-to-immutable`, `&s[i]` as an `inout` argument is
`type-checking.invalid-inout-argument`, and `consume s[i]` is
`type-checking.invalid-consume-argument` (copy the byte into a binding with
`let byte = s[i]` and consume that). Build a new `String` instead.

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

`T?` is sugar for `Optional<T>`. `Optional` is an enum declared in the compiler-bundled
prelude and bound to the language by the `optional` role ([decision 0024](../docs/decisions/0024-library-optional-and-result.md)):

```joyeer
enum Optional<T> {
  None,
  Some(T),
}
```

The case order shown is illustrative; case tag numbering is not part of the language (§19).

The sugar binds to that declaration, not to the name `Optional`: a user-declared
`enum Optional<T>` shadows the name in its scope but never changes the meaning of `T?`, `nil`,
implicit wrapping, `x!`, `x?.field`, or `?`. The one-`?` look-through of an expected type (§20.12.1) does not apply when
the type being constructed is `Optional` itself, so `let x: Optional<Int?> = nil` is the outer `None` (`Int??` is not written: it lexes as the `??` operator).
`==` and `!=` are not defined on `Optional` (§5.2).

Construction: `nil` for `None`, `T` is implicitly coerced to `Some(T)` in
positions expecting `T?`. Force-unwrap with `x!` (traps on `None`).
Optional chaining with `x?.field` (see §5.4).

### 2.5 Result<T, E>

`Result<T, E>` is an enum declared in the compiler-bundled prelude and bound to the language by the
`result` role (no special syntax; [decision 0024](../docs/decisions/0024-library-optional-and-result.md)):

```joyeer
enum Result<T, E> {
  Ok(T),
  Err(E),
}
```

`?` propagation (§8.3) binds to the role, not to the name. `==` and `!=` are not defined on `Result`
(§5.2). See §8 for error-handling semantics.

### 2.6 Generics

The generic containers are `Array<T>` / `[T]`, `Dict<K, V>` /
`[K: V]`, `Optional<T>` / `T?`, and `Result<T, E>` (and the concurrency types
below). `Array` and `Dict` are source-library structs in `lib/Runtime/collections.joyeer`, bound by
`array_type` and `dict_type` (§20.12.5); `Optional` and `Result` are prelude enums bound by roles (§2.4, §2.5).
No generic container is compiler-defined. User code may *use* these containers and may declare **generic
top-level functions** and **generic `struct`s and `enum`s** bounded by protocols
(§20, §20.12, [decision 0022](../docs/decisions/0022-generic-types-and-layout-intrinsics.md)),
but may not declare generic methods or conditional conformances (§20.10).

```joyeer
let xs: [Int] = [1, 2, 3]               // library Array<Int>
let m: [String: Int] = [:]             // library Dict<String, Int>
let r: Result<Int, ParseError> = .Ok(1)
```

Generic functions use `<T: P & Q>` and are instantiated by monomorphization
(§20.4, §20.6). A type parameter may be a container's element, payload or
error type (`[T]`, `T?`, `Result<T, E>`, `[String: T]`), but not a `Dict` key
type (§20.5). Associated types remain deferred (§20.10).

`[T]` and array literals always bind to the declaration carrying `array_type`, regardless of
names in scope. `Array<T>` is its ordinary nominal spelling; a user type named `Array` shadows
that spelling but not `[T]`, and diagnostics qualify the user type to avoid ambiguity. Role-array
types display as `[T]`. The library exposes read-only `count`, mutating `append(element:)` and
`removeLast()`, and consuming `consumeEmpty()`. `append` stores an owned element: an unmarked
place is independently copied when copyable, an owned temporary transfers, and `consume` explicitly
moves an owning place (§4.3). The address and capacity fields and compiler-called helper methods are private.
All literal elements are evaluated left to right as owned temporaries before storage is filled;
an early exit in a later element destroys earlier affine temporaries in reverse order (§4.7).

`[K: V]` and dictionary literals likewise bind to the declaration carrying `dict_type`
([0027](../docs/decisions/0027-library-dict.md)); `Dict<K, V>` is its nominal spelling, a user type named
`Dict` shadows only that spelling, and role-dictionary types display as `[K: V]`. The library exposes
read-only `count` and `get(key:)` (§2.6.1); `d[k]` reads, borrows, consumes or mutates a stored value in
place and traps `dictionary key not found` for a missing key, and `&d[k] = v` inserts or replaces. In
`&d[k] = v` the base's index operands, the key and the value are evaluated, in that order, before the base
is addressed (§4.4.3), so the value is evaluated before a missing outer key traps, and a value that
exits early (`&d[missing][k] = f()?` when `f` fails) leaves without trapping. The evaluated keys are kept
as they were read: a value that consumes or reassigns the key's binding does not change which entry is
addressed. The address and
capacity fields and the compiler-called methods are private. A literal evaluates every key and value
left to right before any entry is stored; a duplicate key keeps the last value.

**Dictionary keys (closed until plan 0012).** The admitted key types are exactly `Int`, `Bool`, `UInt8`
and `String`, by type, not by spelling. Any other key, written at a `[K: V]` annotation, a `Dict<K, V>`
or a literal, is `type-checking.invalid-type-argument`; a function type, a type parameter or `Never` as a
key keeps its own diagnostic. Keys are compared by the runtime's key kind; lookup is a linear scan.

#### 2.6.1 Safe dictionary lookup

The library `Dict<K, V>.get(key:)` (the `dict_get` method, §20.12.5) takes a key of type `K` and returns
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

The library `Array<T>.removeLast()` is a mutating method with no arguments. It
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

The source implementation calls the existing runtime array ABI
([decision 0026](../docs/decisions/0026-library-array.md)); the three-word `JoyeerArray` layout,
ownership behavior and trap texts are unchanged. Capacity, geometric growth and header size are
runtime implementation details, not language guarantees.

#### 2.6.3 Concurrency library types

The source prelude declares `JoinHandle<R>`, `ChannelSender<T>`, `ChannelReceiver<T>`, and the
enumeration `ChannelClosed` ([decision 0023](../docs/decisions/0023-library-concurrency.md)).
They use the generic declaration mechanism of §20.12, not dedicated compiler type kinds.
The first draft's `SendError<T>` remains out of v1 ([decision 0019](../docs/decisions/0019-channels.md)).
A user declaration of the same name takes precedence over the prelude declaration in its scope.

- `JoinHandle<R>` is **linear** (§4.1.1): it must be ended, by `join`, on every path (§4.7.1). Tasks are specified in [§21](21-concurrency.md).
- `ChannelSender<T>` and `ChannelReceiver<T>` are **noncopyable** and close their channel when
  destroyed, like a handle with a `deinit` (§3.7.1); an explicit consuming `close` also exists.
  `T` may not be linear, because a value queued when the channel closes would be destroyed; `Any` and
  `Never` are not element types either ([§21.9](21-concurrency.md#219-channels)).
- A channel is created by `channel(capacity:, sender: &s, receiver: &r)`, a prelude library function returning
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

**Implementation status:** implemented as the library method bound to `container_end`, calling
`joyeer_array_consume_empty_abi`. Its consumed receiver is destroyed after the method returns;
the runtime has already zeroed the empty handle, so that destruction is inert.

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

`Never` is not a type argument of a generic function (§20.4.1), whether
written or inferred: a `Result<Int, Never>` argument for a `Result<T, E>`
parameter is rejected (`type-checking.invalid-type-argument`).

`Never` is not the type of a stored field of a structure, because no value of
that structure could be constructed (`type-checking.never-field-type`).

`Never` is not the element type of an array, written `[Never]` or `Array<Never>`
or inferred from a literal such as `[return]`: such an array could never hold an
element, and iterating or indexing it would bind a value that cannot exist. It is
rejected with `type-checking.invalid-type-argument`, like `Never` in any other
generic. `Never` inside an optional element (`[Never?]`) is an ordinary element,
because `.None` is a value. The same holds for a dictionary: `Never` is neither a key
nor a value type (`[String: Never]`, `[Never: Int]`, or inferred from a literal such as
`["a": return]`).

`Never` may appear in a signature as an explicit "this function never returns"
marker:

```joyeer
func fatalError(message: String): Never { ... }
```

---
