## §15 Reserved for Future

The following keywords are reserved and rejected with a clear error
message in v0.1:

`async`, `await`, `actor`, `throws`, `try`, `catch`, `defer`, `break`,
`continue`, `is`, `protocol`, `trait`, `macro`, `invariant`, `result`,
`unsafe`, `package`, `Any`.

`class` is also reserved (§1.4) and rejected by the current lexer.

Reserved syntactic constructs:
- Trailing closure call syntax (`f { ... }`)
- Record literal struct construction (`Point { x: 1, y: 2 }`)
- Wrapping arithmetic (`&+`, `&-`, `&*`)
- Loop control `break` / `continue`, including labeled forms (`break outer`)
- Type-test / cast operators (`x is T`, `x as T`); `as` remains only for
  `import` aliasing (§3.8)
- String interpolation (`"\(expr)"`, §1.7)

---