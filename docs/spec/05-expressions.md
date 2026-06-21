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
| 14 | `??` (nil / `.Err` coalescing) | right |
| 15 | assignment (`=`  `+=` …) | right |

(Postfix `?` and `!` are optional-chain / force-unwrap, see §5.4.)

### 5.2 Arithmetic, comparison, logical, bitwise

Standard semantics. Integer overflow on signed types is a **trap** in
**all** builds (debug and release alike) — the behavior never changes with the
optimization level (§0.1 principles 4 & 5). Programmers may opt into
two's-complement wrapping with `&+`, `&-`, `&*` (reserved syntax ⏳, deferred
to v0.2).

Comparisons are non-chaining: `1 < x < 10` is a syntax error.

### 5.3 Assignment

```
assignment      ::= [ '&' ] lvalue assign_op expression
assign_op       ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='
                 |  '&=' | '|=' | '^=' | '<<=' | '>>='
```

The `&` prefix is required whenever the assignment target is a mutable
projection that the writer does not own outright — an `inout` / `initializing`
**parameter binding**, or a projection through an `inout` / `initializing`
**subscript**:

```joyeer
&a[0] = 10         // subscript inout projection — & required
&n += 1            // n is an `inout` parameter — & required
p.x = 10           // direct field on a local `var` binding — no & needed
count = count + 1  // direct local `var` — no & needed
```

> **Heuristic.** Write `&` when the assignment targets an `inout` /
> `initializing` parameter or storage reached through a `subscript`. Omit `&`
> only when assigning directly to a local `var` or a direct field of a local
> `var`. (Reads never take `&`; the marker is about the *write* target.) The
> compiler reports the required form in any case.

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

#### 5.4.1 The `?` / `!` forms

| Form | Meaning | Section |
|------|---------|---------|
| `T?` | optional **type** (`Optional<T>`) | §2.4 |
| `x?.field` | optional **chaining** | §5.4 |
| `expr?` | `Result` / `Optional` **propagation** (early return) | §8.3 |
| `x ?? y` | **coalescing** with fallback `y` | §8.5 |
| `x!` | **force-unwrap** (traps on `nil`) | §2.4 / §9.3 |

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

> **📌 Decision D3 (call site).** Argument labels are **mandatory** at every
> call site (§3.2.1); there is no positional / unlabeled form and no `_`
> label. `func f(a: Int, b: Int)` must be called as `f(a: 1, b: 2)`. Supplied
> arguments follow declaration order; a defaulted argument (§3.2.5) may be
> omitted but is never written without its label.

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
match_arm       ::= pattern [ 'where' expression ] '=>' (expression | block) ','?
```

`match` is an **expression**; every arm must produce a value of the same
type (or all be `Void`):

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

> **📌 Decision D9.** *`if`/`match` are expressions; `while`/`for` are
> statements.*  Pure functional `if` improves AI-generated code clarity
> (no scattered `return` paths).

```joyeer
let sign = if x > 0 { 1 } else if x < 0 { -1 } else { 0 }
```

Every branch must produce the same type. The whole `if`-`else` must be
total (i.e., have an `else` arm) when used as an expression.

### 5.11 Closures 📌

> **📌 Decision D8.** *Closures deferred to v0.2.*  v0.1 has only top-level
> and method functions. Iteration uses `for-in`; higher-order patterns
> use free functions. This keeps the memory model simpler (no closure
> capture rules) and matches the v0.1 use cases (JSON parser, quicksort).

---

