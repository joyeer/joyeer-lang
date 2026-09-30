# Joyeer Package Manager Plan

> **Status:** Planning and requirements for the Joyeer package manager
> (provisionally named `joypm`). Named compilation units and the first portable
> host interface provide an implemented foundation (see
> [Implemented Language Surface](../impl/supported-features.md)
> and [Portable Host Operations](../spec/18-host.md)). The manifest, build-tool
> policies, and CLI below are proposals, not implemented tool guarantees.
> Independent binary libraries, host path canonicalization, and anchored
> recursive cleanup require additional contracts and implementation.

---

## 1. Goal and Scope

The package manager, provisionally named `joypm`, is the standard project and
package management tool for Joyeer, written in Joyeer itself. The existing
C++ compiler and C11 native runtime remain the underlying toolchain.
Command-line argument parsing, package manifest validation, build planning,
dependency resolution, and program execution policy belong in this tool.

### Responsibilities

- **CLI driver:** provide consistent commands (`joypm build`, `joypm run`,
  `joypm test`, and `joypm init`).
- **Manifest parsing and validation:** load and validate package declarations,
  target definitions, and module dependencies.
- **Build planning:** select explicit source-file sets using tool-owned source
  roots and include/exclude rules, associate them with logical module names,
  verify acyclic module graphs, and invoke `joyeer` with `--module-name`,
  positional root files, and repeatable `--module-source name=file` arguments.
- **Execution policy:** execute built binaries synchronously with inherited
  environment/streams and translate process exit states to tool exit codes.
- **Artifact lifecycle (future):** track output directories and, once suitable
  host operations exist, clean tool-owned artifacts without following symbolic
  links. Clean-before-rebuild is not a current guarantee.

---

## 2. Host and Language Foundation

The package manager relies on the host facilities and language features
implemented in Joyeer:

- **Modules and visibility (§12):** named explicit source-file sets with
  `--module-name` and `--module-source name=file` inputs, file-local qualified
  imports, and `public` / `internal` / `private` access boundaries. Repeating a
  dependency name adds files to the same unit. Complete dot-qualified names
  are logical identities, not directory trees or implicit parent modules.
- **Program entry and argument passing (§3.2.6):** `func main(args: [String]): Int`
  with borrowed user arguments and an exit code in `0..255`.
- **Error handling (§8):** typed errors (`Result<T, E>`), unit fallible
  results (`Result<Void, E>`), and postfix `?` propagation with deterministic
  scope cleanup.
- **Safe dictionary lookup (§2.6.1):** `Dict.get(key:) -> Optional<V>` for
  safe key lookup during manifest interpretation.
- **Portable filesystem operations (§18.2):** strict UTF-8 path handling,
  binary-preserving `readFileUtf8`, non-replacing `writeFileNew`, directory
  creation/listing, entry classification (`fileKind`), and lexical path joining.
- **Synchronous subprocesses (§18.3):** `runProcess` with separate argument
  arrays, child working directory control, inherited standard streams, and
  typed `ProcessStatus` (`.Exited(code)` and `.Signaled(signal)`).

---

## 3. CLI Design and Status Conventions

### Subcommands

| Command | Purpose |
|---|---|
| `joypm build` | Validates manifests, resolves modules, and invokes the compiler |
| `joypm run` | Builds and executes the root executable target |
| `joypm test` | Builds and executes test targets |
| `joypm init` | Initializes a new package directory with a manifest |

### Exit Status Policy

The CLI commands follow a standard three-state status mapping:

- **0 (Success):** the operation succeeded, or the executed child process
  exited with status 0.
- **1 (Failure):** a build failure, validation failure, tool operation error,
  or non-zero child process exit. The tool reports the original child status
  or platform error rather than truncating it.
- **2 (Usage Error):** invalid command-line syntax, missing required arguments,
  or unrecognized options.

---

## 4. Manifest and Build Planning

### Manifest Structure

A proposed package manifest would describe the following; no concrete manifest
format is specified or implemented by this plan:

1. **Package metadata:** package name, version, and edition.
2. **Targets:** executable targets, each selecting a root compilation unit
   and its source dependency graph. Independently emitted binary library
   targets require future compiler output and ABI support.
3. **Source selection and module mapping:** logical module names with explicit
   files or tool-defined source roots and include/exclude rules that expand
   into explicit file sets before invoking the compiler.
4. **Dependencies:** internal module-to-module dependencies and local path
   dependencies. Remote package fetching is deferred to a future milestone.

A project may initially contain one main compilation unit, but neither a
package nor a target is inherently one module. Future test targets may use
separate compilation units. A module may select files in nested or unrelated
directories, and distinct modules may select distinct files in one directory.
Folders do not create namespaces, modules, or implicit dependencies.

### Graph Validation and Compilation

The proposed build planner:

1. Expands its source-selection policy into nonempty file sets with explicit
   logical names. Any directory traversal belongs to the tool, not the compiler.
2. Resolves tool-relative paths before invoking the compiler, or chooses the
   child working directory consistently: compiler CLI paths use that process
   directory. Portable `joinPath` is lexical, not a canonicalization API.
3. Checks logical names and dependencies, verifies a directed acyclic graph
   (DAG), and leaves authoritative physical-file identity validation to the
   compiler. General canonicalization and hard-link identity APIs are not
   currently exposed by the portable host interface.
4. Generates compiler invocations with separate argument-array elements, for
   example:
   ```text
   joyeer --module-name acme.app root1.joyeer nested\root2.joyeer --module-source acme.config=path\config.joyeer --module-source acme.config=other\parser.joyeer -o app.exe
   ```

The two `acme.config` inputs form one dependency module. The root requires a
valid nonempty name and positional source files; a dependency cannot use the
root name. The compiler canonicalizes and sorts source paths and rejects
missing/nonregular inputs, empty sets, and duplicate physical files within or
across all supplied modules, including symlink and hard-link aliases. This
validation precedes compilation of reachable dependencies, even for unused
source sets. Whole-graph code generation does not merge logical modules.

The compiler does not discover files, add siblings, read manifests, or fetch
packages. Legacy single-file compilation remains available; multiple root
files and dependencies require named mode. Removed `--module-root` and
`--module` directory flags produce migration errors, not directory discovery.
See the [compiler interface](../impl/backend.md#2-cli) for the input contract.

### Key Enumeration Policy

If manifest validation requires enumerating all keys in a dynamic dictionary,
such an operation must return an independent, owned snapshot without promising
a specific iteration order. Callers requiring deterministic output must sort
keys explicitly.

---

## 5. Execution Policy and Host Safety

### Process Invocation

`joypm run` and `joypm test` invoke target executables using `runProcess`:
- The executable path is resolved relative to the calling process before
  switching working directories.
- Arguments are passed as an explicit array without shell interpolation.
- Standard input, output, and error streams are inherited so interactive programs
  and formatted test output work transparently.

### Artifact Management and Deletion

Future artifact policy must be designed around suitable host facilities; the
current interface does not provide anchored recursive cleanup, replacement,
or atomic publication. This plan does not promise automatic cleaning before
rebuilds. Requirements for a later implementation include:
- **Anchored deletion:** cleanup must operate strictly within designated output
  directories.
- **No link following:** deletion must never follow symbolic links into external
  directories; only the leaf symlink entry may be removed.
- **Publication semantics:** `writeFileNew` supports non-replacing tool-written
  files, but does not establish atomic publication of compiler outputs. A later
  publication protocol must define replacement and failure behavior explicitly.

---

## 6. Milestone Plan

### Phase 1: Single-Package Build Planning

- Implement manifest parsing in Joyeer.
- Implement module dependency sorting and validation.
- Expand tool-defined source selection and execute compiler commands for local
  executable targets and their source-module graphs.

### Phase 2: Execution and Test Runner

- Implement `joypm run` with argument forwarding and exit status reporting.
- Implement `joypm test` with test discovery and structured summary reporting.
- Implement anchored output cleaning only after the required host contract and
  operations are available.

### Phase 3: Workspaces and Package Initialization

- Implement `joypm init` for standard project scaffolding.
- Multi-package workspaces with shared dependency graphs.
- Incremental compilation planning (when compiler support becomes available).
