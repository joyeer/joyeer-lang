## Appendix C: Grammar (EBNF)

Compact reference. Whitespace and comments are skipped between tokens.
This is the broader specification grammar, not a list of currently accepted
parser features. The C annotation/bodyless import syntax is implemented for
the restricted subset in [implementation coverage](../docs/impl/c-interop.md).
The `@spec`/`@property` shorthand and quantifier proposals in §11 are not
included in this grammar.

```
file              ::= import_decl* top_decl*

top_decl          ::= func_decl
                   |  struct_decl
                   |  enum_decl
                   |  protocol_decl
                   |  extension_decl
                   |  binding

binding           ::= ( 'let' | 'var' ) pattern [ ':' type ] [ '=' expression ]

func_decl         ::= attribute* [ visibility ] [ method_effect ] 'func' identifier
                      [ generic_params ]
                      '(' [ param , ... ] ')'
                      [ ':' type ]
                      [ block ]

generic_params    ::= '<' generic_param , ... '>'          // functions, structs and enums (§20.4, §20.12)
generic_param     ::= identifier [ ':' constraint ]
constraint        ::= protocol_ref ( '&' protocol_ref )*
protocol_ref      ::= nominal_type

method_effect     ::= 'borrowing' | 'mutating' | 'consuming'
param             ::= label [ identifier ] ':' [ access_effect ] type [ '=' expression ]
label             ::= identifier
access_effect     ::= 'borrowing' | 'inout' | 'consuming' | 'initializing'

type              ::= primary_type [ '?' ]
primary_type      ::= '(' ')'                                  // Void (§2.1.2)
                   |  nominal_type [ '<' type , ... '>' ]
                   |  '[' type ']'
                   |  '[' type ':' type ']'
                   |  function_type
                   |  '(' function_type ')'
function_type     ::= '(' [ param_type , ... ] ')' ':' type      // result extends right (§2.3.1)
param_type        ::= [ access_effect ] type             // no label, not Never
nominal_type      ::= identifier ( '.' identifier )*         // primitive types included

struct_decl       ::= [ visibility ] 'struct' identifier [ generic_params ]     // §20.12
                      '{' struct_member* '}'

struct_member     ::= binding
                   |  init_decl | deinit_decl
                   |  func_decl
                   |  subscript_decl

init_decl         ::= [ visibility ] 'init' '(' [ param , ... ] ')'
                      block

deinit_decl       ::= 'deinit' '(' ')' block

enum_decl         ::= [ visibility ] 'enum' identifier [ generic_params ]       // §20.12
                      '{' enum_case ( ',' enum_case )* ','? ( func_decl | subscript_decl )* '}'

enum_case         ::= [ 'indirect' ] identifier [ '(' assoc_type , ... ')' ]
assoc_type        ::= [ identifier ':' ] type

extension_decl    ::= 'extension' extended_type [ ':' protocol_ref , ... ]
                      '{' ( func_decl | subscript_decl )* '}'
extended_type     ::= nominal_type [ '<' type , ... '>' ]     // a generic type's parameters (§20.12.2)
                   |  '[' type ']'
                   |  '[' type ':' type ']'

protocol_decl     ::= [ visibility ] 'protocol' identifier
                      '{' protocol_requirement* '}'
protocol_requirement
                  ::= [ method_effect ] 'func' identifier
                      '(' [ requirement_param , ... ] ')' [ ':' type ]
requirement_param ::= label [ identifier ] ':' [ access_effect ] type

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
intrinsic_call    ::= INTRINSIC_ID '<' type , ... '>' '(' [ call_arg , ... ] ')'
                                 // toolchain-authorized units only (§20.12.5); a `>>` token
                                 //   inside the type arguments is split
call_arg          ::= [ label ':' ] [ '&' | 'consume' ] expression
                                 // unlabeled only through a function value
                                 //   or for an enum payload (§5.6, §5.6.1, §5.8)

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
attribute_arg     ::= [ identifier ':' ] expression
```

Each annotation fixes its own argument shape: `@c_extern` and `@c_export`
take one `symbol:` string literal ([§19](19-c-interop.md)); `@platform` takes
one or more unlabelled platform names ([§11.5](11-annotations.md#115-platform)),
the one place an annotation argument is unlabelled. An argument of any other
shape is rejected by the annotation that owns it.

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

> **Nesting limit.** An implementation bounds how deeply the syntax may
> nest, because later stages recurse over the syntax tree. Parentheses,
> types, blocks, patterns, prefix operators, and each operand added to a
> chain (`a + b + c`, `a.b.c`, `f(x)(y)`, `x as T as U`) count one level
> each. The reference compiler's limit is 100 levels; a deeper program is
> rejected with the single diagnostic `parser.nesting-too-deep`.
