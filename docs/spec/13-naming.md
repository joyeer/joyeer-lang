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

