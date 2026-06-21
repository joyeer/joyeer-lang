## §12 Modules & Imports

### 12.1 Module unit

A **module** is a directory of `.joyeer` source files compiled together.
v0.1 supports single-file modules only; multi-file modules will be
supported once the build system lands (v0.2).

### 12.2 Visibility 📌

> **📌 Decision D10.** *Three visibility levels: `public`, `internal`
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

`as` for renaming is supported. `import` may only appear at the top of a
file, before any declaration.

### 12.4 Prelude

The standard library implicitly imports the following into every file:

- `Bool`, `Int`, `UInt`, `Float`, `Double`, `Char`, `String`, `Void`, `Never`
- `Optional`, `Result`
- `Array`, `Dict`, `Tuple`
- `print`, `assert`, `precondition`, `fatalError`

The names `Display`, `Equatable`, `Comparable` are reserved for a future
protocol system (§15) but are **not** usable as constraints or conformances
in v0.1.

---

