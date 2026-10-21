# Library Modules and joypm Management Plan

> **Status:** rollout step 1 is implemented: a small `Foundation` source
> module (`src/library/Foundation/`: `substring`, `decimalString`,
> `isValidUtf8`), the `FoundationTests` executable test module, and the
> repository-root `workspace.toml`. Step 2 has started: `System` provides
> lexical path helpers (`pathDirectory`, `trimDirectoryPath`,
> `isPathSeparator`; the host is passed as an explicit `windows` flag because
> no host-detection builtin exists) and message formatters for the host
> error/status types, with `SystemTests`. Filesystem/process wrappers, host
> detection, `Collections`, `Testing`,
> joypm adoption, and installed-library resolution remain pending. Names are
> still not compiler-reserved and no new language syntax is introduced.

## 1. Public Module Direction

Use a small set of named capability modules instead of a `std.text`-style
public hierarchy. The proposed source directory is `src/library/`, not
`src/stdlib/`.

| Module | Responsibility | Boundary |
|---|---|---|
| `Foundation` | Text processing, encoding validation, numeric parsing/formatting, and basic conversions | No dependency on `System`; not a catch-all for platform APIs |
| `System` | Paths, filesystem, processes, environment, and time | May depend on `Foundation` when genuinely needed; preserve existing host contracts |
| `Collections` | Additional containers and collection algorithms | Keep independent where possible; not required to use builtin arrays or dictionaries |
| `Testing` | Assertions, failure reporting, and shared test helpers | May use `Foundation` for diagnostics; test executables declare dependencies on the modules under test |

Future source use:

```joyeer
import Foundation
import System
```

`String`, `Array`, `Dict`, `Optional`, `Result`, and other existing prelude
types remain usable without these imports. String storage and lifecycle
mechanisms remain runtime responsibilities; public text algorithms belong
in `Foundation`.

Each capability module is a real explicit source compilation unit, not an
umbrella that re-exports hidden submodules. Current imports already expose
public declarations by short name and full module qualification; aliases,
wildcards, and re-exports are not needed for this initial organization.
See the [module rules](../spec/12-modules.md).

Keep the dependency graph acyclic. Neither `Foundation` nor the runtime may
depend on `System`, `Testing`, or joypm. `Collections` must not become an
unnecessary dependency of basic text operations.

## 2. Project Layout and Runtime Separation

Keep the current project skeleton and add the public library alongside the
compiler, backend, runtime, and tools. The target layout below includes
existing responsibility groups and proposed additions; it is not an
instruction to move all files at once:

```text
src/
  compiler/                 Existing C++ frontend, IR, diagnostics, and driver
  backend/                  Existing embedded LLVM/LLD backend
  runtime/
    abi/                    Planned private runtime/native interfaces
    native/                 Planned C substrate and platform implementations
    core/                   Planned Joyeer runtime algorithms
  library/
    Foundation/
      project.toml
      text.joyeer
    System/
      project.toml
    Collections/
      project.toml
    Testing/
      project.toml
  tools/
    joypm/                  Existing Joyeer project manager

tests/
  unittests/                Existing compiler/runtime unit tests
  native/                   Existing native behavior/ownership fixtures
  joypm/                    Existing project-manager acceptance tests
  library/                  Planned public-library test modules
    Foundation/
      project.toml
      main.joyeer

docs/                       Existing specification, implementation, and plans
scripts/                    Existing build/install/validation helpers
workspace.toml              Planned library/test workspace
```

Every implemented module needs a nonempty explicit source list. Add the
remaining source files and workspace members only as their APIs land; do
not check in manifest-only or empty modules. Preserve the other existing
stage-specific test directories.

Keep the current compiler/backend/tools structure. The
[hybrid runtime plan](hybrid-runtime.md) separately proposes `abi/`,
`native/`, and `core/` responsibilities inside
[src/runtime](../../src/runtime/). These are internal implementation layers,
not public library modules or automatically separate libraries. Add the
Joyeer runtime implementation layer first; defer moving existing C files
until actual operation migration justifies the change.

The runtime directories are responsibility groups inside one static archive,
not separate public modules or selectable runtime profiles. Conversely, each
public library directory represents one proposed source module with its own
manifest; subdirectories within it may organize files without creating more
modules. Generated objects, archives, and staging products belong in build
directories, not the library source tree.

Runtime-only storage/native bindings must not become available merely
because a source belongs to `Foundation` or lives under `src/library/`.
The runtime does not import the public library to implement itself.

The [C interoperation specification](../spec/19-c-interop.md) defines
`@c_extern`/`@c_export` with implied C ABI for that restricted runtime
context; its [implementation plan](c-interop.md) tracks pending gates.
The complete by-value scalar boundary is [implemented](../impl/c-interop.md), but
it is not a prerequisite for ordinary library modules or wrappers around
existing host builtins.
Public APIs wrap internal bindings rather than exposing native privileges.
No extra ABI selection or native dependency fields are added to library
manifests by this plan.

## 3. Build and Management Ownership

**Use joypm for public library source-module workflows; keep native toolchain
construction and runtime bootstrap under CMake.**

| Work | Owner |
|---|---|
| Library source lists, logical names, and module dependencies | One `project.toml` per module |
| Explicit library/test membership and dependency graph resolution | joypm workspace |
| Checking library sources and building/running library test programs | joypm |
| C++ compiler and embedded native backend | CMake |
| C substrate and future Joyeer runtime objects in one static runtime archive | CMake invoking the implemented internal runtime-object path; production operation migration remains planned |
| First joypm executable | Existing direct-compiler CMake bootstrap |
| Installed compiler/runtime/library source layout and version matching | Toolchain packaging, with future joypm SDK resolution |

Current [joypm](../impl/joypm.md) supports `source`, `bin`, and `test`
modules. A `source` module is passed to the compiler as dependency source:
it is not a precompiled `.a`/`.lib`, has no `output`, and does not establish
a stable binary-library ABI. Checking it does not require an executable
entry point. Tests are separate executable modules with valid entries.

Do not require joypm to build the first compiler, the runtime it runs on,
or its own first executable. The
[current bootstrap](../../src/tools/joypm/CMakeLists.txt) invokes the newly
built compiler directly. Later adoption of library APIs by joypm must update
bootstrap dependency source mappings and staged self-build manifests
together, without introducing a dependency on a previously installed tool.

Library development can begin on the current C runtime; it does not need
to wait for the hybrid runtime's first milestone.

The planned integration order is:

```text
CMake: compiler/backend
  -> CMake + compiler: Joyeer runtime objects and C substrate -> one archive
  -> CMake: first joypm executable
  -> joypm: library source checks and executable library tests
  -> toolchain packaging: validated products and, after the SDK gate, library sources
```

Compiler construction and runtime product staging are already separated.
The hybrid runtime plan owns mixed-archive assembly and operation migration.
Public library source checks can use the current
C runtime before those changes land. The root joypm workspace describes
Joyeer modules and tests; it does not replace CMake for C/C++ construction
or make the compiler and runtime public joypm source modules.

## 4. Proposed Local Workspace

Use one explicit repository-root workspace for the initial library and its
tests. Current member/source paths cannot be absolute or parent-traversing;
a workspace inside `src/library/` cannot reach repository-level tests through
`../../tests`. A root workspace can contain both without changing that rule.
Directory nesting does not imply workspace membership or logical submodules.

The following examples use the current schema, but the paths/files are
proposed and are not created by this plan.

`src/library/Foundation/project.toml`:

```toml
schema-version = 1

[module]
name = "Foundation"
kind = "source"
sources = ["text.joyeer"]
dependencies = []
```

`tests/library/Foundation/project.toml`:

```toml
schema-version = 1

[module]
name = "FoundationTests"
kind = "test"
output = "foundation-tests"
sources = ["main.joyeer"]
dependencies = ["Foundation"]
```

Repository-root `workspace.toml`:

```toml
schema-version = 1

[workspace]
members = [
    "src/library/Foundation",
    "tests/library/Foundation",
]
```

Once those modules exist, the intended workflow is below. Replace
`<compiler-path>` with the selected compiler's explicit path:

```text
joypm check --compiler <compiler-path> --workspace workspace.toml --module Foundation
joypm test --compiler <compiler-path> --workspace workspace.toml --module FoundationTests
joypm test --compiler <compiler-path> --workspace workspace.toml --release
```

Add `System`, `Collections`, `Testing`, and their test modules explicitly as
they are implemented. Dependencies resolve by exact module name; importing
a module does not fetch it or add it to a workspace. Declare actual
dependencies rather than relying on workspace membership or incidental
transitive availability.

`joypm build` selects bin modules, not source libraries. A source-only build
with no output, or a successful `test` reporting `0 tests`, is not library
acceptance. Require real test executables and verify failing tests propagate
a nonzero status. Add shared `Testing` dependencies only when that module
exists and is actually used.

## 5. Installed Libraries Are a Separate Gate

Current joypm resolves dependencies inside an explicitly loaded workspace.
It does not automatically discover toolchain library sources, traverse nested
workspaces, fetch packages, or merge an installed SDK into a user's module
graph. Naming a module `Foundation` does not make an external project's
`import Foundation` work automatically.

Before offering that installed experience:

- Define a toolchain-matched source-module catalog and a relocatable installed
  library layout. Missing sources or compiler/runtime/catalog mismatches must
  produce explicit diagnostics, not silently select unrelated copies.
- Extend joypm resolution to combine declared project dependencies with
  explicitly selected bundled modules and pass complete source mappings to
  the compiler. Do not weaken user workspace path validation as a shortcut.
- Decide official-name reservation and workspace/SDK collision behavior.
  Reject ambiguity or conflicting definitions rather than relying on
  first-found search order. The chosen names are not reserved today.
- Keep compiler source inputs explicit. Do not make the compiler discover
  library directories or read project manifests.
- Preserve existing builtin compatibility. Current prelude lookup precedes
  imported short names, so importing `System` alone must not be claimed to
  redirect an existing builtin call. Use explicit qualification for distinct
  module APIs and separately specify any future builtin migration.

Keep this work distinct from registry packages, dependency fetching, and
binary-library ABI support. Current source/workspace support is sufficient
for local library development, not for automatic installed SDK resolution.

## 6. Rollout and Acceptance

1. **Local Foundation (done; run
   `joypm test --compiler <compiler-path> --workspace workspace.toml`):** add a small nonempty module and an executable test
   module using current capabilities. Extract appropriate algorithms from
   [joypm helpers](../../src/tools/joypm/common.joyeer) only after defining
   public input, error, and complexity contracts.
2. **Capability modules:** add `System` wrappers without changing current
   host encoding/error/ownership semantics; grow `Collections` and `Testing`
   only for concrete consumers. Use ordinary public functions where methods
   or protocols are not implemented.
3. **Bundled distribution:** complete the SDK-resolution gate above and verify
   an external project after toolchain installation and relocation.
4. **joypm adoption:** replace duplicated tool helpers incrementally while
   keeping direct bootstrap, source-module builds, and self-build behavior
   consistent. This can start with explicit local source mappings before
   automatic SDK resolution exists.

For each implemented step, check source/dependency validation, module
visibility, exact case-sensitive identities, ambiguity, prelude collisions,
and rejection of duplicate names/cycles. Run real library tests in Debug and
Release and the affected native/joypm regression gates. Follow the existing
[build acceptance rules](../building.md); record measured implementation
evidence rather than treating these examples as validation results.

Only documentation is introduced here. Do not report planned modules,
manifests, directory moves, or automatic library discovery as implemented.
