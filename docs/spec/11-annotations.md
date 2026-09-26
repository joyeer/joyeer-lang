## §11 Property-Test & Spec Annotations 🔬

**Implementation status:** proposed annotation design. The current MVP lexer
rejects `@`; annotations are not yet parsed or stored. The generic production
and shorthand examples below need a single agreed parser grammar before this
surface is implemented.

```
attribute       ::= '@' identifier [ '(' attribute_args ')' ]
attribute_args  ::= string_literal | expression , ...
```

### 11.1 `@spec`

Free-form natural-language intent. Available to tooling (AI assistants,
documentation generators).

```joyeer
@spec "Returns the nth Fibonacci number, where fib(0) = 0 and fib(1) = 1."
func fib(n: Int): Int { ... }
```

### 11.2 `@property`

Executable Bool expression that the value of `n` (or the function under
test) should satisfy. Test runners may sample inputs.

```joyeer
@property fib(n: 0) == 0
@property fib(n: 1) == 1
@property forall n in 2..<20: fib(n: n) == fib(n: n-1) + fib(n: n-2)
func fib(n: Int): Int { ... }
```

The `forall <name> in <range>: <bool-expr>` quantifier used by `@property` is
a proposed annotation DSL (🔬); it is **not** part of the v0.1 expression
grammar (§5). Its intended scope is `@property`, not ordinary expressions.

### 11.3 Proposed treatment and current scope

- Proposed initial implementation: parse and store annotation metadata.
- Current executable MVP: no annotation parser or property-test runner.
- A runner is follow-on work, not a delivered capability of the draft syntax.

**Design rationale.** Natural-language intent and executable properties can
help reviewers and tooling check generated code. Sampling inputs is testing,
not proof over every input, and storing an annotation does not verify it.
These annotations do not imply an incremental compiler or formal-verification
engine; see the [verification boundary](09-contracts.md#96-verification-boundary).

---
