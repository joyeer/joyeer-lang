## §14 Deprecated / Removed Legacy

| Feature | v0.1 behavior | Removal target |
|---------|---------------|----------------|
| `class` keyword + class declarations | Reserved and rejected; use `struct`. | removed in v0.1 |
| Positional (unlabeled) function calls `f(1)` | No longer accepted; every argument must be written with its label `f(x: 1)` (§3.2.1). | removed in v0.1 |
| `print(message: x)` | Legacy label `message`; canonical form is `print(value: x)`. | v0.2 |
| Implicit reference semantics on aggregates | Not supported; aggregates use value semantics. | removed in v0.1 |

Migration helper: a future `joyeer migrate` tool will rewrite legacy
sources to v0.2 syntax automatically.

---

## §15 Reserved for Future

The following keywords are reserved and rejected with a clear error
message in v0.1:

`async`, `await`, `actor`, `throws`, `try`, `catch`, `defer`, `break`,
`continue`, `is`, `protocol`, `trait`, `macro`, `invariant`, `result`,
`unsafe`, `package`, `Any`.

`class` is also reserved (§1.4) and rejected by the v0.1 parser (§14).

Reserved syntactic constructs:
- Trailing closure call syntax (`f { ... }`)
- Record literal struct construction (`Point { x: 1, y: 2 }`)
- Wrapping arithmetic (`&+`, `&-`, `&*`)
- Loop control `break` / `continue`, including labeled forms (`break outer`)
- Type-test / cast operators (`x is T`, `x as T`); `as` remains only for
  `import` aliasing (§3.8)
- String interpolation (`"\(expr)"`, §1.7)

---

