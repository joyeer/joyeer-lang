# Joyeer Package Manager Plan

> **Status:** Planning and requirements for the Joyeer package manager
> (provisionally named `joypm`). The foundational language, module, and host
> runtime contracts required by this plan are implemented in the compiler and
> native runtime (see [Implemented Language Surface](../impl/supported-features.md)
> and [Portable Host Operations](../spec/18-host.md)).

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
- **Build planning:** map logical module dependencies to physical directories,
  verify acyclic module graphs, and invoke `joyeer` with `--module-root` and
  repeatable `--module` arguments.
- **Execution policy:** execute built binaries synchronously with inherited
  environment/streams and translate process exit states to tool exit codes.
- **Artifact lifecycle:** track output directories and clean tool-owned
  artifacts without following symbolic links.

---

## 2. Host and Language Foundation

The package manager relies on the host facilities and language features
implemented in Joyeer:

- **Modules and visibility (§12):** directory-based modules with explicit
  `--module-root` and `--module name=dir` CLI mappings, qualified imports, and
  `public` / `internal` / `private` access boundaries.
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

A package manifest defines:

1. **Package metadata:** package name, version, and edition.
2. **Targets:** executable targets (with a root module entry) and library
   targets.
3. **Module mapping:** source directory roots and logical module names.
4. **Dependencies:** internal module-to-module dependencies and local path
   dependencies. Remote package fetching is deferred to a future milestone.

### Graph Validation and Compilation

The build planner:

1. Discovers module directories and computes canonical paths.
2. Detects and rejects duplicate physical directory inputs and identity
   collisions (including symlink aliasing).
3. Verifies that module dependencies form a directed acyclic graph (DAG).
4. Generates compiler invocations using:
   ```text
   joyeer --module-root <root-dir> --module <name1>=<dir1> --module <name2>=<dir2> -o <output>
   ```

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

Tool-owned output directories are cleaned before rebuilds:
- **Anchored deletion:** cleanup must operate strictly within designated output
  directories.
- **No link following:** deletion must never follow symbolic links into external
  directories; only the leaf symlink entry may be removed.
- **Create-new semantics:** newly produced build outputs use non-replacing
  creation (`writeFileNew`) or write to temporary locations followed by atomic
  publication when supported by host facilities.

---

## 6. Milestone Plan

### Phase 1: Single-Package Build Planning

- Implement manifest parsing in Joyeer.
- Implement module dependency sorting and validation.
- Generate and execute compiler commands for local executable and library
  targets.

### Phase 2: Execution and Test Runner

- Implement `joypm run` with argument forwarding and exit status reporting.
- Implement `joypm test` with test discovery and structured summary reporting.
- Implement anchored output cleaning.

### Phase 3: Workspaces and Package Initialization

- Implement `joypm init` for standard project scaffolding.
- Multi-package workspaces with shared dependency graphs.
- Incremental compilation planning (when compiler support becomes available).
