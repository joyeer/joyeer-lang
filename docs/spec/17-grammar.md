## §17 Grammar Appendix (EBNF)

Compact reference. Whitespace and comments are skipped between tokens.
This is the broader specification grammar, not a list of currently accepted
parser features. The C annotation/bodyless import syntax is implemented for
the restricted subset in [implementation coverage](../impl/c-interop.md).
The `@spec`/`@property` shorthand and quantifier proposals in §11 are not
included in this grammar.

```
file              ::= import_decl* top_decl*

top_decl          ::= func_decl
                   |  struct_decl
                   |  enum_decl
                   |  extension_decl
                   |  binding

binding           ::= ( 'let' | 'var' ) pattern [ ':' type ] [ '=' expression ]

func_decl         ::= attribute* [ visibility ] [ method_effect ] 'func' identifier
                      '(' [ param , ... ] ')'
                      [ ':' type ]
                      [ block ]

method_effect     ::= 'borrowing' | 'mutating' | 'consuming'
param             ::= label [ identifier ] ':' [ access_effect ] type [ '=' expression ]
label             ::= identifier
access_effect     ::= 'borrowing' | 'inout' | 'consuming' | 'initializing'

struct_decl       ::= [ visibility ] 'struct' identifier
                      '{' struct_member* '}'

struct_member     ::= binding
                   |  init_decl | deinit_decl
                   |  func_decl
                   |  subscript_decl

init_decl         ::= [ visibility ] 'init' '(' [ param , ... ] ')'
                      block

deinit_decl       ::= 'deinit' '(' ')' block

enum_decl         ::= [ visibility ] 'enum' identifier
                      '{' enum_case ( ',' enum_case )* ','? ( func_decl | subscript_decl )* '}'

enum_case         ::= [ 'indirect' ] identifier [ '(' assoc_type , ... ')' ]
assoc_type        ::= [ identifier ':' ] type

extension_decl    ::= 'extension' type
                      '{' ( func_decl | subscript_decl )* '}'

subscript_decl    ::= [ visibility ] 'subscript' [ identifier ]
                      '(' [ param , ... ] ')' ':' type
                      '{' accessor+ '}'

accessor          ::= 'borrowing'   block_with_yield
                   |  'inout'        block_with_yield
                   |  'initializing' block_with_yield
                   |  'consuming'    block

block_with_yield  ::= '{' statement* 'yield' [ '&' ] expression statement* '}'

import_decl       ::= 'import' import_path [ 'as' identifier ]
import_path       ::= identifier ( '.' identifier )*
visibility        ::= 'public' | 'internal' | 'private'

block             ::= '{' statement* '}'
statement         ::= binding ';'?
                   |  expression ';'?
                   |  if_stmt | while_stmt | for_stmt
                   |  return_stmt
                   |  break_stmt | continue_stmt
                   |  block

if_stmt           ::= 'if' expression block ( 'else' if_stmt | 'else' block )?
while_stmt        ::= 'while' expression block
for_stmt          ::= 'for' [ '&' ] pattern 'in' expression block
return_stmt       ::= return_expr
break_stmt        ::= 'break'
continue_stmt     ::= 'continue'

expression        ::= return_expr
                   |  prefix_expr ( binary_op prefix_expr )*
return_expr       ::= 'return' [ expression ]             // diverging expr, type Never (§2.9)
prefix_expr       ::= ( '!' | '-' | '~' ) prefix_expr
                   |  ( '&' | 'consume' ) postfix_expr   // ownership markers (§4.3);
                                                         //   prefix an lvalue / owned path only
                   |  postfix_expr
postfix_expr      ::= primary_expr ( '.' identifier
                                    | '?.' identifier
                                    | '(' [ call_arg , ... ] ')'
                                    | '[' expression , ... ']'
                                    | '?'
                                    | '!' )*
call_arg          ::= label ':' [ '&' | 'consume' ] expression

primary_expr      ::= literal
                   |  '(' ')'                                  // unit value, type Void
                   |  identifier
                   |  '(' expression ( ',' expression )* ')'   // parens (1) or tuple (≥2)
                   |  array_literal
                   |  dict_literal
                   |  if_stmt                                   // if as expr
                   |  match_expr
                   |  enum_ctor_expr
                   |  'self' | 'Self'

enum_ctor_expr    ::= [ type ] '.' identifier [ enum_payload_clause ]
enum_payload_clause
                  ::= '(' enum_arg , ... ')'
enum_arg          ::= [ label ':' ] expression

match_expr        ::= 'match' expression '{' match_arm+ '}'
match_arm         ::= pattern ( ',' pattern )* [ 'where' expression ] '=>' ( expression | block ) ','?

pattern           ::= '_'
                   |  '(' ')'                                  // unit pattern
                   |  literal
                   |  identifier
                   |  '(' pattern , ... ')'
                   |  enum_case_pattern
enum_case_pattern ::= [ type ] '.' identifier [ '(' enum_pattern_arg , ... ')' ]
enum_pattern_arg  ::= [ label ':' ] pattern

binary_op         ::= '+' | '-' | '*' | '/' | '%'
                   |  '==' | '!=' | '<' | '<=' | '>' | '>='
                   |  '&&' | '||' | '??'
                   |  '&' | '|' | '^' | '<<' | '>>'
                   |  '..<' | '...'
                   |  assign_op
assign_op         ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='
                   |  '&=' | '|=' | '^=' | '<<=' | '>>='

attribute         ::= '@' identifier '(' [ attribute_arg , ... ] ')'
attribute_arg     ::= identifier ':' expression
```

**C declaration constraints (§19).** A missing function body is valid only
for `@c_extern`, which must not have a body. Every other function, including
`@c_export`, requires one. Both C annotations require an explicit result
type and exactly one `symbol:` string-literal argument. Placement,
authorization, allowed parameter effects, defaults, and scalar ABI types
are checked by [§19](19-c-interop.md); the optional grammar components do
not relax those rules. Parameter labels and optional internal names have
the same production as [§3.2](03-declarations.md#32-function-declarations).

> **Note on `??`.** The right operand of the coalescing operator `??` may be
> an ordinary expression *or* a diverging expression (`return expr` /
> `fatalError(...)`), which has the bottom type `Never` and satisfies any
> result type. This is what makes `let v = parse(x) ?? return .Err(e)` and
> `let v = opt ?? fatalError(...)` well-typed (§8.5, §9.3).

---
