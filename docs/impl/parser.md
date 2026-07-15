# Parser MVP — JSON-Parser Grammar and Implementation Contract

> **Status:** Parser MVP implemented and parser-only validation green.
> **Profile:** `--lang=v0.1` only. The existing parser remains a legacy
> compatibility implementation while this parser is built and tested in
> isolation.
> **Normative source:** [the language specification](../spec.md). This document
> narrows that future language to the parser surface required by the first
> Joyeer JSON parser; it does not redefine the language.

---

## 1. Goal and non-goals

The Parser MVP converts the explicit token stream produced by the
[Lexer MVP](lexer.md) into a syntax-only AST. Its acceptance target is the
canonical JSON-parser slice in [the v0.1 plan](../plan/v0.1.md):

- typed `func` declarations and mandatory call-site labels;
- `let` / `var` bindings;
- field-only `struct` declarations and memberwise construction;
- payload-carrying `enum` declarations and construction;
- minimal `match` expressions and enum patterns;
- `inout` and `consuming` parameters plus mandatory `&` / `consume` call-site
  markers;
- nominal, array, dictionary, optional, and built-in generic types;
- byte, integer, string, Boolean, and `nil` literals;
- member access, calls, subscripts, arrays, dictionaries, `if`, `while`, and
  `return`;
- correct MVP operator precedence and associativity;
- recoverable syntax diagnostics with stable spans.

The Parser MVP deliberately does **not** implement:

- name resolution, type inference, exhaustiveness, ownership checking, or IR;
- `class`, `for-in`, imports, extensions, explicit `init` / `deinit`, methods,
  subscript declarations, visibility, or user-defined generics;
- `mutating`, `indirect`,
  `where`, or `yield`;
- tuple/function types, tuple destructuring, match guards, alternative/range
  patterns, or recursive direct-payload enums;
- `/`, `%`, `||`, `!`, `??`, `?.`, propagation/force-unwrap postfix forms,
  compound assignment, ranges, shifts, or bitwise expressions;
- semicolons or multiple block items on one physical line;
- a lossless concrete syntax tree. The current lexer does not retain trivia,
  so the MVP produces a spanned AST rather than pretending to be lossless.

These exclusions are syntax boundaries, not permanent removals. They are added
only when a later milestone has a concrete consumer.

---

## 2. Audit of the current parser

The current C++ parser in
[`syntaxparser.cpp`](../../lib/compiler/syntaxparser.cpp) is a useful legacy
reference, but it is not the foundation for the new grammar.

| Area | Current behavior | Parser MVP requirement |
|---|---|---|
| Token access | raw iterators, `previous()`, broad legacy token categories, and string comparisons | bounded `TokenCursor`, explicit terminal kinds, checkpoints, and `expect()` |
| Declarations | bindings, functions, legacy `class`, basic `struct`, legacy constructor/import | context-specific v0.1 declarations with dedicated parameter/field/case nodes |
| Parameters | represented as binding `Pattern` nodes | `ParameterDecl` with external label, local name, borrowing/inout/consuming effect, type, and span |
| Types | identifier, `[T]`, and legacy `T?` / `T!` behavior | nominal/built-in generic, `[T]`, `[K: V]`, and `T?`; no `T!` type |
| Expressions | flat prefix plus binary tail | fully shaped tree produced by a Pratt/precedence-climbing parser |
| Precedence | later rewritten by `TypeGen` using only `high` / `low` buckets | parser owns all precedence and associativity; semantic passes never repair syntax |
| Patterns | identifier plus optional type annotation | tagged wildcard/literal/binding/enum-case pattern syntax |
| Errors | mostly generic `"Error"`; first failure often returns `nullptr` for the file | stable diagnostic IDs, error nodes, synchronization, and continued parsing |
| AST | carries symbol tables, type slots, and runtime descriptors | syntax-only nodes with source spans; semantic state belongs to later layers |
| Tests | no direct parser target | isolated AST/diagnostic snapshots that never enter the VM/runtime |

The legacy parser remains available to legacy golden tests. New syntax should
not be threaded through its flat `Expr::binaries` representation merely to
keep old lowering alive.

The implemented v0.1 path lives in `parser.h` / `parser.cpp` with its separate
syntax-only AST in `syntax.h` / `syntax.cpp`. `--lang=v0.1` runs the MVP lexer
and this parser, reports stable parser diagnostics, then passes a successful
tree through the independent name resolver, type checker, semantic analysis,
Joyeer IR, and native backend. It never enters the old VM/runtime. Default/
legacy mode continues through the old `SyntaxParser`, semantic passes,
bytecode, and VM.

---

## 3. Parser boundaries

### 3.1 What the parser decides

The parser decides only facts visible in the token stream:

- declaration, type, expression, and pattern structure;
- delimiters, commas, labels, and ownership-marker placement;
- operator precedence and associativity;
- obvious assignment-target shape;
- whether a construct belongs to the Parser MVP grammar;
- source spans and syntax recovery points.

### 3.2 What later stages decide

| Question | Owning stage |
|---|---|
| Does a name refer to a value, type, function, field, or enum case? | name resolution |
| Is `Foo.Bar(...)` an enum constructor, and is `.Bar(...)` contextually typed? | name resolution + type checking |
| Do supplied labels match a function, initializer, or enum payload declaration? | name resolution + type checking |
| Is a generic type one of the built-in generic containers allowed in v0.1? | type checking |
| Do `&` / `consume` match inout/consuming signatures and valid storage? | type checking + semantic ownership analysis |
| Are assignment targets mutable and initialized correctly? | semantic analysis |
| Do `if` branches and `match` arms have compatible types? | type checking |
| Is a `match` exhaustive and are payload patterns valid? | pattern type checking + exhaustiveness |
| Is a `return` valid in the current function and of the correct type? | control-flow + type checking |

Enum payloads make invocation syntax context-sensitive: ordinary functions and
struct initializers require labels, while an enum payload may contain
positional and labeled fields. The parser therefore stores an optional label
on every syntactic argument. Resolution later rejects an unlabeled ordinary
function/initializer argument and validates enum payload labels. Accepting this
small syntactic superset is necessary for precise diagnostics without guessing
whether the callee is a type or value from capitalization.

---

## 4. Parser MVP grammar

The notation follows spec §0.4. Every comma-separated list permits a trailing
comma unless a rule says otherwise.

### 4.1 Source file and declarations

```ebnf
source_file        ::= top_level_item* EOF

top_level_item     ::= binding_decl
                     | func_decl
                     | struct_decl
                     | enum_decl

binding_decl       ::= binding_kind identifier
                       [ ':' type ]
                       [ '=' expression ]
binding_kind       ::= 'let' | 'var'

func_decl          ::= 'func' identifier parameter_clause
                       [ ':' type ] block
parameter_clause   ::= '(' [ parameter ( ',' parameter )* ','? ] ')'
parameter          ::= identifier [ identifier ] ':' [ 'borrowing' | 'inout' | 'consuming' | 'initializing' ] type

struct_decl        ::= 'struct' identifier '{' struct_field* '}'
struct_field       ::= binding_kind identifier ':' type
                       [ '=' expression ]

enum_decl          ::= 'enum' identifier '{'
                       enum_case ( ',' enum_case )* ','?
                       '}'
enum_case          ::= identifier
                       [ '(' associated_type
                           ( ',' associated_type )* ','? ')' ]
associated_type    ::= [ identifier ':' ] type
```

Rules and intentional restrictions:

1. A top-level binding may infer its type; a stored field must declare one.
2. A function parameter always has an external label. With one identifier it
   is also the local name; `from source: String` uses `from` externally and
   `source` in the body.
3. All four parameter access effects are in the Parser MVP; borrowing is also
  the implicit default. Calls spell `&` for inout/initializing and `consume`
  for consuming; borrowing has no marker.
4. A Parser MVP `struct` contains stored fields only. Explicit initializers,
   methods, and subscripts are later syntax phases; construction resolves to a
   synthesized memberwise initializer.
5. A Parser MVP `enum` contains one or more cases only. Cases do not use a
   `case` keyword.
6. An associated type may be positional (`Bool(Bool)`) or labeled
   (`Unexpected(UInt8, at: Int)`). Construction and patterns must reproduce
   labels declared on labeled positions; the type checker enforces this.
7. Direct recursive payloads need future `indirect` syntax. The parser records
  the type syntax; a later layout check rejects direct recursion without
  `indirect`. Recursion through fixed-size container handles such as
  `[JsonValue]` is not a direct recursive payload and remains valid.

### 4.2 Types

```ebnf
type               ::= type_primary [ '?' ]

type_primary       ::= identifier [ generic_argument_clause ]
                     | '[' type ']'
                     | '[' type ':' type ']'

generic_argument_clause
                   ::= '<' type ( ',' type )* ','? '>'
```

Examples:

```joyeer
Int
[UInt8]
[String: JsonValue]
UInt8?
Result<JsonValue, JsonError>
```

The parser records generic type syntax without deciding whether the base is a
permitted built-in. In v0.1 the type checker accepts only built-in generic
containers (`Array`, `Dict`, `Optional`, and `Result`); user-defined generics
remain out of scope.

`?` is a type suffix in this phase. It is not parsed as expression propagation
or optional chaining.

The current lexer treats `>>` as one deferred shift token. Adjacent nested
angle closers are not needed by the JSON acceptance source, but no whitespace
workaround becomes part of Joyeer. Before nested built-in generic types become
a completion requirement, the lexer/parser boundary must represent a shift
operator in a way that the type parser can split into two closing `>` tokens.

### 4.3 Blocks and block items

```ebnf
block              ::= '{' block_item* '}'
block_item         ::= binding_decl
                     | while_stmt
                     | expression

while_stmt         ::= 'while' expression block
return_expr        ::= 'return' [ expression ]
```

`if`, `match`, and `return` are expressions. A standalone expression is also a
block item. The last expression in a block is its value; a later type-checking
phase decides when that value is required or must be `Void`.

The same `return_expr` node represents a standalone early return and a
diverging expression in an arm such as:

```joyeer
.None => return .Err(.UnexpectedEof),
```

The optional returned expression must begin on the same physical line as
`return`. A line-starting token after bare `return` begins the next block item;
multiline returned expressions remain possible through an open delimiter or a
continuation token.

### 4.4 Expression precedence

The Parser MVP uses a Pratt or equivalent precedence-climbing parser. From
highest to lowest:

| Binding power | Forms | Associativity |
|---|---|---|
| 8 | member `.`, call `(...)`, subscript `[...]` | left |
| 7 | prefix `-`, access marker `&` | right |
| 6 | `*` | left |
| 5 | `+`, `-` | left |
| 4 | `<`, `<=`, `>`, `>=` | non-associative |
| 3 | `==`, `!=` | non-associative |
| 2 | `&&` | left |
| 1 | `=` | right |

Equivalent structural grammar:

```ebnf
expression         ::= return_expr
                     | assignment_expr
assignment_expr    ::= logical_and_expr [ '=' assignment_expr ]
logical_and_expr   ::= equality_expr ( '&&' equality_expr )*
equality_expr      ::= comparison_expr [ ( '==' | '!=' ) comparison_expr ]
comparison_expr    ::= additive_expr
                       [ ( '<' | '<=' | '>' | '>=' ) additive_expr ]
additive_expr      ::= multiplicative_expr
                       ( ( '+' | '-' ) multiplicative_expr )*
multiplicative_expr
                   ::= prefix_expr ( '*' prefix_expr )*
prefix_expr        ::= '-' prefix_expr
                     | '&' postfix_expr
                     | postfix_expr
postfix_expr       ::= primary_expr postfix_suffix*
postfix_suffix     ::= '.' identifier
                     | argument_clause
                     | '[' expression ']'
```

Consequences that must be visible in AST snapshots:

```text
1 + 2 * 3       => 1 + (2 * 3)
a - b - c       => (a - b) - c
a = b = value   => a = (b = value)
a < b < c       => syntax error: comparison operators do not chain
```

`&` is prefix-only in the Parser MVP and produces an explicit access-marker
node. The full language later also uses it as infix bitwise AND.

An assignment target must have one of these syntactic shapes:

```ebnf
assignment_target  ::= identifier
                     | postfix_expr '.' identifier
                     | postfix_expr '[' expression ']'
                     | '&' assignment_target
```

The parser diagnoses obviously invalid targets such as `(a + b) = c`. Whether
a syntactically valid target is mutable, requires `&`, or is initialized is a
later semantic question.

### 4.5 Primary, postfix, and collection expressions

```ebnf
primary_expr       ::= literal
                     | identifier
                     | contextual_case_expr
                     | parenthesized_expr
                     | array_literal
                     | dict_literal
                     | if_expr
                     | match_expr

parenthesized_expr ::= '(' expression ')'
contextual_case_expr
                   ::= '.' identifier [ argument_clause ]

argument_clause    ::= '(' [ argument ( ',' argument )* ','? ] ')'
argument           ::= [ identifier ':' ] [ '&' ] expression

array_literal      ::= '[' [ expression ( ',' expression )* ','? ] ']'
dict_literal       ::= '[' ':' ']'
                     | '[' dict_entry ( ',' dict_entry )* ','? ']'
dict_entry         ::= expression ':' expression
```

Postfix suffixes compose without special cases:

```joyeer
p.input[p.pos]
Parser(input: source, pos: 0)
JsonValue.Bool(true)
```

The parser records calls uniformly:

- `parseValue(p: &p)` has a labeled `inout` argument;
- `Parser(input: source, pos: 0)` has labeled initializer arguments;
- `.Bool(true)` has one positional enum-payload argument;
- `.Unexpected(c, at: pos)` mixes a positional and labeled payload argument.

The resolver/type checker applies the declaration-specific restrictions from
§3.2.1 and §3.4; the parser does not infer them from capitalization.

### 4.6 Conditional expressions

```ebnf
if_expr            ::= 'if' expression block
                       [ 'else' ( if_expr | block ) ]
```

Syntactically, the same node may appear as a standalone block item or where a
value is expected. Type checking requires an `else` when the value is used and
requires all branches to agree on a result type.

### 4.7 Match expressions and patterns

```ebnf
match_expr         ::= 'match' expression '{' match_arm+ '}'
match_arm          ::= match_pattern '=>' ( expression | block ) ','?

match_pattern      ::= '_'
                     | literal
                     | identifier
                     | enum_case_pattern

enum_case_pattern  ::= [ identifier ] '.' identifier
                       [ '(' pattern_argument
                           ( ',' pattern_argument )* ','? ')' ]
pattern_argument   ::= [ identifier ':' ] match_pattern
```

Parser MVP rules:

1. `_` is wildcard; a plain identifier binds the matched value.
2. `.Case(...)` is contextual. `Type.Case(...)` is explicitly qualified.
3. Patterns nest, so `.Some(.Bool(true))` is syntactically valid.
4. Labels on associated values are preserved and validated later.
5. Each arm has exactly one pattern. Alternative patterns, tuple patterns,
   guards, and ranges are deferred.
6. A comma after an arm is optional. When omitted, the next arm must begin on a
   new physical line so recovery remains deterministic.
7. Exhaustiveness and duplicate/unreachable arms are not parser checks.

### 4.8 Literals

```ebnf
literal            ::= decimal_literal
                     | string_literal
                     | byte_literal
                     | boolean_literal
                     | nil_literal
```

The lexer already validates and decodes literal payloads. The parser wraps the
token and its span; it does not convert or reinterpret the value.

---

## 5. Newlines, commas, and statement boundaries

The full specification permits semicolons, but the JSON-parser lexer profile
rejects them. Parser MVP therefore has one predictable formatting rule:

> The first source item may begin at the start of the file, and the first block
> item may follow `{` on the same line. Every subsequent top-level or block
> item starts on a new physical line; two adjacent items may not share a line.

Newline information comes from `Token::startsLine`; newline is not a general
expression token. A newline does **not** terminate a construct:

- inside unmatched `()`, `[]`, or an argument/type/pattern list;
- after an infix operator, comma, colon, dot, or `=>`;
- while parsing an explicitly expected declaration body or `else` branch.

Delimited lists consistently permit a trailing comma. This includes function
parameters, call/constructor arguments, generic arguments, enum payloads,
array/dictionary elements, enum cases, and pattern payloads. Consistency is
more important than preserving the legacy parser's per-list differences.

---

## 6. Syntax AST contract

The new syntax AST is independent of symbol tables, runtime descriptors,
bytecode types, and VM objects. Every node owns a `SourceSpan` or a token range
from which that span is derived.

Minimum node families:

| Family | Nodes / retained information |
|---|---|
| File | `SourceFileSyntax`, ordered top-level items, EOF span |
| Declarations | `BindingDecl` (`let`/`var` retained), `FunctionDecl`, `ParameterDecl`, `StructDecl`, `StructFieldDecl`, `EnumDecl`, `EnumCaseDecl`, `AssociatedTypeSyntax` |
| Types | nominal, built-in generic argument list, array, dictionary, optional |
| Blocks/control | `BlockExpr`, `WhileStmt`, `IfExpr`, `ReturnExpr`, `MatchExpr`, `MatchArm` |
| Expressions | name, literal, prefix, access marker, binary, assignment, member, call, argument, subscript, array, dictionary, contextual case |
| Patterns | wildcard, literal, binding, enum case, labeled payload argument |
| Recovery | error declaration/type/expression/pattern nodes retaining skipped token span |

Important shape rules:

- Operators are represented by nested expression nodes at parse time. There is
  no flat binary list for a later pass to repair.
- `ParameterDecl` is not a `Pattern`.
- A binding keeps `let` versus `var`; they are not both erased to `VarDecl`.
- `CallArgument` keeps an optional label and optional access marker.
- `EnumCaseDecl` keeps labels per associated payload position.
- `BlockExpr` keeps ordered items. Its final expression is identified without
  moving it into a semantic/type node.
- Syntax nodes contain no mutable semantic fields such as `typeSlot` or
  `symtable`. Later stages use maps keyed by node ID or build a separate
  resolved/typed representation.

A lossless CST can be introduced later if formatter/IDE work requires it. That
requires the lexer to retain trivia first and is not a Parser MVP dependency.

---

## 7. Parser architecture

### 7.1 Token cursor

Use a bounded cursor rather than manipulating iterators directly:

```text
peek(offset = 0) -> Token
at(kind)         -> bool
eat(kind)        -> Token?
expect(kind, diagnostic_id) -> Token or synthetic missing token
checkpoint() / rewind(checkpoint)
position()       -> token index
atEnd()          -> bool
```

`peek()` at or beyond input returns the real terminal EOF token. Rewinding may
only return to a checkpoint and never moves before token zero.

### 7.2 Parsing strategy

- recursive descent for files, declarations, types, blocks, lists, and
  patterns;
- Pratt/precedence climbing for expressions;
- small, explicit lookahead for dictionary literals, external parameter names,
  and contextual case expressions;
- a shared separated-list helper for comma/trailing-comma behavior;
- no speculative parse routine emits diagnostics until its prefix has
  committed to that production.

### 7.3 Progress invariant

Every parser loop must do one of three things:

1. consume at least one real token;
2. return to its caller without consuming;
3. synchronize by consuming tokens up to a known boundary.

User input must never reach `assert`, call `abort`, read beyond EOF, or spin.
If a lexer `invalid` / `deferredKeyword` token is encountered, the parser
consumes it into one error node and avoids a redundant generic diagnostic when
the lexer has already reported the root cause.

---

## 8. Diagnostics and recovery

### 8.1 Required diagnostic categories

At minimum, use stable IDs for:

- expected declaration, identifier, type, expression, pattern, or block;
- expected specific delimiter/token;
- unexpected token in a source file, block, enum body, or match body;
- missing comma or missing list element;
- invalid assignment target;
- chained non-associative comparison/equality operator;
- two block items on one physical line;
- unsupported Parser MVP syntax that reached parsing;
- unexpected EOF while parsing a delimited construct.

Diagnostics point at the unexpected token span; missing-token diagnostics use a
zero-length insertion span. Human rendering may remain simple initially, but
IDs and spans must be stable enough for snapshot tests.

### 8.2 Synchronization sets

| Context | Synchronize before/at |
|---|---|
| Source file | EOF or next line-starting `let`, `var`, `func`, `struct`, `enum` |
| Block | `}`, EOF, or next line-starting block-item prefix |
| Parameter/argument/type list | `,`, matching `)`, or EOF |
| Array/dictionary/subscript | `,`, matching `]`, or EOF |
| Enum body | `,`, `}`, EOF, or next line-starting identifier |
| Match arm | `=>`, `,`, `}`, EOF, or next line-starting plausible pattern |
| Nested expression | current closing delimiter, comma, line-starting item, or EOF |

Recovery must preserve the outer closing delimiter whenever possible so one
malformed inner item does not consume the remainder of the file.

---

## 9. Implementation sequence

### P0 — Isolated parser foundation ✅

1. Introduce the syntax-only AST and node spans.
2. Introduce `TokenCursor`, diagnostic IDs, error nodes, and a parser-only test
   executable labeled `parser`.
3. Select the new parser only for `--lang=v0.1`; leave legacy lowering on the
   legacy parser.
4. Add deterministic AST and diagnostic dump formats.

**Exit:** empty input and malformed token streams always produce a file node,
one EOF boundary, and no crash/hang.

### P1 — Types and declarations ✅

1. Parse MVP type syntax, including `Result<T, E>` and `[K: V]`.
2. Parse bindings while retaining `let`/`var`.
3. Parse functions, labels/local names, `inout`/`consuming`, and trailing commas.
4. Parse field-only structs and payload-only enums.

**Exit:** all declarations and signatures in the focused JSON source parse with
exact spans.

### P2 — Pratt expressions ✅

1. Parse literals, names, parenthesized expressions, arrays, and dictionaries.
2. Parse member/call/subscript chains and contextual `.Case` expressions.
3. Parse call arguments with optional syntax labels and `&` / `consume` markers.
4. Parse prefix/infix/assignment operators with the table in §4.4.
5. Diagnose chained comparisons and invalid assignment targets.

**Exit:** precedence and postfix-chain snapshots are exact; `TypeGen` is not
involved.

### P3 — Control flow and patterns ✅

1. Parse blocks, line boundaries, trailing expressions, `while`, and `return`.
2. Parse `if` as one expression node usable in statement or value position.
3. Parse minimal match arms and wildcard/literal/binding/enum-case patterns.
4. Parse `return .Err(...)` as a diverging match-arm expression.

**Exit:** the focused JSON-parser source produces a complete AST with no parser
diagnostics.

### P4 — Recovery and corpus ✅

1. Implement synchronization sets and missing-token/error nodes.
2. Add positive and negative parser corpus snapshots.
3. Add deterministic token deletion/insertion/replacement mutation tests.
4. Verify every malformed case terminates, stays within token bounds, and
   reports later independent errors when recovery permits.

**Exit:** all direct parser tests pass without invoking name resolution,
type checking, IR, VM, or runtime.

Only after P0–P4 are green should later phases add AST consumers for enum
layout, match exhaustiveness, ownership, and lowering.

---

## 10. Test matrix

Parser tests consume in-memory source through the v0.1 lexer, then compare a
normalized syntax AST or diagnostic stream. They never execute the program.

### 10.1 Positive corpus

| Area | Required cases |
|---|---|
| Empty/file | empty input, one and several top-level items |
| Bindings | inferred/annotated, initialized/uninitialized, `let` vs `var` |
| Parameters | one-name, external/local names, borrowing/inout/consuming/initializing, empty/non-empty/trailing comma |
| Types | nominal, array, dictionary, optional, `Result<T, E>`, nesting through `[]` / `?`; adjacent angle closers after the documented lexer handshake |
| Struct | typed fields, field initializer, multiline body |
| Enum | empty-payload, positional payload, labeled payload, mixed payload, trailing comma |
| Literals | every Lexer MVP literal kind, especially `byteLiteral` |
| Collections | empty/non-empty arrays and dictionaries, nesting, trailing commas |
| Postfix | member/call/subscript chains and multiline argument clauses |
| Calls/cases | labeled function/initializer calls, marker-free borrowing, `&` inout/initializing, `consume` arguments, positional/labeled enum payloads, contextual and qualified cases |
| Precedence | every neighboring precedence pair, left/right/non-associativity |
| Assignment | name, member, subscript, `&` target, right-associative chain |
| Control | standalone/value `if`, else-if, `while`, empty/value blocks, early return |
| Match | literal/wildcard/binding/case/nested-case patterns, expression/block/return arms |
| Locations | exact node spans across LF, CR, CRLF, comments, and UTF-8 strings |

### 10.2 Negative corpus

| Area | Required failures |
|---|---|
| Declarations | missing name/type/parameter delimiter/body; unsupported member kind |
| Types | missing type argument/comma/closer, malformed `?`, empty generic clause |
| Struct/enum | missing brace/comma/case name, malformed associated type, unsupported `indirect` |
| Expressions | missing operand/closer, invalid prefix/postfix, chained comparison, invalid assignment target |
| Calls | missing argument after label/marker/comma, missing close parenthesis |
| Collections | mixed array/dictionary entries, missing key/value/closer |
| If/while | missing condition or block, dangling `else` |
| Match | no arms, missing pattern/arrow/body, malformed payload pattern, missing close brace |
| Boundaries | two same-line block items, unexpected top-level token, unexpected EOF |
| Lexer handoff | `invalid` and `deferredKeyword` tokens consumed once without diagnostic cascades |

### 10.3 Invariants

For every input:

1. the parser terminates;
2. cursor reads are bounded by EOF;
3. each loop consumes, returns, or synchronizes;
4. all non-synthetic node spans are ordered and inside the source buffer;
5. parsing does not mutate or duplicate lexer tokens;
6. a successful parse consumes through EOF;
7. a recovered parse retains all later top-level declarations it can safely
   identify;
8. AST shape is independent of name resolution and type checking;
9. no user input reaches an assertion or old VM/runtime path.

A cross-implementation corpus should eventually live under
`tests/parser/{ok,err}/` with source plus normalized `.ast.txt` or `.diag.txt`
files. That corpus, rather than C++ class layout, becomes the durable Parser
contract for any future implementation rewrite.

---

## 11. Completion criteria

Parser MVP is complete when:

- [x] a clean build produces a parser-only test target;
- [x] 21 direct cases cover the core grammar/recovery matrix, deterministic
  dumps, diagnostic snapshots, and token deletion/insertion/replacement
  mutation;
- [x] positive and negative CLI acceptance tests run through
  `--lang=v0.1` without starting the legacy VM/runtime;
- [x] the Parser MVP acceptance source parses with no lexical or parser diagnostic;
- [x] expression ASTs encode precedence and associativity directly;
- [x] `enum`, minimal `match`, byte literals, contextual cases, `Result<T, E>`,
  optional types, `inout`, `consuming`, `&`, and `consume` all reach stable syntax nodes;
- [x] malformed input yields stable diagnostic IDs/spans and parsing continues at
  documented boundaries;
- [x] direct `ParserTest` cases enter no semantic pass, and no v0.1 test enters
  legacy `TypeGen`, `TypeBinding`, bytecode, VM, or runtime code;
- [x] the existing legacy parser remains isolated until its tests are intentionally
  migrated or archived.

Validate this milestone with:

```pwsh
ctest --test-dir build --output-on-failure -L parser
```

Type-directed completion of deferred name references, type checking, enum
layout, match exhaustiveness, ownership enforcement, and code generation are
explicit later gates. A source file parsing and resolving successfully does
not imply that it is fully typed or executable.
