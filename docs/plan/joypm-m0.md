# joypm M0: Language and Host Contracts

**Status:** discussion draft except M0-02 (single-file native entry), M0-03
(error propagation), M0-04 (unit values), M0-05 (`Dict.get(key:)`), and M0-06
(control flow and checked division/remainder), which
have been implemented. Key enumeration remains deferred.
The remaining recommendations are not approved language changes or implemented
APIs. M0 is not complete until the other decisions and compatibility
boundaries are accepted.

## Goal and scope

The first project manager, provisionally named `joypm`, will be written in
Joyeer. Argument parsing, manifest validation, build planning, and execution
policy belong in Joyeer. The existing C++ compiler and C11 runtime remain.

M0 defines observable behavior before implementation. Its deliverables are
semantic contracts, API sketches, compatibility decisions, and acceptance
cases for later milestones. The accepted entry, propagation, unit-value,
safe dictionary lookup, and M0-06 control-flow/arithmetic slices are implemented; modules,
filesystem operations, and subprocesses are not.

The current executable baseline is documented in
[Implemented Language Surface](../impl/supported-features.md). Specification
examples may describe a broader design than that baseline.

## Decision register

Recommendations are **proposed** unless explicitly marked implemented.

| ID | Area | Recommended direction | Implementation milestone |
|---|---|---|---|
| M0-01 | Modules | Directory-based modules, explicit dependencies, qualified imports, existing visibility levels | M1 |
| M0-02 | Program entry | Implemented for single-file native executables: preserve `main()`; accept borrowed arguments and an integer exit status | M3 entry slice done |
| M0-03 | Error propagation | Implemented: postfix `?` for `Result` / `Optional`, with exact error types and owned early-return cleanup | M2 propagation slice done |
| M0-04 | Fallible procedures | Implemented: `()` and `Result<Void, E>` through native execution | M2 unit-value slice |
| M0-05 | Dictionaries | Implemented: owned `get(key:)` returning `Optional<V>` without changing subscript behavior; key enumeration deferred | M2 lookup slice done |
| M0-06 | Control flow | Implemented: Boolean negation, short-circuit OR, unlabeled loop exits, checked division and remainder | M2 control-flow/arithmetic slice done |
| M0-07 | Paths and files | Separate byte strings from portable path encoding and define non-destructive operations | M3 |
| M0-08 | Processes | Synchronous execution with separate arguments and explicit completion states | M3 |

## M0-01: Modules and visibility

Follow [the module design](../spec/12-modules.md), with a deliberately small
first implementation:

- One module is one directory of directly contained `.joyeer` files.
  Subdirectories are not recursively merged into the same module.
- The compiler receives a root module and an explicit mapping from logical
  dependency names to module directories. It does not fetch packages or
  interpret the project manager's manifest.
- The single-file compiler invocation remains supported.
- Declaration collection spans the module before function bodies are checked.
  Source-file discovery order must not affect name resolution.
- Imports are file-local and precede declarations. Initially, import a module
  and access its public names through a qualified name; do not implicitly
  inject all exported names into the importing file.
- Import aliases, wildcard imports, and re-exports are deferred.
- Reject module dependency cycles with a diagnostic that identifies the
  cycle. This does not prohibit recursive functions.
- Preserve the existing levels: `internal` by default within a module,
  file-local `private`, and `public` across imports.
- Apply visibility to exposed types as well as names. Public function
  signatures, public fields, and public enum payloads cannot expose
  inaccessible types.
- Preserve the synthesized initializer visibility restriction in
  [declarations](../spec/03-declarations.md): construction cannot make a
  private field publicly writable.
- Preserve per-file source spans and debug scopes. Module compilation is not
  source-text concatenation.

Example of the proposed import style:

```joyeer
import project.config

func main() {
    project.config.describe()
}
```

This example is not accepted by the current compiler. `describe` would have
to be public in the imported module.

Before accepting this contract, specify the compiler CLI representation of
the module mapping, duplicate-file handling, collisions between imported
module names and local declarations, and file-private shadowing rules.
The initial backend may compile the entire module graph together; separate
binary modules, caching, and a stable library ABI are not prerequisites.

## M0-02: Program entry and exit status

Accepted and implemented for single-file native executables; see
[declarations](../spec/03-declarations.md#326-executable-entry-point) for the
normative contract. Keep the original entry form:

```joyeer
func main() {
    print(value: "Hello")
}
```

The additional entry form is:

```joyeer
func main(args: [String]): Int {
    print(value: args.count)
    return 0
}
```

Implemented contract:

- A single-file executable has exactly one entry point; module-root entry
  selection remains part of M0-01.
- Legacy `main()` succeeds with status zero after normal cleanup.
- `args` contains user arguments only, excluding the executable name.
- Empty arguments, spaces, and argument boundaries are preserved.
- Argument ownership follows the existing default borrowing convention.
  Runtime-owned argument storage outlives the entry call and is released
  before the final allocation-balance check.
- Normal return cleans local values and temporaries before reporting status.
- Joyeer's own entry return value is restricted to `0..255`. Out-of-range
  values produce an explicit runtime diagnostic and failure, not truncation.
- POSIX arguments preserve their original bytes; Windows uses UTF-16 to UTF-8
  conversion and reports invalid encoding.
- Do not add an unrestricted termination primitive as a substitute for entry
  return semantics.

When M0-01 adds modules, preserve the single root-module entry rule without
changing either signature. A subprocess status must still retain the full
platform exit code; it is not restricted by the range for Joyeer's own entry
points.

The project manager's meanings for individual codes are tool policy, not
language semantics. A possible convention is zero for success, one for an
operation failure, and two for invalid CLI usage.

## M0-03: Error propagation

Implemented using [the propagation design](../spec/08-error-handling.md),
with the following first-version limits:

- Evaluate the operand of `?` exactly once.
- Unwrap one `.Ok` or `.Some` layer on success.
- Return the corresponding `.Err` or `.None` on failure.
- A `Result<T, E>` operand requires an enclosing `Result<U, E>` return type
  with the same error type. Do not introduce implicit error conversions.
- An `Optional<T>` operand requires an enclosing optional return type.
- Do not implicitly convert between `Optional` and `Result`.
- Preserve ordinary value ownership and cleanup on the early-return path.
- Do not silently flatten nested optional or result values.

A supported example:

```joyeer
func load(path: String): Result<String, IOError> {
    let contents = readFile(path: path)?
    return .Ok(contents)
}
```

This does not make `?` valid inside an entry point returning `Int`. The CLI
boundary must explicitly handle its final `Result`, print a diagnostic, and
choose an exit status. Error wrapping between subsystems remains explicit.

## M0-04: Fallible procedures and the unit value

Writing a file and creating a directory can succeed without returning data.
They should not need a dummy Boolean or integer success payload.

Accepted and implemented:

- `Void` has one value, spelled `()`.
- A fallible procedure can return `Result<Void, E>`.
- Its success construction is `.Ok(())`, not an empty payload clause `.Ok()`.
- This addition does not introduce general tuples or tuple destructuring.
- Unit values and zero-sized payloads must have valid type, ownership, IR, and
  LLVM representations; an LLVM `void` type is not a stored aggregate field.

The compiler now accepts `()` in expression, type, and pattern positions.
Unit values work in bindings, calls, returns, aggregates, and builtin
containers, with initialization and consumption checks preserved.
`.Ok()` and general tuples remain rejected. The native acceptance fixture is
[`unit_values.joyeer`](../../tests/native/unit_values.joyeer). Filesystem
operations remain separate work. The normative rules are in
[types](../spec/02-types.md#212-void-and-the-unit-value) and
[error handling](../spec/08-error-handling.md#822-fallible-operations-without-success-data).

## M0-05: Safe dictionary lookup

Implemented: builtin `Dict<K, V>.get(key:)` returns `Optional<V>`.
This does not require general user-defined methods or generics. The receiver
and key are borrowed, evaluated once in that order, and require no access markers.

- A present key returns `.Some` containing an independently owned value.
- A missing key returns `.None` without trapping.
- Lookup does not modify the dictionary.
- Subsequent dictionary mutation or destruction cannot invalidate the result.
- Preserve `dictionary[key]` and its current missing-key trap.
- For optional stored values, distinguish a missing key from a present key
  whose value is `.None`; do not flatten the result.
- Key enumeration is deferred. If manifest validation requires it, return an
  owned snapshot.
  Do not promise a traversal order; callers needing deterministic output sort
  explicitly.

The generic return type describes a compiler-supported container operation,
not a claim that Joyeer can already declare generic library functions.
See [dictionary lookup](../spec/02-types.md#261-safe-dictionary-lookup) for the
normative contract and
[`dictionary_get.joyeer`](../../tests/native/dictionary_get.joyeer) for native
acceptance coverage, including nested optionals, zero-sized values, ownership,
temporary cleanup, and unchanged strict subscripts. Lookup remains linear.

## M0-06: Control-flow and arithmetic boundaries

Accepted and implemented:

- Prefix `!` accepts `Bool` and returns its negation.
- `||` accepts Boolean operands, evaluates left first, and evaluates right
  only when left is false.
- Unlabeled `break` exits the nearest enclosing loop.
- Unlabeled `continue` starts the next iteration of the nearest enclosing
  loop; for `while`, this means re-evaluating the condition.
- Reject loop-control statements outside a loop in the current function.
- Every jump cleans exited scopes but preserves values in scopes that remain
  active. Loop back edges must retain initialization and consumption checks.
- Signed division truncates toward zero. A nonzero remainder has the sign of
  the dividend.
- Division or remainder by zero traps at every optimization level.
- Both minimum-`Int` division by `-1` and the corresponding remainder
  operation trap, rather than inheriting LLVM undefined or poison behavior.

Do not add labeled jumps, `for-in`, coalescing, optional chaining, or force
unwrap as part of this contract.

See [expressions](../spec/05-expressions.md#52-arithmetic-comparison-logical-bitwise)
and [loop exits](../spec/06-statements.md#65-loop-exits) for the normative rules.
Native acceptance uses
[`control_flow_arithmetic.joyeer`](../../tests/native/control_flow_arithmetic.joyeer)
at O0-O3 and with full debug information, plus separate zero-divisor and
overflow trap fixtures at every optimization level.

## M0-07: Paths and filesystem operations

Keep three concepts distinct: arbitrary file bytes, language strings, and
operating-system paths. The current runtime preserves arbitrary string bytes;
valid UTF-8 is not an enforced invariant.

Recommended boundary:

- Guarantee UTF-8 paths for the new portable filesystem interface.
- Reject embedded NUL bytes in paths and report invalid encoding explicitly;
  never replace invalid data silently.
- Use Windows wide-character APIs for the portable interface.
- Preserve binary file contents, including embedded NUL bytes.
- Define relative paths against an explicit base or the current process
  directory, never against the executable's installation directory.
- Separate lexical path operations from filesystem canonicalization.
  Collapsing `..` must not be treated as proof of confinement in the presence
  of symlinks.
- Define missing paths, wrong entry types, permissions, and already-existing
  destinations as recoverable failures.
- Distinguish create-new from replacement. `init` requires create-new
  semantics enforced by the operation, not just an earlier existence check.
- Do not follow symlinks during recursive cleanup of tool-owned outputs.
- Treat enumeration order as unspecified; sort where reproducibility matters.

The existing `readFile(path:) -> Result<String, IOError>` is a compatibility
surface. Decide how its Windows path behavior relates to the new portable
interface rather than silently changing its encoding or result type.

Also decide whether richer filesystem failures use a new error enum or extend
`IOError`. Adding enum cases affects existing exhaustive matches. The
recommended compatibility-preserving direction is a separate richer error
type for new APIs, with explicit adapters where needed.

Non-UTF-8 POSIX paths, atomic replacement guarantees, detailed symlink
behavior, and the exact operation signatures remain decisions to close.
M0 must either define them or explicitly defer unsupported behavior.

## M0-08: Synchronous subprocesses

Propose an operation with these conceptual inputs:

- An executable path.
- An array of arguments excluding the executable name.
- A working directory for the child.

Use a result that separates launch/wait failures from process completion.
The proposed completion type can be represented with existing enum concepts:

```joyeer
enum ProcessStatus {
    Exited(Int),
    Signaled(Int),
}
```

`Signaled` describes platforms that expose signal termination. A nonzero
exit, including a Windows native failure status, is still an `Exited` value
with the original code preserved.

Required semantics:

- Wait for the child before returning.
- Inherit standard input, output, error, and environment initially.
- Pass arguments separately. Do not perform shell expansion, wildcard
  expansion, variable substitution, or command-string evaluation.
- On Windows, define argument encoding for the supported executable argument
  convention; do not claim that an arbitrary program's private command-line
  parser can be made equivalent to an argument array.
- Resolve executable lookup separately from process creation. An explicit
  executable path must not silently fall back to another executable on PATH.
- Set the child's working directory without changing the parent's directory.
- Preserve empty arguments, spaces, Unicode, and literal metacharacters.
- Distinguish invalid input, executable-not-found, permission, launch, and
  wait failures through typed errors and original platform codes.
- Define cleanup if an error occurs after a child was created; do not leave
  an unreported orphan or zombie.

Pipes, output capture, timeouts, asynchronous handles, and shell scripts as
implicit executable substitutes are outside the first interface.
Cancellation behavior and the mapping from a child status to `joypm run`'s
own CLI status must be settled explicitly.

## Acceptance matrix to prepare in M0

These are acceptance cases for the planned work, except the implemented entry,
propagation, unit-value, dictionary lookup, and M0-06 slices, which have native and
compiler tests.

| Area | Positive cases | Negative or boundary cases |
|---|---|---|
| Modules | Same-module forward calls; public imported calls; correct per-file diagnostics | Duplicate declarations; invisible names; unresolved imports; dependency cycles; multiple root entries |
| Entry (implemented for single-file programs) | Legacy entry; zero arguments; empty and spaced arguments; normal nonzero return | Unsupported signature; invalid exit range; cleanup before reporting status. Windows encoding failure still needs a Windows-only test |
| Propagation (implemented) | Successful unwrap; error propagation; nested calls | Wrong error type; wrong enclosing return family; single evaluation; early-return temporary cleanup |
| Unit results | Construct and match `Result<Void, E>`; propagate success and failure | Invalid zero-sized payload lowering; confusion between `.Ok()` and `.Ok(())` |
| Dictionary lookup (implemented) | Present key; absent key; owned result survives mutation | Existing subscript still traps; nested optional preserves absence distinctions |
| Control flow (implemented) | Short-circuit OR; nested loop exits; repeated `continue` | Skipped side effects; use outside loops; skipped cleanup; invalid loop re-entry state |
| Arithmetic (implemented) | Positive and negative division/remainder | Zero divisor; minimum-`Int` edge; consistent optimized and unoptimized behavior |
| Filesystem | Binary contents; Unicode and space-containing paths; create-new | Existing destination; missing parent; permission failure; invalid encoding/NUL; symlink escape |
| Processes | Successful exit; nonzero exit; Unicode, empty, and metacharacter arguments | Missing executable; bad working directory; launch/wait failure; signal termination; unchanged parent directory |

## Documentation work and M0 exit criteria

During drafting:

- Keep recommendations in this plan, separate from normative accepted rules.
- Correct implementation-status notes that could imply an unimplemented
  feature already works.
- Record compatibility changes and unresolved decisions explicitly.

After acceptance:

- Update the affected type, declaration, expression, statement, error, and
  module chapters rather than duplicating normative rules in this plan.
- Update the implementation plan with exact scope and dependency changes,
  including the newly identified unit-value requirement.
- Assign stable diagnostic categories and stage/native acceptance cases.
- Define public host API signatures and private runtime ABI ownership rules
  without exposing LLVM C++ types across the backend boundary.
- Close or explicitly defer every unresolved decision above.

M0 is complete when M1-M3 can be implemented without inventing language or
host behavior while coding. A design document alone does not establish that
the proposed features are executable.
