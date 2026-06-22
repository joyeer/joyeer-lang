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

func_decl         ::= [ visibility ] [ method_effect ] 'func' identifier
                      '(' [ param , ... ] ')'
                      [ ':' type ]
                      [ effect_clause ]
                      block

method_effect     ::= 'borrowing' | 'mutating' | 'consuming'
param             ::= label [ identifier ] ':' [ access_effect ] type [ '=' expression ]
label             ::= identifier
access_effect     ::= 'borrowing' | 'inout' | 'consuming' | 'initializing'

effect_clause     ::= 'performs' effect_label , ...
effect_label      ::= identifier [ '<' type , ... '>' ]

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

typealias_decl    ::= 'typealias' identifier '=' type
import_decl       ::= 'import' import_path [ 'as' identifier ]
import_path       ::= identifier ( '.' identifier )*
visibility        ::= 'public' | 'internal' | 'private'

statement         ::= binding ';'?
                   |  expression ';'?
                   |  if_stmt | while_stmt | for_stmt
                   |  return_stmt | break_stmt | continue_stmt
                   |  block

if_stmt           ::= 'if' expression block ( 'else' if_stmt | 'else' block )?
while_stmt        ::= 'while' expression block
for_stmt          ::= 'for' [ '&' ] pattern 'in' expression block
return_stmt       ::= 'return' [ expression ]
break_stmt        ::= 'break'
continue_stmt     ::= 'continue'

expression        ::= prefix_expr ( binary_op prefix_expr )*
prefix_expr       ::= [ '!' | '-' | '~' ] postfix_expr
                   |  ( '&' | 'consume' ) postfix_expr   // ownership markers (§4.3);
                                                         //   prefix an lvalue / owned path only
                   |  'return' [ expression ]            // diverging expr, type Never (§2.9);
                                                         //   only valid as the RHS of '??' (§8.5)
postfix_expr      ::= primary_expr ( '.' identifier
                                    | '?.' identifier
                                    | '(' [ call_arg , ... ] ')'
                                    | '[' expression , ... ']'
                                    | '?'
                                    | '!' )*
call_arg          ::= label ':' [ '&' | 'consume' ] expression

primary_expr      ::= literal
                   |  identifier
                   |  '(' expression ( ',' expression )* ')'   // parens (1) or tuple (≥2)
                   |  array_literal
                   |  dict_literal
                   |  if_stmt                                   // if as expr
                   |  match_expr
                   |  'self' | 'Self'
                   |  type '.' identifier [ '(' ... ')' ]        // qualified ctor

match_expr        ::= 'match' expression '{' match_arm+ '}'
match_arm         ::= pattern ( ',' pattern )* [ 'where' expression ] '=>' ( expression | block ) ','?

pattern           ::= '_'
                   |  literal
                   |  identifier
                   |  '(' pattern , ... ')'
                   |  [ type ] '.' identifier [ '(' pattern , ... ')' ]
                   |  identifier 'as' type

binary_op         ::= '+' | '-' | '*' | '/' | '%'
                   |  '==' | '!=' | '<' | '<=' | '>' | '>='
                   |  '&&' | '||' | '??'
                   |  '&' | '|' | '^' | '<<' | '>>'
                   |  '..<' | '...'
                   |  assign_op
assign_op         ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='
                   |  '&=' | '|=' | '^=' | '<<=' | '>>='

attribute         ::= '@' identifier [ '(' attribute_args ')' ]
attribute_args    ::= string_literal | expression , ...
```

> **Note on `??`.** The right operand of the coalescing operator `??` may be
> an ordinary expression *or* a diverging expression (`return expr` /
> `fatalError(...)`), which has the bottom type `Never` and satisfies any
> result type. This is what makes `let v = parse(x) ?? return .Err(e)` and
> `let v = opt ?? fatalError(...)` well-typed (§8.5, §9.3).

---

