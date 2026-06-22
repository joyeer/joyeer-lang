## §6 Statements

```
statement       ::= binding ';'?
                 |  expression ';'?
                 |  if_stmt | while_stmt | for_stmt
                 |  return_stmt
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

```joyeer
for x in arr      { print(value: x) }
for &x in arr     { &x *= 2 }
```

### 6.4 return

Standard. `return` may be omitted in single-expression function bodies
(§3.2.2). `break` and `continue` (including the labeled `break label` /
`continue label` forms) are reserved ⏳ for v0.2 (§15); a v0.1 loop is exited
only by its condition or by `return`.

Used as an expression, `return e` has type `Never` (§2.9); this is why it may
appear as the right-hand side of `??` (§5.12) or as a `match` arm body.

---

