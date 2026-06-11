## §6 Statements

```
statement       ::= binding ';'?
                 |  expression ';'?
                 |  if_stmt | while_stmt | for_stmt
                 |  return_stmt | break_stmt | continue_stmt
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
(if the collection's `Iterable` conformance provides an inout view).

```joyeer
for x in arr      { print(value: x) }
for &x in arr     { &x *= 2 }
```

### 6.4 return / break / continue

Standard. `return` may be omitted in single-expression function bodies
(§3.2.2). `break label` / `continue label` reserved ⏳ for v0.2.

Used as expressions, `return e`, `break`, and `continue` have type `Never`
(§2.9); this is why they may appear as the right-hand side of `??` (§5.12)
or as a `match` arm body.

---

