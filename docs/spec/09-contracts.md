## §9 Contracts & Runtime Checks

Joyeer uses Swift-style runtime checks instead of declaration-level contract
keywords. There is no `requires` / `ensures` / `invariant` syntax in v0.1.

### 9.1 Contract style in Joyeer

Contract intent is expressed in executable code:

- Preconditions at API boundaries: `precondition(...)`
- Internal invariants and debug-only checks: `assert(...)`
- Unrecoverable paths: `fatalError(...)`

```joyeer
func divide(_ a: Int, _ b: Int): Int {
  precondition(b != 0, "divide: denominator must be non-zero")
  return a / b
}
```

### 9.2 Standard checks

```
assert(condition: Bool, _ message: String = "")
precondition(condition: Bool, _ message: String = "")
fatalError(_ message: String)
```

- `assert`: for development-time validation. Implementations may elide it in
  optimized builds.
- `precondition`: for caller obligations that must hold in all builds.
- `fatalError`: immediately terminates execution and does not return.

### 9.3 Optional-first contract discipline

Uncertainty is modeled with `T?` (Optional), not with nullable-by-default
references. Code should unwrap explicitly and fail explicitly when required.

```joyeer
func parsePort(_ s: String): Int {
  let v = parseInt(s) ?? fatalError("invalid port")
  precondition(v >= 0 && v <= 65535, "port out of range")
  return v
}
```

Use `x!` only when a prior check or control-flow proof guarantees non-`nil`.
If `x!` fails at runtime, execution traps.

### 9.4 Writing postconditions in code

Postconditions are expressed with local snapshots and `assert`:

```joyeer
func bumpAndGet(_ x: inout Int): Int {
  let old = x
  &x += 1
  assert(x == old + 1)
  return x
}
```

### 9.5 v0.1 compiler treatment

- `assert`, `precondition`, and `fatalError` are ordinary callable symbols
  provided by the prelude / standard library.
- They are type-checked like normal function calls.
- The compiler does not perform theorem proving.

### 9.6 Migration note

Specs and code that previously used declaration-level contracts should be
migrated mechanically:

- `requires P` -> `precondition(P)` at function entry
- `ensures Q`  -> `assert(Q)` before each return (or before final return)
- `invariant I` -> `assert(I)` at loop/struct consistency checkpoints

---

