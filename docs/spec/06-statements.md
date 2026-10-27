## §6 Statements

```
statement       ::= binding ';'?
                 |  expression ';'?
                 |  if_stmt | while_stmt | for_stmt
                 |  return_stmt
                 |  break_stmt | continue_stmt
                 |  block

block           ::= '{' statement* '}'
```

Semicolons are optional between statements (line breaks separate them);
required only when multiple statements share a line.

### 6.1 if statement / expression

See §5.10. Same syntax in both positions.

### 6.2 while

```
while_stmt      ::= 'while' expression block
```

### 6.3 for-in

```
for_stmt        ::= 'for' [ '&' ] pattern 'in' expression block
```

The optional `&` requests an `inout` iteration over the source collection
(when the collection — a built-in container such as `Array` — provides an
inout view). A user-extensible iteration protocol is reserved for a future
version (§15).

**Implemented subset.** The pattern is a name or `_`. The source is an array,
a `String` (iterating its bytes as `UInt8`), or an integer range:

```joyeer
for x in arr       { print(value: x) }     // each element
for b in "hi"      { print(value: b) }     // each byte
for i in 0..<n     { print(value: i) }     // 0, 1, ... n - 1
for i in 1...n     { print(value: i) }     // 1, 2, ... n
for _ in 0..<3     { }
```

- The loop variable is an immutable binding with the element type, copied from
  the collection like `let x = arr[i]`.
- The source is evaluated once and iterated as a snapshot. Changing the
  original collection inside the body does not change the iteration. Iterating
  a variable copies it first, as any `let` binding of it would; index with
  `while` to avoid the copy.
- A range `a..<b` or `a...b` is allowed only as a `for` source, not as a
  value. Both bounds have the same integer type, and a bare literal bound takes
  the other bound's type. `a..<b` is empty when `a >= b` and `a...b` is empty
  when `a > b`. Neither computes `b + 1`, so `250...255` over `UInt8` is safe.
- `break` and `continue` apply to the `for` loop. `for &x in` (inout
  iteration), dictionary sources, and tuple patterns are not implemented.

### 6.4 return

```
return_expr     ::= 'return' [ expression ]
return_stmt     ::= return_expr
```

`return` may be omitted in single-expression function bodies (§3.2.2).

The statement and expression forms are the same syntax node. Used as an
expression, `return e` has type `Never` (§2.9); this is why it may appear as
the right-hand side of `??` (§5.12) or as a `match` arm body.

When present, the returned expression must begin on the same physical line as
`return`; a newline immediately after `return` terminates a bare return. Once
the expression has begun, it may continue across lines according to the usual
delimiter and operator-continuation rules.

### 6.5 Loop exits

```
break_stmt      ::= 'break'
continue_stmt   ::= 'continue'
```

`break` exits the nearest enclosing loop in the current function. `continue`
starts that loop's next iteration; for `while`, execution resumes by evaluating
the condition again. A loop context includes its condition and body, so a
block nested in a condition can also exit or restart that loop.

These are statements, not value expressions. A block path ending in either
does not fall through and can be combined with value-producing branches.
Using either outside a loop is an error. Neither takes a value or label;
labeled forms remain reserved (§15).

Every jump destroys values and pending temporaries in exited scopes in reverse
cleanup order, without destroying values in enclosing scopes that remain active.
Ownership and definite initialization are checked across all break exits and
continue back edges. In particular, `continue` cannot bypass reinitialization
required before the next use of consumed storage. Statements after an
unconditional jump are unreachable.

---
