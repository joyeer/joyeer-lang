## §8 Error Handling

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

### 8.3 `?` propagation 📌

> **📌 Decision D6.** *Postfix `?` on `Result<T,E>` (and `Optional<T>`)
> propagates the failure.*  Equivalent to `match x { .Ok(v) => v,
> .Err(e) => return .Err(e) }`.

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
ergonomically heavy, a `try-block` may be added in v0.3. For v0.1, `?`
plus `match` covers all cases.

---

