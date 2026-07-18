# Joyeer Lexer Implementation

> **Status:** The JSON-parser v0.1 lexical surface is implemented as the
> compiler's only lexer. The normative grammar is
> [../spec/01-lexical.md](../spec/01-lexical.md).

## Boundary

`LexParser` lives in `include/joyeer/compiler/lexparser.h` and
`lib/compiler/lexparser.cpp`. It accepts a `Diagnostics*`, tokenizes one
`SourceFile`, and replaces that file's token and line-start tables on every
call. It does not own parser, semantic, type, or backend state.

```text
SourceFile -> LexParser -> Token stream -> Parser
```

The lexer decides token boundaries, literal decoding, trivia, and source
spans. Pattern structure, precedence, name binding, and types belong to later
stages.

## Token model

Each token carries:

- an explicit `TokenKind`;
- `SourceSpan { offset, length }` in UTF-8 bytes;
- zero-based line and byte-column values;
- `startsLine`, derived from skipped newline trivia;
- `rawValue`, plus `intValue` for integer and byte literals.

`SourceFile::lineStarts` always begins with zero and records the byte offset
after every LF, CR, or CRLF sequence. Diagnostics convert these zero-based byte
locations to one-based display locations.

The lexer clears existing output before scanning and appends exactly one EOF
token. Every scanner either advances or returns, so malformed input cannot
create an infinite loop.

## Implemented surface

The lexer recognizes the terminals needed by the current parser and native
JSON fixture:

- identifiers, wildcard `_`, and current/deferred keywords;
- decimal `Int` literals with `int64_t` overflow diagnostics;
- strings with the fixed escape set `\0`, `\t`, `\n`, `\r`, `\"`, `\'`, and
  `\\`;
- strict `UInt8` byte literals such as `b'{'`, `b'\n'`, and `b'\\'`;
- delimiters, labels, member access, `=>`, `?`, and `&`;
- assignment, arithmetic/comparison operators, and `&&` used by v0.1;
- line comments and nested block comments.

The longest valid terminal wins. Unsupported compound, shift, range,
coalescing, optional-chain, interpolation, float/base-prefixed numeric, and
other deferred forms are consumed as coherent invalid tokens where possible,
then reported with stable `lexer.*` IDs.

## Invalid input and recovery

No non-trivia byte is silently discarded. Invalid source bytes, non-ASCII
identifier bytes, malformed escapes, unterminated literals/comments,
unsupported numeric forms, and reserved syntax all produce diagnostics.

For malformed multi-byte forms, recovery consumes the complete recognizable
unit instead of emitting misleading valid prefixes. Examples include `+=`,
`<<=`, `?.`, `1.5`, and `0xff`.

Source files larger than the 32-bit span capacity are rejected before scanning.

## Deferred syntax policy

The lexer reserves words needed by later language phases without pretending
their grammar is implemented. Adding a terminal requires a concrete parser or
standard-library consumer and focused positive/negative tests. The removed
general `performs`/`pure` effect system must not be reintroduced through token
reservation.

## Validation

Focused validation:

```pwsh
ctest --test-dir build -L lexer --output-on-failure
```

Coverage includes:

- every supported keyword, literal, delimiter, and operator;
- LF, CR, and CRLF line tracking;
- nested/unterminated comments;
- integer limits and unsupported numeric forms;
- malformed strings and bytes;
- deterministic arbitrary byte buffers with ordered bounded spans;
- retokenization reset behavior and exactly one EOF;
- the JSON-parser acceptance source;
- CLI rendering of lexer source diagnostics.

The complete unfiltered CTest suite is the final gate.