## §8 Error Handling

**Implementation status:** v0.1 supports `Result`, `Optional`, explicit
`match`, and the typed `readFile` errors below. Postfix `?`, `??`, and several
library helpers used in examples are broader design, not implemented
features. See [the current scope](../plan/v0.1.md).

### 8.1 No exceptions

Joyeer has **no `throw` / `try` / `catch`**, no `errno`, no nullable-by-
default. Errors are values.

### 8.2 Result<T, E>

```joyeer
enum Result<T, E> { Ok(T), Err(E) }
```

A function that can fail returns `Result<T, ErrorEnum>`:

```joyeer
enum ParseError { Empty, Invalid(String) }

func parseInt(s: String): Result<Int, ParseError> {
  if s.isEmpty() { return .Err(.Empty) }
  // ...
  return .Ok(n)
}
```

#### 8.2.1 Typed I/O errors

The prelude defines a stable error enum for file operations:

```joyeer
enum IOError {
  NotFound(Int),
  PermissionDenied(Int),
  InvalidPath(Int),
  Other(Int),
}

func readFile(path: String): Result<String, IOError>
```

The case identifies a portable category. Its `Int` payload preserves the
nonzero platform error code for diagnostics; programs must branch on the case,
not on a particular numeric value. Embedded NUL paths are `InvalidPath`.
Unclassified open, read, size, or close failures are `Other`.

#### 8.2.2 Fallible operations without success data

`Result<Void, E>` reports either success without business data or an error of
type `E`. The successful case still has one logical payload, the unit value:

```joyeer
func complete(failed: Bool): Result<Void, String> {
  if failed { return .Err("operation failed") }
  return .Ok(())
}
```

`.Ok(())`, `.Ok(value)` for a `Void` value, and `.Ok(aVoidReturningCall())`
are valid. The call in the last form is evaluated exactly once. `.Ok()` is
invalid because it supplies no payload; `.Ok(42)` has the wrong payload type.

Unit success adds no resource ownership. An error payload is copied, moved,
and destroyed under the ordinary value rules. Replacing an error with success
must still destroy the old error. The enclosing result retains its case
discriminant and storage needed by `E`; it is not itself a zero-sized value.

The current implementation supports explicit `match` for these results.
Postfix `?` remains separate, unimplemented work.

### 8.3 `?` propagation

Postfix `?` on `Result<T,E>` (and `Optional<T>`) propagates the failure.
For `Result`, it is equivalent to `match x { .Ok(v) => v,
.Err(e) => return .Err(e) }`.

```joyeer
func parsePair(s: String): Result<(Int, Int), ParseError> {
  let parts = s.split(separator: ",")
  let a = parseInt(s: parts[0])?
  let b = parseInt(s: parts[1])?
  .Ok((a, b))
}
```

The `?` operator is only valid in functions whose return type is
`Result<_, E>` or `Optional<_>` and whose `E` is compatible with the
propagated error.

### 8.4 No `try` / `catch`

Reserved keywords ⏳. If error-handling syntax for `Result` chains becomes
ergonomically heavy, a `try-block` may be added in v0.3. The broader design
uses `?` plus `match`; the executable v0.1 subset uses explicit `match`
without propagation syntax.

### 8.5 `??` coalescing

Binary `??` supplies a fallback for a `nil` `Optional` or an `.Err` `Result`.
`a ?? b` evaluates to the unwrapped value when `a` is `.Some` / `.Ok`,
otherwise to `b`. The right operand `b` may be an ordinary value **or** a
diverging expression (`return ...`, `fatalError(...)`), which has the bottom
type `Never` and satisfies any result type.

```joyeer
let c = peek(p: p) ?? return .Err(.UnexpectedEof)            // Optional → early return
let port = parseInt(s: s) ?? fatalError(message: "bad port") // Result → abort
let n = maybe ?? 0                                          // plain fallback value
```

`??` binds looser than the comparison and logical operators and is
right-associative (§5.1).

---
