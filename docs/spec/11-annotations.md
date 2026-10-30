## §11 Declaration Annotations

**Specification status:** C binding annotations have semantic rules in
[§19](19-c-interop.md). The `@spec` and `@property` forms below remain
independent proposals, not additional accepted annotation syntax.

**Implementation status:** the runtime-only `Int`/`Void` C binding subset is
parsed and checked. `@spec` and `@property` remain unsupported; their proposed
tooling examples below are not currently compilable programs. See
[current C binding coverage](../impl/c-interop.md).

```
attribute       ::= '@' identifier '(' [ attribute_arg , ... ] ')'
attribute_arg   ::= identifier ':' expression
```

This is the parenthesized, named-argument production used for C annotations
in [§17](17-grammar.md). Attribute placement and arguments are checked
against the named annotation's rules; this production does not authorize
unknown annotations or silently ignored metadata. It does not include the
proposed shorthand or quantifier DSL in §11.1 and §11.2.

### 11.1 `@spec`

Proposed free-form natural-language intent for tooling (AI assistants,
documentation generators). The shorthand below is not part of the accepted
attribute production above.

```joyeer
@spec "Returns the nth Fibonacci number, where fib(0) = 0 and fib(1) = 1."
func fib(n: Int): Int { ... }
```

### 11.2 `@property`

Proposed executable Bool expression that the value of `n` (or the function
under test) should satisfy. A future test runner could sample inputs.

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

- For `@spec`/`@property`, first settle the shorthand/DSL grammar, then parse
  and store metadata; that proposal does not govern C annotation semantics.
- No `@spec`/`@property` shorthand/DSL parser or property-test runner is
  implemented.
- A runner is follow-on work, not a delivered capability of the draft syntax.

**Design rationale.** Natural-language intent and executable properties can
help reviewers and tooling check generated code. Sampling inputs is testing,
not proof over every input, and storing an annotation does not verify it.
These annotations do not imply an incremental compiler or formal-verification
engine; see the [verification boundary](09-contracts.md#96-verification-boundary).

### 11.4 C binding annotations

`@c_extern(symbol: "...")` and `@c_export(symbol: "...")` use the named
argument production above. Their authorization, declaration, symbol, type,
and ABI rules have one normative source: [§19 C Interoperation](19-c-interop.md).
They select C ABI without an `abi` argument and must not be ignored as
tooling-only metadata. Implementing them does not require implementing
`@spec`, `@property`, or the property quantifier DSL.

---
