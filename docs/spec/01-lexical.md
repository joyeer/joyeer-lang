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
| Effects 🔬 | `performs`, `pure` |
| Pattern | `where`, `as`, `is`, `_` |
| Reserved ⏳ | `async`, `await`, `actor`, `throws`, `try`, `catch`, `defer`, `class`, `protocol`, `trait`, `macro`, `requires`, `ensures`, `invariant`, `old`, `forall`, `exists`, `result` |

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

char_literal     ::= "'" ( escape | <any unicode except "'" or '\'> ) "'"   // Char (32-bit scalar)
byte_literal     ::= "b'" ( escape | <any ASCII except "'" or '\'> ) "'"    // UInt8 (single byte)

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

A `char_literal` such as `'a'` has type `Char` (a 32-bit Unicode scalar). A
`byte_literal` such as `b'a'` has type `UInt8` and may only contain a single
ASCII character; it is the natural element of a UTF-8 `String` or a `[UInt8]`
buffer (§2.1.1). For example `b'"'` is `0x22` and `b'\n'` is `0x0A`.

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

