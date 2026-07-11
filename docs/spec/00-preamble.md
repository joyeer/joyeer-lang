## §0 Preamble

### 0.1 Design philosophy

Joyeer is an **AI-era systems language**. AI writes the bulk of the code;
humans review, audit, and refine. The language optimizes for:

1. **Verifiability over brevity.** Stronger types catch more AI mistakes at
   compile time.
2. **Explicit intent over implicit behavior.** Mutation and ownership transfer
   must be visible at the call site and in the signature.
3. **Value semantics by default.** No GC, no reference counting, no hidden
   aliasing. Heap allocation is opt-in.
4. **One way to do each thing.** Reduce stylistic variance so AI output is
   predictable and code review is fast.
5. **C-replacement performance.** Zero-cost abstractions; predictable
   layout; no runtime overhead for safety features.

References: Hylo's mutable value semantics, Swift's syntactic surface,
Rust's memory safety guarantees, and Dafny's contracts.

### 0.2 Versioning

| Version | Scope |
|---------|-------|
| **v0.1** | Lexical, types, declarations, memory model, expressions, statements, match patterns, built-in containers, errors, modules. Property annotations parse but are not executed. User-defined generics and protocols are reserved (§15). The general effect system is not part of the language. |
| **v0.2** | Removal of legacy syntax (§14). Improved diagnostics. Standard library. |
| **v0.3** | Runtime contract enforcement (debug mode). Property-based test runner. |
| **future** | Concurrency, SMT-backed verification, FFI, macros, traits. |

### 0.3 Feature status legend

| Mark | Meaning |
|------|---------|
| ✅ | Locked in v0.1; semantics fully enforced. |
| 🔬 | Syntax-only in v0.1; parsed, type-checked at signature level, but no deeper semantic enforcement yet. |
| ⏳ | Reserved keyword / future syntax; rejected with a clear error message in v0.1. |
| ⛔ | Deprecated legacy; accepted by the parser with a warning in v0.1, removed in v0.2. |

### 0.4 Notation

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

📌 **Decisions** are inline callouts that record a committed design choice,
the alternatives considered, and the rationale. They are **not numbered**
during internal development — cross-reference a decision by its section
number (e.g. "see §4.3"). A stable numbering scheme may be introduced once
the spec is frozen for the first public release. Form:

> **📌 Decision.** *Inout call-site marker is `&x`.*  Alternatives: `inout x`
> (keyword form). Chosen `&x` because it is concise and consistent with Swift,
> which our target audience already knows.

---

