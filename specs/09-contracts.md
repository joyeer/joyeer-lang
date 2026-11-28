## §9 Contracts & Runtime Checks

Joyeer uses runtime checks instead of declaration-level contract
keywords. There is no `requires` / `ensures` / `invariant` syntax in v0.1.

### 9.1 Contract style in Joyeer

Contract intent is expressed in executable code:

- Preconditions at API boundaries: `precondition(...)`
- Internal invariants: `assert(...)`
- Unrecoverable paths: `fatalError(...)`

```joyeer
func divide(a: Int, b: Int): Int {
  precondition(condition: b != 0, message: "divide: denominator must be non-zero")
  return a / b
}
```

### 9.2 Standard checks

```
func assert(condition: Bool, message: String): Void
func precondition(condition: Bool, message: String): Void
func fatalError(message: String): Never
```

- `assert` checks an invariant; `precondition` checks a caller obligation.
  Both are active in every build. The condition and message are evaluated
  once in order, even when the condition is true, as for any ordinary call.
- The `message:` argument is required and may be empty. No special default
  argument or overload is supplied for these functions.
- A failed check calls `fatalError`, which tries to flush standard output,
  writes `Joyeer runtime error: ` followed by the exact message bytes and a
  newline to standard error, and ends the process with status 1. Host stream
  failures can prevent diagnostic output. The failure path does not unwind
  or run destructors.

Compiler-generated arithmetic and bounds checks are separate. Optimizations
may remove a redundant check only when observable behavior is preserved.

### 9.3 Optional-first contract discipline

Uncertainty is modeled with `T?` (Optional), not with nullable-by-default
references. Code should unwrap explicitly and fail explicitly when required.

```joyeer
func requirePort(parsed: Int?): Int {
  let v = match parsed {
    .Some(port) => port,
    .None => fatalError(message: "invalid port"),
  }
  precondition(condition: v >= 0 && v <= 65535, message: "port out of range")
  return v
}
```

Postfix force unwrap (`x!`) remains deferred. Use an explicit `match`, or `?`
in a function that can propagate the absence.

### 9.4 Writing postconditions in code

Postconditions are expressed with local snapshots and `assert`:

```joyeer
func bumpAndGet(x: inout Int): Int {
  let old = x
  &x = x + 1
  assert(condition: x == old + 1, message: "bump must advance by one")
  return x
}
```

### 9.5 Compiler treatment

- These are ordinary callable functions provided by the bundled prelude.
- They follow ordinary lookup and typing rules. A user function with the
  same name does not gain checking or termination semantics; divergence
  follows from the actual callee's `Never` result.
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
