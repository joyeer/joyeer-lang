## §20 Protocols and Bounded Generic Functions

> **Status:** normative for v1
> ([decision 0021](../docs/decisions/0021-protocols-and-generic-functions.md)). This chapter
> defines a deliberately small first version ("v1") of protocols and generic
> functions. **Implementation status:** see
> [implemented language surface](../docs/impl/supported-features.md). Conformances
> are declared by extensions, and extensions of builtin types or of a type from
> another module are not implemented (§3.5), so a conformance can be declared
> only in the module of the conforming user type.

### 20.1 Purpose and scope

A **protocol** names a set of method requirements. A **bounded generic
function** is written once and is compiled separately for each concrete type
it is used with. Together they let a library accept "any type that can do X"
without closures, inheritance, vtables, or boxing.

**Design rationale.** Joyeer has no closures, no subtyping, and no reference
types (§4.8), so a callback or interface cannot be passed as a value with its
own state. Protocol-bounded generics fill that gap with static dispatch: the
call is resolved at compile time, costs nothing beyond the called method, and
keeps ownership and access effects visible in the signature (§0.1). Protocols
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
                    '(' [ requirement_param , ... ] ')' [ ':' type ]
requirement_param
                ::= label [ identifier ] ':' [ access_effect ] type
```

A requirement is a method signature with no body. It has no visibility
modifier, no attributes, no default parameter values, and no type
parameters (`type-checking.invalid-protocol-member`). Every requirement has
the visibility of its protocol. Two requirements with one name in one protocol
are a duplicate declaration (`name-resolution.duplicate-declaration`).

`method_effect` and parameter access effects (§3.2.4, §4.2) are part of the
requirement. A requirement that mutates its receiver is written
`mutating func reset()`, exactly as in a method declaration, and generic code
calls it with the `&` receiver marker.

Inside a protocol, `Self` denotes the conforming type (§3.9) and may appear in
parameter and result types, including as `Self?`, `[Self]`, and
`Result<Self, E>`. It is a placeholder here, not an alias: each conforming type
substitutes itself for `Self` when its witnesses are checked (§20.3). `Self`
may not be a `Dict` key type (§20.5).

```joyeer
public protocol Shape {
    func area(): Int
    func scaled(by factor: Int): Self
}
```

A protocol is a **type-namespace** declaration (§13.1): it may not share a
name with a struct, enum, or another protocol in the same scope. The names
`Display`, `Equatable`, `Comparable`, `Any`, and `Copyable` cannot be declared
by user code (`type-checking.reserved-protocol-name`); `Copyable` is the
builtin protocol of §20.5 and the others stay reserved (§12.4, Appendix A).

A protocol has no stored state and no instances. It is **not a type**: using
a protocol name where a type is expected is an error
(`type-checking.protocol-used-as-type`) whose help text points to a
generic parameter.

**Design rationale.** Banning protocol-typed values in v1 means there is no
existential layout, no hidden allocation, and no dynamic dispatch to specify.
A future `any P` (§20.10) can be added without changing any rule here.

### 20.3 Conformance

```
extension_decl  ::= 'extension' extended_type [ ':' protocol_ref , ... ]
                    '{' ( func_decl | subscript_decl )* '}'
extended_type   ::= nominal_type [ '<' type , ... '>' ]
                 |  '[' type ']'
                 |  '[' type ':' type ']'
protocol_ref    ::= nominal_type      // names a protocol
```

There is exactly one way to declare a conformance: an `extension` with a
conformance list. Struct and enum declarations do not list protocols. The
extended type may be a user `struct`/`enum` or a builtin nominal such as
`Int`, `String`, or `Array<Int>` (a concrete instantiation); it is not a
function type or an optional. A protocol named twice in one list, or a name
in the list that is not a protocol, is an error.

```joyeer
struct Square {
    let side: Int
    func area(): Int { self.side * self.side }
}

extension Square: Shape {
    func scaled(by factor: Int): Self { Square(side: self.side * factor) }
}
```

A conformance is valid when every requirement has exactly one **witness**: a
method of the extended type, found by name, declared in the conformance
extension or elsewhere in the type or its other extensions. Only declared
methods are witnesses: a compiler-known member of a builtin type (such as
`Array.removeLast` or `Dict.get`) is not a function and cannot witness a
requirement. A requirement with no witness is
`type-checking.missing-witness`. The witness must match the requirement
exactly after substituting `Self` with the extended type
(`type-checking.witness-mismatch` otherwise):

- the same method effect (`borrowing`, `mutating`, `consuming`);
- the same labels (internal parameter names may differ);
- the same parameter access effects and types;
- the same result type.

There is no subtyping, implicit conversion, or effect weakening between a
requirement and its witness (§2). A witness's default parameter values are
ignored when it is called through the protocol: generic code supplies every
argument the requirement lists. A witness may not be a C binding (§19).

A witness must be at least as accessible as the **conformance**, whose
accessibility is the lesser of the protocol's and the extended type's
(`type-checking.witness-not-accessible`). A `public` protocol on a `public`
type therefore needs `public` witnesses; the check prevents a conformance from
exposing a method the type does not.

**Coherence.** A conformance of type `T` to protocol `P` must be declared in
the module that declares `T` or the module that declares `P`
(`type-checking.orphan-conformance`). Builtin types count as declared by
neither user module, so only the protocol's module may conform them. A module
may declare at most one conformance of a given type to a given protocol
(`type-checking.duplicate-conformance`). Because the module graph is acyclic
(§12.1), these two rules guarantee that at most one conformance of `(T, P)`
exists in any program, with no global search and no import-order dependence.

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
bindings (`@c_extern`, `@c_export`) may not declare type parameters
(`type-checking.invalid-generic-declaration`; for a C binding,
`type-checking.generic-native-binding`). A protocol repeated in one constraint
(`P & P`) is an error.

Type parameters are named like types (§13). Their scope is the signature and
body of the function. A type parameter may be used anywhere a type may appear
in the function, including inside builtin containers and function types:
`[T]`, `T?`, `Result<T, E>`, `[String: T]`, `(borrowing T): Int`. It may not be
given type arguments, and it may not be a `Dict` key type (§20.5).

```joyeer
func totalArea<S: Shape>(shapes: [S]): Int {
    var total = 0
    var index = 0
    while index < shapes.count {
        total += shapes[index].area()
        index += 1
    }
    return total
}
```

The index loop borrows each element. `for shape in shapes` would copy the
array and its elements, which would require `S: Copyable` (§6.3, §20.5).

`&` in a constraint joins protocols and is unrelated to the call-site `&`
marker (§4.3); the two never appear in the same syntactic position.

**A type parameter is a distinct type.** Inside the generic function it is
equal only to itself. Two different parameters, or a parameter and a concrete
type, are never the same type, whatever the type arguments of a later call
are.

Joyeer has no overloading (§12.2, §13): a generic function's name follows the
ordinary uniqueness and shadowing rules.

#### 20.4.1 Inference of type arguments

**Type arguments are inferred, never written.** A call supplies no `<...>`.
Each type parameter is determined by structural unification of the argument
types with the parameter types; inside another generic function, a parameter
may be determined to be one of the caller's own type parameters. After that,
each argument is checked against its substituted parameter type with the
ordinary rules.

- Arguments that are not literals are unified first. A literal then takes the
  type already inferred for its position, and only a literal whose position is
  still undetermined takes its default type (`Int`, `Double`, §2.8). A literal
  here is an integer, floating-point, string or boolean literal, optionally
  negated; `1 + 2` is an expression with its usual type. So with
  `func pair<T>(a: T, b: T)`, `pair(a: 1, b: 2.5)` gives `T = Double`.
- An argument that carries no type of its own (`nil`, `[]`, `[:]`, an
  implicit-member `.Case(...)`) and a diverging (`Never`-typed) argument
  determine nothing.
- Implicit wrapping into `.Some` (§2.4) is not applied during unification: an
  `Int` argument for a `T?` parameter determines nothing about `T`. The
  argument is checked afterwards, where wrapping applies as usual.
- An omitted argument with a default value determines nothing; the default is
  checked against the substituted parameter type.
- A function-value argument for a parameter of function type unifies
  structurally, access effects included (§2.3.1).

Every use of one parameter must unify to one type, otherwise
`type-checking.conflicting-type-argument`. A parameter that nothing
determines is `type-checking.cannot-infer-type-parameter`; return-type-only
inference is not performed.

After inference, every constraint must hold, otherwise
`type-checking.unsatisfied-constraint` names the type argument, the parameter
and the protocol; for `Copyable` it adds the reason the type is noncopyable
(§4.1.1). A type argument may not be linear (§4.1.1,
`type-checking.linear-type-argument`) or `Never` (§2.9,
`type-checking.invalid-type-argument`), including when it is inferred, such as
`E` from a `Result<Int, Never>` argument.

Because calls never contain `<`, the grammar of
[Appendix C](appendix-c-grammar.md) is not made ambiguous with the comparison
operator.

A generic function can only be called. It cannot be named as a function value
(§2.3.1, `type-checking.not-a-function-value`) and cannot be a `spawn` target
(§21.2.1).

#### 20.4.2 Constraint satisfaction

A concrete type satisfies a user protocol `P` when a conformance of it to `P`
exists (§20.3), and satisfies `Copyable` when §4.1.1 makes it copyable.

Inside a generic function, a type that mentions the function's parameters
satisfies:

- `Copyable` when §4.1.1 makes it copyable, treating each parameter as
  copyable exactly when its constraint lists `Copyable`. A function type is
  always copyable, whatever it mentions;
- a user protocol `P` only when it is exactly a parameter whose constraint
  lists `P`. A type built from a parameter (`[T]`, `T?`) satisfies no user
  protocol in v1.

### 20.5 What generic code may do

Inside the body, a value of type `T` is opaque. The only operations that
exist for it are:

1. the requirement methods of the protocols in its constraint, called with
   the same access markers (`&`, `consume`) as any method (§3.2.4, §5.4). A
   requirement call has the receiver effect and parameter access effects of
   the requirement, with `Self` replaced by `T`;
2. passing it through `borrowing`, `inout`, `consuming`, and `initializing`
   parameters (§4.2) of functions whose parameter types accept `T` (generic
   functions, and function values whose type mentions `T`), and storing it in
   locals, builtin containers, and `Optional`/`Result` payloads;
3. destroying it when its owner's scope ends (§4.7), using whatever
   destruction the concrete type has.

Field access, `match` on a `T`, construction, literals, operators (`+`, `==`,
`<`), string interpolation, `print`, `as` conversions, and subscripts are not
available on `T`
(`type-checking.unknown-member-on-type-parameter` for a member, the ordinary
type error otherwise). A type parameter, and `Self` in a requirement, may not
be a `Dict` key type: keys are restricted (§2.6) and no protocol states that
restriction in v1.

If two protocols of one constraint require a method with the same name,
identical requirements (after `Self` is replaced) are one requirement with one
witness; differing ones are an error at the constraint, because witnesses are
found by name and no type could have both.

**Copyability.** A type parameter is **noncopyable** unless its constraint
includes the builtin marker protocol `Copyable`. Any operation that the memory
model defines as a copy (§4.1) is rejected for a noncopyable `T` value,
including `let b = a` from an existing binding, returning a borrowed
parameter, storing a borrowed value into a container, and `[String: T].get`.
The diagnostic is the ordinary `type-checking.copy-of-noncopyable` with an
added note that adding `& Copyable` to the constraint permits it.

**Linearity.** A type parameter is never linear, so a `T` value may be
dropped: it is destroyed at the end of its owner's scope like any noncopyable
value. A linear type therefore cannot be a type argument (§4.1.1).

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
that type-checks is valid for every type argument that satisfies them. Every
name, member, literal type, copy and move decision, and access check in the
body is fixed then; no diagnostic depends on a type argument, except the
instantiation limit below. There is no specialization: a generic function
cannot behave differently for a particular type argument.

The function is then **instantiated once per distinct tuple of concrete type
arguments** used by the program. An instance differs from the checked body in
two ways only: a requirement call calls the witness of the §20.3
conformance, and a `T` value is moved, copied (only when `T: Copyable`) and
destroyed with the concrete type's operation. Which moves, copies, accesses
and destructions happen, and in what order, is fixed by the checked body.

Consequences that are part of this specification:

- There is no dynamic dispatch, no hidden allocation, and no per-call
  protocol table. A requirement call has the cost of calling its witness.
- Instantiation is transitive: a generic function calling another generic
  function instantiates it with the substituted arguments. A generic function
  that no non-generic code reaches has no instances and no code; its body is
  still checked.
- **Expanding recursion is rejected.** Consider the calls from generic
  functions to generic functions, each with its type arguments. A cycle of
  such calls in which some type parameter is passed on as a type that
  strictly contains a type parameter of the cycle (`f<T>` calling `f` with a
  `[T]` argument, or `f` and `g` passing `T?` to each other) is
  `type-checking.expanding-generic-recursion` at the call, whether or not the
  cycle is reachable.
- An implementation may limit the number of instances, the nesting depth of
  type arguments, and the length of an instantiation chain; a nesting limit is
  at least 32. Exceeding one is `type-checking.instantiation-limit`, reported
  at the call that required the instance.
- Instances are internal to the whole-graph build. They have no C symbol, no
  stable ABI, and are not separately importable binary entities (§12.1).
- Source locations and debug scopes of an instance refer to the generic
  function's own source (§12.1).

**Cost note.** Monomorphization trades code size for speed: each distinct
type argument tuple duplicates the function body. Binary size is therefore a
measurable cost of generic use, not a free abstraction (§0.1.2). The
compiler can report every instance and its size
([implemented surface](../docs/impl/supported-features.md)).

### 20.7 Interaction with other chapters

- **Modules (§12).** `protocol` and generic functions use ordinary
  visibility. An exposed signature may name a type parameter's constraint
  protocols only if each protocol is at least as accessible as the function
  (§12.2).
- **Naming (§13).** Protocols are PascalCase, usually named for a capability
  or role (`Shape`, `Handler`). Type parameters are single capitals or
  PascalCase.
- **Memory (§4).** No new access effect, ownership state, or reference
  concept is added. `Self` and `T` are ordinary value types.
- **Errors (§8).** `Result<T, E>` with a type parameter in either position is
  allowed in a generic signature. Postfix `?` requires the operand's error
  type and the enclosing function's error type to be the same type in the
  body: the same type parameter or the same concrete type.
- **C interoperation (§19).** A generic function cannot carry `@c_extern` or
  `@c_export`, and a protocol requirement is not a C signature
  (`type-checking.generic-native-binding`). A generic function may call
  non-generic functions that wrap native bindings.
- **Patterns (§7).** No pattern matches a protocol or a type parameter.

### 20.8 Diagnostics

Diagnostics follow the stable-ID and source-span rules of
[diagnostics](../docs/impl/diagnostics.md).

| ID | Condition |
|---|---|
| `type-checking.protocol-used-as-type` | A protocol name used where a type is required |
| `name-resolution.self-outside-type` | `Self` outside a struct, enum, extension, or protocol body (§3.9) |
| `type-checking.invalid-protocol-member` | A requirement with a body, visibility, attribute, default, or type parameters |
| `type-checking.reserved-protocol-name` | User declaration of `Copyable`, `Display`, `Equatable`, `Comparable`, or `Any` |
| `type-checking.invalid-conformance` | A conformance list naming a non-protocol, `Copyable`, or one protocol twice; a constraint repeating a protocol or with conflicting requirements |
| `type-checking.missing-witness` | A conformance without a witness for a requirement |
| `type-checking.witness-mismatch` | A witness whose effect, labels, types, or result differ |
| `type-checking.witness-not-accessible` | A witness less accessible than the conformance |
| `type-checking.orphan-conformance` | A conformance in a module declaring neither type nor protocol |
| `type-checking.duplicate-conformance` | Two conformances of one `(type, protocol)` in a module |
| `type-checking.invalid-generic-declaration` | Type parameters on anything but a top-level function; a type parameter given type arguments |
| `type-checking.unsatisfied-constraint` | An inferred type argument does not satisfy a constraint |
| `type-checking.cannot-infer-type-parameter` | No argument determines a type parameter |
| `type-checking.conflicting-type-argument` | Arguments imply different types for one parameter |
| `type-checking.linear-type-argument` | A linear type inferred as a type argument |
| `type-checking.invalid-type-argument` | `Never` inferred as a type argument; a type parameter as a `Dict` key |
| `type-checking.unknown-member-on-type-parameter` | A member not provided by the constraint |
| `type-checking.generic-native-binding` | Generics on a C binding (§19) |
| `type-checking.expanding-generic-recursion` | A cycle of generic calls whose type arguments grow |
| `type-checking.instantiation-limit` | An implementation limit on instances exceeded |

### 20.9 Grammar and lexical changes

- `protocol` is in the **Declarations** row of §1.4. `trait` stays reserved;
  there is one spelling for the concept (§0.1, principle 4).
- [Appendix C](appendix-c-grammar.md) has `protocol_decl` as a `top_decl`,
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
