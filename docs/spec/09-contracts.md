## §9 Contracts 🔬 (syntax-only in v0.1)

Contracts attach **machine-checkable** specifications to declarations.
In v0.1 they parse and type-check but the compiler does not yet prove or
runtime-check them. This section locks the syntax so the future
implementation has a stable target.

### 9.1 Placement

A contract clause appears between the function signature and body:

```
contract_clause ::= ( requires_clause | ensures_clause )+
requires_clause ::= 'requires' expression
ensures_clause  ::= 'ensures'  expression
```

```joyeer
func divide(_ a: Int, _ b: Int): Int
  requires b != 0
  ensures  result * b + (a % b) == a
{
  return a / b
}
```

### 9.2 `requires` (preconditions)

Any side-effect-free Bool expression in scope at function entry. Multiple
`requires` clauses are conjoined.

### 9.3 `ensures` (postconditions)

A Bool expression evaluated at function exit. The pseudo-binding `result`
refers to the returned value. `old(x)` refers to the value of `x` at
function entry.

```joyeer
func bumpAndGet(_ x: inout Int): Int
  ensures result == old(x) + 1
  ensures x == old(x) + 1
{
  &x += 1
  return x
}
```

### 9.4 `invariant` (struct & loop)

Struct invariant:

```joyeer
struct SortedRun {
  var data: [Int]
  invariant forall i in 0..<data.count - 1: data[i] <= data[i+1]
}
```

Loop invariant:

```joyeer
while i < n
  invariant 0 <= i && i <= n
  invariant forall k in 0..i: array[k] <= pivot
{
  // ...
}
```

### 9.5 Quantifiers & ranges

```
forall identifier 'in' range_or_collection ':' bool_expression
exists identifier 'in' range_or_collection ':' bool_expression
```

Side-effect-free; bounded iteration. (In v0.1 they only need to *parse*;
v0.3 will add finite expansion or SMT translation.)

### 9.6 v0.1 compiler treatment

- Parsed into the AST.
- Type-checked: the expressions must be Bool, side-effect-free, and
  reference only in-scope names.
- **Not** evaluated, **not** proven, **not** runtime-checked.

### 9.7 Future runtime mode

A compiler flag (`--check-contracts`) will lower each `requires` to an
assertion at function entry and each `ensures` to an assertion at exit.
Targeted for v0.3.

---

