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
| Control flow | `if`, `else`, `while`, `for`, `in`, `match`, `return`, `yield` |
| Declarations | `func`, `struct`, `enum`, `extension`, `subscript`, `init`, `deinit`, `typealias`, `import`, `as`, `indirect` |
| Access conventions | `inout`, `borrowing`, `consuming`, `initializing`, `consume`, `mutating` |
| Visibility | `public`, `internal`, `private` |
| Types & literals | `true`, `false`, `nil`, `self`, `Self` |
| Match guard | `where` |
| Reserved ⏳ | `async`, `await`, `actor`, `throws`, `try`, `catch`, `defer`, `break`, `continue`, `is`, `class`, `protocol`, `trait`, `macro`, `invariant`, `result`, `unsafe`, `package`, `Any` |

`match` does not require a separate "Pattern" lexer feature. The lexer emits
ordinary identifier/literal/punctuation tokens plus `match`, `where`,
`indirect`, and `=>`; the parser builds pattern nodes and the type checker
performs binding and exhaustiveness checks (§7).

> `class` is in the **reserved ⏳** list because it is removed in v0.2; unlike
> the other reserved words it is **not** rejected — the parser still accepts
> it through v0.1 with a deprecation warning (§14). The remaining reserved
> words are rejected with a clear error message in v0.1 (§15).

> `as` is a contextual keyword: in v0.1 it is used **only** for `import`
> aliasing (§3.8). Its type-cast meaning (`x as T`) and the `is` type-test
> operator are deferred to v0.2 (§15).

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
string_item      ::= escape | <any unicode except '"' or '\'>
escape           ::= '\\' ( 'n' | 't' | 'r' | '"' | '\\' | '0' | 'u{' hex+ '}' )
// interpolation ::= '\(' expression ')'   // reserved ⏳ — future version (§1.7)

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
| Coalescing | `??` (nil / `.Err` coalescing; right operand may be a diverging expression, §8.5) |
| Bitwise | `&` `\|` `^` `~` `<<` `>>` |
| Assignment | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` |
| Range | `..<` `...` |
| Postfix | `?` (optional chain / Result-propagation) `!` (force-unwrap) |
| Member / call | `.` `(` `)` `[` `]` `{` `}` `,` `;` `:` `=>` `_` |
| Memory marker | `&` (call-site inout marker) |

`&` is both bitwise-AND (binary infix) and the inout call-site marker
(prefix on argument). Disambiguated by syntactic position.

### 1.7 String interpolation ⏳

> **📌 Decision.** *String interpolation is reserved for a future version,
> not part of v0.1.*  Swift-style `"\(expr)"` is convenient sugar, but it is
> not required for the v0.1 use cases; debug and test output use `print` with
> separate arguments or manual concatenation. Deferring it keeps the v0.1
> lexer and type checker simpler and can be added later without breaking
> existing string literals.

When added, the syntax will be Swift-style `"\(expr)"`, where the expression
inside `\(...)` is any `expression` whose result is one of the built-in
printable types (the integer and floating types, `Bool`, `Char`, `UInt8`,
`String`):


```joyeer
// future syntax (⏳, not v0.1):
let n = 42
print(value: "answer is \(n), squared is \(n * n)")
```

A user-extensible `Display` protocol is also reserved for a future version
(§15).

---

