# The Joyeer Programming Language — Specification

> **Status:** Draft v0.1-spec. This document defines the future language. Some
> features are marked **syntax-only** (parsed and type-checked, but not yet
> semantically enforced). Legacy syntax that exists in the current parser
> (`class`, named-only calls, `print(message: x)`) is listed in §14 and will
> be removed.

---

## §0 Preamble

### 0.1 Design philosophy

Joyeer is an **AI-era systems language**. AI writes the bulk of the code;
humans review, audit, and refine. The language optimizes for:

1. **Verifiability over brevity.** Stronger types catch more AI mistakes at
   compile time.
2. **Explicit intent over implicit behavior.** Side effects, mutation, and
   ownership transfer must be visible at the call site and in the signature.
3. **Value semantics by default.** No GC, no reference counting, no hidden
   aliasing. Heap allocation is opt-in.
4. **One way to do each thing.** Reduce stylistic variance so AI output is
   predictable and code review is fast.
5. **C-replacement performance.** Zero-cost abstractions; predictable
   layout; no runtime overhead for safety features.

References: Hylo's mutable value semantics, Swift's syntactic surface,
Rust's memory safety guarantees, Dafny's contracts, Koka's effect system.

### 0.2 Versioning

| Version | Scope |
|---------|-------|
| **v0.1** | Lexical, types, declarations, memory model, expressions, statements, patterns, generics, errors, modules. Contracts/effects/property annotations parse but are not enforced. |
| **v0.2** | Removal of legacy syntax (§14). Improved diagnostics. Standard library. |
| **v0.3** | Runtime contract enforcement (debug mode). Property-based test runner. |
| **future** | Concurrency, SMT-backed verification, FFI, macros, traits. |

### 0.3 Feature status legend

| Mark | Meaning |
|------|---------|
| ✅ | Locked in v0.1; semantics fully enforced. |
| 🔬 | Syntax-only in v0.1; parsed, type-checked at signature level, but no deeper semantic enforcement yet. |
| ⏳ | Reserved keyword / future syntax; rejected with a clear error message in v0.1. |
| ⛔ | Deprecated legacy; accepted by the parser with a warning in v0.1, removed in v0.2. |

### 0.4 Notation

Grammar rules use a compact EBNF dialect:

```
nonterminal  ::= alternative1 | alternative2
'literal'      — terminal token (verbatim)
[ X ]          — optional
{ X }          — zero or more
( X )          — grouping
X+             — one or more
X*             — zero or more
X , ...        — comma-separated list of one or more X
```

Examples are written in fenced ` ```joyeer ` blocks.

📌 **Decisions** are inline callouts of the form:

> **📌 Decision D1.** *Inout call-site marker is `&x`.*  Alternatives: `inout x`
> (keyword form). Chosen `&x` because it is concise and consistent with Swift,
> which our target audience already knows.

---

## §1 Lexical Structure

### 1.1 Source encoding & whitespace

- Source files are UTF-8.
- Line breaks: `U+000A` (LF), `U+000D` (CR), or `CRLF`.
- Whitespace separates tokens but is otherwise insignificant.
- Indentation is **not** semantically significant (unlike Python).

### 1.2 Comments

```
// single-line comment until end of line
/* block comment, may /* nest */ */
/// doc comment for the following declaration
```

Doc comments (`///`) are attached to the next declaration and available to
tooling.

### 1.3 Identifiers

```
identifier      ::= ident_head ident_tail*
ident_head      ::= 'a'..'z' | 'A'..'Z' | '_'
ident_tail      ::= ident_head | '0'..'9'
```

Identifiers are case-sensitive. The lone `_` is the **wildcard** and is not
an identifier.

### 1.4 Keywords (full reserved list)

| Category | Keywords |
|----------|----------|
| Bindings | `let`, `var` |
| Control flow | `if`, `else`, `while`, `for`, `in`, `match`, `return`, `break`, `continue`, `yield` |
| Declarations | `func`, `struct`, `enum`, `extension`, `subscript`, `init`, `deinit`, `typealias`, `import` |
| Memory effects | `inout`, `borrowing`, `consuming`, `initializing`, `consume`, `mutating` |
| Visibility | `public`, `internal`, `private` |
| Types & literals | `true`, `false`, `nil`, `self`, `Self`, `Any` |
| Contracts 🔬 | `requires`, `ensures`, `invariant`, `old`, `forall`, `exists`, `result` |
| Effects 🔬 | `performs`, `pure` |
| Pattern | `where`, `as`, `is`, `_` |
| Reserved ⏳ | `async`, `await`, `actor`, `throws`, `try`, `catch`, `defer`, `class`, `protocol`, `trait`, `macro` |

> `class` is in the **reserved ⏳** list because it is removed in v0.2; the
> parser still accepts it through v0.1 with a deprecation warning (§14).

### 1.5 Literals

```
integer_literal  ::= decimal_lit | hex_lit | binary_lit | octal_lit
decimal_lit      ::= digit ( digit | '_' )*
hex_lit          ::= '0x' hex_digit ( hex_digit | '_' )*
binary_lit       ::= '0b' ( '0' | '1' | '_' )+
octal_lit        ::= '0o' ( '0'..'7' | '_' )+

float_literal    ::= decimal_lit '.' decimal_lit [ exponent ]
                  |  decimal_lit exponent
exponent         ::= ('e'|'E') ['+'|'-'] decimal_lit

bool_literal     ::= 'true' | 'false'
nil_literal      ::= 'nil'

string_literal   ::= '"' string_item* '"'
string_item      ::= escape | interpolation | <any unicode except '"' or '\'>
escape           ::= '\\' ( 'n' | 't' | 'r' | '"' | '\\' | '0' | 'u{' hex+ '}' )
interpolation    ::= '\(' expression ')'

array_literal    ::= '[' [ expression , ... ] ']'
dict_literal     ::= '[' ':' ']'  |  '[' dict_entry , ... ']'
dict_entry       ::= expression ':' expression
tuple_literal    ::= '(' expression , expression , ... ')'
```

Underscores in numeric literals are visual separators only: `1_000_000`.

### 1.6 Operators & punctuation

| Class | Tokens |
|-------|--------|
| Arithmetic | `+` `-` `*` `/` `%` |
| Comparison | `==` `!=` `<` `<=` `>` `>=` |
| Logical | `&&` `\|\|` `!` |
| Bitwise | `&` `\|` `^` `~` `<<` `>>` |
| Assignment | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` |
| Range | `..<` `...` |
| Postfix | `?` (optional chain / Result-propagation) `!` (force-unwrap) |
| Member / call | `.` `(` `)` `[` `]` `{` `}` `,` `;` `:` `->` `=>` `_` |
| Memory marker | `&` (call-site inout marker) |

`&` is both bitwise-AND (binary infix) and the inout call-site marker
(prefix on argument). Disambiguated by syntactic position.

### 1.7 String interpolation 📌

> **📌 Decision D7.** *String interpolation included in v0.1, Swift-style
> `"\(expr)"`.*  Alternative: defer to v0.2. Included because the bytecode-printer
> use case (writing tests, debug output) is pervasive and otherwise requires
> manual concatenation.

```joyeer
let n = 42
print("answer is \(n), squared is \(n * n)")
```

The expression inside `\(...)` is any `expression`. The result must implement
`Display` (see §12 prelude).

---

## §2 Type System

### 2.1 Primitive types

| Type | Bits | Range / domain |
|------|------|----------------|
| `Bool` | 1 (stored as byte) | `true` / `false` |
| `Int` | 64 | signed two's complement |
| `Int8` `Int16` `Int32` `Int64` | as named | signed |
| `UInt` | 64 | unsigned |
| `UInt8` `UInt16` `UInt32` `UInt64` | as named | unsigned |
| `Float` | 32 | IEEE 754 single |
| `Double` | 64 | IEEE 754 double |
| `Char` | 32 | Unicode scalar |
| `String` | (pointer, length) | UTF-8 |
| `Void` / `()` | 0 | unit type |

`Int` and `UInt` are platform-pointer-sized but always 64-bit on the targets
Joyeer supports. (No 32-bit target in v0.1.)

### 2.2 Nominal types

A nominal type is introduced by a `struct` or `enum` declaration (§3.3,
§3.4). Names are PascalCase (§13).

### 2.3 Composite types

```
type            ::= primitive_type
                 |  nominal_type [ generic_args ]
                 |  array_type
                 |  dict_type
                 |  tuple_type
                 |  function_type
                 |  optional_type

array_type      ::= '[' type ']'
dict_type       ::= '[' type ':' type ']'
tuple_type      ::= '(' type , type , ... ')'
function_type   ::= '(' [ param_type , ... ] ')' '->' type
optional_type   ::= type '?'
generic_args    ::= '<' type , ... '>'
```

`function_type` parameters carry effect annotations exactly like declared
functions (§3.2):

```joyeer
let f: (inout Int) -> Int = ...
let g: (consuming String) -> ()  = ...
```

### 2.4 Optional<T>

`T?` is sugar for `Optional<T>`:

```joyeer
enum Optional<T> {
  None,
  Some(T),
}
```

Construction: `nil` for `None`, `T` is implicitly coerced to `Some(T)` in
positions expecting `T?`. Force-unwrap with `x!` (traps on `None`).
Optional chaining with `x?.field` (see §5.4).

### 2.5 Result<T, E>

`Result<T, E>` is a standard-library enum (no special syntax):

```joyeer
enum Result<T, E> {
  Ok(T),
  Err(E),
}
```

See §8 for error-handling semantics.

### 2.6 Generics 📌

> **📌 Decision D2.** *Generic syntax uses `<T>`*  Alternative: Hylo's `[T]`.
> Chosen `<T>` for familiarity with Swift, Rust, C#, TypeScript users.

```joyeer
func swap<T>(a: inout T, b: inout T) { ... }
struct Pair<A, B> { var first: A; var second: B }
extension Array<T> { ... }
```

Constraints via `where` clauses (§3.2.3). v0.1 supports parametric generics
only (no associated types, no higher-kinded types).

### 2.7 Type aliases

```
typealias_decl ::= 'typealias' identifier [ generic_params ] '=' type
```

```joyeer
typealias StringMap<V> = [String: V]
typealias Bytes = [UInt8]
```

### 2.8 Type inference

Local bindings infer their type from the initializer when not annotated:

```joyeer
let x = 42          // inferred Int
let y: Double = 1   // explicit Double (literal coerced)
var a: [Int] = []   // annotation required when initializer is ambiguous
```

Function parameters and return types **must** be annotated explicitly.
This is a deliberate AI-era choice (§0.1): signatures are contracts.

```joyeer
func add(a: Int, b: Int): Int { return a + b }  // ✅
func add(a, b) { return a + b }                  // ❌ error: missing types
```

---

## §3 Declarations

### 3.1 Bindings: let / var

```
binding         ::= ( 'let' | 'var' ) pattern [ ':' type ] [ '=' expression ]
```

- `let` introduces an **immutable** binding. Reassignment is a compile error.
  The value's interior fields may be mutated only if the type permits
  through `inout` projections, but the binding itself never re-binds.
- `var` introduces a **mutable** binding.

```joyeer
let pi = 3.14159
var count: Int = 0
let (x, y) = somePair        // tuple destructuring
```

### 3.2 Function declarations

```
func_decl       ::= [ visibility ] [ method_effect ] 'func' identifier [ generic_params ]
                    '(' [ param , ... ] ')'
                    [ ':' return_type ]
                    [ where_clause ]
                    [ effect_clause ]
                    [ contract_clause ]
                    function_body

method_effect   ::= 'borrowing' | 'mutating' | 'consuming'   // self access; 'borrowing' is default
param           ::= [ label ] identifier ':' [ access_effect ] type [ '=' default_expr ]
label           ::= '_' | identifier
access_effect   ::= 'borrowing' | 'inout' | 'consuming' | 'initializing'   // 'borrowing' is default
return_type     ::= type
function_body   ::= '{' statement* '}'
```

`method_effect` is only meaningful for functions that have a receiver
(methods declared inside a `struct`, `enum`, or `extension`). Free functions
may not carry it. See §3.2.4.

#### 3.2.1 Parameters: labels & positions 📌

> **📌 Decision D3.** *Labels optional at call site; positional default.*
> The legacy `print(message: x)` style is supported via labels but no longer
> required.

```joyeer
func add(_ a: Int, _ b: Int): Int { a + b }
func transfer(from src: Account, to dst: Account, amount: Int) { ... }

add(1, 2)
transfer(from: alice, to: bob, amount: 100)
```

- A parameter has a *label* (used at call site) and an *internal name*
  (used in body). When omitted, the label is the internal name.
- `_` as the label means the argument is **positional only** at the call site.
- A parameter declared `func f(x: Int)` may be called either as `f(1)`
  (positional) **or** `f(x: 1)` (labeled). Mixing across multiple params
  must follow declaration order.

#### 3.2.2 Return type

A `Void` return may be written as `: ()` or omitted entirely. Single-expression
bodies may omit the `return` keyword:

```joyeer
func sq(_ x: Int): Int { x * x }
```

#### 3.2.3 Generic parameters & where clauses

```
generic_params  ::= '<' generic_param , ... '>'
generic_param   ::= identifier [ ':' constraint ]
where_clause    ::= 'where' constraint ( ',' constraint )*
constraint      ::= type ':' type            // T : Comparable
                 |  type '==' type           // associated equality
```

```joyeer
func minimum<T>(_ a: T, _ b: T): T where T: Comparable {
  if a < b { a } else { b }
}
```

#### 3.2.4 Method receiver effects 📌

> **📌 Decision D12.** *A method declares how it accesses `self` with a
> prefix `method_effect` keyword: `mutating` / `consuming` / `borrowing`
> (default).*  Borrowed from Swift's `mutating func`. The same access
> effects as parameters apply to the implicit `self` receiver, but the
> spelling differs: `mutating` (not `inout`) reads naturally before `func`,
> and matches the audience's Swift intuition.

| Receiver effect | Declaration | `self` in body | Call-site marker |
|-----------------|-------------|----------------|-------------------|
| `borrowing` (default) | `func peek(): UInt8` | read-only | none: `b.peek()` |
| `mutating` | `mutating func append(_ s: String)` | mutable, exclusive | `&`: `&b.append(s)` |
| `consuming` | `consuming func build(): String` | owned; consumed | `consume`: `consume b.build()` |

```joyeer
extension StringBuilder {
  func length(): Int { ... }                    // borrowing self (default)
  mutating func append(_ s: String) { ... }      // mutates self
  consuming func build(): String { ... }         // consumes self
}

var b = StringBuilder()
let n = b.length()           // borrowing: no marker
&b.append("hi")              // mutating: '&' marks the receiver (§4.3 / D1)
let s = consume b.build()    // consuming: 'consume' marks the receiver (§4.3 / D11)
// b is now uninitialized
```

The call-site markers are the **same** ones used for parameters (§4.3):
`mutating` reuses `&`, `consuming` reuses `consume`. The marker attaches to
the receiver expression: `&b.append(...)` means "`append` exclusively
borrows `b`"; `consume b.build()` means "`build` consumes `b`." `initializing`
is not available as a method receiver effect (it only applies to `init`,
which initializes `self` field-by-field, §3.7).

### 3.3 Struct declarations 📌

> **📌 Decision D4.** *Struct construction uses call syntax `Point(x:1, y:2)`,
> not record-literal `Point { x:1, y:2 }`.*  Chosen for consistency with
> function calls and Swift familiarity. The `{}` brace form is reserved (⏳).

```
struct_decl     ::= [ visibility ] 'struct' identifier [ generic_params ]
                    [ where_clause ]
                    '{' struct_member* '}'

struct_member   ::= variable_decl
                 |  constant_decl
                 |  init_decl
                 |  deinit_decl
                 |  func_decl
                 |  subscript_decl
                 |  invariant_clause           // 🔬
```

Structs are **value types** with predictable layout. No vtables, no
inheritance.

```joyeer
public struct Point {
  public var x: Double
  public var y: Double

  public init(x: Double, y: Double) {
    self.x = x; self.y = y
  }

  public func distance(to other: Point): Double {
    let dx = x - other.x
    let dy = y - other.y
    (dx * dx + dy * dy).sqrt()
  }
}

let p = Point(x: 1.0, y: 2.0)
```

If no `init` is declared, the compiler synthesizes a memberwise initializer
exposing all fields in declaration order, labeled by field name.

### 3.4 Enum declarations (ADT)

```
enum_decl       ::= [ visibility ] 'enum' identifier [ generic_params ]
                    [ where_clause ]
                    '{' enum_case+ enum_member* '}'

enum_case       ::= 'case' identifier [ '(' assoc_type , ... ')' ]   // form A
                 |  identifier [ '(' assoc_type , ... ')' ] ','      // form B (terse)

assoc_type      ::= [ label ':' ] type

enum_member     ::= func_decl | subscript_decl
```

Both forms allowed; mixing in one declaration is disallowed for clarity.

```joyeer
public enum JsonValue {
  Null,
  Bool(Bool),
  Number(Double),
  Str(String),
  Array([JsonValue]),
  Object([String: JsonValue]),
}

let v: JsonValue = .Number(3.14)
let arr: JsonValue = .Array([.Null, .Bool(true)])
```

Construction uses `Type.Case(args)` or `.Case(args)` when the target type
is inferable from context. Pattern-match with `match` (§5.9).

Enums are **tagged unions**. Size is `discriminant + max(variant)`,
stack-allocatable, no heap unless the user requests indirection (§3.4.1).

#### 3.4.1 Indirect cases (for recursive enums)

```joyeer
public enum LinkedList<T> {
  Empty,
  indirect Cons(T, LinkedList<T>),
}
```

`indirect` boxes the case payload on the heap; required for recursive
enums (otherwise the layout has infinite size). Heap allocation is **explicit
and visible** in the declaration — no hidden boxing.

### 3.5 Extension declarations

```
extension_decl  ::= 'extension' type [ where_clause ]
                    '{' extension_member* '}'

extension_member ::= func_decl | subscript_decl
```

Extensions add methods (functions tied to a type) and subscripts to an
existing type. They cannot add stored fields.

```joyeer
extension Array<T> {
  public func isEmpty(): Bool { count == 0 }
}
```

### 3.6 Subscript declarations

Subscripts are projection accessors — they are the **only way to expose
internal storage** for in-place mutation without leaking reference types.

```
subscript_decl  ::= 'subscript' [ identifier ] '(' [ param , ... ] ')' ':' type
                    '{' accessor+ '}'

accessor        ::= 'borrowing'   '{' yield_stmt '}'
                 |  'inout'        '{' yield_stmt '}'
                 |  'initializing' '{' yield_stmt '}'
                 |  'consuming'    '{' statement* 'return' expression '}'

yield_stmt      ::= 'yield' [ '&' ] expression
```

A subscript may declare any combination of `borrowing`, `inout`,
`initializing`, `consuming` accessors. Only the accessor matching the
call-site usage is invoked.

```joyeer
extension Array<T> {
  public subscript(_ i: Int): T {
    borrowing { yield  storage[i] }
    inout     { yield &storage[i] }
  }
}

var a = [1, 2, 3]
print(a[0])         // → let accessor
&a[0] += 10         // → inout accessor; a is exclusively borrowed during the += expression
```

Yielded storage has a **lexical lifetime** ending at the end of the
enclosing statement (§4.5.3).

### 3.7 init / deinit

```
init_decl       ::= [ visibility ] 'init' '(' [ param , ... ] ')'
                    [ contract_clause ]
                    function_body

deinit_decl     ::= 'deinit' '(' ')' function_body
```

- `init` constructs a value. The body must initialize every stored field
  exactly once before the body ends (data-flow checked).
- `deinit` runs when a value is destroyed: end of scope, overwritten, or
  consumed by a `consuming` parameter. Destruction order is deterministic (§4.7).

### 3.8 Import declarations

```
import_decl     ::= 'import' import_path
import_path     ::= identifier ( '.' identifier )*
```

```joyeer
import std.io
import std.collections
```

See §12 for module rules.

---

## §4 Memory Model  ★ CORE ★

This is the chapter that distinguishes Joyeer from "another Swift clone."
Read it carefully.

### 4.1 Value semantics is the only semantics

Every binding holds a **value**. Assignment is conceptually a copy:

```joyeer
var a = [1, 2, 3]
var b = a            // semantically: b is an independent copy of a
&b[0] = 99
print(a)              // [1, 2, 3]
print(b)              // [99, 2, 3]
```

The compiler is free to implement this as a move, a copy-on-write, or an
in-place reuse, **as long as the observable behavior is value semantics**.
Programmers do not see and do not control this — the compiler picks the
optimal lowering using last-use analysis (§4.6).

There are **no reference types** in Joyeer. No `&T`, no pointers, no
`Box`/`Rc`/`Arc`. The closest equivalents are *access effects* on
parameters (§4.2) and *projections* via subscripts (§4.5), neither of
which is a first-class value.

### 4.2 Access effects on parameters

A function parameter declares **how** the function will access the
argument's storage. The effect is part of the function signature; callers
must match it explicitly at the call site (§4.3).

| Effect | Meaning | Caller obligation | In-body usage |
|--------|---------|-------------------|----------------|
| `borrowing` (default) | Read-only projection. Multiple `borrowing` projections of the same value may coexist. | Pass without marker (may write `borrowing x` for emphasis). | Use as immutable value. |
| `inout` | **Exclusive** mutable projection. While held, the original storage is inaccessible to anyone else. | Mark with `&x` at call site. | Use and mutate like a local `var`. |
| `consuming` | **Consume** the argument. Caller's binding becomes uninitialized after the call. | Mark with `consume x` at call site; caller must own the value. | Owned outright; may be moved into return value, into another `consuming` parameter, or destroyed. |
| `initializing` | Write-only into uninitialized storage. | Mark with `&x` at call site, where `x` is uninitialized or has been consumed. | Must initialize before the body ends. |

#### 4.2.1 `borrowing` (default)

```joyeer
func max(_ a: Int, _ b: Int): Int { if a > b { a } else { b } }

var x = 10
var y = 20
let m = max(x, y)    // both x and y are borrow-projected; safe to coexist
print(x)              // ✅ x still accessible
```

The default may be written explicitly for emphasis:
`func max(_ a: borrowing Int, _ b: borrowing Int): Int`.

#### 4.2.2 `inout`

```joyeer
func increment(_ n: inout Int) { &n += 1 }

var k = 5
&increment(&k)
print(k)              // 6
```

While `increment` holds an `inout` projection of `k`, no other access to
`k` is permitted — checked at compile time by the law of exclusivity
(§4.4).

#### 4.2.3 `consuming`

```joyeer
func store(_ s: consuming String) { /* s is mine; printable, destroyable, returnable */ }

var greeting = "hello"
store(consume greeting)
// print(greeting)    // ❌ error: use of consumed value 'greeting'
```

After the call, `greeting` is in an **uninitialized state**. The compiler
rejects any subsequent read. A subsequent assignment (`greeting = "world"`)
re-initializes the storage and re-enables reads.

#### 4.2.4 `initializing`

```joyeer
func produceLargeBuffer(_ out: initializing [UInt8]) {
  &out = makeBuffer(size: 1_000_000)
}

var buf: [UInt8]                  // declared but uninitialized
produceLargeBuffer(&buf)          // 'initializing' writes without destructing prior contents
```

`initializing` is the emplace pattern: the callee promises to initialize the
storage; the caller promises the storage was uninitialized. This avoids a
destruct-then-construct round-trip for large objects.

### 4.3 Call-site markers `&` and `consume` 📌

> **📌 Decision D1.** *Call-site marker for `inout` / `initializing` is `&x`;
> for `consuming` it is `consume x`.*  Caller readability: any visible `&` or
> `consume` at a call site signals "this argument's storage will be
> exclusively borrowed, or given away, across this call."

```joyeer
swap(&a, &b)              // both args are inout
produceLargeBuffer(&buf)  // 'initializing' is also marked &
store(consume s)          // 'consuming' is marked with consume
plain(x, y)               // no marker → both are borrowing (read-only)
```

The `&` marker is **mandatory** on `inout` and `initializing` arguments,
and `consume` is **mandatory** on `consuming` arguments. Omitting either is
a syntax-level error, not just a type error — this guarantees that mutation
and ownership transfer are always visible at the call site by simple
scanning (§0.1 principle 2).

> **📌 Decision D11.** *`consume` at the call site is mandatory, not optional.*
> Swift makes its `consume` operator optional, relying on implicit last-use
> analysis to move otherwise. Joyeer requires it explicitly because the bulk
> of code is AI-generated: a mandatory, always-visible marker makes ownership
> transfer trivially greppable for both reviewers and tools, and removes the
> "did the compiler move or copy here?" ambiguity (§0.1 principles 2 and 4).
> Last-use *optimization* still applies to plain `borrowing` bindings (§4.6);
> `consume` is about **semantic** ownership transfer, not the optimization.

### 4.4 The Law of Exclusivity

> At any program point, for any storage location:
> **either** any number of `borrowing` projections coexist,
> **or** exactly one `inout` / `consuming` / `initializing` projection exists.
> The two regimes never overlap in time or in storage.

Checked statically by the compiler. Examples:

```joyeer
var x = 10
let a = x            // ✅ borrowing projection
let b = x            // ✅ another borrowing projection coexists
&increment(&x)       // ❌ error: cannot establish inout projection while
                     //    borrowing projections 'a' and 'b' are active
```

```joyeer
var x = 10
&increment(&x)       // ✅ inout for the duration of the call
let a = x            // ✅ inout ended; borrowing now allowed
```

```joyeer
func add(_ dst: inout Int, _ src: Int) { &dst += src }

var n = 5
&add(&n, n)          // ❌ error: 'n' has overlapping inout + borrowing projections
```

#### 4.4.1 Field-level disjointness

The compiler analyzes access paths. For struct fields, distinct paths are
treated as disjoint storage:

```joyeer
struct Point { var x: Int; var y: Int }

var p = Point(x: 1, y: 2)
&p.x += p.y          // ✅ &p.x is inout, p.y is let — disjoint paths
```

#### 4.4.2 Index-level conservatism

The compiler does **not** prove that two array indices are different:

```joyeer
var a = [1, 2, 3]
&a[0] += a[1]        // ❌ error: cannot prove a[0] and a[1] are disjoint
```

Workaround: introduce a `let` snapshot:

```joyeer
let tmp = a[1]
&a[0] += tmp         // ✅
```

Or use a standard-library subscript that takes both indices:

```joyeer
&a.bumpAt(0, by: a[1])      // hypothetical: API takes care of disjointness
```

### 4.5 Subscripts & projection

#### 4.5.1 Accessor blocks

A subscript may expose any combination of `borrowing`, `inout`,
`initializing`, `consuming` accessors (§3.6). Which accessor runs is decided
by the usage context:

| Call-site form | Accessor invoked |
|---------------|-------------------|
| `let v = a[i]` | `borrowing` |
| `&a[i] = expr` or `&a[i] += ...` or `&f(&a[i])` | `inout` |
| `&a[i] = expr` (uninitialized storage) | `initializing` (rare; usually via init helpers) |
| `consume a[i]` | `consuming` |

#### 4.5.2 `yield`

`yield` (and its `yield &` variant for inout) hands control back to the
caller with a projection of the named storage. When the caller's use-site
ends (end of the enclosing statement), control returns to the accessor
and the storage is "ungiven."

```joyeer
extension Buffer {
  public subscript(_ i: Int): UInt8 {
    borrowing {
      assert(i >= 0 && i < count)
      yield  data[i]
    }
    inout {
      assert(i >= 0 && i < count)
      yield &data[i]
    }
  }
}
```

#### 4.5.3 Lexical lifetime of projections

The lifetime of a yielded projection equals the smallest enclosing
**statement** (not expression). No flow analysis (no NLL-style refinement) —
this is a deliberate simplicity choice for AI tooling.

```joyeer
&a[0] += a[1]        // both projections established at start of this statement
                     //   → conflict detected here
```

#### 4.5.4 Disjoint paths

Paths are sequences of `.field` and `[index]` steps. Two paths are
*disjoint* when they differ at a `.field` step. They are *potentially
overlapping* when they only differ at `[index]` steps (the compiler does
not solve index equality in v0.1).

### 4.6 Last-use optimization (move elision)

When the compiler proves the source of a copy is **not used afterward**,
the copy is downgraded to a move (storage transfer). Example:

```joyeer
var src = makeBigArray()
var dst = src        // last use of src → move, not copy
print(dst.count)
```

If `print(src.count)` were added between the two lines, the compiler would
instead emit a true copy. Programmers never write `move(x)` — the compiler
infers it.

### 4.7 Deinitialization & destruction order

A value's `deinit` runs when:

1. The owning binding goes out of scope.
2. The binding is **overwritten** by a new value (the previous value's
   `deinit` fires *before* the new value is stored).
3. The value is **consumed** by a `consuming` parameter and the receiver
   finishes with it.

Order within a scope is **reverse declaration order**, for stored fields
of a struct as well. Order is fully deterministic; no finalizer queue,
no GC.

```joyeer
public struct FileHandle: Deinitializable {
  var fd: Int32
  public init(path: String) requires path.notEmpty() {
    fd = sys.open(path: path)
  }
  deinit() {
    sys.close(fd: fd)
  }
}

public func work() {
  var f = FileHandle(path: "a.txt")
  // ...
}  // f.deinit() runs exactly here
```

### 4.8 No reference types

There is no syntax in Joyeer to express "a value that aliases another
value" outside of subscript yields. The following do **not** exist:

- `&T` as a value type
- Pointers (`*T`)
- `Box<T>`, `Rc<T>`, `Arc<T>`
- Weak references

Cycles between values are therefore expressible only via `indirect` enum
cases (§3.4.1) and explicit collection indices. This keeps the memory
model trivially analyzable by both the compiler and AI tools.

### 4.9 Raw memory (FFI)

Out of scope for v0.1. A future `unsafe` block will expose raw pointers
for C interop. Until then, the standard library is the only producer of
heap-backed types (`Array`, `String`, `Dict`).

---

## §5 Expressions

### 5.1 Operator precedence

From highest (binds tightest) to lowest. Same-row operators have equal
precedence; associativity is shown.

| Level | Operators | Associativity |
|-------|-----------|---------------|
| 1 | `.`  `(...)`  `[...]`  postfix `?`  postfix `!` | left |
| 2 | prefix `!`  prefix `-`  prefix `~` | right |
| 3 | `*`  `/`  `%` | left |
| 4 | `+`  `-` | left |
| 5 | `<<`  `>>` | left |
| 6 | `&` (bitwise) | left |
| 7 | `^` | left |
| 8 | `\|` | left |
| 9 | `..<`  `...` | none |
| 10 | `<`  `<=`  `>`  `>=` | none |
| 11 | `==`  `!=` | none |
| 12 | `&&` | left |
| 13 | `\|\|` | left |
| 14 | assignment (`=`  `+=` …) | right |

(Postfix `?` and `!` are optional-chain / force-unwrap, see §5.4.)

### 5.2 Arithmetic, comparison, logical, bitwise

Standard semantics. Integer overflow on signed types is a **trap** in
debug builds and **wraps** in release builds — programmers may opt into
wrapping with `&+`, `&-`, `&*` (reserved syntax ⏳, deferred to v0.2).

Comparisons are non-chaining: `1 < x < 10` is a syntax error.

### 5.3 Assignment

```
assignment      ::= [ '&' ] lvalue assign_op expression
assign_op       ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='
                 |  '&=' | '|=' | '^=' | '<<=' | '>>='
```

The `&` prefix is required when the lvalue is a projection through an
`inout` or `initializing` subscript:

```joyeer
&a[0] = 10         // subscript inout
p.x = 10           // direct field on a var binding — no & needed
```

> **Heuristic.** Whenever the assignment touches storage that came from a
> `subscript`, write `&`. Whenever you assign directly to a `var` or a
> direct field of a `var`, no `&`. The compiler will tell you the right
> form in any case.

### 5.4 Member access & methods

```
member_expr     ::= postfix_expr '.' identifier
optional_chain  ::= postfix_expr '?.' identifier
```

Methods declared in a `struct`, `enum`, or `extension` are called via
member syntax: `a.size()`, `point.distance(to: other)`.

Optional chaining short-circuits to `nil`:

```joyeer
let lengths = optionalString?.count   // type: Int?
```

### 5.5 Subscript expressions

```
subscript_expr  ::= postfix_expr '[' expression , ... ']'
```

Resolves to the matching `subscript` declaration. The accessor invoked
depends on the surrounding context (§4.5.1).

### 5.6 Function calls 📌

```
call_expr       ::= callee '(' [ call_arg , ... ] ')'
call_arg        ::= [ label ':' ] [ '&' | 'consume' ] expression
```

> **📌 Decision D3 (call site).**  Both `f(1, 2)` and `f(a: 1, b: 2)` are
> legal for `func f(a: Int, b: Int)`. A parameter declared with `_` as
> label is positional only; a parameter declared with an external label
> may be called either way, but mixed calls must preserve order.

### 5.7 Struct construction

```joyeer
let p = Point(x: 1.0, y: 2.0)         // explicit init or synthesized
let q = Point(x: 0.0, y: p.y)
```

### 5.8 Enum construction

```joyeer
let v: JsonValue = .Number(3.14)       // contextual: target type known
let w = JsonValue.Bool(true)            // fully qualified
```

### 5.9 Match expression

```
match_expr      ::= 'match' expression '{' match_arm+ '}'
match_arm       ::= pattern [ 'where' expression ] '=>' (expression | block) ','?
```

`match` is an **expression**; every arm must produce a value of the same
type (or all be `Void`):

```joyeer
let label = match v {
  .Null         => "null",
  .Bool(true)   => "true",
  .Bool(false)  => "false",
  .Number(n) where n < 0 => "negative",
  .Number(_)    => "non-negative",
  _             => "other",
}
```

Exhaustiveness is checked (§7.7).

### 5.10 if as expression 📌

> **📌 Decision D9.** *`if`/`match` are expressions; `while`/`for` are
> statements.*  Pure functional `if` improves AI-generated code clarity
> (no scattered `return` paths).

```joyeer
let sign = if x > 0 { 1 } else if x < 0 { -1 } else { 0 }
```

Every branch must produce the same type. The whole `if`-`else` must be
total (i.e., have an `else` arm) when used as an expression.

### 5.11 Closures 📌

> **📌 Decision D8.** *Closures deferred to v0.2.*  v0.1 has only top-level
> and method functions. Iteration uses `for-in`; higher-order patterns
> use free functions. This keeps the memory model simpler (no closure
> capture rules) and matches the v0.1 use cases (JSON parser, quicksort).

---

## §6 Statements

```
statement       ::= binding ';'?
                 |  expression ';'?
                 |  if_stmt | while_stmt | for_stmt
                 |  return_stmt | break_stmt | continue_stmt
                 |  block

block           ::= '{' statement* '}'
```

Semicolons are optional between statements (line breaks separate them);
required only when multiple statements share a line.

### 6.1 if statement / expression

See §5.10. Same syntax in both positions.

### 6.2 while

```
while_stmt      ::= 'while' expression block
```

### 6.3 for-in

```
for_stmt        ::= 'for' [ '&' ] pattern 'in' expression block
```

The optional `&` requests an `inout` iteration over the source collection
(if the collection's `Iterable` conformance provides an inout view).

```joyeer
for x in arr      { print(x) }
for &x in arr     { &x *= 2 }
```

### 6.4 return / break / continue

Standard. `return` may be omitted in single-expression function bodies
(§3.2.2). `break label` / `continue label` reserved ⏳ for v0.2.

---

## §7 Patterns

Used in `let`/`var`, `match` arms, and `for-in`.

```
pattern         ::= '_'                                       // wildcard
                 |  identifier                                 // bind
                 |  literal                                    // literal compare
                 |  '(' pattern , ... ')'                       // tuple
                 |  type '.' identifier [ '(' pattern , ... ')' ]   // enum case
                 |  '.' identifier [ '(' pattern , ... ')' ]        // contextual enum case
                 |  identifier 'as' type                       // type test + bind
```

### 7.1 Identifier

Binds a fresh name to the matched value:

```joyeer
match v {
  .Number(n) => print(n),    // n bound
  _ => (),
}
```

### 7.6 Where guards

```joyeer
match v {
  .Number(n) where n > 0 => "positive",
  .Number(_)              => "non-positive",
  _                       => "other",
}
```

### 7.7 Exhaustiveness

`match` over an enum or `Bool` must cover all cases or include a `_`
arm. Missing cases are a compile error with a list of the missing
constructors.

---

## §8 Error Handling

### 8.1 No exceptions

Joyeer has **no `throw` / `try` / `catch`**, no `errno`, no nullable-by-
default. Errors are values.

### 8.2 Result<T, E>

```joyeer
enum Result<T, E> { Ok(T), Err(E) }
```

A function that can fail returns `Result<T, ErrorEnum>`:

```joyeer
enum ParseError { Empty, Invalid(String) }

func parseInt(_ s: String): Result<Int, ParseError> {
  if s.isEmpty() { return .Err(.Empty) }
  // ...
  return .Ok(n)
}
```

### 8.3 `?` propagation 📌

> **📌 Decision D6.** *Postfix `?` on `Result<T,E>` (and `Optional<T>`)
> propagates the failure.*  Equivalent to `match x { .Ok(v) => v,
> .Err(e) => return .Err(e) }`.

```joyeer
func parsePair(s: String): Result<(Int, Int), ParseError> {
  let parts = s.split(",")
  let a = parseInt(parts[0])?
  let b = parseInt(parts[1])?
  .Ok((a, b))
}
```

The `?` operator is only valid in functions whose return type is
`Result<_, E>` or `Optional<_>` and whose `E` is compatible with the
propagated error.

### 8.4 No `try` / `catch`

Reserved keywords ⏳. If error-handling syntax for `Result` chains becomes
ergonomically heavy, a `try-block` may be added in v0.3. For v0.1, `?`
plus `match` covers all cases.

---

## §9 Contracts 🔬 (syntax-only in v0.1)

Contracts attach **machine-checkable** specifications to declarations.
In v0.1 they parse and type-check but the compiler does not yet prove or
runtime-check them. This section locks the syntax so the future
implementation has a stable target.

### 9.1 Placement

A contract clause appears between the function signature and body:

```
contract_clause ::= ( requires_clause | ensures_clause )+
requires_clause ::= 'requires' expression
ensures_clause  ::= 'ensures'  expression
```

```joyeer
func divide(_ a: Int, _ b: Int): Int
  requires b != 0
  ensures  result * b + (a % b) == a
{
  return a / b
}
```

### 9.2 `requires` (preconditions)

Any side-effect-free Bool expression in scope at function entry. Multiple
`requires` clauses are conjoined.

### 9.3 `ensures` (postconditions)

A Bool expression evaluated at function exit. The pseudo-binding `result`
refers to the returned value. `old(x)` refers to the value of `x` at
function entry.

```joyeer
func bumpAndGet(_ x: inout Int): Int
  ensures result == old(x) + 1
  ensures x == old(x) + 1
{
  &x += 1
  return x
}
```

### 9.4 `invariant` (struct & loop)

Struct invariant:

```joyeer
struct SortedRun {
  var data: [Int]
  invariant forall i in 0..<data.count - 1: data[i] <= data[i+1]
}
```

Loop invariant:

```joyeer
while i < n
  invariant 0 <= i && i <= n
  invariant forall k in 0..i: array[k] <= pivot
{
  // ...
}
```

### 9.5 Quantifiers & ranges

```
forall identifier 'in' range_or_collection ':' bool_expression
exists identifier 'in' range_or_collection ':' bool_expression
```

Side-effect-free; bounded iteration. (In v0.1 they only need to *parse*;
v0.3 will add finite expansion or SMT translation.)

### 9.6 v0.1 compiler treatment

- Parsed into the AST.
- Type-checked: the expressions must be Bool, side-effect-free, and
  reference only in-scope names.
- **Not** evaluated, **not** proven, **not** runtime-checked.

### 9.7 Future runtime mode

A compiler flag (`--check-contracts`) will lower each `requires` to an
assertion at function entry and each `ensures` to an assertion at exit.
Targeted for v0.3.

---

## §10 Effects 🔬 (syntax-only in v0.1)

### 10.1 Pure by default

Any function without a `performs` clause is **pure**: it may only read
its `borrowing` parameters, write through its `inout`/`initializing`
parameters, allocate via `consuming` returns (covered by the `Alloc` effect,
see below),
and call other pure functions.

This is the AI-era guarantee: functions without `performs` annotations
have no observable side effects beyond data flow.

### 10.2 `performs` clause

```
effect_clause   ::= 'performs' effect_label ( ',' effect_label )*
effect_label    ::= identifier [ '<' type , ... '>' ]
```

```joyeer
func readFile(path: String): String
  performs IO, Throw<FileError>
{
  // ...
}
```

### 10.3 Built-in effect labels (v0.1 vocabulary)

| Label | Meaning |
|-------|---------|
| `IO` | Filesystem, network, terminal access. |
| `Throw<E>` | May produce a `.Err(E)` even from a `Result`-typed return. (Annotation for analysis; semantics in §8.) |
| `Async` | Suspends and resumes. Reserved ⏳; used once concurrency lands. |
| `Alloc` | Performs heap allocation. Pure functions may still allocate transient buffers used to produce their return value. |
| `Random` | Non-deterministic. |
| `Time`  | Reads wall-clock or monotonic time. |

### 10.4 Effect polymorphism

Deferred. v0.1 effects are concrete sets only — no `performs <E>` generic.

### 10.5 v0.1 compiler treatment

- Parsed and stored on the function symbol.
- Propagated: if `f` calls `g`, `f`'s effects must be a superset of `g`'s.
  Violation is a **warning** in v0.1, a **hard error** in v0.2.
- Not used for codegen.

---

## §11 Property-Test & Spec Annotations 🔬

```
attribute       ::= '@' identifier [ '(' attribute_args ')' ]
attribute_args  ::= string_literal | expression , ...
```

### 11.1 `@spec`

Free-form natural-language intent. Available to tooling (AI assistants,
documentation generators).

```joyeer
@spec "Returns the nth Fibonacci number, where fib(0) = 0 and fib(1) = 1."
func fib(_ n: Int): Int { ... }
```

### 11.2 `@property`

Executable Bool expression that the value of `n` (or the function under
test) should satisfy. Test runners may sample inputs.

```joyeer
@property fib(0) == 0
@property fib(1) == 1
@property forall n in 2..20: fib(n) == fib(n-1) + fib(n-2)
func fib(_ n: Int): Int { ... }
```

### 11.3 v0.1 treatment

- Parsed and stored.
- No test runner shipped in v0.1. Runner targeted for v0.3.

---

## §12 Modules & Imports

### 12.1 Module unit

A **module** is a directory of `.joyeer` source files compiled together.
v0.1 supports single-file modules only; multi-file modules will be
supported once the build system lands (v0.2).

### 12.2 Visibility 📌

> **📌 Decision D10.** *Three visibility levels: `public`, `internal`
> (default), `private` (file-local).*

| Visibility | Reachable from |
|-----------|---------------|
| `public` | Any module that imports this module. |
| `internal` (default) | Same module. |
| `private` | Same file only. |

### 12.3 Imports

```joyeer
import std.io
import std.collections.Dict as Map
```

`as` for renaming is supported. `import` may only appear at the top of a
file, before any declaration.

### 12.4 Prelude

The standard library implicitly imports the following into every file:

- `Bool`, `Int`, `UInt`, `Float`, `Double`, `Char`, `String`, `Void`
- `Optional`, `Result`
- `Array`, `Dict`, `Tuple`
- `Display`, `Equatable`, `Comparable` (the *de facto* protocol vocabulary;
  protocols are reserved ⏳, but these names are reserved already).
- `print`, `assert`, `panic`

---

## §13 Naming Conventions (style guide)

These are not syntactic rules but the compiler issues stylistic warnings
when they are violated. AI code generators must follow them.

| Construct | Convention | Example |
|-----------|-----------|---------|
| Types (struct, enum, typealias) | PascalCase | `JsonValue`, `LinkedList` |
| Functions, methods | camelCase | `parseInt`, `appendByte` |
| Variables, fields, parameters | camelCase | `firstName`, `count` |
| Constants (let bindings) | camelCase | `let maxRetries = 5` |
| Enum cases | PascalCase | `.Ok`, `.NotFound` |
| Generic type parameters | single capital or PascalCase | `T`, `K`, `Element` |
| Effect labels | PascalCase | `IO`, `Throw<E>` |
| Modules / files | lowercase | `std.io`, `json_parser.joyeer` |

---

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

## §16 Worked Examples

### 16.1 Quicksort (canonical demo)

```joyeer
import std.io

func medianOfThree(a: Int, b: Int, c: Int): Int
  ensures result == a || result == b || result == c
{
  if (a <= b && b <= c) || (c <= b && b <= a) { return b }
  if (b <= a && a <= c) || (c <= a && a <= b) { return a }
  return c
}

func swap<T>(_ a: inout T, _ b: inout T) {
  let tmp = a
  a = b
  b = tmp
}

func partition(_ array: inout [Int], lo: Int, hi: Int): Int
  requires 0 <= lo && lo <= hi && hi < array.count
  ensures lo <= result && result <= hi
{
  let pivot = medianOfThree(
    a: array[lo],
    b: array[(lo + hi) / 2],
    c: array[hi],
  )
  var i = lo
  var j = hi
  while i <= j {
    while array[i] < pivot { i += 1 }
    while array[j] > pivot { j -= 1 }
    if i <= j {
      swap(&array[i], &array[j])
      i += 1
      j -= 1
    }
  }
  return j
}

func quickSortRange(_ array: inout [Int], lo: Int, hi: Int)
  requires hi < array.count
{
  if lo < hi {
    let p = partition(&array, lo: lo, hi: hi)
    quickSortRange(&array, lo: lo,    hi: p)
    quickSortRange(&array, lo: p + 1, hi: hi)
  }
}

public func quickSort(_ input: consuming [Int]): [Int]
  ensures result.count == input.count
{
  var arr = input
  if arr.count > 1 {
    quickSortRange(&arr, lo: 0, hi: arr.count - 1)
  }
  return arr
}

public func main() {
  let unsorted = [8, 6, 1, 2, 1, 12, 3, 4, 34]
  let sorted = quickSort(consume unsorted)
  for x in sorted { print(x) }
}
```

### 16.2 JSON parser (sketch matching v0.1-plan.md)

```joyeer
public enum JsonValue {
  Null,
  Bool(Bool),
  Number(Double),
  Str(String),
  Array([JsonValue]),
  Object([String: JsonValue]),
}

public enum JsonError {
  UnexpectedEof,
  Unexpected(Char, at: Int),
  InvalidNumber(String),
}

struct Parser {
  var input: String
  var pos: Int
}

func peek(_ p: Parser): Char? {
  if p.pos >= p.input.count { return nil }
  return p.input[p.pos]
}

func advance(_ p: inout Parser): Char?
  ensures p.pos == old(p.pos) + 1 || (result == nil && p.pos == old(p.pos))
{
  if p.pos >= p.input.count { return nil }
  let c = p.input[p.pos]
  p.pos += 1
  return c
}

func parseValue(_ p: inout Parser): Result<JsonValue, JsonError> {
  skipWhitespace(&p)
  let c = peek(p) ?? return .Err(.UnexpectedEof)
  return match c {
    '"' => parseString(&p),
    '{' => parseObject(&p),
    '[' => parseArray(&p),
    't', 'f' => parseBool(&p),
    'n' => parseNull(&p),
    _   => parseNumber(&p),
  }
}

public func parse(_ source: String): Result<JsonValue, JsonError> {
  var p = Parser(input: source, pos: 0)
  return parseValue(&p)
}
```

### 16.3 Linked list (indirect enum + deinit demo)

```joyeer
public enum List<T> {
  Empty,
  indirect Cons(T, List<T>),
}

extension List<T> {
  public func count(): Int {
    match self {
      .Empty       => 0,
      .Cons(_, t)  => 1 + t.count(),
    }
  }

  public func prepended(_ x: consuming T): List<T> {
    .Cons(x, self)
  }
}
```

### 16.4 String builder (subscript + mutating/consuming demo)

```joyeer
public struct StringBuilder {
  var buffer: [UInt8]

  public init() { buffer = [] }

  public mutating func append(_ s: String) {
    for b in s.utf8() { &buffer.append(b) }
  }

  public subscript(_ i: Int): UInt8 {
    borrowing { yield  buffer[i] }
    inout     { yield &buffer[i] }
  }

  public consuming func build(): String {
    String(utf8: buffer)
  }
}

public func main() {
  var sb = StringBuilder()
  &sb.append("hello, ")
  &sb.append("world")
  let s = consume sb.build()    // build() is consuming; consume marks the receiver
  print(s)
}
```

---

## §17 Grammar Appendix (EBNF)

Compact reference. Whitespace and comments are skipped between tokens.

```
file              ::= import_decl* top_decl*

top_decl          ::= func_decl
                   |  struct_decl
                   |  enum_decl
                   |  extension_decl
                   |  binding
                   |  typealias_decl

binding           ::= ( 'let' | 'var' ) pattern [ ':' type ] [ '=' expression ]

func_decl         ::= [ visibility ] [ method_effect ] 'func' identifier [ generic_params ]
                      '(' [ param , ... ] ')'
                      [ ':' type ]
                      [ where_clause ]
                      [ effect_clause ]
                      contract_clause*
                      block

method_effect     ::= 'borrowing' | 'mutating' | 'consuming'
param             ::= [ label ] identifier ':' [ access_effect ] type [ '=' expression ]
label             ::= '_' | identifier
access_effect     ::= 'borrowing' | 'inout' | 'consuming' | 'initializing'

generic_params    ::= '<' generic_param , ... '>'
generic_param     ::= identifier [ ':' type ]
where_clause      ::= 'where' constraint , ...
constraint        ::= type ':' type | type '==' type

effect_clause     ::= 'performs' effect_label , ...
effect_label      ::= identifier [ '<' type , ... '>' ]

contract_clause   ::= 'requires' expression
                   |  'ensures'  expression
                   |  'invariant' expression

struct_decl       ::= [ visibility ] 'struct' identifier [ generic_params ]
                      [ where_clause ]
                      '{' struct_member* '}'

struct_member     ::= binding
                   |  init_decl | deinit_decl
                   |  func_decl
                   |  subscript_decl
                   |  'invariant' expression

init_decl         ::= [ visibility ] 'init' '(' [ param , ... ] ')'
                      contract_clause*
                      block

deinit_decl       ::= 'deinit' '(' ')' block

enum_decl         ::= [ visibility ] 'enum' identifier [ generic_params ]
                      [ where_clause ]
                      '{' enum_case+ ( func_decl | subscript_decl )* '}'

enum_case         ::= [ 'indirect' ] identifier [ '(' assoc_type , ... ')' ]
assoc_type        ::= [ identifier ':' ] type

extension_decl    ::= 'extension' type [ where_clause ]
                      '{' ( func_decl | subscript_decl )* '}'

subscript_decl    ::= [ visibility ] 'subscript' [ identifier ]
                      '(' [ param , ... ] ')' ':' type
                      '{' accessor+ '}'

accessor          ::= 'borrowing'   block_with_yield
                   |  'inout'        block_with_yield
                   |  'initializing' block_with_yield
                   |  'consuming'    block

block_with_yield  ::= '{' statement* 'yield' [ '&' ] expression statement* '}'

typealias_decl    ::= 'typealias' identifier [ generic_params ] '=' type
import_decl       ::= 'import' import_path [ 'as' identifier ]
import_path       ::= identifier ( '.' identifier )*
visibility        ::= 'public' | 'internal' | 'private'

statement         ::= binding ';'?
                   |  expression ';'?
                   |  if_stmt | while_stmt | for_stmt
                   |  return_stmt | break_stmt | continue_stmt
                   |  block

if_stmt           ::= 'if' expression block ( 'else' if_stmt | 'else' block )?
while_stmt        ::= 'while' expression ( 'invariant' expression )* block
for_stmt          ::= 'for' [ '&' ] pattern 'in' expression block
return_stmt       ::= 'return' [ expression ]
break_stmt        ::= 'break'
continue_stmt     ::= 'continue'

expression        ::= prefix_expr ( binary_op prefix_expr )*
prefix_expr       ::= [ '!' | '-' | '~' ] postfix_expr
                   |  ( '&' | 'consume' ) postfix_expr   // ownership markers (§4.3);
                                                         //   prefix an lvalue / owned path only
postfix_expr      ::= primary_expr ( '.' identifier
                                    | '?.' identifier
                                    | '(' [ call_arg , ... ] ')'
                                    | '[' expression , ... ']'
                                    | '?'
                                    | '!' )*
call_arg          ::= [ label ':' ] [ '&' | 'consume' ] expression

primary_expr      ::= literal
                   |  identifier
                   |  '(' expression ')'                       // parens or tuple
                   |  array_literal
                   |  dict_literal
                   |  if_stmt                                   // if as expr
                   |  match_expr
                   |  'self' | 'Self'
                   |  type '.' identifier [ '(' ... ')' ]        // qualified ctor

match_expr        ::= 'match' expression '{' match_arm+ '}'
match_arm         ::= pattern [ 'where' expression ] '=>' ( expression | block ) ','?

pattern           ::= '_'
                   |  literal
                   |  identifier
                   |  '(' pattern , ... ')'
                   |  [ type ] '.' identifier [ '(' pattern , ... ')' ]
                   |  identifier 'as' type

binary_op         ::= '+' | '-' | '*' | '/' | '%'
                   |  '==' | '!=' | '<' | '<=' | '>' | '>='
                   |  '&&' | '||'
                   |  '&' | '|' | '^' | '<<' | '>>'
                   |  '..<' | '...'
                   |  assign_op
assign_op         ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='
                   |  '&=' | '|=' | '^=' | '<<=' | '>>='

attribute         ::= '@' identifier [ '(' attribute_args ')' ]
attribute_args    ::= string_literal | expression , ...
```

---

## Appendix: change log

- **2026-05-30** — Initial draft (this document). Locks A + B language
  features, includes C contracts/effects/property as syntax-only.
