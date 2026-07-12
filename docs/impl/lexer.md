# Joyeer Lexer MVP — C++ Implementation Design

> **Status:** implementation plan for the first JSON-parser milestone.
> The normative lexical grammar remains in [../spec/01-lexical.md](../spec/01-lexical.md).
> This document deliberately defines a smaller implementation profile: only
> the Joyeer syntax needed to write a JSON parser is required initially.

---

## 1. Goal

Implement a small, deterministic lexer in C++ that can tokenize a Joyeer
program containing the first JSON parser.

The data flow is:

```text
Joyeer source text
    -> C++ Joyeer lexer
    -> token stream
    -> Joyeer parser / type checker / VM
    -> running Joyeer JSON parser
    -> JSON input text
```

There are two different scanning jobs here:

1. The **C++ Joyeer lexer** tokenizes Joyeer source code. This document
   specifies that lexer.
2. The future **JSON parser written in Joyeer** reads JSON text at runtime.
   It will use `String` byte indexing and does not require the Joyeer compiler
   lexer to understand JSON syntax.

Self-hosting is explicitly **not** a goal for this milestone.

### 1.1 Success criterion

The lexer is sufficient when the C++ compiler can tokenize the Joyeer source
of a JSON parser using:

- bindings and functions;
- `struct` parser state;
- payload-carrying `enum` values;
- `if`, `while`, and `match`;
- named calls and return values;
- arrays and dictionaries;
- optional types plus `Optional` / `Result` matching;
- byte literals for JSON structural characters.

### 1.2 Design principles

1. **Small language surface first.** Do not implement syntax merely because it
   may be convenient later.
2. **No lexer-level pattern subsystem.** The lexer recognizes terminals;
   `PatternNode` construction and exhaustiveness are parser/type-checker work.
3. **Longest valid token wins.** Multi-character terminals are recognized
   before their prefixes.
4. **No silent loss.** Every non-trivia input byte either belongs to a token or
   produces a diagnostic.
5. **Spans, not copied strings.** A token points into the immutable source
   buffer by byte offset and length.
6. **Deterministic semantics.** Build mode and platform do not change tokenization.
7. **Linear time.** Tokenization is $O(n)$ in source bytes with no backtracking.

---

## 2. Explicit non-goals

The first lexer does **not** need the following to reach the JSON-parser
milestone:

- self-hosting or writing the lexer in Joyeer;
- a general `performs` / `pure` effect system;
- user-defined generics or generic constraints;
- protocols, classes, inheritance, or dynamic dispatch syntax;
- closures and capture syntax;
- macros or conditional compilation;
- string interpolation;
- Unicode identifiers;
- character literals (`'x'`); JSON scanning uses byte literals (`b'x'`);
- hexadecimal, binary, or octal integer literals;
- floating-point literals in Joyeer source;
- numeric separators;
- range operators and range patterns;
- `for-in` loops; the JSON parser can use `while`;
- `where` match guards;
- alternative patterns in one arm;
- tuple destructuring patterns;
- compound assignment (`+=`, `-=`, and similar);
- division, remainder, bitwise, and shift operators;
- logical OR and standalone logical NOT (`||`, `!`); `!=` remains supported;
- semicolons; the MVP writes one statement per line;
- optional chaining (`?.`);
- nil/error coalescing (`??`);
- annotations such as `@spec` and `@property`;
- special doc-comment tokens.

These are deferred—not approximated. An unsupported construct should receive a
clear diagnostic instead of being tokenized into a misleading sequence.

### 2.1 Why floating literals are not initially required

JSON numbers are characters in the runtime JSON input. The Joyeer JSON parser
can scan those bytes itself. The compiler lexer needs a floating literal only
when the *Joyeer source code* contains one. The first milestone may represent
`JsonValue.Number` with `Int`, as already allowed by the v0.1 plan.

---

## 3. Minimum lexical surface

### 3.1 Keywords

Only these words need dedicated keyword tokens for the first milestone:

| Group | Keywords | Why required |
|---|---|---|
| Declarations | `func`, `struct`, `enum` | Functions, parser state, JSON values/errors |
| Bindings | `let`, `var` | Immutable and mutable locals/fields |
| Branching | `if`, `else`, `match` | Control flow and enum/byte dispatch |
| Loops | `while` | Byte and collection iteration |
| Function exit | `return` | Normal and early returns |
| Mutation convention | `inout` | Mutating parser state without reference types |

`true`, `false`, and `nil` are literal tokens rather than declaration/control
keywords.

The following words are intentionally deferred even though the future
specification mentions them:

- `public`, `internal`, `private`;
- `for`, `in`;
- `extension`, `subscript`, `init`, `deinit`;
- `borrowing`, `consuming`, `initializing`, `consume`, `mutating`;
- `yield`, `indirect`, `where`;
- `import` and `as`;
- all reserved future words.

The JSON parser can use synthesized memberwise initialization, free functions,
built-in containers, and the default borrowing convention. Deferred/reserved
words are recognized through one `DeferredKeyword` token kind and diagnosed as
unsupported in this profile; they are never accepted as identifiers. This
keeps later activation source-compatible without adding parser productions now.
Permanently removed words such as `performs` and `pure` remain ordinary
identifiers.

### 3.2 Identifiers

The MVP uses ASCII identifiers:

```ebnf
identifier_head ::= "A".."Z" | "a".."z" | "_"
identifier_tail ::= identifier_head | "0".."9"
identifier      ::= identifier_head identifier_tail*
```

Rules:

- identifiers are case-sensitive;
- a keyword is recognized only after scanning the complete identifier;
- `matchValue` is one identifier, not `match` followed by `Value`;
- the lone `_` is emitted as `Wildcard`, not `Identifier`;
- non-ASCII bytes outside string literals produce an unsupported-identifier
  diagnostic in the MVP.

### 3.3 Literals

| Literal | Examples | MVP behavior |
|---|---|---|
| Decimal integer | `0`, `42`, `65535` | Supported |
| Boolean | `true`, `false` | Supported |
| Nil | `nil` | Supported |
| String | `"hello"`, `"{\"n\":42}"` | Supported with a small fixed escape set |
| Byte | `b'{'`, `b'"'`, `b'\n'` | Supported |
| Float | `3.14`, `1e9` | Deferred with diagnostic |
| Character | `'é'` | Deferred with diagnostic |
| Base-prefixed integer | `0xff`, `0b10`, `0o77` | Deferred with diagnostic |

#### 3.3.1 Decimal integers

```ebnf
decimal_integer ::= digit+
digit           ::= "0".."9"
```

- A leading zero does **not** imply octal: `077` means decimal 77.
- `-42` is two tokens, `Minus` and `IntegerLiteral`; the parser handles unary
  negation.
- The lexer stores the source span. Integer conversion and overflow checking
  belong to literal semantic analysis, which can correctly handle the
  `-9223372036854775808` boundary.
- A letter or `_` immediately after digits is diagnosed as an invalid numeric
  suffix rather than silently producing two plausible tokens.
- A digit sequence followed by `.` and another digit is consumed for recovery
  and diagnosed as an unsupported floating literal during the MVP.

#### 3.3.2 Strings

```ebnf
string_literal ::= '"' string_item* '"'
string_item    ::= string_byte | escape
escape         ::= "\\" ("n" | "t" | "r" | '"' | "\\" | "0")
```

Rules:

- raw UTF-8 is allowed between quotes;
- an unescaped LF, CR, or end of file terminates scanning with an
  unterminated-string diagnostic;
- unknown escapes are errors;
- interpolation is not recognized;
- tokens retain the full source span, including quotes;
- decoding escapes is performed once by literal lowering, not while deciding
  token boundaries.

This small escape set is retained because, before `readFile()` exists, JSON
tests embed inputs such as `"{\"n\":42}"` in Joyeer source. General string
interpolation and Unicode escape syntax remain deferred.

#### 3.3.3 Byte literals

```ebnf
byte_literal ::= "b'" byte_item "'"
byte_item    ::= printable_ascii_except_quote_or_backslash | byte_escape
byte_escape  ::= "\\" ("n" | "t" | "r" | "'" | '"' | "\\" | "0")
```

Rules:

- exactly one decoded byte is required;
- unescaped content must be ASCII;
- empty, multi-byte, multi-character, invalid-escape, and unterminated byte
  literals are errors;
- `b` not followed by `'` starts a normal identifier;
- byte literals produce `UInt8` values after semantic lowering.

The required JSON punctuation can therefore be written directly:

```joyeer
b'{'  b'}'  b'['  b']'  b':'  b','  b'"'  b'\\'
b' '  b'\t' b'\r' b'\n'
```

### 3.4 Punctuation

The MVP punctuation set is:

| Token | Text | Purpose |
|---|---|---|
| `LeftParen` / `RightParen` | `(` / `)` | Calls, parameters, grouping |
| `LeftBrace` / `RightBrace` | `{` / `}` | Blocks and declaration bodies |
| `LeftBracket` / `RightBracket` | `[` / `]` | Arrays, dictionaries, subscripts |
| `Comma` | `,` | Lists and match arms |
| `Colon` | `:` | Types, labels, dictionaries |
| `Dot` | `.` | Members and contextual enum cases |
| `FatArrow` | `=>` | Match arm separator |

Whitespace and comments are trivia and do not appear in the token stream.
Each token records whether trivia before it contained a line break, so the
parser can separate statements. The MVP requires separate statements to start
on separate lines or be structurally delimited by braces; semicolons are
deferred.

### 3.5 Operators and markers

The MVP recognizes:

| Category | Tokens |
|---|---|
| Assignment | `=` |
| Arithmetic | `+`, `-`, `*` |
| Comparison | `==`, `!=`, `<`, `<=`, `>`, `>=` |
| Logical | `&&` |
| Optional type marker | `?` |
| Inout marker | `&` |

`?` is required initially for optional types such as `UInt8?`. Postfix error
propagation, optional chaining, and `??` coalescing are deferred.

`&` is only a token at the lexer level. The parser determines whether it marks
an `inout` argument or mutation site. Bitwise-AND semantics are deferred.

Compound assignment is omitted deliberately. The JSON parser writes:

```joyeer
p.pos = p.pos + 1
```

instead of:

```joyeer
p.pos += 1
```

This keeps both parsing and lowering smaller without reducing expressiveness.

### 3.6 Match terminals versus patterns

The lexer support needed for `match` is small:

- keyword `match`;
- punctuation `=>`, `.`, `(`, `)`, `,`, `{`, `}`;
- ordinary identifiers and literals;
- wildcard `_`.

The lexer does **not** understand enum cases, bindings, exhaustiveness, or arm
result types. Those belong to later stages.

The first parser profile should support only:

```ebnf
match_pattern ::= "_"
                | literal
                | identifier
                | "." identifier [ "(" match_pattern_list ")" ]

match_pattern_list ::= match_pattern ("," match_pattern)*
```

Required examples:

```joyeer
match byte {
  b'{' => parseObject(p: &p),
  b'[' => parseArray(p: &p),
  b't' => parseBool(p: &p),
  b'f' => parseBool(p: &p),
  b'n' => parseNull(p: &p),
  _    => parseNumber(p: &p),
}
```

```joyeer
match result {
  .Ok(value) => value,
  .Err(error) => return .Err(error),
}
```

Not required initially:

- `where` guards;
- alternatives such as `b't', b'f' => ...`;
- tuple patterns;
- range patterns;
- type-test patterns;
- mutable or consuming patterns.

Exhaustiveness checking for `enum` and `Bool` is still required in the type
checker. Integer and byte matches require `_` unless every value can be proven
covered; the MVP simply requires `_`.

---

## 4. Comments and whitespace

### 4.1 Whitespace

The lexer accepts and skips:

- space (`U+0020`);
- horizontal tab (`U+0009`);
- LF (`U+000A`);
- CR (`U+000D`);
- CRLF as one line break.

Vertical tab, form feed, and NUL are not ordinary source whitespace in the
MVP. They receive an invalid-source-character diagnostic. This avoids silently
accepting corrupted input.

### 4.2 Comments

Milestone requirements:

```joyeer
// line comment
/* block comment */
```

- `//` ends before LF, CR, or EOF;
- `/* ... */` may span lines;
- unterminated block comments are diagnosed at the opening delimiter;
- comment contents do not produce tokens;
- line tracking continues through comments;
- `///` is treated as an ordinary line comment in this milestone.

Nested block comments are desirable and straightforward with a depth counter,
but they are not a JSON-parser blocker. The implementation may land them after
the MVP; lexical conformance with the full specification requires them.

---

## 5. Token representation

The current token model copies lexemes into `std::string` and mixes source
position with literal values. The MVP should use an immutable source buffer and
absolute byte spans.

Recommended representation:

```cpp
struct SourceSpan {
    uint32_t offset;  // UTF-8 byte offset from the start of the file
    uint32_t length;  // UTF-8 byte count
};

struct Token {
    TokenKind kind;
    SourceSpan span;
  bool startsLine; // skipped trivia contained LF, CR, or CRLF
};
```

`TokenKind` should identify fixed terminals directly rather than putting every
operator into one `operators` bucket and comparing strings later:

```cpp
enum class TokenKind {
    EndOfFile,
    Invalid,

    Identifier,
    DeferredKeyword,
    Wildcard,
    IntegerLiteral,
    StringLiteral,
    ByteLiteral,
    TrueLiteral,
    FalseLiteral,
    NilLiteral,

    KwFunc,
    KwStruct,
    KwEnum,
    KwLet,
    KwVar,
    KwIf,
    KwElse,
    KwWhile,
    KwMatch,
    KwReturn,
    KwInout,

    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    Comma,
    Colon,
    Dot,
    FatArrow,

    Equal,
    EqualEqual,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Plus,
    Minus,
    Star,
    Ampersand,
    AmpersandAmpersand,
    Question,
};
```

Benefits:

- parser code switches on enums instead of string values;
- one deferred-keyword kind reserves future spellings without implementing
  their grammar;
- operator spelling cannot be mistyped in multiple maps;
- tokens require no per-token lexeme allocation;
- source excerpts are obtained with `source.substr(span.offset, span.length)`;
- literal values can be decoded by the semantic stage with the expected type;
- future terminals can be appended without changing existing token meaning.

### 5.1 Source positions

The canonical location is the absolute byte span. Human-facing locations are
computed from a line-start table owned by `SourceFile`:

```cpp
std::vector<uint32_t> lineStarts; // starts with 0
```

Rules:

- internal offsets and lengths are zero-based byte counts;
- diagnostics display one-based line and column numbers;
- CRLF adds one line start after both bytes;
- token location is always its **first** byte, never its length;
- `startsLine` is true for the first token and whenever skipped trivia
  contains at least one line break;
- source files and token spans use at least 32-bit fields, not `uint16_t`;
- line/column derivation is centralized instead of recomputed inconsistently by
  individual scanner functions.

For the MVP, columns may count UTF-8 bytes because identifiers are ASCII.
A later diagnostic renderer may convert byte columns to display columns for
Unicode string contents without changing token spans.

### 5.2 EOF

The lexer always emits exactly one `EndOfFile` token, including after a lexical
error. Its span is `{source.size(), 0}`. This removes end-iterator special cases
from the parser and gives unexpected-EOF diagnostics a stable location.

---

## 6. Scanner architecture

### 6.1 Public API

Keep the pipeline shape small and explicit:

```cpp
class Lexer {
public:
    Lexer(const SourceFile& source, Diagnostics& diagnostics);
    std::vector<Token> tokenize();
};
```

If the existing `LexParser::parse(SourceFile::Ptr)` API is retained during the
transition, it must:

1. clear `sourceFile->tokens` before scanning;
2. reset all cursor state;
3. scan the entire source;
4. append one EOF token;
5. report all recoverable lexical errors through `Diagnostics`.

The preferred name is `Lexer`, not `LexParser`: this stage tokenizes source and
does not parse syntax.

### 6.2 Cursor primitives

Use byte indices rather than `std::string::const_iterator` arithmetic:

```cpp
bool atEnd() const;
uint8_t peek(uint32_t lookahead = 0) const;
uint8_t advance();
bool consumeIf(uint8_t expected);
void emit(TokenKind kind, uint32_t start);
void error(SourceSpan span, DiagnosticId id);
```

`peek()` returns a sentinel only internally; NUL in source is diagnosed rather
than treated as ordinary whitespace.

### 6.3 Main loop

```text
tokenize:
    clear output
    while not EOF:
        skip whitespace and comments
        if EOF: break

        start = cursor
        c = advance()

        if c begins identifier:
            if c == 'b' and next byte is apostrophe:
                scan byte literal
            else:
                scan identifier or keyword
        else if c is digit:
            scan decimal integer or unsupported numeric form
        else if c is double quote:
            scan string
        else:
            scan punctuation/operator using longest match
            or diagnose invalid character

    emit EOF
```

No scanner routine rewinds beyond its token start, and no routine recursively
calls the main lexer.

### 6.4 Longest-match table

Required ambiguous prefixes are resolved in this order:

| Prefix | Result |
|---|---|
| `=` | `=>`, then `==`, then `=` |
| `!` | `!=`; standalone `!` is unsupported in the MVP |
| `<` | `<=`, then `<` |
| `>` | `>=`, then `>` |
| `&` | `&&`, then `&` |
| `|` | Unsupported in the MVP, including `||` |
| `?` | `?`; `??` and `?.` receive deferred-syntax diagnostics |
| `/` | `//` or `/*`; bare division is unsupported in the MVP |
| `b` | byte literal only when immediately followed by `'`; otherwise identifier |

Unsupported but recognizable forms such as `+=`, `??`, `?.`, `||`, bare `!`,
bare `/`, `%`, `;`, `<<`, and `...` should produce one targeted diagnostic.
They should not silently become several valid MVP tokens when that would hide
an accidental use of deferred syntax.

### 6.5 Complexity

For $n$ source bytes:

- time: $O(n)$;
- token storage: $O(t)$ for $t$ tokens;
- temporary lexer storage: $O(1)$, excluding diagnostics and optional line
  starts;
- no token requires a lexeme allocation.

---

## 7. Diagnostics and recovery

### 7.1 Required lexical diagnostics

| Diagnostic | Recovery |
|---|---|
| Invalid source character | Emit `Invalid`, advance one byte/code point |
| Unsupported keyword/syntax form | Consume the complete recognizable form |
| Unterminated string | Stop at line end or EOF; emit `Invalid` |
| Invalid string escape | Consume escape and continue to closing quote |
| Unterminated byte literal | Stop at line end or EOF; emit `Invalid` |
| Empty/multi-byte byte literal | Consume through closing quote |
| Invalid byte escape | Consume through closing quote |
| Unsupported floating literal | Consume the complete numeric-looking form |
| Unsupported base prefix | Consume alphanumeric numeric candidate |
| Invalid numeric suffix | Consume suffix to the next delimiter |
| Unterminated block comment | Consume to EOF |

The compiler may stop before parsing when lexical failures exist, but the lexer
should still collect multiple independent errors where recovery is unambiguous.

### 7.2 Diagnostic shape

Every error should contain:

- stable diagnostic identifier;
- file path;
- source span;
- one-based line and column for display;
- concise message;
- optional help text for deferred syntax.

Example:

```text
error[LEX004]: floating-point literals are not supported in the JSON-parser MVP
  --> parser.joyeer:12:17
   |
12 | let scale = 1.25
   |             ^^^^
help: use an Int representation for the first milestone
```

### 7.3 No silent default branch

The main dispatch must never use an empty `default:` branch. Unknown bytes are
errors. Silent skipping changes program meaning and can turn a typo into a
valid but different token sequence.

---

## 8. Current implementation assessment

The existing C++ lexer is a useful prototype but is not the implementation
specified above.

| Area | Current behavior | Required change |
|---|---|---|
| Token model | Broad `keyword` / `operators` / `punctuation` kinds with copied strings | Explicit terminal kinds and source spans |
| EOF | No EOF token | Always append one EOF token |
| Repeated compile | Token vector is not cleared by the lexer | Clear output before scanning |
| Keywords | Legacy subset; no `enum` / `match` / `inout` | Implement the MVP keyword table |
| Match | No `match` or `=>` token | Add keyword and fat arrow |
| Byte literal | Missing | Add strict `b'x'` scanner |
| Integer | Legacy leading-zero octal behavior | Decimal-only; no implicit octal |
| Float | Fraction is scanned but emitted as decimal and converted to `int` | Diagnose as deferred or implement later correctly |
| Strings | Skips escaped byte but does not validate it | Validate the MVP escape set |
| Comments | Block comments do not nest and do not update line state | Correct line tracking; nesting may follow MVP |
| Unknown input | Silently ignored | Emit diagnostic and recover |
| Position | Several constructors receive token length as column; two-byte operators may get column 0 | Store absolute start span |
| Newlines in comments/strings | Line state is not consistently updated | Centralize cursor/location handling |
| Source range width | `uint16_t` line/column | Use 32-bit offsets/ranges |
| Literal payload | `int` conversion happens in lexer | Defer typed conversion to semantic analysis |

The current pipeline order remains valid:

```text
SourceFile -> Lexer -> SyntaxParser -> TypeGen -> TypeBinding -> IRGen
```

Only the lexer/token contract needs replacement; a new compiler stage is not
required.

---

## 9. Implementation sequence

### L0 — Token and span foundation

1. Introduce `SourceSpan` and explicit `TokenKind`.
2. Add `EndOfFile` and `Invalid`.
3. Store token spans as absolute byte offsets.
4. Add centralized line-start indexing and location rendering.
5. Clear tokens and lexer state before every scan.

**Exit condition:** punctuation-only input produces exact kinds/spans plus EOF.

### L1 — Trivia, identifiers, and MVP keywords

1. Implement whitespace and CR/LF/CRLF handling.
2. Implement line and block comments.
3. Implement ASCII identifiers.
4. Classify the MVP keyword and literal words.
5. Diagnose invalid source bytes.

**Exit condition:** every MVP word is classified correctly; `matchValue`
remains an identifier.

### L2 — Literals

1. Decimal integer spans.
2. Strings with the fixed MVP escape set and termination checks.
3. Strict byte literals.
4. Targeted diagnostics for deferred float/base-prefixed/character forms.

**Exit condition:** all JSON punctuation and whitespace bytes can be expressed
as Joyeer byte literals.

### L3 — Operators and match terminals

1. Implement longest-match operator dispatch.
2. Add `=>`, `?`, and `&`.
3. Reject unsupported compound/bitwise/range forms clearly.

**Exit condition:** the target JSON-parser `match` source tokenizes exactly.

### L4 — Parser integration and tests

1. Update parser token comparisons to explicit `TokenKind` values.
2. Add token-dump test support or direct lexer unit tests.
3. Add positive and negative acceptance cases.
4. Run all legacy end-to-end tests and migrate only intentional differences.

**Exit condition:** lexer unit tests and existing end-to-end tests are green;
the complete JSON-parser target source reaches the parser without lexical
errors.

---

## 10. Test matrix

Lexer tests should operate directly on in-memory source strings. Golden
end-to-end tests alone cannot reliably distinguish lexer and parser defects.

### 10.1 Positive cases

| Area | Inputs / assertions |
|---|---|
| Empty | `""` -> EOF |
| Whitespace | space, tab, LF, CR, CRLF; correct EOF span/line |
| Identifier | `x`, `_x`, `matchValue`, `b0` |
| Wildcard | `_` -> Wildcard |
| Keywords | every MVP keyword, each followed by delimiter and EOF |
| Literal words | `true false nil` |
| Integers | `0`, `00`, `077`, `9223372036854775808` remain integer spans |
| Strings | empty, ASCII, raw UTF-8, every valid MVP escape, embedded JSON object |
| Bytes | every JSON structural byte and whitespace escape |
| Operators | each one- and two-byte MVP operator |
| Longest match | `=> == = != <= < >= > && & ?` |
| Comments | line at EOF, line before CRLF, multiline block |
| Match | enum case, payload bind, wildcard, byte arm, block arm |
| Locations | first line, after LF, CR, CRLF, comment, UTF-8 string |

### 10.2 Negative cases

| Area | Inputs / expected diagnostic |
|---|---|
| Unknown | backtick, stray apostrophe, NUL |
| String | newline before close, EOF before close, unknown escape |
| Byte | `b''`, `b'ab'`, `b'é'`, `b'\q'`, missing close quote |
| Number | `1.2`, `1e3`, `0xff`, `0b1`, `0o7`, `12abc`, `1_000` |
| Comment | unterminated `/*` |
| Deferred syntax | `+=`, `??`, `?.`, `||`, `!`, `/`, `%`, `;`, `<<`, `...` |
| Deferred words | `for`, `where`, `public`, and every other reserved/deferred word -> targeted diagnostic |
| Removed effects | `performs` and `pure` tokenize as identifiers, never keywords |

### 10.3 Invariants

Property-style tests should verify:

1. token spans are ordered and never overlap;
2. every non-trivia byte is covered by a valid or invalid token;
3. the final token is exactly one EOF;
4. tokenization always advances or terminates;
5. re-tokenizing the same `SourceFile` produces the same token list without
   duplication;
6. concatenating token source slices and skipped trivia accounts for the full
   source buffer;
7. no malformed input causes out-of-bounds reads.

Fuzzing arbitrary byte buffers is appropriate even before the parser is
complete; the primary initial property is “diagnose or tokenize, never crash or
hang.”

---

## 11. Definition of done for the JSON-parser milestone

The Lexer MVP is complete when all of the following hold:

- the target Joyeer JSON-parser source uses only terminals in this document;
- all terminals tokenize with correct byte spans;
- `enum`, payload cases, `match`, byte literals, `=>`, `?`, and `&` reach the
  parser correctly;
- every invalid byte produces a diagnostic;
- strings and byte escapes are validated;
- LF, CR, and CRLF locations are correct;
- an EOF token is always emitted;
- direct lexer unit tests cover every token and every lexical diagnostic;
- the lexer is linear-time and performs no per-token lexeme allocation;
- deferred syntax is rejected intentionally rather than partially accepted;
- the existing compiler pipeline consumes the new token contract;
- the JSON-parser target source has no lexical errors.

This milestone does **not** require every chapter of the future Joyeer
specification. New syntax should be added only when a concrete language feature
or standard-library implementation requires it.

---

## 12. Decisions summary

| Question | Decision |
|---|---|
| Implementation language | C++20 |
| Self-hosting now? | No |
| Immediate product target | JSON parser written in Joyeer |
| General effect syntax | Removed |
| `enum + match` | Required |
| Pattern work in lexer? | No; lexer emits terminals only |
| Match guards/ranges/alternatives | Deferred |
| Number literals in Joyeer source | Decimal `Int` only |
| JSON decimal parsing | Runtime responsibility of the JSON parser |
| Character literals | Deferred; use `UInt8` byte literals |
| `for-in` | Deferred; use `while` |
| `??` coalescing | Deferred; use `match` on `Optional` / `Result` |
| Compound assignment | Deferred; write explicit assignment |
| Token text ownership | Source span into immutable source |
| Source position | Absolute UTF-8 byte span; displayed as 1-based line/column |
| Unknown characters | Always diagnostic |
| EOF token | Mandatory |
| First optimization target | Correctness and simplicity, not micro-optimization |
