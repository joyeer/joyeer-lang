## §12 Modules & Imports

**Implementation status:** directory modules, explicit dependency mappings,
qualified file-local imports, and visibility are implemented. Import aliases,
wildcards, re-exports, package fetching, and separate binary modules are not.

### 12.1 Module unit

A **module** is a directory of directly contained `.joyeer` source files
compiled together. Subdirectories do not contribute source files. Files are
discovered in deterministic path order, but declaration visibility does not
depend on that order: all module declarations are collected before signatures
and bodies are resolved.

The compiler accepts `--module-root <directory>` and repeatable
`--module <logical.dotted.name>=<directory>` dependency mappings. These options
are mutually exclusive with a positional single-file input. The original
single-file invocation remains supported as a module with one source file and
no dependency mappings.

Directories are canonicalized. Duplicate logical names, directory identities,
and source-file identities are errors, including aliases through symlinks or
hard links. Directory enumeration and file reads must report failures rather
than silently omitting inputs. Module resolution uses explicit mappings only:
the compiler does not fetch packages or interpret manifests.

Imports determine the dependency graph before declaration resolution. Unknown
imports and dependency cycles are errors; a cycle diagnostic identifies the
cycle. Recursive functions within an acyclic module graph remain legal.
Reachable modules may be compiled together, but source texts are never
concatenated: every syntax/semantic/IR location retains its source-file identity
and its file-local byte span.

### 12.2 Visibility

The three visibility levels are `public`, `internal` (default), and
`private` (file-local).

| Visibility | Reachable from |
|-----------|---------------|
| `public` | Any module that imports this module. |
| `internal` (default) | Same module. |
| `private` | Same file only. |

Top-level declarations and stored fields accept visibility modifiers. Enum
cases inherit the visibility of their enum. Duplicate non-private top-level
declarations in a module are rejected, including collisions between type and
value names. A private declaration may shadow a declaration from another file
only in its own file. Two declarations with the same name in one file are
always an error, regardless of visibility.

An exposed type must be at least as accessible as the declaration exposing it.
This includes function parameters/results, public stored fields, enum payloads,
and types nested inside containers or optionals. A member's effective
accessibility is limited by its enclosing type. Inferred member lookup must
enforce the same visibility rules as lookup from an explicit type annotation.

A synthesized struct initializer is limited by the least visible stored
field and by the struct's own visibility. In particular, a private field
cannot become publicly writable through memberwise construction.

Only the root module contributes an executable entry point: exactly one root
function named `main` is required for native executable output, using one of
the signatures in [declarations](03-declarations.md#326-executable-entry-point).
Dependency functions named `main` are ordinary functions and are not subject
to entry-point signature restrictions. Multiple private root `main` functions
are not a way to declare multiple entries.

### 12.3 Imports

```joyeer
import project.config

func main() {
    project.config.describe()
}
```

Imports appear before all declarations and apply only to the importing file.
An import makes a module's public names available through its complete logical
name, not as unqualified names. The example requires a mapping for
`project.config` and a public function `describe` in that module. Qualified
types and enum cases use the same prefix, for example
`project.config.Record` and `project.config.Mode.Release`.

An import's first name component must not collide with a top-level declaration
in its importing file. A declaration in another file does not create that
collision. Aliases (`as`), wildcards, and re-exports are not accepted.

### 12.4 Prelude

The draft standard-library prelude includes:

- `Bool`, `Int`, `UInt`, `Float`, `Double`, `Char`, `String`, `Void`, `Never`
- `Optional`, `Result`, `IOError`
- `Array`, `Dict`, `Tuple`
- `print`, `readFile`, `assert`, `precondition`, `fatalError`

This is a long-term design vision, not the current compiler prelude inventory.
For example, the compiler does not supply contract APIs, `Float`/`Double`,
`Char`, or general tuple support, while it does provide portable filesystem
and process types (§18). See the
[implemented name-resolution prelude](../impl/name-resolution.md#3-scopes-and-symbols)
for the authoritative current list.

The names `Display`, `Equatable`, `Comparable` are reserved for a future
protocol system (§15) but are **not** usable as constraints or conformances
in v0.1.

---
