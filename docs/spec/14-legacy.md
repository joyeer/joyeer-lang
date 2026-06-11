## §14 Deprecated / Removed Legacy

| Feature | v0.1 behavior | Removal target |
|---------|---------------|----------------|
| `class` keyword + class declarations | Parsed; warning "class is deprecated, use struct"; methods compile via VM bytecode path (existing implementation). | v0.2 |
| Positional (unlabeled) function calls `f(1)` | No longer accepted; every argument must be written with its label `f(x: 1)` (§3.2.1). | removed in v0.1 |
| `print(message: x)` | Legacy label `message`; canonical form is `print(value: x)`. | v0.2 |
| Implicit reference semantics on arrays / class instances | Still present for `class`; structs use value semantics. | Removed when `class` is removed. |

Migration helper: a future `joyeer migrate` tool will rewrite legacy
sources to v0.2 syntax automatically.

---

## §15 Reserved for Future

The following keywords are reserved and rejected with a clear error
message in v0.1:

`async`, `await`, `actor`, `throws`, `try`, `catch`, `defer`,
`protocol`, `trait`, `macro`, `unsafe`, `package`, `invariant`, `result`.

`class` is also reserved (§1.4) but, unlike the words above, is **not**
rejected: the parser accepts it through v0.1 with a deprecation warning and
removes it in v0.2 (§14).

Reserved syntactic constructs:
- Trailing closure call syntax (`f { ... }`)
- Record literal struct construction (`Point { x: 1, y: 2 }`)
- Wrapping arithmetic (`&+`, `&-`, `&*`)
- Labeled break/continue (`break outer`)

---

