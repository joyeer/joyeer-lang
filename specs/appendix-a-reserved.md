## Appendix A: Reserved Keywords and Syntax

The authoritative list of reserved keywords is in
[§1.4](01-lexical.md#14-keywords-full-reserved-list).
Those keywords are rejected with a clear error message in v0.1.

The restricted scalar [C interoperation rules](19-c-interop.md) do not
unreserve `unsafe` or select pointer, dereference, or unsafe-block syntax.
The source representation of native addresses remains a separate proposal.

Reserved syntactic constructs:
- Trailing closure call syntax (`f { ... }`)
- Record literal struct construction (`Point { x: 1, y: 2 }`)
- Wrapping arithmetic (`&+`, `&-`, `&*`)
- Labeled loop control (`break outer` / `continue outer`); unlabeled forms
  are supported (§6.5)
- The type-test operator `x is T` and non-integer casts; `as` also remains
  reserved for `import` aliasing (§3.8)
- String interpolation (`"\(expr)"`, §1.7)

---