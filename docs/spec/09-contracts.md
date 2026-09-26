## §9 Contracts & Runtime Checks

Joyeer uses Swift-style runtime checks instead of declaration-level contract
keywords. There is no `requires` / `ensures` / `invariant` syntax in v0.1.

**Implementation status:** this chapter specifies the intended library API.
The current compiler does not provide `assert`, `precondition`, or
`fatalError` in its prelude. Compiler-generated arithmetic and bounds traps
are separate mechanisms. The examples below are draft-library examples, not
programs supported by the current builtin inventory.

### 9.1 Contract style in Joyeer

Contract intent is expressed in executable code:

- Preconditions at API boundaries: `precondition(...)`
- Internal invariants and debug-only checks: `assert(...)`
- Unrecoverable paths: `fatalError(...)`

```joyeer
func divide(a: Int, b: Int): Int {
  precondition(condition: b != 0, message: "divide: denominator must be non-zero")
  return a / b
}
```

### 9.2 Standard checks

```
assert(condition: Bool, message: String = "")
precondition(condition: Bool, message: String = "")
fatalError(message: String): Never
```

- `assert`: for development-time validation. Implementations may elide it in
  optimized builds.
- `precondition`: for caller obligations that must hold in all builds.
- `fatalError`: immediately terminates execution and does not return.

Eliding an `assert` does not disable compiler-generated arithmetic or bounds
checks. Required trapping semantics remain in optimized builds; a redundant
check may be removed only when observable behavior is preserved.

### 9.3 Optional-first contract discipline

Uncertainty is modeled with `T?` (Optional), not with nullable-by-default
references. Code should unwrap explicitly and fail explicitly when required.

```joyeer
func parsePort(s: String): Int {
  let v = parseInt(s: s) ?? fatalError(message: "invalid port")
  precondition(condition: v >= 0 && v <= 65535, message: "port out of range")
  return v
}
```

Use `x!` only when a prior check or control-flow proof guarantees non-`nil`.
If `x!` fails at runtime, execution traps.

### 9.4 Writing postconditions in code

Postconditions are expressed with local snapshots and `assert`:

```joyeer
func bumpAndGet(x: inout Int): Int {
  let old = x
  &x = x + 1
  assert(condition: x == old + 1)
  return x
}
```

### 9.5 Intended compiler treatment

- `assert`, `precondition`, and `fatalError` are ordinary callable symbols
  to be provided by the prelude / standard library.
- Once provided, they are type-checked like normal function calls; declaring
  an ordinary user function with one of these names does not automatically
  give it checking or termination semantics.
- The compiler does not perform theorem proving.

### 9.6 Verification boundary

**Design rationale.** Runtime checks test conditions on an execution;
compile-time checking proves only the properties covered by its rules.
Neither establishes arbitrary program correctness.

Keep three possible layers distinct:

- runtime checks with specified failure and optimization behavior;
- deliberately bounded static analyses, such as type and exhaustiveness
  checking;
- optional solver-assisted verification with explicit proof obligations,
  timeouts, and unproved outcomes.

Refinement types and solver integration remain future design work, not
capabilities implied by the contract APIs. Any future proof system must define
its supported logic and treatment of unproved properties; a timeout or failed
proof must not be reported as successful verification. Automatic proof of
arbitrary properties is not a release promise.

---
