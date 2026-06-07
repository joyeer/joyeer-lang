## §7 Patterns

Used in `let`/`var`, `match` arms, and `for-in`.

```
pattern         ::= '_'                                       // wildcard
                 |  identifier                                 // bind
                 |  literal                                    // literal compare
                 |  '(' pattern , ... ')'                       // tuple
                 |  type '.' identifier [ '(' pattern , ... ')' ]   // enum case
                 |  '.' identifier [ '(' pattern , ... ')' ]        // contextual enum case
                 |  identifier 'as' type                       // type test + bind
```

### 7.1 Identifier

Binds a fresh name to the matched value:

```joyeer
match v {
  .Number(n) => print(value: n),    // n bound
  _ => (),
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

