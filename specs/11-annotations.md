## §11 Declaration Annotations

**Specification status:** C binding annotations have semantic rules in
[§19](19-c-interop.md), and `@platform` in §11.5. The `@spec` and `@property`
forms below remain independent proposals, not additional accepted annotation
syntax.

**Implementation status:** the C binding annotations (every §19.4 scalar,
restricted buffers, bare byte pointers and the intrinsics) and `@platform` are
parsed and checked. `@spec` and `@property` remain
unsupported; their proposed tooling examples below are not currently
compilable programs. See [current C binding coverage](../docs/impl/c-interop.md).

```
attribute       ::= '@' identifier '(' [ attribute_arg , ... ] ')'
attribute_arg   ::= [ identifier ':' ] expression
```

This is the parenthesized production of [Appendix C](appendix-c-grammar.md).
Each annotation fixes its own argument shape and rejects any other:
`@c_extern`, `@c_export` and `@c_data`
each take one `symbol:` string literal, `@c_extern` may add `pointer:` arguments
naming bare-pointer parameters (§19.4.4), and `@platform` takes unlabelled
platform names. Attribute placement and arguments are checked against the named
annotation's rules; this production does not authorize unknown annotations or
silently ignored metadata. It does not include the proposed shorthand or
quantifier DSL in §11.1 and §11.2.

### 11.1 `@spec`

Proposed free-form natural-language intent for tooling (AI assistants,
documentation generators). The shorthand below is not part of the accepted
attribute production above.

```joyeer
@spec "Returns the nth Fibonacci number, where fib(0) = 0 and fib(1) = 1."
func fib(n: Int): Int { ... }
```

### 11.2 `@property`

Proposed executable Bool expression that the value of `n` (or the function
under test) should satisfy. A future test runner could sample inputs.

```joyeer
@property fib(n: 0) == 0
@property fib(n: 1) == 1
@property forall n in 2..<20: fib(n: n) == fib(n: n-1) + fib(n: n-2)
func fib(n: Int): Int { ... }
```

The `forall <name> in <range>: <bool-expr>` quantifier used by `@property` is
a proposed annotation DSL (🔬); it is **not** part of the v0.1 expression
grammar (§5). Its intended scope is `@property`, not ordinary expressions.

### 11.3 Proposed treatment and current scope

- For `@spec`/`@property`, first settle the shorthand/DSL grammar, then parse
  and store metadata; that proposal does not govern C annotation semantics.
- No `@spec`/`@property` shorthand/DSL parser or property-test runner is
  implemented.
- A runner is follow-on work, not a delivered capability of the draft syntax.

**Design rationale.** Natural-language intent and executable properties can
help reviewers and tooling check generated code. Sampling inputs is testing,
not proof over every input, and storing an annotation does not verify it.
These annotations do not imply an incremental compiler or formal-verification
engine; see the [verification boundary](09-contracts.md#96-verification-boundary).

### 11.4 C binding annotations

`@c_extern(symbol: "...")` and `@c_export(symbol: "...")` use the named
argument production above. Their authorization, declaration, symbol, type,
and ABI rules have one normative source: [§19 C Interoperation](19-c-interop.md).
They select C ABI without an `abi` argument and must not be ignored as
tooling-only metadata. Implementing them does not require implementing
`@spec`, `@property`, or the property quantifier DSL.

### 11.5 `@platform`

```joyeer
@platform(windows)
func separator(): UInt8 { return b'\\' }

@platform(posix)
func separator(): UInt8 { return b'/' }
```

`@platform(name, ...)` selects a top-level function by target platform. It
takes one or more unlabelled identifiers, each a platform name or a group.
Platform names select operating systems only; architectures are not
selectable.

| Name | Selects |
|---|---|
| `windows`, `linux`, `macos` | that operating system |
| `posix` | every supported platform other than `windows` |

A list is a union, so repeating or overlapping names within one list is
accepted and harmless. Names are case-sensitive; `unix` is not a name.

**Target platform.** A compilation has one target platform: the operating
system it generates code for, which is the host. Code generation for any
other target is rejected (`driver.unsupported-target-platform`); a toolchain
client may select another target only to validate, without generating code.
The implementation detail is in
[the implemented surface](../docs/impl/supported-features.md).

**Selection.** Before names are resolved, a function whose platform list does
not contain the target is removed from the program; a function that does
contain it keeps its body and loses the annotation. Removed functions leave
no symbol, no IR, and no debug information, and their bodies are not
name-resolved or type-checked. They must still parse, and the annotations on
them are still checked (see Restrictions). Imports are resolved before
selection, so `@platform` cannot make an import conditional.

**Variants.** Functions of one name in one module are variants when at least
one of them carries `@platform`. A `private` function belongs to the variants
of its own file only, unless a non-private function of that name exists in the
module. For every module, independent of the target:

- Platform sets must not overlap. An unannotated function of the same name
  selects every platform, so it overlaps every `@platform` variant.
- Variants must have the same signature: visibility, argument labels, access
  effects, parameter types, and result type. Types are compared by canonical
  spelling, before resolution: an omitted result, `()` and `Void` are one
  type, as are `[T]` and `Array<T>`, `T?` and `Optional<T>`, and `[K: V]` and
  `Dict<K, V>`; an omitted access effect is `borrowing`. A module-qualified
  spelling is a different spelling from an unqualified one. Parameter internal names and bodies may differ.
  Default argument values would be part of the signature if they were
  supported.
- A variant's name must not also name a struct, enum, or top-level constant
  of the same scope, since that declaration exists on every platform.

Variants need not cover every platform. A reference to a name that has no
variant for the target is an ordinary unresolved-name error, and the
`public` interface of a module is the one its selected variants give.
Variants may be `main`, in which case §3.2.6 applies to the selected one.

**Restrictions.** `@platform` is valid only on top-level functions, not on
methods, types, fields, constants, or imports, and appears at most once per
function. It may be combined with `@c_extern`, `@c_export` and `@c_data` in either
order; their rules in [§19](19-c-interop.md) apply to the selected variant, so
disjoint variants may name the same C symbol. Selecting a platform grants no
native-binding authorization. On every variant, selected or not, the compiler
checks that other annotations beside `@platform` are known, that a function
without a body carries `@c_extern` or `@c_data`, and, for a C annotation, its `symbol:`
argument, body rule, explicit result type, and runtime authorization of the
unit. Parameter and result type admissibility ([§19.4](19-c-interop.md)) is
checked only on the selected variant.

**Diagnostics.** `platform.invalid-annotation` (empty, labelled,
non-identifier, repeated, an unknown companion annotation, or a missing body),
`platform.unknown-name`, `platform.overlapping-variants`,
`platform.signature-mismatch`, and `platform.name-collision`. A misplaced
annotation is reported by the parser as `parser.unsupported-syntax`. A
compilation with any `platform.*` diagnostic reports only those; name
resolution and later stages do not run.

**Design rationale.** See [decision 0002](../docs/decisions/0002-platform-annotation.md).

---
