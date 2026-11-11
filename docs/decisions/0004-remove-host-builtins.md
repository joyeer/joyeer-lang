# 0004: Replace the compiler-known host built-ins with the System library

- Status: accepted and implemented (decided and built 2026-11-07), verified on
  Windows x64 Debug only.
  The Joyeer variants for Linux and macOS have not run, so those platforms are
  unverified; the C implementations were removed with the built-ins.
- Spec: [§18](../../specs/18-host.md) now says the operations are `System` functions
  and no longer calls them compiler-known.

## Context

The host operations (`readFileUtf8`, `readFilePrefix`, `writeFileNew`,
`createDirectory`, `listDirectory`, `fileKind`, `removeFile`, `removeDirectory`,
`joinPath`, legacy `readFile`, `writeStderr`, `readStdinLine`, `runProcess`) are
names the compiler knows. It lowers each to a `joyeer_*_abi` function in the C
runtime ([hostbuiltins.h](../../compiler/frontend/hostbuiltins.h)).
[Decision 0003](0003-pure-joyeer-filesystem.md) implemented the filesystem
operations in Joyeer in `System`. Keeping two implementations under two spellings
(`readFileUtf8` and `System.readFileUtf8`) is a transition state, not a design.

## Options considered

1. **Keep the names compiler-known and lower them to `System` functions.**
   Source stays compatible and nothing changes for fern and moss. The compiler
   keeps a table of names that really belong to a library, loads a module
   implicitly, and has two ways to spell one function.
2. **Remove the built-ins; callers `import System`.** One definition, one
   spelling, no special cases in name resolution, type checking, or the
   backend. Callers change.

## Decision

Option 2. After the Joyeer implementations are verified on every claimed
platform, the host names are removed from the compiler and runtime, and
programs write `import System` and call the functions as ordinary public
`System` functions.

Order, because each step must keep the suite green:

1. Port each operation into `System` with a differential test against the
   built-in (done: the nine filesystem operations, legacy `readFile`,
   `runProcess` ([0006](0006-runprocess-in-joyeer.md)), `writeStderr` and
   `readStdinLine` ([0007](0007-stdio-in-joyeer.md))).
2. Verify the POSIX variants on Linux and macOS.
3. In one change set: update fern, moss, the Compression library, tests, and
   examples to `import System`; delete the built-in descriptors, their type
   checking and lowering, and the C functions they called; point the
   runtime-archive check at the absence of `joyeer_fs_*`.

## Consequences

- Easier: a smaller compiler (the descriptor table, special enum shapes, and
  backend lowering for host calls go away) and one place to read each operation.
- Harder: every program that touches the filesystem or stdio imports `System`,
  which resolves only inside a workspace until the installed-library catalog in
  [library](../plan/library.md) §4 exists. Removal therefore also depends on that
  catalog, or on `System` being always available to the compiler.
- The shared enum types the built-ins use (`FileSystemError`, `FileKind`,
  `ProcessStatus`, ...) are currently prelude types known to the compiler. They
  need a home in `System` before step 3, or stay in the prelude on purpose.
- Tests that call the C ABI directly (`filesystem_test.cpp`, runtime tests) move
  to go through the language first.

## As built

Step 3 was done without step 2, at the owner's request, so Linux and macOS now
run only Joyeer code that has never executed there
([filesystem plan](../plan/filesystem.md) §2).

- **Enumerations stay in the prelude on purpose.** `FileSystemError`, `FileKind`,
  `ProcessStatus`, `ProcessError`, `StderrError`, `StdinError` and `IOError` are
  still compiler-declared prelude types; only their ABI tag mapping went away.
  Moving them into `System` would have forced every program that names one to
  import the module, and `ConversionError` cannot move at all. The descriptor
  table in `hostbuiltins.h` is now just their case lists.
- **Availability needed no new machinery.** `import System` already resolves
  from the standard library staged beside the compiler, so a plain `joyeer`
  compile and a `fern` project both reach it without a manifest dependency. fern
  rejects a `System` dependency listed in a manifest outside a workspace that
  loads it, so the manifests do not list it.
- **Differential tests became expectation tests.** With the built-ins gone,
  `SystemTests` states each result by error case rather than platform code. The
  raw `errno`/`GetLastError` agreement it used to check is no longer asserted.
- **Removed:** the host function descriptors, their name resolution, type
  checking, IR lowering and LLVM emission; `filesystem.c`, `process.c`, the C
  `readFile`, `writeStderr` and `readStdinLine`; and their C unit tests. The
  runtime archive holds no `joyeer_fs_`, `joyeer_run_process`,
  `joyeer_read_file`, `joyeer_write_stderr` or `joyeer_read_stdin` symbol
  (checked with `llvm-nm` on the Windows x64 Debug archive).
