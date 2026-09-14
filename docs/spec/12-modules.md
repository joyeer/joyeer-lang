## §12 Modules & Imports

**Implementation status:** this chapter describes the broader module design.
The current compiler accepts one source file per invocation and has no
source-language import/visibility implementation. Building the compiler with
CMake is separate from designing a multi-file Joyeer module system.

### 12.1 Module unit

A **module** is a directory of `.joyeer` source files compiled together.
The executable v0.1 milestone is single-file. Multi-file module discovery,
dependency tracking, visibility enforcement, and linking remain follow-on work.

### 12.2 Visibility 📌

> **📌 Decision.** *Three visibility levels: `public`, `internal`
> (default), `private` (file-local).*

| Visibility | Reachable from |
|-----------|---------------|
| `public` | Any module that imports this module. |
| `internal` (default) | Same module. |
| `private` | Same file only. |

### 12.3 Imports

```joyeer
import std.io
import std.collections.Dict as Map
```

In this draft, `as` denotes import renaming and `import` appears only at the
top of a file, before declarations. This syntax is not implemented by the
current JSON-parser MVP.

### 12.4 Prelude

The draft standard-library prelude includes:

- `Bool`, `Int`, `UInt`, `Float`, `Double`, `Char`, `String`, `Void`, `Never`
- `Optional`, `Result`, `IOError`
- `Array`, `Dict`, `Tuple`
- `print`, `readFile`, `assert`, `precondition`, `fatalError`

This is not the current compiler prelude inventory. For example, the MVP does
not supply the contract APIs, `Float`/`Double`, `Char`, or general tuple support.
See the [implemented name-resolution prelude](../impl/name-resolution.md#3-scopes-and-symbols)
for the current list.

The names `Display`, `Equatable`, `Comparable` are reserved for a future
protocol system (§15) but are **not** usable as constraints or conformances
in v0.1.

---
