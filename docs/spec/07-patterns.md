## §7 Patterns

Used in `let`/`var`, `match` arms, and `for-in`.

```
pattern         ::= '_'                                       // wildcard
                 |  identifier                                 // bind
                 |  literal                                    // literal compare
                 |  '(' pattern , ... ')'                       // tuple
                 |  type '.' identifier [ '(' pattern , ... ')' ]   // enum case
                 |  '.' identifier [ '(' pattern , ... ')' ]        // contextual enum case
```

> **Note.** Type-test / cast patterns (`x as T`) and the `is` / `as` type-test
> operators are deferred to v0.2 (§15). v0.1 has no runtime type tests because
> it has no subtyping, protocols, or inheritance, so there is no type
> relationship to query.

### 7.1 Identifier

Binds a fresh name to the matched value:

```joyeer
match v {
  .Number(n) => print(value: n),    // n bound
  _ => (),
}
```

### 7.2 Alternative patterns

A match arm may list several comma-separated patterns; the arm runs when
**any** of them matches. To keep each arm's bindings unambiguous,
alternative patterns may only combine patterns that bind no variables
(wildcards and literals):

```joyeer
match c {
  b'"'             => parseString(p: &p),
  b't', b'f'       => parseBool(p: &p),
  b'0', b'1', b'2' => parseDigit(c: c),
  _                => parseOther(p: &p),
}
```

### 7.6 Where guards

```joyeer
match v {
  .Number(n) where n > 0 => "positive",
  .Number(_)              => "non-positive",
  _                       => "other",
}
```

### 7.7 Exhaustiveness

`match` over an enum or `Bool` must cover all cases or include a `_`
arm. Missing cases are a compile error with a list of the missing
constructors.

---

