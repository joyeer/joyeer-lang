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
