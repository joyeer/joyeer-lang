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

(Postfix `?` propagates `Result`/`Optional` failure; postfix `!` forces an
unwrap. Optional chaining is the separate `?.` form; see §5.4 and §8.3.)

### 5.2 Arithmetic, comparison, logical, bitwise

Standard semantics. Integer overflow on signed types is a **trap** in
**all** builds (debug and release alike) — the behavior never changes with the
optimization level (§0.1 principles 4 & 5). Programmers may opt into
two's-complement wrapping with `&+`, `&-`, `&*` (reserved syntax ⏳, deferred
to v0.2).

Comparisons are non-chaining: `1 < x < 10` is a syntax error.

Signed `Int` division (`/`) truncates toward zero. The remainder (`%`) is
zero or has the sign of the dividend; when defined, `a == (a / b) * b + a % b`.
Both operators trap when the divisor is zero. Both also trap for the minimum
`Int` and divisor `-1`, including remainder even though its mathematical
result would be zero. These checks apply in every optimization mode.

Bitwise `&`, `|`, `^` take two integers of the same type; prefix `~` takes one
integer. `<<` and `>>` take an integer on the left and an integer of any type
as the shift count. A count that is negative or not less than the bit width of
the left operand traps in every build. `>>` is arithmetic for signed types and
logical for unsigned types; `<<` discards bits shifted out and never traps for
that reason.

Prefix `!` takes a `Bool` and returns its negation. Logical `&&` and `||`
take Boolean operands and produce `Bool`.

Logical `&&` evaluates its left operand first and evaluates the right operand
only when the left is `true`. The skipped operand produces no side effects,
traps, ownership transfers, or early returns. The result is `false` when the
left operand is `false`, independently of optimization level.

Logical `||` evaluates its left operand first and evaluates the right operand
only when the left is `false`. Its result is `true` when the left is `true`;
the same skipped-side-effect and cleanup rules apply. `&&` binds more tightly
than `||`. Prefix negation is not postfix force unwrap, which remains deferred.

### 5.2.1 Integer conversion

`x as T` converts an integer expression to integer type `T`. The conversion
traps with "integer conversion out of range" in every build when the value is
not representable in `T`. `x &as T` instead keeps the low bits: it truncates
when narrowing and extends by the source type's signedness when widening, so
it never traps. `x as? T` returns `Result<T, ConversionError>`: `.Ok` with the
converted value, or `.Err(.OutOfRange)` when the value is not representable in
`T`, never trapping. `ConversionError` is a prelude enum with the single case
`OutOfRange`; use postfix `?` to propagate it, or `match` to handle it. With `as?`
a literal operand stays an `Int` (so `300 as? UInt8` is `.Err(.OutOfRange)`, not a
compile-time error). `T` is a plain type name. With `as` and `&as`, a numeric
literal operand takes type `T`, so an out-of-range literal is a compile-time error. Only integer-to-integer
conversion is defined; floating-point and non-numeric conversions are rejected.
A cast binds tighter than binary operators and looser than prefix operators:
`a + b as UInt32 << 8` is `(a + (b as UInt32)) << 8`, and `-x as T` is
`(-x) as T`.

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

`a.f(args)` always calls a method. When `a` has a type-parameter type (§20.4), the
method is a requirement of a protocol in its constraint and the call reaches
that requirement's witness (§20.5, §20.6). A stored field of function type is called
with a parenthesized callee, `(a.f)(args)` (§5.6.1). A type may not have a
stored field and a method with the same name
(`name-resolution.duplicate-declaration`).

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

### 5.6 Function calls

```
call_expr       ::= callee '(' [ call_arg , ... ] ')'
callee          ::= identifier | member_expr | '(' expression ')'
call_arg        ::= [ label ':' ] [ '&' | 'consume' ] expression
```

Argument labels are **mandatory** at every call by name (§3.2.1);
there is no positional / unlabeled form for one and no `_` label.
`func f(a: Int, b: Int)` must be called as `f(a: 1, b: 2)`. Supplied arguments
follow declaration order; a defaulted argument (§3.2.5) may be omitted but is
never written without its label. Unlabeled arguments appear only in a call
through a function value (§5.6.1) and in enum construction (§5.8).

#### 5.6.1 Function values and calls through them

The layout intrinsics of §20.12.5 are compiler-known functions, not function
values: they can only be called, and only in toolchain-authorized units.

The name of a function, used as an expression where it is not called, is a
**function value** of its function type (§2.3.1,
[decision 0020](../docs/decisions/0020-function-values.md)):

```joyeer
func square(value: Int): Int { value * value }
func bump(count: inout Int) { count = count + 1 }

let f = square                    // f: (Int): Int
let g: (inout Int): () = bump
var n = f(3)                      // 9
g(&n)                             // n == 10
```

A module-qualified name works too. Only the name's visibility is checked
(§12.2); a private function's value may be returned by a public function.

These are not function values (`type-checking.not-a-function-value`, which
states the reason):

- methods, declared and synthesized initializers, and enum cases;
- the compiler-known builtins (`print`, `spawn`, `channel`, `byteToInt`,
  `byteToString`) and the runtime intrinsics (§19.4.5); a user declaration
  that shadows a builtin is an ordinary function and is a value;
- `@c_extern` and `@c_data` declarations (§19.5);
- the root module's `main` (§3.2.6); a dependency's `main` is ordinary.

A function's default parameter values (§3.2.5) do not carry over: a call
through the value supplies every argument.

**Calls.** A function value named by a plain identifier is called directly:
`f(a, &b, consume c)`. Any other callee is parenthesized: `(s.handler)(x)`,
`(table[i])(x)`, `(maybe!)(x)`; `s.f(args)` always names a method (§5.4).

- Arguments are positional. A label is an error
  (`type-checking.labeled-function-value-call`), and so is a wrong number of
  arguments.
- Each argument is checked against its parameter's type and effect exactly as
  for a call by name (§4.2): `inout` and `initializing` arguments are written
  `&x`, `consuming` arguments `consume x`, with `consume` also before a literal or
  temporary, as for every consuming argument (§4.3).
- **Evaluation order.** The callee expression is evaluated first, to a copy;
  no access to its storage is held while the arguments are evaluated or during
  the call. The arguments are then evaluated left to right and committed as for
  a call by name (§4.4, §4.7.1). So `(s.f)(&s)` and `(handlers[0])(&handlers)`
  are valid.
- A call whose result type is `Never` does not return.
- An optional function value must be unwrapped first, with `(maybe!)(x)` or a
  `match`. `maybe?(x)` propagates `nil` out of the enclosing function and then
  calls (§5.4.1); it is not "call if present".
- Calling a value that is not a function is `type-checking.not-callable`.

### 5.7 Struct construction

```joyeer
let p = Point(x: 1.0, y: 2.0)         // explicit init or synthesized
let q = Point(x: 0.0, y: p.y)
```

### 5.8 Enum construction

```
enum_ctor_expr     ::= [ type ] '.' identifier [ enum_payload_clause ]
enum_payload_clause
                   ::= '(' enum_arg , ... ')'
enum_arg           ::= [ label ':' ] expression
```

```joyeer
let command: Message = .Move(x: 3, y: 4)       // contextual: target type known
let text = Message.Text("ready")                // fully qualified
let error: DecodeError = .Unexpected(b'?', at: 12)
```

The declaration determines the payload arity and labels. A payload position
declared without a label is positional; a labeled position must use exactly
that label. Positional and labeled payloads may be mixed in declaration order.
Besides calls through a function value (§5.6.1), this is the only unlabeled
invocation form; calls by name and struct-initializer calls require labels
(§3.2.1, §5.6). A payload-less
case is written without an empty argument clause (`.Quit`, not `.Quit()`).

### 5.9 Match expression

```
match_expr      ::= 'match' expression '{' match_arm+ '}'
match_arm       ::= pattern ( ',' pattern )* [ 'where' expression ] '=>' (expression | block) ','?
```

`match` is an **expression**; every arm must produce a value of the same
type (or all be `Void`). An arm may list several comma-separated
alternative patterns and runs when any of them matches (§7.2):

```joyeer
enum Reading {
  Missing,
  Value(Int),
}

let label = match reading {
  .Missing                  => "missing",
  .Value(n) where n < 0     => "negative",
  .Value(_)                 => "non-negative",
}
```

Exhaustiveness is checked (§7.7).

Because `return` is a diverging expression (§2.9, §6.4), it may be an arm body:

```joyeer
let value = match result {
  .Ok(value) => value,
  .Err(error) => return .Err(error),
}
```

### 5.10 if as expression

`if`/`match` are expressions; `while`/`for` are statements.

```joyeer
let sign = if x > 0 { 1 } else if x < 0 { -1 } else { 0 }
```

Every branch must produce the same type. The whole `if`-`else` must be
total (i.e., have an `else` arm) when used as an expression.

### 5.10.1 Unit expressions

An empty pair of parentheses in expression position, `()`, is the unit value
of type `Void` (§2.1.2), not a missing expression or a general tuple.
Parenthesized expressions such as `(value)` keep their existing meaning.
In `.Ok(())`, the outer parentheses contain one payload expression, `()`;
`.Ok()` is not an alternative unit construction.

### 5.11 Closures

Closures are deferred to v0.2. A function value (§5.6.1) refers to a top-level
function and captures nothing; higher-order code passes such values, or a struct
together with a function value when it needs context.

### 5.12 Nil-coalescing `??`

The coalescing operator `??` supplies a fallback for an absent `Optional` or a
failed `Result`; its `.Some` / `.Ok` unwrapping semantics are specified
in §8.5. `b` is evaluated only when needed (short-circuit), and `??`
is right-associative (§5.1), so `a ?? b ?? c` parses as `a ?? (b ?? c)`. When
the error of a `Result` must be inspected rather than discarded, use `match`
or `?` (§8.3).

```joyeer
let port: Int = parseInt(s: s) ?? 8080                       // plain fallback value
let v = parseInt(s: s) ?? fatalError(message: "bad input")   // fallback diverges (Never, §2.9)
let c = peek(p: p) ?? return .Err(.UnexpectedEof)            // fallback returns from the caller
```

For `a: T?`, the type of `a ?? b` is `T` when `b: T`, or `T?` when `b: T?`.
The right-hand side may have type `Never` (§2.9) — as with `fatalError` or
`return` — in which case the whole expression has the left-hand side's
unwrapped type `T`.

---
