## §12 Modules & Imports

**Implementation status:** named explicit compilation units, dependency source sets,
qualified file-local imports, and visibility are implemented. Import aliases,
wildcards, re-exports, package fetching, and separate binary modules are not.

### 12.1 Module unit

A **module** is a named, explicit, nonempty set of source files compiled
together as one **compilation unit**. Its name is a nonempty dot-qualified
logical identity. The complete name identifies the module: `acme.config`
does not create or imply a parent module `acme`, a submodule relationship,
or a directory hierarchy.

Folders do not create modules or namespaces. One module's files may span
nested or unrelated directories; distinct modules may select different files
from the same directory. Module membership comes only from the supplied
source-file set, not from a file's location or an import's spelling.
Declaration visibility does not depend on source-file order: all module
declarations are collected before signatures and bodies are resolved.

Source selection is tool policy, not source-language syntax. A build or
package tool may choose source roots and include/exclude rules, but it must
supply the resulting file paths and module names to the compiler. The
compiler does not discover source files, recursively or otherwise, add sibling
files, read manifests, or fetch dependencies. A package or build target is not
inherently one module: a project may begin with one main compilation unit and
later use separate units for dependencies or test targets.

The legacy single-file invocation remains supported as a one-file root unit
without an explicit name or dependency mappings. Named input validation,
path identity, and migration from the removed directory flags are documented
in the [compiler interface](../impl/backend.md#2-cli), separately from these
source-language rules.

Imports determine the dependency graph before declaration resolution. Unknown
imports and dependency cycles are errors; a cycle diagnostic identifies the
cycle. Recursive functions within an acyclic module graph remain legal.
Reachable modules may share whole-graph code generation, but this does not
merge their logical identities or visibility boundaries. Source texts are
never concatenated: every syntax/semantic/IR location retains its source-file
identity and its file-local byte span and debug scope.

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
name, not as unqualified names. The example requires an explicit source set for
`project.config` and a public function `describe` in that module. Qualified
types and enum cases use the same prefix, for example
`project.config.Record` and `project.config.Mode.Release`.
Importing `project.config` does not import `project` or any other name sharing
its prefix; each dependency is identified by its exact complete name.

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
