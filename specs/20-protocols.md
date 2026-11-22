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
| Orphan rule for conformances | Operator-backed `Comparable` and `Display` |
| `Equatable`, the protocol of `==` and `!=` (§20.13) | |
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
by user code (`type-checking.reserved-protocol-name`). `Equatable` is also
rejected as the name of a user `struct`, `enum` or type parameter, so the name
in a constraint always means the prelude protocol. `Copyable` is the builtin
protocol of §20.5, `Equatable` is the prelude protocol of §20.13 (whose
compiler-bundled declaration is exempt), and the others stay reserved (§12.4,
Appendix A).

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
function type or an optional. The compiler-bundled `Equatable` conformances of
`Optional` and `Result` (§20.13.3) are the one exception. A protocol named twice in one list, or a name
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
methods are witnesses. The library `Array` and `Dict` belong to the bundled prelude and cannot be
extended, so their methods (such as `removeLast` or `get`) never witness a user protocol's
requirement. The one kind of witness that is not a declared method is the compiler-provided witness of
`Equatable` (§20.13.3). A requirement with no witness is
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

In v1, `generic_params` is accepted on **top-level functions** and on
**`struct` and `enum` declarations** (§20.12). Methods,
`init`, subscripts, extensions (which bind their type's parameters instead,
§20.12.2), protocol requirements, and C
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
inference is not performed for function calls. Initializer and enum-case
construction of a generic type is the one exception (§20.12.1).

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
  protocol in v1, with one exception: a generic type `Box<...>` with an
  `extension Box<T>: P` (§20.12.2) satisfies `P`, since that extension
  declares the conformance for every argument;
- `Equatable` by the structural rule of §20.13.5, so `[T]`, `T?`,
  `Result<T, E>` and a generic type with a derived conformance satisfy it when
  their components do.

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

Field access, `match` on a `T`, construction, literals, operators (`+`, `<`;
`==` and `!=` only when `T` satisfies `Equatable`, §20.13.5), string interpolation, `print`, `as` conversions, and subscripts are not
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
value. A linear type therefore cannot be a type argument (§4.1.1), except for the
closed compiler-prelude transfer declarations described in §20.12.3.

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
cannot behave differently for a particular type argument. A derived `Equatable`
witness (§20.13.5) is generated per concrete type, like a copy or a destruction;
it is not a specialization of a checked body.

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
| `type-checking.reserved-protocol-name` | User declaration of a protocol `Copyable`, `Display`, `Equatable`, `Comparable`, or `Any`, or of a struct, enum or type parameter `Equatable` |
| `type-checking.invalid-conformance` | A conformance list naming a non-protocol, `Copyable`, or one protocol twice; a constraint repeating a protocol or with conflicting requirements; a linear type conforming to `Equatable` (§20.13.3) |
| `type-checking.non-equatable-member` | A derived `Equatable` conformance with a field or payload that does not conform (§20.13.5) |
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
| `type-checking.invalid-type-argument` | `Never` inferred as a type argument or written as an array element or dictionary key or value; a type parameter as a `Dict` key |
| `type-checking.unknown-member-on-type-parameter` | A member not provided by the constraint |
| `type-checking.generic-native-binding` | Generics on a C binding (§19) |
| `type-checking.expanding-generic-recursion` | A cycle of generic calls whose type arguments grow |
| `type-checking.instantiation-limit` | An implementation limit on instances exceeded |
| `type-checking.invalid-type-arguments` | A generic type written with the wrong number of type arguments, or a non-generic type given some (§20.12) |
| `type-checking.invalid-extension-header` | An extension of a generic type whose header is not its parameters, once each, without constraints (§20.12.2) |
| `type-checking.unauthorized-intrinsic` | A layout intrinsic used outside a toolchain-authorized unit (§20.12.5) |

### 20.9 Grammar and lexical changes

- `protocol` is in the **Declarations** row of §1.4. `trait` stays reserved;
  there is one spelling for the concept (§0.1, principle 4).
- [Appendix C](appendix-c-grammar.md) has `protocol_decl` (and, in the
  compiler-bundled prelude only, `prelude_protocol`, §20.13) as a `top_decl`,
  the optional `generic_params` in `func_decl`, and the optional conformance
  list in `extension_decl`, all as given in §20.2 to §20.4.
- `extension_decl` still admits only `func_decl` and `subscript_decl` bodies;
  conformance does not allow adding stored fields (§3.5).

### 20.10 Deferred, in a suggested order

1. ~~Generic structs and enums~~ (specified in §20.12). Still deferred: generic
   methods, `where` clauses and conditional conformance
   (`extension Array<T>: P where T: P`); the derived `Equatable` conformance
   of §20.13.5 is the one conditional form.
2. **Operator-backed protocols**: ~~`Equatable`~~ (specified in §20.13).
   Still deferred: `Comparable` and `Display`, with `<` and string
   interpolation (§1.7) defined through them, and `Hashable` for dictionary
   keys (§2.6).
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

### 20.12 Generic structs and enums

[Decision 0022](../docs/decisions/0022-generic-types-and-layout-intrinsics.md). A `struct` or `enum` may
declare type parameters with the constraint syntax of §20.4: `struct Box<T> { var value: T }`,
`enum Tree<T> { Leaf, Node(T) }`, `struct Pair<A, B: Copyable>`. Inside the declaration and its
extensions each parameter is a distinct type (§20.4), noncopyable unless its constraint lists `Copyable`,
never linear, and not `Never`. A generic declaration is checked once against its constraints and
instantiated by monomorphization like a generic function (§20.6): each distinct `Box<Int>` is a concrete
type, and each method is an instance keyed by the method and the type's arguments. `where` clauses, generic
methods and conditional conformance are deferred (§20.10).

A generic type is written with all its type arguments in every type position (`Box<Int>`, `Tree<[T]>`).
A wrong number of arguments, or arguments given to a non-generic type, is
`type-checking.invalid-type-arguments`; each argument must satisfy its constraints
(`type-checking.unsatisfied-constraint`). A user type named like a builtin or prelude type (`Array`, `Optional`) is an
ordinary generic type in its scope, with its own arity.

#### 20.12.1 Construction

Type arguments are inferred by the rules of §20.4.1 from the arguments of an initializer or enum case,
and, for a parameter they leave open, from the **expected type**. This is the only place a result type
informs inference; a call of a generic function never does.

- An expected type exists for a binding annotation, a `return` in a function with a declared result, an
  assignment target or field of known type, and an argument whose parameter type does not mention a type
  parameter still being inferred. A parameter of a generic callee provides none.
- Non-literal arguments are unified first, then the expected type binds the parameters still open, then a
  literal takes its default type. `var b: Box<Double> = Box(value: 1)` is a `Box<Double>`. The expected
  type never overrides what non-literal arguments determined (`conflicting-type-argument`).
- An expected `Box<Int>?` or `Tree<Int>?` supplies `Box<Int>` or `Tree<Int>` (one `?`, as §2.4 wrapping).
- `nil`, `[]`, `[:]` and `.Case(...)` arguments determine nothing; the expected type may fill them.
- A parameter still open is `type-checking.cannot-infer-type-parameter`.

`Box<Int>(value: 1)`, `Box<Int>.make()` and `Tree<Int>.Leaf` are not expression forms. A case or static
function of a generic type is reached as `Tree.Node(1)` with inference or through an expected type
(`let t: Tree<Int> = .Leaf`). Static stored members of a generic type are not accepted.

#### 20.12.2 Extensions and conformance

`extension Box<T>: P { ... }` lists exactly the declaration's parameters, once each, as fresh names (§3.5).
It cannot restate or loosen constraints, and a concrete argument (`extension Box<Int>: P`) would be a
specialization (§20.6); each is `type-checking.invalid-extension-header`. The conformance is
**unconditional**: `Box<X>` conforms to `P` for every `X` that satisfies `Box`'s constraints. Witnesses are
checked once against the parameters' declared constraints. The conformance is keyed by `(type, protocol)`,
so two such extensions in one module are `type-checking.duplicate-conformance` whatever the parameter names.
The one exception is a derived `Equatable` conformance, which holds for `Box<X>` only when the fields do
(§20.13.5).

#### 20.12.3 Ownership

`Box<X>` is copyable and linear as §4.1.1 states. A generic type, like a generic function, does not accept
a linear type argument (`type-checking.linear-type-argument`), or `Any` or `Never`
(`type-checking.invalid-type-argument`). A `linear struct` may be generic; its parameters are affine. Where
§21 names a more specific diagnostic (`linear-collection-element`, `linear-in-deinit-type`,
`linear-channel-element`), that one is reported.

There are two closed exceptions to the linear-argument prohibition. The compiler-prelude task
declarations have their parameters checked as must-transfer values (§20.12.5), not as ordinary affine
placeholders. The role-bound enums `Optional` and `Result` (§2.4, §2.5) accept linear and `Never`
arguments: an instance is linear exactly when a payload type is (§4.1.1), and `Never` makes a case
uninhabited (§2.9). The exception is keyed on the role, not on the name, and does not change the rule for
user or explicit runtime-module generics; `Box<JoinHandle<Int>?>` is still `linear-type-argument`.

#### 20.12.4 Termination

The expanding-recursion rule of §20.6 extends to types: a call graph edge exists for every construction of
a generic type, every method or initializer call on one, every conformance witness, and every field,
signature or function-typed field naming a generic type. A cycle in which a parameter maps to a type
strictly containing a parameter of the cycle (`struct S<T> { var next: S<[T]>? }`) is
`type-checking.expanding-generic-recursion`. Instances are reported and bounded as in §20.6.

A struct that owns values of `T` through an `Int` address has no field of type `T`; it must declare
`deinit` (§4.1.1) or a copy would destroy the values twice.

#### 20.12.5 Layout intrinsics (toolchain-authorized units only)

Five compiler-known generic functions give runtime code the layout of a type (the three further operations of the second table below are authorized the same way). They are not `@c_extern`
imports (§19.4.5), need no declaration, are in scope only in toolchain-authorized units (elsewhere
`type-checking.unauthorized-intrinsic`), and cannot be used as function values or appear in a public
signature. An address is an `Int`; no pointer type exists. `T` is not `Never`, `Any` or linear, except in the closed toolchain-prelude task declarations (decision 0023).

| Function | Meaning |
|---|---|
| `joyeer_intrinsic_size_of<T>(): Int` | Size in bytes of one `T` in memory, a multiple of its alignment, the same for every use of the same concrete type in a program |
| `joyeer_intrinsic_align_of<T>(): Int` | Alignment in bytes |
| `joyeer_intrinsic_move_in<T>(address: Int, value: consuming T)` | Stores `value` at `address`; the slot now owns it |
| `joyeer_intrinsic_move_out<T>(address: Int): T` | Returns the value at `address` as an owned `T`; the slot is uninitialized afterwards |
| `joyeer_intrinsic_destroy_at<T>(address: Int)` | Destroys the value at `address` as a dropped `T` is (a `deinit`, then fields, releasing heap storage); the slot is uninitialized afterwards |

Explicit type arguments are accepted only on these names, only in authorized units (`intrinsic_call`,
Appendix C); in an ordinary program `joyeer_intrinsic_x < y > (z)` is a comparison. `Void` has size 0 and
alignment 1; for `Void` the three memory operations do nothing (a struct without stored fields is accessed like any other value). Every Joyeer value is bitwise
relocatable, so a buffer of `T` may be moved with `memcpy` or `memmove`. The address must be aligned for `T`
and, for `move_in`, hold no live value; nothing checks either. `destroy_at` equals discarding the result of
`move_out`. `move_in`'s `value` is an ordinary `consuming` argument for exclusivity and ownership (§4). A
leaked slot leaks, and destroying a slot twice is undefined: no safety claim is made. There is no borrowing
access to a slot yet.

**Provisional Phase 4 extension** ([0023](../docs/decisions/0023-library-concurrency.md)): three
additional runtime-only operations use the same explicit-intrinsic syntax:

| Function | Meaning |
|---|---|
| `joyeer_intrinsic_stack_slot<T>(): Int` | Uninitialized, correctly sized/aligned storage in the enclosing invocation, with no implicit destruction; valid only until that invocation returns |
| `joyeer_intrinsic_needs_destroy<T>(): Bool` | A compile-time query: whether destroying a `T` needs code (including heap storage, an indirect case, or a `deinit`) |
| `joyeer_intrinsic_callback_address<T...>(function: entry): Int` | The address of a named top-level generic function specialized with one written type argument per type parameter of the entry, in its declaration order |

A callback is itself declared in an authorized runtime unit, has exactly one by-value
`consuming Int` parameter and explicitly returns `Void`, with no receiver or native binding.
This is the scalar entry compatible with the runtime's
`void(void*)` callback on supported 64-bit targets; it does not enable generic `@c_export` or
generic function values in user code. Its address reference participates in ordinary instantiation
discovery, constraints, caching, reports, expanding-recursion checks, and limits, even if no direct
call reaches the function. A raw stack slot is allocated once per invocation, not on loop
backedges. Code must transfer, move out, or destroy each value stored in it exactly once. These
operations preserve the existing authorization gate and make no additional memory-safety claim.

**Provisional compiler-prelude transfer declarations** (0023): the bundled source prelude may mark
its task declarations with `@compiler_transfer()`. Their unconstrained parameters are conservatively
must-transfer, noncopyable, and potentially linear. Every owning value must be moved into a consuming
call, a raw slot, or a return on every returning path, even for affine substitutions. A marked
declaration may accept a linear type argument; no user declaration or `--runtime-module` unit may
claim this metadata. The body is still checked once. Structures with an implicitly destructible
field of such a parameter remain subject to the ordinary linear/deinit restrictions.

In the compiler-owned prelude only, size, alignment, stack slots, `move_in`, `move_out`, and the collection
intrinsics below except `clone_at` (`destroy_range`, the two callback queries, `slot_address`, `stack_block`,
`place_address` and `borrow_address`) may operate on linear/must-transfer types. `destroy_at` and `needs_destroy` never accept one, and `clone_at` needs a copyable
type. The private affine task-block
owner traps on abandonment rather than attempting to destroy a raw linear task input or result.
`@compiler_role(...)` is a closed compatibility role for prelude declarations: it selects the
existing contextual §21 diagnostics and restrictions, never layout. The `optional` and `result` roles
(§2.4, §2.5) additionally bind syntax and propagation lowering (`T?`, `nil`, `?`, and the values the
compiler creates for `as?`, `removeLast` and `get`) to their declarations; they still select no
layout. This amends [decision 0023](../docs/decisions/0023-library-concurrency.md), whose contracts
did not construct results.

The `equatable` role marks the prelude protocol `Equatable` and binds `==` and `!=` to it (§20.13); the
`array_equal` role marks the collections prelude's generic function that is the `Equatable` witness of
`[X]` (§20.13.3). Neither selects layout.

**Provisional collection capabilities** ([0025](../docs/decisions/0025-library-collection-capabilities.md)):
the role `array_type` marks a bundled `public struct Array<T>` with exactly one type parameter and the
`private` stored fields `address: Int`, `count: Int`, `capacity: Int`, in that order. It is the one
role that selects layout-relevant behavior, through a fixed table:

| Capability | Rule |
|---|---|
| Owns | the first type parameter: noncopyability, linearity and destruction treat the struct as if it held a `T` |
| Inert zero | no hidden live flag; destruction runs `deinit`, then zero-fills the instance, so the handle `{0, 0, 0}` is destroyable and a second destroy is a no-op |
| Copy | copyable exactly when `T` is, copied by the method with role `array_copy`; a `deinit` does not make it noncopyable |
| Linear policy | `T` may be linear |
| Read-only `count` | the second field may be read, and borrowed, outside the declaration; it is never writable there |

Four method roles, shape-checked and unique per declaration (`invalid-prelude` otherwise): `array_element`
(`borrowing func ...(index: Int): Int`, the element address, trapping with `array index out of bounds`),
`array_copy` (`borrowing func ...(): Self`), `container_end` (`consuming func ...()` returning `Void`) and
`array_literal` (`private consuming func ...(address: Int, count: Int): Self`, called on an inert zero value by an array
literal with `count` elements moved to `address`, [0026](../docs/decisions/0026-library-array.md)). `self` of an
`array_literal` method is exempt from the linear-not-consumed check, like that of `container_end`.
An `extension` of an `array_type` struct is `invalid-prelude`. `self` of a `container_end` method is exempt
from the linear-not-consumed exit check and is destroyed after the body; the method must trap before
returning unless the container is empty (`array not empty`), and `deinit` of a linear container that is not
empty traps `linear container dropped while not empty`.

A consuming parameter of a method of this role, declared with exactly its owned type parameter,
is an owned argument context (§4.3), determined before substitution. It may receive an owned
temporary, an unmarked copyable place, or an explicitly consumed owning place; every other
consuming parameter and every consuming receiver still requires its marker.
The library body's must-transfer rules remain unchanged.

**Dictionary capabilities** ([0027](../docs/decisions/0027-library-dict.md)): the role `dict_type` marks the bundled
`public struct Dict<K, V>`, which `[K: V]` and dictionary literals bind to, a `public struct` with exactly two
unconstrained type parameters, the key and the value, and the same three `private` `Int` fields and `deinit` as
`array_type`. Its capabilities are those of the table above with both parameters owned, except the linear policy:
neither may be linear. Five method roles, shape-checked and unique per declaration: `dict_element` (`private borrowing
func ...(key: K): Int`, the address of the stored value, trapping on a missing key), `dict_set` (`private mutating
func ...(key: consuming K, value: consuming V): Void`, insert or replace), `dict_get` (`public borrowing func get(key:
K): Optional<V>`, the one public method role), `dict_copy` (`private borrowing func ...(): Self`) and `dict_literal`
(`private consuming func ...(address: Int, count: Int): Self`). The role `dict_entry` marks one `private struct` with
exactly the fields `key: K` then `value: V` and no methods or `deinit`; a dictionary literal packs its entries as that
struct, whose layout is the runtime's entry layout, and an entry is never a value. An `extension` of either struct is
`invalid-prelude`. The prelude is required to declare `dict_type` only when a program mentions a dictionary; the key
rules (§2.6) do not apply to the declaration's own type parameters.

Literal lowering first evaluates every owned element left to right, then initializes one contiguous
stack block and transfers it to the private literal method on an inert-zero receiver. The compiler-generated
sequence may occur in ordinary functions only when its complete raw-address use graph is verified:
matching element type and constant count, one in-range initialization per non-unit slot, no fallible
operation while packing, and one matching literal call. No address escapes this sequence.
Empty and `Void` literals use no element storage. `Never` is not an array element (§2.9), so no array of it
exists; this does not authorize an explicit source layout intrinsic with `Never`.

Seven further layout intrinsics, authorized units only (the bundled prelude and `--runtime-module` units),
with explicit type arguments:

| Function | Meaning |
|---|---|
| `joyeer_intrinsic_clone_at<T>(destination: Int, source: Int)` | Copies the value at `source` into the uninitialized slot `destination` as an implicit copy of a `T`; `T` must be copyable |
| `joyeer_intrinsic_destroy_range<T>(address: Int, count: Int)` | Destroys `count` live elements; for a linear `T` it instead traps `linear container dropped while not empty` when `count != 0` |
| `joyeer_intrinsic_clone_callback<T>(): Int` | The address of the per-type clone helper kept in an array header, or `0` when none applies (always for a noncopyable `T`) |
| `joyeer_intrinsic_destroy_callback<T>(): Int` | The address of the per-type destroy helper, or `0` when none applies (always for a linear or trivial `T`) |
| `joyeer_intrinsic_stack_block<T>(count: N): Int` | Uninitialized contiguous stack storage for `N` slots of `T` (an integer literal, at least 0), valid until the invocation returns, allocated once per invocation, never destroyed; `0` for `N == 0` or a `Void` `T`. Code must move or destroy each value stored in it, as for `stack_slot` |
| `joyeer_intrinsic_place_address<T>(value: &place): Int` | The address of an `inout` place of type `T`, valid for the call it is an argument of. The place's type is exactly `T` (an `Int` place does not stand for `Int?`). It is accepted only as a direct argument of another call that does not store its arguments (not a structure initializer or an enum case), which holds the `inout` access for its duration, so a sibling argument that reads or writes the same place is `overlapping-access`; a binding, a return value or any other use is `type-checking.invalid-call-arguments`. The place is a local `var`, an `inout`, `mutating` or `consuming` receiver, or a field path of those; a `let`, a `borrowing` parameter or receiver, a constant or an rvalue is rejected. It is authorized exactly like the other layout intrinsics (the bundled preludes and `--runtime-module` units) |
| `joyeer_intrinsic_borrow_address<T>(value: place): Int` | The read-only address of the local storage holding the value of a named readable place, whose type is exactly `T`: a local (`let` or `var`), any parameter, or a field path of those; not a subscript, a computed member, a top-level constant or an enum case. For a `borrowing` parameter that storage is the callee's copy aliasing the caller's; for a `String` it is the handle's address. It is accepted only as a direct argument of another call that does not store its arguments and is valid until that call returns; the call holds a read of the place, so a sibling argument that writes it is `overlapping-access`. An rvalue has no place and is `type-checking.invalid-call-arguments`, as is any other use. Writing through it is the library author's error, not checked ([0027](../docs/decisions/0027-library-dict.md)). It is authorized like the other layout intrinsics |
| `joyeer_intrinsic_key_kind<K>(): Int` | The runtime key kind of an admitted dictionary key type, a constant per instance: 1 for `Int`, 2 for `Bool`, 3 for `String`, 4 for `UInt8`. Any other `K` is `type-checking.invalid-type-argument` when it is concrete, and a lowering error when an instance substitutes it; it never yields 0 ([0027](../docs/decisions/0027-library-dict.md)) |
| `joyeer_intrinsic_slot_address<T>(base: Int, index: Int): Int` | `base + index * size_of<T>()` with no overflow or bounds check ([0026](../docs/decisions/0026-library-array.md)): the address of element `index` of a run of `T` slots starting at `base`. For a zero-size `T` it is `base`. The result is an ordinary integer, defined for any operands, but it is a valid place only when `base` is the start of a run of at least `n` `T` slots in one live block and `0 <= index < n` (`index == n` is the end address, which must not be accessed). Library code checks the index before it accesses the result, and keeps byte counts for allocation in checked arithmetic |

For an `array_type` value `a`, `a[i]` calls the `array_element` addressor and then accesses the returned
address as a place of the first type argument. Ordering, exclusivity (§4.4) and the consuming, borrowing,
mutating and copying forms are those of a builtin subscript. No source form names the place; library code
reaches elements through `self[i]`, `move_in`, `move_out` and `destroy_at`. `consume a[i]` of a linear
element is `semantic-analysis.consume-of-linear-element`. These capabilities add no source syntax and make
no memory-safety claim about an address held across a reallocation.

### 20.13 `Equatable`

[Decision 0029](../docs/decisions/0029-equatable.md). **Implementation status:** in part
([plan 0012](../docs/plan/0012-equatable.md)). Implemented: the protocol, the builtin conformers,
hand-written and derived conformances, the conformances of `Optional`, `Result` and arrays, `==` in
generic code, and the presence test. Recursive types need arrays until `indirect` cases are implemented.

#### 20.13.1 The protocol

The compiler-bundled role prelude declares

```joyeer
@compiler_role(equatable)
public protocol Equatable {
    func equals(other: Self): Bool
}
```

`==` and `!=` bind to the declaration with the `equatable` role (§20.12.5), never to the name. The
receiver and the argument are borrowed (§3.2.4, §4.2). `Equatable` states no law that the compiler
checks; a witness decides what equality means for its type.

#### 20.13.2 `==` and `!=`

For two operands of one type `X`, `a == b` type-checks exactly when `X` conforms to `Equatable`, and is
true when `X`'s witness returns `true`. `a != b` is the negation of `a == b`; there is no separate
requirement for it. Otherwise the operands are `type-checking.invalid-operator-operands`, whose help names
`Equatable` and, for a user type of the current module without a conformance, the extension to add.

**Typing.** Neither operand is given the enclosing expression's expected type. An operand that has a type
of its own is checked first; an operand without one (`nil`, an implicit member such as `.Red`, an array
literal whose elements have none either, an empty dictionary literal, a numeric literal, a case of a
generic enum named without type arguments) is then checked against that type, on either side. If neither operand has a type of
its own, it is `type-checking.missing-contextual-type`, except that two numeric literals take their default
types, a floating-point literal first (`0 == 0` compares two `Int`s, `0 == 0.5` two `Double`s). There is no conversion or
implicit wrapping between the operands: `opt == 5` is rejected, with help suggesting `.Some(5)`.

**Evaluation and access.** The left operand is evaluated, then the right, then the result. For a builtin
conformer (§20.13.3, item 1) the left operand is read as a value before the right is evaluated, so
`n == bump(&n)` compares the old value of `n`. For every other conformer, and for every `==` and `!=` in
a generic body, `a == b` is analyzed exactly like the call `a.equals(other: b)`: the left operand is a
borrow that stays active while the right is evaluated (§4.4). So `p == bump(&p)` is an overlapping access
and `a == reopen(t: consume a)` uses `a` after it is moved, while `a == a` is two shared borrows.

#### 20.13.3 Conforming types

1. **Builtin conformers.** Every integer type, `Bool`, `UInt8`, `Float`, `Double`, `String` and `Void`
   conform by the compiler, and `Char` conforms when it is implemented. Integer, `Bool` and `String`
   equality compare values. `Float` and `Double` use IEEE 754 equality: a NaN is not equal to itself, and
   `-0.0 == 0.0`. All `Void` values are equal. A user extension of a builtin type is
   `name-resolution.invalid-extension-target` while such extensions are not implemented (§3.5), and would
   be `type-checking.orphan-conformance` after that (§20.3).
2. **User structs and enums** conform only by an `extension X: Equatable { ... }` in their own module
   (§20.3). If the type or one of its extensions declares a member named `equals`, the conformance is
   **hand-written**: that member is the witness, checked as any witness (`witness-mismatch` when its
   labels, effects or types differ). Otherwise the conformance is **derived** (§20.13.5).
3. **`Optional` and `Result`** conform by derived conformances that the role prelude declares
   (`extension Optional<T>: Equatable {}`, `extension Result<T, E>: Equatable {}`): `X?` conforms when `X`
   does, and `Result<X, Y>` when `X` and `Y` do.
4. **Arrays.** `[X]` conforms when `X` does. Its witness is the instance for `X` of the collections
   prelude's generic function with the `array_equal` role, which compares the counts and then the elements
   in index order with `==`, stopping at the first that differs.
5. **Never conforming:** a function type (§2.3.1); a `Dict` (§2.6), until a `Hashable` decision; a
   linear type, including a type that is linear because of a field or payload (§4.1.1), whose conformance
   is `type-checking.invalid-conformance`, reported instead of any `non-equatable-member` for the same
   extension. A noncopyable type may conform: equality only borrows. A struct with a function or `Dict`
   field may conform with a hand-written `equals`, but cannot derive one.

Only a hand-written witness is a member: `x.equals(other: y)` on a concrete type is an unknown member
unless `equals` is declared, so `==` is the spelling for builtin, derived and array equality. `match`
patterns are structural and need no conformance.

#### 20.13.4 Presence test

When one operand is the absent case of the `optional` role written without a payload (`nil` or `.None`,
on either side and also in parentheses) and the other has a type `X?`, `==` and `!=` test whether the
other operand is present. The test is recognized from the syntax before ordinary equality, so it applies
whatever `X` is: a function type, a noncopyable or a linear type. It never calls a witness, borrows the
operand, and costs one tag comparison once optimized (unoptimized, it is a branch on the tag).
`nil == nil` is `type-checking.missing-contextual-type`. A linear operand must be a place: comparing a linear temporary, such as `spawnMaybe() == nil`, drops it and is
`semantic-analysis.linear-result-dropped`.

#### 20.13.5 Derived and conditional conformance

A derived witness compares, for a struct, its stored fields in declaration order, and for an enum, its
tags and then, for equal tags, the payloads in order; it stops at the first pair that differs. A struct
with no stored fields is always equal. Floating-point fields are compared one by one with no identity
shortcut, so a value holding a NaN is not equal to itself. Every stored field and payload type must
conform; a `Never` payload counts as conforming, since it has no value.

A derived conformance is **structural and conditional**: for a generic type, `Box<X>` conforms when
every field and payload type, with `X` substituted, conforms. This is the one exception to the
unconditional conformance of §20.12.2; a hand-written `equals` on a generic type stays unconditional.

- **Order.** All conformance declarations of the program are known before any derived conformance is
  evaluated, so the order of extensions within or across files does not matter.
- **Recursion.** While `X: Equatable` is being decided, a nested question about the same `X` is answered
  yes. Recursive types such as `indirect enum List { Nil, Cons(Int, List) }` or
  `struct Node { var kids: [Node] }`, and mutually recursive generic types, therefore conform when their
  other fields do. Types whose arguments grow are rejected by §20.12.4.
- **Generic code** (§20.4.2, §20.5). Inside a generic body, a type parameter conforms exactly when its
  constraint lists `Equatable`, and the structural rule applies to every type built from it: `[T]`, `T?`,
  `Result<T, E>` and a `Box<T>` with a derived conformance. A type that conforms under `T`'s constraint
  conforms for every type argument that satisfies it, so the body is still checked once (§20.6).
  `func f<T>(a: T?, b: T?): Bool { return a == b }` is rejected; with `<T: Equatable>` it is accepted.
  Without the constraint, `==` on `T` is `invalid-operator-operands` with a note suggesting `Equatable` in
  `T`'s constraint. A requirement call `a.equals(other: b)` on a `T: Equatable` is allowed and calls the
  same witness as `a == b` in each instance.
- **The derived witness has no checked body.** It is generated for each concrete type whose conformance
  holds, like a copy or a destruction (§20.6).

**Diagnostics.** A field or payload whose type cannot conform for any type argument (a function type, a
`Dict`, a non-conforming concrete type) is `type-checking.non-equatable-member` at the extension, naming
the field, or the case and payload position, and its type. A field that conforms only for some arguments
(`var value: T`) is checked where the conformance is needed: `Box<Fn> == Box<Fn>` is
`type-checking.non-equatable-member` naming the field path, with a note at the extension, and a generic
call that infers such a type for a `U: Equatable` parameter is `type-checking.unsatisfied-constraint`
whose help names the same field path. `Optional`, `Result` and arrays are the exception: `==` on one whose
payload or element does not conform is `type-checking.invalid-operator-operands`, whose help names that
payload or element. When a derived conformance is rejected because a member's own conformance is, only
the member's is reported.

#### 20.13.6 Cost

A builtin conformer's `==` compiles exactly as before `Equatable` existed: a scalar comparison, or the
inlined `String` comparison. Every other `==` is a direct call of its witness, which may be inlined; `!=`
adds one Boolean negation. There is no protocol table, boxing or dynamic dispatch. A derived or array
comparison costs one comparison per field or element compared.
