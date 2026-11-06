# 0001: Standard-input line reading

- Status: accepted (runtime, compiler, and native tests pass on Windows ARM64
  Debug; full validation and role reviews were not run)
- Spec: [§18.5 Standard input](../spec/18-host.md#185-standard-input)

## Context

The host API (spec §18) can read whole files by path and write to stderr, but a
Joyeer program has no way to read its own standard input. A long-lived tool
that serves requests over stdin/stdout, such as a persistent `moss serve`
session that keeps a parsed binary in memory between queries, cannot be
written. `runProcess` inherits stdin for a child but gives the parent no way to
consume it, and `readFilePrefix` on a device path blocks until end of input.

Constraints: no GC, deterministic destruction, no new language syntax, and the
existing table-driven host-builtin pipeline (`hostbuiltins.h` descriptor,
flat C ABI, no struct-by-value across the ABI).

## Options considered

1. `readStdinLine(maximumBytes: Int): Result<String, StdinError>`. Fits the
   existing descriptor shape (`String` result, `Int` argument, `Int`-payload
   error enum). End of input and an oversized line are distinct error cases.
2. `readStdinLine(): Result<String?, StdinError>` with `nil` at end of input.
   Needs a new `ValueKind` for an optional string and an optional ABI result
   shape, and no bound, so a hostile peer could force unbounded allocation.
3. `readStdin(maximumBytes:)` returning raw chunks. More general, but every
   caller then reimplements line framing, and a short read becomes ambiguous
   with end of input on terminals and pipes.

## Decision

Add `readStdinLine(maximumBytes: Int): Result<String, StdinError>` (option 1).
`StdinError` has `EndOfInput(Int)`, `LineTooLong(Int)`, and `ReadFailed(Int)`;
every case carries an `Int` payload like the other host errors (the first two
carry 0, `ReadFailed` carries the native `errno`). The bound is mandatory so
every read has a memory ceiling. Lines are framed on `\n`; one preceding `\r`
is removed; bytes are returned unchanged otherwise.

## Consequences

- Easier: line-oriented servers and filters in Joyeer; Moss can offer a
  persistent session without a JSON layer.
- Harder: the runtime now owns a read of the process-wide stdin stream. It
  switches Windows stdin to binary mode so bytes, CRLF and Ctrl-Z pass through
  unchanged. This is a process-global side effect and is documented.
- Tests: runtime ABI cases (framing, CRLF, EOF with and without a trailing
  newline, the limit boundary, resynchronization after `LineTooLong`, invalid
  arguments), compiler signature/rejection cases, and a stage fixture.
- Added during the first consumer (`moss serve`): the read flushes stdout before
  blocking, because a pipe peer otherwise never sees the reply it is waiting for.
  A flush failure is ignored since `print` has no error path.
- Not provided: unbounded reads, partial-line reads, non-blocking or timed
  reads, and a stdout write that can report failure (still deferred).

## Evidence

Moss's delivery plan lists "MCP server, network service" as excluded and a
persistent session as the way to avoid re-parsing large inputs per query; the
missing stdin primitive is the only blocker identified in the host API.
