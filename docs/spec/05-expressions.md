## §5 Expressions

### 5.1 Operator precedence

From highest (binds tightest) to lowest. Same-row operators have equal
precedence; associativity is shown.

| Level | Operators | Associativity |
|-------|-----------|---------------|
| 1 | `.`  `(...)`  `[...]`  postfix `?`  postfix `!` | left |
| 2 | prefix `!`  prefix `-`  prefix `~` | right |
| 3 | `*`  `/`  `%` | left |
| 4 | `+`  `-` | left |
| 5 | `<<`  `>>` | left |
| 6 | `&` (bitwise) | left |
| 7 | `^` | left |
| 8 | `\|` | left |
| 9 | `..<`  `...` | none |
| 10 | `<`  `<=`  `>`  `>=` | none |
| 11 | `==`  `!=` | none |
| 12 | `&&` | left |
| 13 | `\|\|` | left |
| 14 | `??` (nil-coalescing) | right |
| 15 | assignment (`=`  `+=` …) | right |

(Postfix `?` and `!` are optional-chain / force-unwrap, see §5.4.)

### 5.2 Arithmetic, comparison, logical, bitwise

Standard semantics. Integer overflow on signed types is a **trap** in
debug builds and **wraps** in release builds — programmers may opt into
wrapping with `&+`, `&-`, `&*` (reserved syntax ⏳, deferred to v0.2).

Comparisons are non-chaining: `1 < x < 10` is a syntax error.

### 5.3 Assignment

```
assignment      ::= [ '&' ] lvalue assign_op expression
assign_op       ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='
                 |  '&=' | '|=' | '^=' | '<<=' | '>>='
```

The `&` prefix is required when the lvalue is a projection through an
`inout` or `initializing` subscript:

```joyeer
&a[0] = 10         // subscript inout
p.x = 10           // direct field on a var binding — no & needed
```

> **Heuristic.** Whenever the assignment touches storage that came from a
> `subscript`, write `&`. Whenever you assign directly to a `var` or a
> direct field of a `var`, no `&`. The compiler will tell you the right
> form in any case.

### 5.4 Member access & methods

```
member_expr     ::= postfix_expr '.' identifier
optional_chain  ::= postfix_expr '?.' identifier
```

Methods declared in a `struct`, `enum`, or `extension` are called via
member syntax: `a.size()`, `point.distance(to: other)`.

Optional chaining short-circuits to `nil`:

```joyeer
let lengths = optionalString?.count   // type: Int?
```

### 5.5 Subscript expressions

```
subscript_expr  ::= postfix_expr '[' expression , ... ']'
```

Resolves to the matching `subscript` declaration. The accessor invoked
depends on the surrounding context (§4.5.1).

### 5.6 Function calls 📌

```
call_expr       ::= callee '(' [ call_arg , ... ] ')'
call_arg        ::= label ':' [ '&' | 'consume' ] expression
```

> **📌 Decision (call site).**  Every argument is written with its label:
> the only legal call of `func f(a: Int, b: Int)` is `f(a: 1, b: 2)`. There
> is **no** positional / unlabeled call form and **no** `_` wildcard label
> (§3.2.1); `f(1, 2)` is a syntax error. Supplied arguments follow
> declaration order; defaulted arguments may be omitted (§3.2.5).

### 5.7 Struct construction

```joyeer
let p = Point(x: 1.0, y: 2.0)         // explicit init or synthesized
let q = Point(x: 0.0, y: p.y)
```

### 5.8 Enum construction

```joyeer
let v: JsonValue = .Number(3.14)       // contextual: target type known
let w = JsonValue.Bool(true)            // fully qualified
```

### 5.9 Match expression

```
match_expr      ::= 'match' expression '{' match_arm+ '}'
match_arm       ::= pattern ( ',' pattern )* [ 'where' expression ] '=>' (expression | block) ','?
```

`match` is an **expression**; every arm must produce a value of the same
type (or all be `Void`). An arm may list several comma-separated
alternative patterns and runs when any of them matches (§7.2):

```joyeer
let label = match v {
  .Null         => "null",
  .Bool(true)   => "true",
  .Bool(false)  => "false",
  .Number(n) where n < 0 => "negative",
  .Number(_)    => "non-negative",
  _             => "other",
}
```

Exhaustiveness is checked (§7.7).

### 5.10 if as expression 📌

> **📌 Decision.** *`if`/`match` are expressions; `while`/`for` are
> statements.*  Pure functional `if` improves AI-generated code clarity
> (no scattered `return` paths).

```joyeer
let sign = if x > 0 { 1 } else if x < 0 { -1 } else { 0 }
```

Every branch must produce the same type. The whole `if`-`else` must be
total (i.e., have an `else` arm) when used as an expression.

### 5.11 Closures 📌

> **📌 Decision.** *Closures deferred to v0.2.*  v0.1 has only top-level
> and method functions. Iteration uses `for-in`; higher-order patterns
> use free functions. This keeps the memory model simpler (no closure
> capture rules) and matches the v0.1 use cases (JSON parser, quicksort).

### 5.12 Nil-coalescing `??` 📌

> **📌 Decision.** *`??` supplies a fallback for an absent / failed
> value.*  For `a: T?`, `a ?? b` evaluates to the wrapped value when `a` is
> `.Some(v)`, otherwise to `b`. For `a: Result<T, E>`, `a ?? b` evaluates to
> the `Ok` value when `a` is `.Ok(v)`, otherwise to `b` (the error is
> discarded; use `match` or `?` (§8.3) when the error must be inspected).
> `b` is evaluated only when needed (short-circuit), and `??` is
> right-associative, so `a ?? b ?? c` parses as `a ?? (b ?? c)`.

```joyeer
let port: Int = parseInt(s: s) ?? 8080                       // plain fallback value
let v = parseInt(s: s) ?? fatalError(message: "bad input")   // fallback diverges (Never, §2.9)
let c = peek(p: p) ?? return .Err(.UnexpectedEof)            // fallback returns from the caller
```

For `a: T?`, the type of `a ?? b` is `T` when `b: T`, or `T?` when `b: T?`.
The right-hand side may have type `Never` (§2.9) — as with `fatalError`,
`return`, `break`, or `continue` — in which case the whole expression has
the left-hand side's unwrapped type `T`.

---

