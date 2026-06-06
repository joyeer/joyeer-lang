## §14 Deprecated / Removed Legacy

| Feature | v0.1 behavior | Removal target |
|---------|---------------|----------------|
| `class` keyword + class declarations | Parsed; warning "class is deprecated, use struct"; methods compile via VM bytecode path (existing implementation). | v0.2 |
| Named-only function calls (`f(x: 1)` where `_` was not used) | Both labeled and positional forms accepted. Existing-style call sites continue to work. | n/a (positional now supported alongside) |
| `print(message: x)` | Equivalent to `print(_ message: x)`; legacy form accepted. | v0.2 (replaced by `print(_:)`) |
| Implicit reference semantics on arrays / class instances | Still present for `class`; structs use value semantics. | Removed when `class` is removed. |

Migration helper: a future `joyeer migrate` tool will rewrite legacy
sources to v0.2 syntax automatically.

---

## §15 Reserved for Future

The following keywords are reserved and rejected with a clear error
message in v0.1:

`async`, `await`, `actor`, `throws`, `try`, `catch`, `defer`, `class`,
`protocol`, `trait`, `macro`, `unsafe`, `package`.

Reserved syntactic constructs:
- Trailing closure call syntax (`f { ... }`)
- Record literal struct construction (`Point { x: 1, y: 2 }`)
- Wrapping arithmetic (`&+`, `&-`, `&*`)
- Labeled break/continue (`break outer`)

---

