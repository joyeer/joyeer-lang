## §20 Protocols and Bounded Generic Functions ⏳

> **Status:** proposed, not implemented. This chapter defines a deliberately
> small first version ("v1") of protocols. Nothing here is accepted by the
> current compiler; `protocol` and `trait` remain reserved and rejected (Appendix A)
> until the implementation lands. Where this chapter conflicts with §2.6 or
> §3.2.3, those sections describe the current language and this chapter
> describes the proposal. See
> [implemented language surface](../docs/impl/supported-features.md).

### 20.1 Purpose and scope

A **protocol** names a set of method requirements. A **bounded generic
function** is written once and is compiled separately for each concrete type
it is used with. Together they let a library accept "any type that can do X"
without closures, inheritance, vtables, or boxing.

**Design rationale.** Joyeer has no closures, no subtyping, and no reference
types (§4.8), so a callback or interface cannot be passed as a value today.
Protocol-bounded generics fill that gap with static dispatch: the call is
resolved at compile time, costs nothing beyond the called method, and keeps
ownership and access effects visible in the signature (§0.1). Protocols
without generics would have no use, because there would be nothing to
constrain; v1 therefore includes the smallest generic mechanism that makes
them useful.

| Included in v1 | Deferred (§20.10) |
|---|---|
| `protocol` declarations with method requirements | Associated types |
| Conformance by `extension Type: Protocol` | Default method implementations |
| Generic **top-level functions** with `<T: P & Q>` | Protocol inheritance/refinement |
| `Self` in requirements | Initializer, static, property, and subscript requirements |
| Static dispatch by monomorphization | Existential values (`any P`), dynamic dispatch |
| Builtin marker protocol `Copyable` | Generic structs, enums, methods, and extensions |
| Orphan rule for conformances | Operator-backed protocols (`Equatable`, `Comparable`, `Display`) |
| | Explicit type arguments at call sites |

### 20.2 Protocol declarations

```
protocol_decl   ::= [ visibility ] 'protocol' identifier
                    '{' protocol_requirement* '}'

protocol_requirement
                ::= [ method_effect ] 'func' identifier
                    '(' [ param , ... ] ')' [ ':' type ]
```

A requirement is a method signature with no body. It has no visibility
modifier, no attributes, no default parameter values, and no type
parameters. Every requirement has the visibility of its protocol.

`method_effect` and parameter access effects (§3.2.4, §4.2) are part of the
requirement. A requirement that mutates its receiver is written
`mutating func reset()`, exactly as in a method declaration, and generic code
calls it with the `&` receiver marker.

Inside a protocol, `Self` denotes the conforming type (§3.9) and may appear in
parameter and result types, including as `Self?`, `[Self]`, and
`Result<Self, E>`. It is a placeholder here, not an alias: each conforming type
substitutes itself for `Self` when its witnesses are checked (§20.3).

```joyeer
public protocol Shape {
    func area(): Int
    func scaled(by factor: Int): Self
}
```

A protocol is a **type-namespace** declaration (§13.1): it may not share a
name with a struct, enum, or another protocol in the same scope. The names
`Display`, `Equatable`, `Comparable`, `Any`, and `Copyable` cannot be declared
by user code in v1; `Copyable` is the builtin protocol of §20.5 and the
others stay reserved (§12.4, Appendix A).

A protocol has no stored state and no instances. It is **not a type**: using
a protocol name where a type is expected is an error
(`type-checking.protocol-used-as-type`) whose help text points to a
generic parameter.

**Design rationale.** Banning protocol-typed values in v1 means there is no
existential layout, no hidden allocation, and no dynamic dispatch to specify.
A future `any P` (§20.10) can be added without changing any rule here.

### 20.3 Conformance

```
extension_decl  ::= 'extension' type [ ':' protocol_ref , ... ]
                    '{' ( func_decl | subscript_decl )* '}'
protocol_ref    ::= type_path          // names a protocol
```

There is exactly one way to declare a conformance: an `extension` with a
conformance list. Struct and enum declarations do not list protocols. The
extended type may be a user `struct`/`enum` or a builtin nominal such as
`Int`, `String`, or `Array<Int>` (a concrete instantiation).

```joyeer
struct Square {
    let side: Int
    func area(): Int { side * side }
}

extension Square: Shape {
    func scaled(by factor: Int): Self { Square(side: side * factor) }
}
```

A conformance is valid when every requirement has exactly one **witness**: a
method of the extended type, found by name and labels, declared in the
conformance extension or elsewhere in the type or its other extensions. The
witness must match the requirement exactly after substituting `Self` with the
extended type:

- the same method effect (`borrowing`, `mutating`, `consuming`);
- the same labels (internal parameter names may differ);
- the same parameter access effects and types;
- the same result type.

There is no subtyping, implicit conversion, or effect weakening between a
requirement and its witness (§2). A witness's default parameter values are
ignored when it is called through the protocol: generic code supplies every
argument the requirement lists. A witness may not be a C binding (§19).

A witness must be at least as accessible as the **conformance**, whose
accessibility is the lesser of the protocol's and the extended type's. A
`public` protocol on a `public` type therefore needs `public` witnesses; the
check prevents a conformance from exposing a method the type does not.

**Coherence.** A conformance of type `T` to protocol `P` must be declared in
the module that declares `T` or the module that declares `P`. Builtin types
count as declared by neither user module, so only the protocol's module may
conform them. A module may declare at most one conformance of a given type
to a given protocol. Because the module graph is acyclic (§12.1), these two
rules guarantee that at most one conformance of `(T, P)` exists in any
program, with no global search and no import-order dependence.

A conformance is visible wherever both the protocol and the extended type are
visible, regardless of which files import which modules.

**Design rationale.** The orphan rule is what makes static resolution
deterministic: a generic call can never pick up a different conformance
because an unrelated import happened to be present (compare §12.3's refusal of
first-import-wins lookup).

### 20.4 Generic functions and constraints

```
func_decl       ::= attribute* [ visibility ] [ method_effect ] 'func' identifier
                    [ generic_params ]
                    '(' [ param , ... ] ')' [ ':' type ] [ block ]

generic_params  ::= '<' generic_param , ... '>'
generic_param   ::= identifier [ ':' constraint ]
constraint      ::= protocol_ref ( '&' protocol_ref )*
```

In v1, `generic_params` is accepted only on **top-level functions**. Methods,
`init`, subscripts, structs, enums, extensions, protocol requirements, and C
bindings (`@c_extern`, `@c_export`) may not declare type parameters.

Type parameters are named like types (§13). Their scope is the signature and
body of the function. A type parameter may be used anywhere a type may appear
in the function, including inside builtin containers: `[T]`, `T?`,
`Result<T, E>`, and `[String: T]`.

```joyeer
func totalArea<S: Shape>(shapes: [S]): Int {
    var total = 0
    for shape in shapes { total += shape.area() }
    return total
}
```

`&` in a constraint joins protocols and is unrelated to the call-site `&`
marker (§4.3); the two never appear in the same syntactic position.

**Type arguments are inferred, never written.** A call supplies no `<...>`.
Each type parameter must be determined by the argument types:

- every use of one parameter must unify to one concrete type, otherwise
  `type-checking.conflicting-type-argument`;
- a parameter that no argument determines is
  `type-checking.cannot-infer-type-parameter`; return-type-only inference is
  not performed;
- an unsuffixed literal whose type is otherwise undetermined takes its default
  type (`Int`, `Double`, §2.8) before constraints are checked.

After inference, every constraint must hold, otherwise
`type-checking.unsatisfied-constraint` names the type and protocol. Because
calls never contain `<`, the grammar of [Appendix C](appendix-c-grammar.md)
is not made ambiguous with the comparison operator.

A generic function can only be called. It cannot be named as a function value
(§2.3), and two generic functions cannot be overloaded against each other.

### 20.5 What generic code may do

Inside the body, a value of type `T` is opaque. The only operations that
exist for it are:

1. the requirement methods of the protocols in its constraint, called with
   the same access markers (`&`, `consume`) as any method (§3.2.4);
2. passing it through `borrowing`, `inout`, `consuming`, and `initializing`
   parameters (§4.2), and storing it in locals, builtin containers, and
   `Optional`/`Result` payloads;
3. destroying it when its owner's scope ends (§4.7), using whatever
   destruction the concrete type has.

Field access, `match`, construction, literals, operators (`+`, `==`, `<`),
`as` conversions, and subscripts are not available on `T`. Free functions of
the program are ordinary and may be called with `T` values as arguments.

**Copyability.** A type parameter is assumed **noncopyable** unless its
constraint includes the builtin marker protocol `Copyable`. Any operation that
the memory model defines as a copy (§4.1) is rejected for a noncopyable `T`
value, including `let b = a` from an existing binding, returning a borrowed
parameter, and storing a borrowed value into a container. The diagnostic is
the ordinary noncopyable-copy error with an added note that adding
`& Copyable` to the constraint permits it.

`Copyable` is compiler-defined. It has no requirements, cannot be declared or
conformed manually, and is satisfied by a concrete type exactly when §4.1.1
makes it copyable. A user type with `deinit` can conform to ordinary protocols
but fails a `Copyable` constraint at the call site, not at the conformance.

```joyeer
func larger<S: Shape & Copyable>(first: S, second: S): S {
    if second.area() > first.area() { return second }   // copies a borrowed S
    return first
}
```

**Design rationale.** A copy of a heap-backed value can allocate and clone
recursively (§0.1.2, §4.1). Making `Copyable` explicit puts that cost in the
signature where a reviewer sees it, and keeps a generic function sound for
resource-owning types whose copy would release a resource twice.

### 20.6 Static semantics: monomorphization

A generic function is **checked once** against its constraints, so a body
that type-checks is valid for every conforming type. It is then **instantiated
once per distinct tuple of concrete type arguments** used by the program, with
each instantiation behaving exactly like a hand-written function in which the
type parameters have been replaced by their arguments. Calls to protocol
requirements resolve to the witnesses of the §20.3 conformance.

Consequences that are part of this specification:

- There is no dynamic dispatch, no hidden allocation, and no per-call
  protocol table. A requirement call has the cost of calling its witness.
- Ownership, copying, exclusivity, and destruction order in an instantiation
  are those of the concrete types (§4).
- Instantiation is transitive: a generic function calling another generic
  function instantiates it with the substituted arguments. A program whose
  instantiation would not terminate (for example `f<T>` calling `f<[T]>`) is
  rejected with `type-checking.instantiation-limit`. Implementations may also
  impose a finite nesting limit of at least 32 and report the same ID.
- Instances are internal to the whole-graph build. They have no C symbol, no
  stable ABI, and are not separately importable binary entities (§12.1).
- Source locations and debug scopes of an instance refer to the generic
  function's own source (§12.1).

**Cost note.** Monomorphization trades code size for speed: each distinct
type argument tuple duplicates the function body. Binary size is therefore a
measurable cost of generic use, not a free abstraction (§0.1.2).

### 20.7 Interaction with other chapters

- **Modules (§12).** `protocol` and generic functions use ordinary
  visibility. An exposed signature may name a type parameter's constraint
  protocols only if each protocol is at least as accessible as the function.
- **Naming (§13).** Protocols are PascalCase, usually named for a capability
  or role (`Shape`, `Handler`).
- **Memory (§4).** No new access effect, ownership state, or reference
  concept is added. `Self` and `T` are ordinary value types.
- **Errors (§8).** `Result<T, E>` with a type parameter in either position is
  allowed in a generic signature; postfix `?` works when the error types are
  the same concrete type after instantiation, as today.
- **C interoperation (§19).** A generic function cannot carry `@c_extern` or
  `@c_export`, and a protocol requirement is not a C signature
  (`type-checking.generic-native-binding`). A generic function may call
  non-generic functions that wrap native bindings.
- **Patterns (§7).** No pattern matches a protocol or a type parameter.

### 20.8 Diagnostics

Diagnostics follow the stable-ID and source-span rules of
[diagnostics](../docs/impl/diagnostics.md). Proposed IDs:

| ID | Condition |
|---|---|
| `type-checking.protocol-used-as-type` | A protocol name used where a type is required |
| `name-resolution.self-outside-type` | `Self` outside a struct, enum, extension, or protocol body (§3.9); implemented |
| `type-checking.invalid-protocol-member` | A requirement with a body, visibility, default, or type parameters |
| `type-checking.reserved-protocol-name` | User declaration of `Copyable`, `Display`, `Equatable`, `Comparable`, or `Any` |
| `type-checking.missing-witness` | A conformance without a witness for a requirement |
| `type-checking.witness-mismatch` | A witness whose effect, labels, types, or result differ |
| `type-checking.witness-not-accessible` | A witness less accessible than the conformance |
| `type-checking.orphan-conformance` | A conformance in a module declaring neither type nor protocol |
| `type-checking.duplicate-conformance` | Two conformances of one `(type, protocol)` in a module |
| `type-checking.invalid-generic-declaration` | Type parameters on anything but a top-level function |
| `type-checking.unsatisfied-constraint` | An inferred type argument does not conform |
| `type-checking.cannot-infer-type-parameter` | No argument determines a type parameter |
| `type-checking.conflicting-type-argument` | Arguments imply different types for one parameter |
| `type-checking.unknown-member-on-type-parameter` | An operation not provided by the constraint |
| `type-checking.generic-native-binding` | Generics on a C binding (§19) |
| `type-checking.instantiation-limit` | Nonterminating or too-deep instantiation |

### 20.9 Grammar and lexical changes

- `protocol` joins the **Declarations** row of §1.4 and is removed from the
  reserved list. `trait` stays reserved; there is one spelling for the
  concept (§0.1, principle 4).
- [Appendix C](appendix-c-grammar.md) gains `protocol_decl` as a `top_decl`,
  the optional `generic_params` in `func_decl`, and the optional conformance
  list in `extension_decl`, all as given in §20.2 to §20.4.
- `extension_decl` still admits only `func_decl` and `subscript_decl` bodies;
  conformance does not allow adding stored fields (§3.5).

### 20.10 Deferred, in a suggested order

1. **Generic structs and enums**, so user containers can exist, plus generic
   methods and extensions (`extension Array<T>: P where T: P`).
2. **Operator-backed protocols**: `Equatable`, `Comparable`, `Display`, with
   `==`, `<`, and string interpolation (§1.7) defined through them.
3. **Default implementations and refinement** (`protocol A: B`).
4. **Associated types**, then a user-extensible iteration protocol for
   `for ... in` (§6.3).
5. **Existentials** (`any P`) with an explicit layout, allocation, and
   dispatch cost model.
6. **Explicit type arguments** at call sites, which require a disambiguation
   rule for `f<T>(x)` against comparison.

Each step is additive: a program valid under v1 keeps its meaning.

### 20.11 Worked example

A server library cannot take a closure, but it can take any `Handler`:

```joyeer
public protocol Handler {
    func handle(request: Request): Response
}

public struct StaticFiles {
    public let root: String
}

extension StaticFiles: Handler {
    public func handle(request: Request): Response {
        // map request.path under root, read the file, build the response
    }
}

public func serveOnce<H: Handler>(handler: H, connection: Int): Result<Void, NetError> {
    let request = readRequest(connection: connection)?
    let response = handler.handle(request: request)
    return writeResponse(connection: connection, response: response)
}

func main() {
    let site = StaticFiles(root: "site")
    // serveOnce(handler: site, connection: fd) instantiates serveOnce<StaticFiles>
}
```

`serveOnce` borrows `handler` and never copies it, so it needs no `Copyable`
bound. The call compiles to a direct call of `StaticFiles.handle`.

---
