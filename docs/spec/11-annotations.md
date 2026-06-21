## §11 Property-Test & Spec Annotations 🔬

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
a reserved annotation DSL (🔬); it is **not** part of the v0.1 expression
grammar (§5) and is recognized only inside `@property`.

### 11.3 v0.1 treatment

- Parsed and stored.
- No test runner shipped in v0.1. Runner targeted for v0.3.

---

