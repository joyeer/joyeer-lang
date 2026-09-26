## §0 Preamble

### 0.1 Design philosophy

Joyeer is an **AI-era systems language**. AI writes the bulk of the code;
humans review, audit, and refine. The language optimizes for:

1. **Verifiability over brevity.** Stronger types catch more AI mistakes at
   compile time.
2. **Explicit intent over implicit behavior.** Mutation and ownership transfer
   must be visible at the call site and in the signature.
3. **Value semantics by default.** No GC, no reference counting, no hidden
   aliasing. Heap-backed values have explicit ownership and copy semantics.
4. **One way to do each thing.** Reduce stylistic variance so AI output is
   predictable and code review is fast.
5. **C-replacement performance.** Aim for predictable layout and no avoidable
   abstraction cost beyond the specified value operations and safety checks.

References: Hylo's mutable value semantics, Swift's syntactic surface,
Rust's memory safety guarantees, and Dafny's contracts.

#### 0.1.1 Reviewable intent

**Design rationale.** Explicit access conventions let a reviewer distinguish
reading, mutation, ownership transfer, and initialization without tracing a
callee's implementation. The compiler must still enforce those obligations;
readable syntax does not replace ownership or control-flow analysis.

Strong types and explicit ownership constrain generated code, but do not prove
that it satisfies the author's intent. Runtime contracts (§9) and proposed
property annotations (§11) describe additional checks, not automatic proof of
arbitrary program correctness.

#### 0.1.2 Cost goals

**Design rationale.** Zero-cost abstractions aim to avoid work unrelated to the
specified operation, not to make every operation free. Heap-backed value copies
can allocate and scale with the copied data; borrowing avoids an ownership
transfer but materializing an owned value can still require a copy (§4).

Performance comparisons must preserve equivalent ownership, copying, error
handling, arithmetic, and bounds-check semantics. Comparing a checked operation
with an unchecked one does not establish abstraction overhead. Optimizations
may remove redundant work only without changing source validity or observable
behavior. Eliding a development-time `assert` does not disable required language
safety checks (§9).

Performance parity and fixed binary-size budgets are measurement goals, not
language guarantees. Current costs and measurement requirements belong in the
[native implementation notes](../impl/native.md#7-performance-and-footprint-measurement).

### 0.2 Notation

Grammar rules use a compact EBNF dialect:

```
nonterminal  ::= alternative1 | alternative2
'literal'      — terminal token (verbatim)
[ X ]          — optional
{ X }          — zero or more
( X )          — grouping
X+             — one or more
X*             — zero or more
X , ...        — comma-separated list of one or more X (a trailing comma is permitted)
```

Examples are written in fenced ` ```joyeer ` blocks.

---
