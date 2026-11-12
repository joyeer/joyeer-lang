## §12 Modules & Imports

**Implementation status:** named explicit compilation units, dependency source sets,
file-local imports with public short names and qualified lookup, and visibility are implemented. Import aliases,
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
in the [compiler interface](../docs/impl/backend.md#2-cli), separately from these
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

Joyeer visibility is separate from
[C binding authorization and linkage](19-c-interop.md#193-source-calls-and-native-symbols).
`public` does not export a C symbol or grant a runtime compilation context,
and `@c_export` does not make a source declaration public. Runtime permission
is a separate explicit compilation input, not a privilege inherited through
imports; see [the implemented subset](../docs/impl/c-interop.md).

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
    describe()
}
```

Imports appear before all declarations and apply only to the importing file.
An import makes that module's public declarations available by short name
and through its complete logical name. The example requires an explicit source
set for `project.config` and a public function `describe` in that module.
Imported public types, structure constructors, and functions can be written
as `Record`, `Record(...)`, and `describe()`. Enum cases and patterns can use
`Mode.Release`; cases themselves are not imported as bare `Release` names.
Fully qualified forms such as `project.config.Record`,
`project.config.describe()`, and `project.config.Mode.Release` remain valid.
Importing `project.config` does not import `project` or any other name sharing
its prefix; each dependency is identified by its exact complete name.

Short-name lookup first searches existing lexical scopes, the current module,
and the compiler prelude. An existing declaration in the relevant namespace
therefore shadows imported declarations. Only when that search has no match
does lookup consider public declarations from the current file's imports.
Type syntax considers only types; expression lookup considers both values
and types, including constructors.

If multiple imported modules provide a matching public declaration, using its
short name is an ambiguity error. There is no first-import-wins rule or
argument-based overload selection between modules. Use the full module name
to disambiguate. An unused collision is not an error; importing the same
module again does not add another candidate. Internal and private declarations
do not participate in imported short-name lookup.

Imports do not re-export their dependency's declarations or make short names
visible in another file of the same module. Each importing file declares its
own imports. Symbol identity, visibility, and source locations are unchanged
by using a short name.

An import's first name component must not collide with a top-level declaration
in its importing file. A declaration in another file does not create that
collision. Aliases (`as`), wildcards, and re-exports are not accepted.

### 12.4 Prelude

The draft standard-library prelude includes:

- `Bool`, `Int`, `UInt`, `Float`, `Double`, `Char`, `String`, `Void`, `Never`
- `Optional`, `Result`, `IOError`
- the host error and status types of §18: `FileSystemError`, `FileKind`,
  `ProcessStatus`, `ProcessError`, `StderrError`, `StdinError`
- `Array`, `Dict`, `Tuple`
- `print`, `assert`, `precondition`, `fatalError`

The prelude holds the language's own vocabulary: types and the few functions
every program needs without an import. Operations on the host are **not**
prelude functions; they are ordinary functions of the `System` standard module
(§12.5). The host error and status types stay in the prelude so that user code,
`System` and any other module share one definition of each.

This is a long-term design vision, not the current compiler prelude inventory.
For example, the compiler does not yet supply the contract APIs or general
tuple support. See the
[implemented name-resolution prelude](../docs/impl/name-resolution.md#3-scopes-and-symbols)
for the authoritative current list.

The names `Display`, `Equatable`, `Comparable` are reserved for a future
protocol system (Appendix A) but are **not** usable as constraints or conformances
in v0.1.

### 12.5 Standard modules

A **standard module** is a module that ships with the toolchain and is written
in Joyeer. A program uses one with an ordinary `import`; no manifest entry is
required. The toolchain finds standard modules relative to the compiler itself,
never through a search path, and a module supplied explicitly with the same
name takes precedence (which is how the library itself is developed). The
resolution mechanism is an implementation contract, described in
[toolchain libraries](../docs/plan/0006-toolchain-libraries.md).

| Module | Provides |
|---|---|
| `Foundation` | Text helpers, UTF-8 validation, decimal formatting, checksums. No host access |
| `System` | The host operations of §18 (filesystem, processes, standard error and input, legacy `readFile`), lexical path helpers, and message formatting for the host error types |
| `Compression` | ZIP and JAR reading; optional, and backed by a native library |

Rules:

- Standard modules follow every rule of this chapter. They gain no privilege
  from their name: a standard module that binds native code is authorized by the
  toolchain (§19.1), and importing it grants the importer nothing.
- Prelude lookup precedes imported short names (§12.3), so a standard module
  cannot redefine a prelude name, and a prelude name cannot be moved into a
  module without a separate migration.
- Each standard module states its dependencies; the graph is acyclic and
  `Foundation` depends on no other standard module.
- The set of modules and their APIs is not frozen in v0.1. Adding a module is
  a library decision; changing a §18 contract is a specification change.

---
