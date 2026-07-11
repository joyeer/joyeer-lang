## §10 General Effect System — Removed

Joyeer does not have `performs`, `pure`, effect labels, effect rows, or effect
polymorphism. These words are not keywords and require no lexer, parser, AST,
or type-checker support.

This decision does **not** remove the ownership access conventions
`borrowing`, `inout`, `consuming`, and `initializing`. Those conventions are
part of parameter passing and the memory model (§3.2, §4), not a general
side-effect system.

Observable failures remain explicit through `Result<T, E>` (§8), and mutation
or ownership transfer remains explicit through call-site `&` and `consume`
markers (§4.3).

---

