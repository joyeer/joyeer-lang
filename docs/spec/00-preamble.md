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

### 0.2 Versioning

The table below records draft language targets, not an implementation
completion checklist. `v0.1-spec` covers more than the current executable
JSON-parser milestone; [the v0.1 plan](../plan/v0.1.md) records the supported
subset. In particular, modules and annotations below are not implemented by
the current MVP merely because they appear in the draft.

| Version | Scope |
|---------|-------|
| **v0.1** | Draft scope: lexical structure, types, declarations, memory model, expressions, statements, patterns, containers, errors, and modules. Property annotations have a proposed parse-only treatment. The executable milestone is narrower; user-defined generics and protocols are reserved (§15), and the general effect system is not part of the language. |
| **v0.2** | Broader diagnostics and standard library. The retired compatibility surface is already rejected (§14). |
| **v0.3** | Proposed runtime check APIs, with the `assert` / `precondition` distinction in §9, and a property-based test runner. |
| **future** | Concurrency, SMT-backed verification, FFI, macros, traits. |

### 0.3 Feature status legend

| Mark | Meaning |
|------|---------|
| ✅ | Specified semantics in this draft; implementation availability is tracked separately in the v0.1 plan. |
| 🔬 | Proposed syntax-only treatment; it does not imply the current lexer/parser accepts the feature. |
| ⏳ | Reserved keyword / future syntax; rejected with a clear error message in v0.1. |
| ⛔ | Deprecated or removed historical syntax; consult §14. The current compiler does not provide a legacy compatibility mode. |

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
