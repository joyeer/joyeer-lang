# Joyeer Documentation Index

The normative language definition is the [specification](../specs/README.md), with design
explanations alongside the rules in each chapter. Implementation notes
describe the current compiler, and plan documents track incomplete work.
Explanations and plans do not add language rules or implementation guarantees.

## Start here

| Goal | Document |
|---|---|
| Learn the language | [Language specification](../specs/README.md) |
| Use Joyeer with an AI coding agent | [Portable Joyeer skill](../skills/README.md) |
| Understand the design and cost goals | [specs/00-preamble.md](../specs/00-preamble.md) |
| Understand memory and ownership | [specs/04-memory.md](../specs/04-memory.md) |
| Understand parameter access | [Access effects](../specs/04-memory.md#42-access-effects-on-parameters) |
| Understand contracts and verification limits | [specs/09-contracts.md](../specs/09-contracts.md) |
| Read the C binding design and distinguish its implemented subset | [specs/19-c-interop.md](../specs/19-c-interop.md) and [impl/c-interop.md](impl/c-interop.md) |
| See what the compiler implements | [impl/supported-features.md](impl/supported-features.md) |
| Use the first local project-manager version | [impl/fern.md](impl/fern.md) |
| Use the optional miniz-backed ZIP library | [Compression](impl/compression.md) |
| Disassemble Java classes and JARs with an AI agent | [moss](../tools/moss/README.md) and its [skill](../tools/moss/skills/moss/SKILL.md) |
| Plan runtime and public-library development | [Runtime in Joyeer](plan/runtime-in-joyeer.md) and [Library modules](plan/library.md) |
| Plan how the compiler finds the standard library | [Toolchain library resolution](plan/toolchain-libraries.md) |
| See a larger program (a Markdown web server over the provisional [TCP API](../specs/18-host.md#186-tcp-networking)) | [example/](../example/README.md) |
| Build from source | [building.md](building.md) |
| Install a local Debug compiler | [Debug installation](building.md#local-debug-installation) |
| Contribute | [AGENTS.md](../AGENTS.md) |

## Specification

The files under [specs/](../specs/) contain the normative chapters:
lexical structure, types, declarations, memory, expressions, statements,
patterns, errors, contracts, modules, naming, portable host operations, and
restricted C interoperation. Reserved keywords and syntax, worked examples,
and the consolidated grammar follow the numbered chapters as appendices.
[specs/README.md](../specs/README.md) indexes the chapters and appendices.

Design rationale, design notes, and cost notes explain the adjacent rules.
They are not separate specifications; future proposals remain explicitly
distinguished from adopted rules and current implementation support.

## Implementation

The compiler lowers typed Joyeer IR through LLVM to native executables.

| Document | Topic |
|---|---|
| [supported-features.md](impl/supported-features.md) | Current executable language surface, pipeline, and limits |
| [lexer.md](impl/lexer.md) | Token model, spans, recovery, and supported lexical surface |
| [parser.md](impl/parser.md) | Parser grammar, syntax AST, and recovery contract |
| [name-resolution.md](impl/name-resolution.md) | Scopes, symbols, references, and diagnostics |
| [type-checking.md](impl/type-checking.md) | Canonical compile-time types and typed overlays |
| [methods.md](impl/methods.md) | Methods, extensions, `self` and `Self`: receiver model, per-stage behavior, and limits |
| [semantic-analysis.md](impl/semantic-analysis.md) | Return, reachability, initialization, and warning analysis |
| [diagnostics.md](impl/diagnostics.md) | Stable IDs, source rendering, fix-its, and notes |
| [ir.md](impl/ir.md) | Typed backend-neutral IR, verifier, ownership, and lowering |
| [backend.md](impl/backend.md) | LLVM emission, backend ABI, platform linking, debug artifacts, and performance measurement |
| [c-interop.md](impl/c-interop.md) | Authorized scalar and buffer C bindings, object generation, explicit native-library inputs, and acceptance gates |
| [compression.md](impl/compression.md) | Optional miniz-backed ZIP module, limits, ownership, build and packaging |
| [runtime.md](impl/runtime.md) | Process entry, the Joyeer runtime (`lib/Runtime/`), ownership, collections, and where the host operations live |
| [fern.md](impl/fern.md) | Joyeer-written module/workspace workflow, manifest rules, and bootstrap/self-build |
| [string.md](impl/string.md) | String representation and operations |

## Plans

[plan/](plan/README.md) tracks incomplete multi-step work. Every plan follows
the [plan rules](plan/README.md#rules): a `Todo` checklist updated in every
iteration, a progress log, and deletion once every item is done. Start from the
[template](plan/template.md).

| Document | Topic |
|---|---|
| [c-interop.md](plan/c-interop.md) | Remaining scalar ABI implementation, cross-platform acceptance, and separate memory/alias proposals |
| [toolchain-libraries.md](plan/toolchain-libraries.md) | How the compiler finds standard modules (implemented) and what is left: the runtime in `lib/`, `--sysroot` in fern, precompiled modules |
| [native-link-inputs.md](plan/native-link-inputs.md) | Linking native libraries by logical name and search path (proposed) |
| [test-audit.md](plan/test-audit.md) | Test coverage gaps: fuzzing, sanitizers, CI, ownership generators, spec traceability |
| [runtime-in-joyeer.md](plan/runtime-in-joyeer.md) | The plan to write the whole runtime in Joyeer: definition of done, the core dialect, open design decisions, capabilities, phases and acceptance gates |
| [filesystem.md](plan/filesystem.md) | Remaining work after the host operations moved into `System`: POSIX verification, Compression/moss verification, and the allocation contract |
| [concurrency.md](plan/concurrency.md) | What is left of the concurrency work (tasks, channels and the worker-pool server are done): gates not yet run, compiler gaps, deferred features |
| [generics.md](plan/generics.md) | Function values, protocols and generic functions, generic types and layout intrinsics, then moving the task and channel builtins into the library (proposed) |
| [library.md](plan/library.md) | Standard modules: remaining Foundation/System work (contract APIs, host detection, environment and time), Collections/Testing, and fern adoption |
| [package-manager.md](plan/package-manager.md) | Remaining native platform/release gates and future dependency, workspace, cache, and registry contracts |

## Decisions

[decisions/](decisions/README.md) records contested or non-obvious choices and
why the alternatives were rejected. They are not normative; the adopted rule is
in the spec.

| Record | Decision |
|---|---|
| [0001](decisions/0001-stdin-line-reading.md) | Line-oriented standard input with a required byte limit |
| [0002](decisions/0002-platform-annotation.md) | `@platform` selects per-OS function variants |
| [0003](decisions/0003-pure-joyeer-filesystem.md) | The filesystem in Joyeer, with bare byte pointers |
| [0004](decisions/0004-remove-host-builtins.md) | Remove the compiler's host built-ins; `System` is the only provider |
| [0005](decisions/0005-runtime-unsafe-memory.md) | A runtime-only unsafe memory layer of `Int`-address intrinsics |
| [0006](decisions/0006-runprocess-in-joyeer.md) | `runProcess` in Joyeer over `CreateProcessW` and `posix_spawn` |
| [0007](decisions/0007-stdio-in-joyeer.md) | `writeStderr` and `readStdinLine` over the C library's own streams |
| [0008](decisions/0008-runtime-core-build-shape.md) | The Joyeer runtime core is one module compiled by the build-tree compiler into one object in `JoyeerRuntime` |
| [0009](decisions/0009-runtime-core-capabilities.md) | What the Joyeer runtime core needs from the compiler: static storage, indirect calls, core units, the entry point, data symbols, panic messages |
| [0010](decisions/0010-floating-point-printing.md) | `Float` and `Double` printing in Joyeer: exact `%.17g` / `%.9g` bytes, big-integer digit generation, `nan` |
| [0011](decisions/0011-runtime-without-the-c-library.md) | The runtime without the C library (Tier 2): size-class allocator over the system, owned output and exit, own entry |
| [0012](decisions/0012-tcp-networking.md) | Blocking IPv4 TCP in `System` (provisional, Windows and Linux implemented): opaque listener and connection handles, bounded receive, no concurrency yet |
| [0013](decisions/0013-concurrency-model.md) | Concurrency (accepted direction): share-nothing threads, ownership moved through tasks and bounded channels, no shared mutable state; timeouts and readiness polling first |
| [0014](decisions/0014-network-timeouts.md) | `timeoutMillis` on `tcpAccept` and `tcpReceive` (accepted, implemented on Windows and Linux): a bounded wait per call, no send or connect timeout, no total deadline |
| [0015](decisions/0015-noncopyable-and-linear-types.md) | Noncopyable and linear types and handles that close (accepted, implemented): copy and consume checks, a "must be consumed" analysis, consuming a pattern binding; the open fork is `deinit` versus a restricted resource annotation |
| [0016](decisions/0016-thread-primitives.md) | Thread, mutex and condition primitives in the runtime (accepted, implemented): runtime-owned trampoline, caller-supplied storage, 8 MiB stacks, at most 256 unjoined threads |
| [0017](decisions/0017-runtime-thread-safety.md) | Runtime thread safety (accepted, implemented): locks taken only while a second thread exists, OS-assisted sleeping locks, atomic `print`, first-panic-wins |
| [0018](decisions/0018-tasks.md) | Tasks (accepted design, in implementation): `spawn`, `JoinHandle<R>` and `consume h.join()`, one heap block per task, `Array.consumeEmpty()` |
| [0019](decisions/0019-channels.md) | Channels (accepted, implemented): single-sender, single-receiver bounded channels, `send` returns `Result<Void, ChannelClosed>`, a runtime object with a lock-protected close protocol |

## Conventions

- Keep one topic per document and link instead of duplicating.
- Keep design explanations beside the rules they motivate; do not repeat
  normative rules in separate design documents.
- Keep implementation details and measurements out of normative rules, and
  distinguish proposed features from implemented ones.
- Keep short-lived plans current; delete obsolete implementation plans.
- Use repository-relative links and verify them when deleting source areas.