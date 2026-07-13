## §13 Naming Conventions (style guide)

These are not syntactic rules but the compiler issues stylistic warnings
when they are violated. AI code generators must follow them.

| Construct | Convention | Example |
|-----------|-----------|---------|
| Types (struct, enum, typealias) | PascalCase | `JsonValue`, `IntList` |
| Functions, methods | camelCase | `parseInt`, `appendByte` |
| Variables, fields, parameters | camelCase | `firstName`, `count` |
| Constants (let bindings) | camelCase | `let maxRetries = 5` |
| Enum cases | PascalCase | `.Ok`, `.NotFound` |
| Generic type parameters | single capital or PascalCase | `T`, `K`, `Element` |
| Modules / files | lowercase | `std.io`, `json_parser.joyeer` |

---

### 13.1 Name lookup and lexical scope

Names are resolved to declarations, not compared as strings after parsing.
Every declaration has one identity even when an inner declaration uses the
same spelling.

The following lexical scopes exist in v0.1:

- a source-file scope for top-level bindings, functions, structs, and enums;
- a function scope containing its parameters;
- a nested scope for every block;
- a member scope for every struct or enum;
- a separate scope for every `match` arm and its pattern bindings.

Lookup begins in the innermost applicable scope and walks outward. A
declaration may shadow a declaration in an outer lexical scope. Declaring the
same name twice in one scope is an error.

All top-level declarations are visible throughout their source file, including
from declarations that occur textually earlier. This permits recursive and
mutually recursive functions and forward references to struct/enum types.
Local `let` / `var` bindings are visible only after their initializer; an
initializer therefore cannot refer to the binding it is declaring. Function
parameters are visible throughout the function body. Pattern bindings are
visible only in the body of their own `match` arm.

Type syntax searches the type namespace. Expression syntax searches the value
namespace first and may also name a type when it is used as a constructor or
qualified member base. A type and value may not introduce the same spelling in
one scope because forms such as `Foo(...)` and `Foo.Bar` would otherwise be
ambiguous. Struct fields and enum cases live in the member scope of their
declaring type and are selected through that type or through type-directed
contextual lookup.

---

