# Methods, Extensions, `self` and `Self`

> **Status:** implemented subset of
> [spec §3.2.4, §3.5 and §3.9](../spec/03-declarations.md). This is the
> prerequisite for the proposed [protocols](../spec/20-protocols.md); nothing
> here depends on generics or protocols.

## 1. Surface

```joyeer
struct Counter {
    var count: Int
    func read(): Int { return self.count }
    mutating func bump(by amount: Int) { &self.count += amount }
    consuming func finish(): Int { return self.count }
    func doubled(): Self { return Self(count: self.count * 2) }
}

extension Counter {
    func isAbove(limit: Int): Bool { return self.count > limit }
}

var c = Counter(count: 1)
let n = c.read()
&c.bump(by: 2)
let total = consume c.finish()
```

| Receiver | Declaration | Call-site marker |
|---|---|---|
| borrowing (default) | `func m()` | none |
| mutating | `mutating func m()` | `&value.m()` |
| consuming | `consuming func m()` | `consume value.m()` |

Methods may appear after the stored fields of a `struct`, after the cases of an
`enum`, and in `extension T { ... }`. Visibility modifiers apply to methods, not
to the extension. A method named like a field or another method of the same type
is `name-resolution.duplicate-declaration`; there is no overloading.

## 2. Representation: the receiver is a parameter

The parser inserts an implicit first parameter `self: <effect> Self` into every
method (`ParameterDeclSyntax::isReceiver`). `mutating` becomes `inout`,
`consuming` stays `consuming`, and the default is borrowing. Because of this,
symbols, parameter types, definite initialization, exclusivity, debug
variables and IR parameters need no separate receiver model.

Only the places that match arguments to parameters know the receiver exists:

- `CallableSignature::receiver` and `TypedCallableSignature::receiverType` hold
  the receiver. It is **not** in `parameters`, so label matching, argument
  access checks and `validateCallArguments` see only the written parameters.
- `SymbolKind::method` marks a member symbol that lives in its type's member
  scope, with `containingSymbol` set to the type.
- IR lowering names a method `Type.method`, so it can never be mistaken for the
  entry `main`, and passes the receiver expression as operand 0 of the `call`.

## 3. Stage behavior

**Parser.** `mutating` and `extension` are keywords, as is `self`. `Self` stays an
identifier handled by name resolution. A line-leading `func` that is not indented
deeper than its enclosing declaration ends a body missing its `}`, so recovery
still keeps later top-level functions; members must be indented.

**Name resolution.** Members are collected in a pass after every file's types are
declared, so an extension may precede its type. An extension must name a
`struct` or `enum` of the same module (`invalid-extension-target`). `Self` is
the enclosing type while a member's signature and body are resolved, and
`self-outside-type` elsewhere. A type may not be named `Self` (`reserved-name`).
A member is never more accessible than its type for exposed-type checks.

**Type checking.** The receiver marker is checked against the method effect
(`invalid-access-marker`, `invalid-consume-argument`). A mutating receiver must
be writable storage, a consuming receiver must be consumable, and the receiver
takes part in call exclusivity like an argument. `Type.method()` is rejected
because a method needs a receiver value.

**Semantic analysis.** Method bodies get return-path, initialization and
unused-binding analysis. A consuming receiver is consumed like a consuming
argument, so later use is `use-after-consume`.

## 4. Limits

- Fields must be written `self.field`. Implicit-`self` field lookup is not
  implemented.
- `extension` accepts only methods, and only for a struct or enum declared in
  the same module. Builtin types (`Int`, `Array<Int>`) cannot be extended.
- No `init`, subscripts, static members, generic methods or
  protocols.
- `consume` before a call that is itself an argument, as in
  `f(x: consume a.m())`, is read as the *argument* marker. Write the receiver
  consumption in a binding first, or parenthesize it.

## 5. Validation

Unit tests cover the parser (`ParserTest.*Method*`, `*Extension*`), name
resolution, the type checker, semantic analysis and IR lowering. Native
fixtures [methods.joyeer](../../tests/e2e/methods.joyeer) and
[methods_ownership.joyeer](../../tests/e2e/methods_ownership.joyeer) run at
`-O0` and `-O2`, and the first also with full debug information. The ownership
fixture mutates `String` and array fields through nested receivers and relies on
the runtime's allocation-balance check to catch leaks and double frees.
