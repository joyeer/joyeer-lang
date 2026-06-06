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
func fib(_ n: Int): Int { ... }
```

### 11.2 `@property`

Executable Bool expression that the value of `n` (or the function under
test) should satisfy. Test runners may sample inputs.

```joyeer
@property fib(0) == 0
@property fib(1) == 1
@property forall n in 2..20: fib(n) == fib(n-1) + fib(n-2)
func fib(_ n: Int): Int { ... }
```

### 11.3 v0.1 treatment

- Parsed and stored.
- No test runner shipped in v0.1. Runner targeted for v0.3.

---

