## §7 Match Patterns

Patterns are parser/type-checker constructs used in `let`/`var`, `match`
arms, and `for-in`; they are not a separate lexer subsystem.

```
pattern         ::= '_'                                       // wildcard
                 |  identifier                                 // bind
                 |  literal                                    // literal compare
                 |  '(' ')'                                    // unit
                 |  '(' pattern , ... ')'                       // tuple
                 |  enum_case_pattern

enum_case_pattern
                ::= [ type ] '.' identifier [ '(' enum_pattern_arg , ... ')' ]
enum_pattern_arg
                ::= [ label ':' ] pattern
```

> **Note.** Type-test / cast patterns (`x as T`) and the `is` / `as` type-test
> operators are deferred to v0.2 (§15). v0.1 has no runtime type tests because
> it has no subtyping, protocols, or inheritance, so there is no type
> relationship to query.

### 7.1 Identifier

Binds a fresh name to the matched value:

```joyeer
match reading {
  .Value(n) => print(value: n),    // n bound
  _ => (),
}
```

An enum payload label is present in a pattern exactly when that payload
position was labeled in the case declaration (§3.4):

```joyeer
match error {
  .Unexpected(byte, at: position) => position,
}
```

### 7.1.1 Unit pattern

The unit pattern `()` requires a scrutinee of type `Void` and covers its only
value. It may appear inside an enum payload pattern:

```joyeer
match outcome {
  .Ok(()) => print(value: "completed"),
  .Err(error) => print(value: error),
}
```

Here `outcome` can have type `Result<Void, String>`. `.Ok(_)` and `.Ok(value)`
are also valid, with `value` bound as `Void`. An empty pattern clause `.Ok()`
is invalid. General tuple patterns remain outside the implemented subset.

### 7.1.2 Binding a noncopyable payload

An identifier pattern normally binds a copy of the matched value. A payload whose
type is noncopyable (§4.1.1) cannot be copied, so a pattern that binds one
**moves** it out of the scrutinee, and the scrutinee must be something that can
give it up: an owned temporary, or `consume` of an owning local or consuming
parameter (§4.3).

```joyeer
func close(slot: consuming Slot): Int {
  return match consume slot {
    .Full(connection) => connection.id,   // `connection` owns the handle
    .Empty => 0,
  }
}

match tcpAccept(listener: listener, timeoutMillis: 5000) {    // an owned temporary
  .Ok(connection) => serve(connection: consume connection),
  .Err(error) => report(error: error),
}
```

The binding is an owning local of its arm: it is destroyed when the arm ends
unless the arm moves it on (`consume connection`). Payloads the pattern does not
bind, including those matched by `_`, stay in the scrutinee and are destroyed
with it. Matching a place without `consume` while binding a noncopyable payload
is `type-checking.copy-of-noncopyable`. A `match consume x` consumes all of `x`:
`x` may not be read again until it is reinitialized. **Implementation status:**
implemented for enum payloads, including nested enum patterns.

### 7.2 Alternative patterns

A match arm may list several comma-separated patterns; the arm runs when
**any** of them matches. To keep each arm's bindings unambiguous,
alternative patterns may only combine patterns that bind no variables
(wildcards and literals):

```joyeer
let category = match byte {
  b'+', b'-'       => "sign",
  b'0', b'1', b'2' => "digit",
  b' ', b'\t'      => "space",
  _                => "other",
}
```

### 7.6 Where guards

```joyeer
match reading {
  .Value(n) where n > 0 => "positive",
  .Value(_)             => "non-positive",
  .Missing              => "missing",
}
```

### 7.7 Exhaustiveness

`match` over an enum or `Bool` must cover all cases or include a `_`
arm. Missing cases are a compile error with a list of the missing
constructors.

---
