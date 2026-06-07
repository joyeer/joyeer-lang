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

#### 3.2.1 Parameters: labels 📌

> **📌 Decision D3.** *Argument labels are mandatory at every call site;
> there is no positional / unlabeled form.*  Every parameter has a label and
> the caller must always write it.

```joyeer
func add(a: Int, b: Int): Int { a + b }
func transfer(from src: Account, to dst: Account, amount: Int) { ... }

add(a: 1, b: 2)
transfer(from: alice, to: bob, amount: 100)
```

- A parameter has a *label* (used at call site) and an *internal name*
  (used in body). When only one name is written it serves as both: the label
  and the internal name are identical.
- A distinct external label may precede the internal name, as in `from src`:
  the caller writes `from:`, the body uses `src`.
- There is **no** `_` wildcard label and **no** positional call form. A
  parameter declared `func f(x: Int)` must be called as `f(x: 1)`.

#### 3.2.2 Return type

A `Void` return may be written as `: ()` or omitted entirely. Single-expression
bodies may omit the `return` keyword:

```joyeer
func sq(x: Int): Int { x * x }
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
func minimum<T>(a: T, b: T): T where T: Comparable {
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
| `mutating` | `mutating func append(s: String)` | mutable, exclusive | `&`: `&b.append(s: s)` |
| `consuming` | `consuming func build(): String` | owned; consumed | `consume`: `consume b.build()` |

```joyeer
extension StringBuilder {
  func length(): Int { ... }                    // borrowing self (default)
  mutating func append(s: String) { ... }      // mutates self
  consuming func build(): String { ... }         // consumes self
}

var b = StringBuilder()
let n = b.length()           // borrowing: no marker
&b.append(s: "hi")              // mutating: '&' marks the receiver (§4.3 / D1)
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
  public subscript(i: Int): T {
    borrowing { yield  storage[i] }
    inout     { yield &storage[i] }
  }
}

var a = [1, 2, 3]
print(value: a[0])         // → let accessor
&a[0] += 10         // → inout accessor; a is exclusively borrowed during the += expression
```

Yielded storage has a **lexical lifetime** ending at the end of the
enclosing statement (§4.5.3).

### 3.7 init / deinit

```
init_decl       ::= [ visibility ] 'init' '(' [ param , ... ] ')'
                    function_body

deinit_decl     ::= 'deinit' '(' ')' function_body
```

- `init` constructs a value. The body must initialize every stored field
  exactly once before the body ends (data-flow checked).
- `deinit` runs when a value is destroyed: end of scope, overwritten, or
  consumed by a `consuming` parameter. Destruction order is deterministic (§4.7).
- Preconditions inside `init` are expressed with `precondition(...)` (§9),
  not declaration-level clauses.

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

