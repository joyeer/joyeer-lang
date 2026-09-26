# Joyeer Language — AI Era Design Considerations

> **Status:** design rationale, not an implementation checklist. The current
> compiler boundary is described in
> [Implemented Language Surface](../impl/supported-features.md).
> Contracts, annotations, incremental compilation, and formal verification
> below are design directions, not delivered compiler guarantees.

## The Shift in Programming

```
Past:   Human writes every line → Compiler checks → Machine executes
Now:    Human describes intent → AI generates code → Human reviews → Compiler checks → Machine executes
Future: Human describes intent → AI generates + verifies → Machine executes
```

## Design Optimization Shift

| Past (human writes code) | Future (AI writes, human reviews) |
|--------------------------|-----------------------------------|
| Minimize keystrokes | Maximize correctness verifiability |
| Syntax brevity | Semantic clarity (less ambiguity) |
| Flexibility | Constraints (stronger types = fewer AI mistakes) |
| Readability for writing | Readability for reviewing |

---

## Key Features for an AI-Era Language

### 1. Strong Type System — Guardrails for AI

AI-generated code's biggest risk: looks correct but has subtle bugs.
A strong type system catches important classes of errors automatically; it
does not prove that a program satisfies its intended business logic.

```
// Weak: AI generates this, compiles, crashes at runtime
func transfer(from, to, amount) {
    from.balance -= amount
    to.balance += amount
}

// Strong + explicit checks: correctness is reviewable
func transfer(from: inout Account, to: inout Account, amount: Int) {
    precondition(condition: amount > 0)
    precondition(condition: from.balance >= amount)
    let oldFrom = from.balance
    &from.balance = from.balance - amount
    &to.balance = to.balance + amount
    assert(condition: from.balance == oldFrom - amount)
}
```

References: Dependent types, Design by Contract, Refinement types

### 2. Formal Verification — Prove Code Correct

```
func sort(arr: [Int]): [Int] {
    var out = arr
    // ... AI generates the sorting implementation ...
    assert(condition: out.count == arr.count)
    assert(condition: isPermutation(a: out, b: arr))
    assert(condition: isSorted(a: out))
    return out
}
// This sketches the runtime-checking API specified in §9, not current MVP
// library support. Static verification of these properties is future work.
```

References: Dafny, Lean 4, F*, Ada/SPARK

Automatic proof of arbitrary program properties is not a release promise.
Any future verification integration must define its supported logic, proof
obligations, timeout behavior, and what happens when a property cannot be proved.

A feasible progression keeps distinct guarantees separate:

| Layer | Possible guarantee |
|---|---|
| Runtime checks | Execute specified preconditions, assertions, arithmetic checks, and bounds checks with defined optimization behavior |
| Decidable static checks | Prove deliberately bounded properties such as type compatibility, exhaustiveness, nullability, and selected ranges |
| Solver-assisted checks | Attempt explicitly scoped proof obligations with visible timeout and unproved outcomes |

The first two layers can grow independently of solver integration. A failed or
timed-out proof must never silently become a successful verification result.

### 3. Explicit State Changes Without a General Effect System

Joyeer deliberately does not add a `performs` / `pure` effect language. The
implementation and annotation burden is not justified for the initial systems
language core. Instead, the high-risk state transitions remain explicit:

- `&` marks exclusive mutation;
- `consume` marks ownership transfer;
- `Result<T, E>` represents recoverable failure;
- `precondition` / `assert` document executable obligations.

I/O and allocation are ordinary APIs rather than type-level effect labels.
This keeps the lexer, parser, function type identity, and higher-order APIs
smaller while preserving the ownership changes reviewers most need to see.

### 4. No Implicit Behavior — Semantic Clarity

```
// Bad: 50 ways to do the same thing (C++)
// AI mixes styles, produces inconsistent code

// Good: one way to do each thing
// Error handling is only Result — no exceptions, error codes, errno, optional all at once
```

```
let x = a + b    // Static operand types determine the operation:
                // Int + Int adds; String + String concatenates.
                // Mixed operands do not trigger dynamic coercion.
```

### 5. Incremental Compilation — Real-time AI Collaboration

```
Future workflow: AI edits code → Compiler provides incremental feedback:
  ✅ Types correct
  ⚠️ This branch doesn't handle None
  ❌ A `precondition` may not hold
```

Incremental compilation must be designed into the language, not added later.
The current CLI compiles a single source file through the pipeline; there is
no incremental invalidation engine or measured interactive-latency guarantee.

### 6. Natural Language Integration — Intent to Code Bridge

```
@spec "Returns the nth Fibonacci number"
@property fib(n: 0) == 0
@property fib(n: 1) == 1
@property forall n in 2..<20: fib(n: n) == fib(n: n-1) + fib(n: n-2)
func fib(n: Nat): Nat {
    // AI generates implementation from @spec and @property
    // Compiler uses @property for property-based testing
}
```

---

## Existing Languages in the AI Era

The following is a qualitative design comparison, not a measured ranking of
AI-generated code quality.

| Language | AI Friendliness | Reason |
|----------|----------------|--------|
| **Lean 4** | ★★★★★ | Theorem proving + programming unified |
| **Dafny** | ★★★★★ | Built-in verification conditions |
| **Rust** | ★★★★ | Strong types + ownership = strong guardrails |
| **F#/OCaml** | ★★★★ | Algebraic types + immutable-first, clear semantics |
| **TypeScript** | ★★★ | Good type system, but JavaScript semantics have traps |
| **Python** | ★★ | Most used by AI, but dynamic types = runtime-only errors |
| **C/C++** | ★ | Too much undefined behavior, AI easily writes hidden bugs |

---

## Design Philosophy Shift

```
Old philosophy (human writes code):
  "Let programmers type less"     → syntax sugar, implicit conversion, overloading
  "Flexibility first"            → dynamic types, duck typing, metaprogramming

New philosophy (AI writes code + human reviews):
  "Prevent classes of errors"   → strong types, contracts, formal verification
  "Make intent visible"          → explicit mutation, ownership transfer, docs as code
  "Automate verification"       → property testing, theorem proving, invariant checking
  "Make review efficient"       → readability, consistency, no implicit behavior
```

---

## Joyeer AI-Era Feature Candidates

Candidate priorities; these estimates are not implementation commitments:

| Feature | Difficulty | Impact |
|---------|-----------|--------|
| Contracts (precondition/assert) | Medium | High |
| Property-based testing built-in (@property) | Requires syntax, generators, and a runner | Medium |
| Refinement types (PositiveInt, NonEmpty) | Hard | High |
| Incremental compilation (language-level design) | Architecture | High |
| Formal verification integration | Very Hard | Very High |

The current guardrails are static typing and explicit ownership conventions.
Runtime checks, property testing, and verification tooling can extend them
once their semantics and implementation are ready.

## Feasibility and evidence boundaries

The individual systems-language mechanisms have substantial precedent, but
their combination still requires Joyeer-specific evidence. The native JSON
parser demonstrates a scoped integration of types, ownership, containers,
control flow, and deterministic cleanup; it does not establish broad
ownership ergonomics, ABI cost, platform completeness, or the performance of
future features.

Two design boundaries need particular care:

- Longer-lived projections must be tested on mutation-heavy workloads before
  the exclusivity model is made more permissive.
- Bounds checks and exclusivity do not prove that an integer index belongs to
  the intended container generation. Graph-like storage needs a checked
  generational handle or arena design before stale or cross-container indices
  can be described as safe identities.

These are engineering and language-design risks rather than evidence that the
core direction is infeasible. Their unresolved work is tracked in the
[implementation roadmap](../plan/roadmap.md).
