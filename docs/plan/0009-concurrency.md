# 0009: Concurrency

- Status: blocked (updated 2026-11-26); native Windows and macOS ARM64
  acceptance passed. WSL2 AArch64 needs its build tools and LLVM 22.1.8 SDK.
  The older-glibc link check and native Linux socket validation remain open.
- Decisions: [0013](../decisions/0013-concurrency-model.md), [0014](../decisions/0014-network-timeouts.md),
  [0015](../decisions/0015-noncopyable-and-linear-types.md) to [0019](../decisions/0019-channels.md),
  [0032](../decisions/0032-tcp-read-queue.md)
- Spec: [§21](../../specs/21-concurrency.md)

## Goal

Finish share-nothing concurrency: run the acceptance gates that have not run, close the language and
compiler gaps the feature work found, and keep the deferred features on record.

## Definition of done

- [Full platform validation](../building.md#full-platform-validation) passes in Debug and Release
  on each supported native target.
- Every item under "Language and compiler gaps" is fixed or moved to a decision record.

## Current state

Noncopyable and linear types with `deinit` (P2), thread primitives (P3), a thread-safe runtime (P4),
tasks (P5a), channels (P5b, P6), and the worker-pool web server (P7) are implemented. The
[P1a timeouts](../decisions/0014-network-timeouts.md) cover receive and accept on Windows and Linux.
The bounded send/connect and poll APIs and the fixed-membership TCP read queue pass
Windows x64, Windows ARM64, and macOS ARM64 loopback tests. The Linux socket and queue backends are
implemented; their native verification remains open. The implementation notes are in
[runtime](../impl/runtime.md) and [supported features](../impl/supported-features.md).

## Todo

### Platform acceptance

The initial runs are recorded in [0023](../decisions/0023-library-concurrency.md#final-comparison-and-validation).

- [x] Windows x64: Debug and Release each passed unfiltered CTest 1665/1665 and `fern test` 7/7.
- [x] Linux x86-64 (WSL Ubuntu): Debug and Release each passed unfiltered CTest 1641/1641 and
  `fern test` 7/7.
- [x] macOS ARM64 (`799dce4`): Debug and Release each passed unfiltered CTest 2019/2019 and
  `fern test` 7/7. This includes task and channel fixtures and
  [thread-safety tests](../../tests/unittests/runtime/thread_safety_test.cpp) using the macOS
  [`os_unfair_lock` implementation](../../lib/Runtime/core/locks.joyeer).
- [x] Windows ARM64: MSVC 14.52.37025 and LLVM 22.1.8. Debug and Release
  each passed unfiltered CTest 2260/2260 and `fern test` 7/7, including
  `SystemTests`, `E2ETcpReadQueueTransfer`, `E2ETcpReadQueueWindows`, and the
  web-server regressions. The final unfiltered CTest runs needed no retries.
- [ ] Linux AArch64: WSL2 Ubuntu 26.04.1 reports `aarch64` and glibc 2.43.
  Blocked: the guest needs CMake, Ninja, a C++20 compiler and a native
  LLVM/LLD/Clang 22.1.8 development SDK before Debug and Release validation.
  Run `python3 scripts/validate.py --llvm-root <linux-arm64-sdk>` from a
  separate Linux checkout so the build and fern output trees stay native.
- [x] Find the cause of the `SystemTests` "write zero bytes to a closed stderr" failure under `fern test`
  on Linux WSL2. Evidence: fix `863530f` probes empty writes; the full Debug and Release runs above
  include `SystemTests`.

### Linker follow-ups

- [ ] Test the Linux `-lpthread` link argument on glibc older than 2.34. The
  [linker](../../compiler/codegen/linker.cpp) already emits it for programs that use tasks or channels;
  this remains blocked on a host with older glibc. The new WSL guest has glibc 2.43.
- [x] Load only needed Windows C-startup archive members. A dedicated `/INCLUDE` anchor in the
  bundled `wmain` member replaced `/WHOLEARCHIVE`. Windows x64 evidence:
  `RuntimeCRuntimeProgramKeepsTheCStartup` checks that C startup remains without unused thread
  imports, and `RuntimeCStartupRejectsDuplicateWmain` checks that a second `wmain` cannot replace it.
- [x] Supply CRT-free memory helpers for the Windows ARM64 own-entry link.
  The `JoyeerCInterop.MemoryIntrinsics` fixtures reproduced the unresolved C
  heap symbols before the fix, then passed at `-O0` and `-O2` in the native
  validator. The symbol, archive-member, and own-entry import checks passed in
  both profiles, as did both tests for the separate C-startup path.
  See [0011](../decisions/0011-runtime-without-the-c-library.md).

### Language and compiler gaps

- [x] Support `==` and `!=` on structs and enums that opt into `Equatable`, with a diagnostic for
  nonconforming types. Evidence: [0031](../decisions/0031-equatable.md), `4089941`, and the
  `E2EEquatableDerived` fixtures at `-O0` through `-O3`.
- [x] Lower `ChannelClosed.Closed` as a qualified source enum case. Evidence: the
  [generic callbacks fixture](../../tests/c-interop/generic-callbacks.joyeer) and macOS validation
  in [0023](../decisions/0023-library-concurrency.md#final-comparison-and-validation).
- [x] Reject `Never` in channel element types with `type-checking.channel-element-type`. Evidence:
  `CInteropTest.ChannelArgumentsHaveOneSpecificDiagnosticAtTheTypeOccurrence` in
  [cinterop_test.cpp](../../tests/unittests/compiler/cinterop_test.cpp).
- [x] Reject early exits that would drop a pending linear move in a call or array literal. Evidence:
  `AQuestionMarkAfterAMovedHandleInTheSameCallIsReported` in
  [semanticanalysis_test.cpp](../../tests/unittests/compiler/semanticanalysis_test.cpp) and the array
  `return` case in `CInteropTest.PendingCompilerTransferCannotEscapeBeforeTheCallCommits`.
- [x] Transfer payloads from owned temporary matches without an extra clone. The
  `ADirectMatchOnReceiveOrRemoveLastTransfersOwnedPayloads` IR test checks `receive()` and
  `removeLast()` with `String` and array payloads. Borrowed matches still copy; the queue still
  copies the value's representation bytes.
- [x] Treat direct matches on `&handles.removeLast()` as owning the result. The
  [linear diagnostic fixture](../../tests/semantic-analysis/err/match_linear_remove_last_drop.joyeer)
  rejects a dropped handle, and `E2EOwnedMatchPayloads` joins a bound one.
- [x] Lower qualified builtin enum cases. `E2EQualifiedBuiltinCases` covers `ConversionError.OutOfRange`
  and a payload-bearing `IOError.Other` at `-O0` through `-O3`.
- [x] Accept `consume Chain.End` as a temporary, not a move from an enum type's storage.
  `E2EConsumeQualifiedCase` passes two independent cases in one call.
- [x] Diagnose `spawn(...).join()` and `consume spawn(...).join()` in type checking.
  `JoiningATemporaryReportsOnlyThatTheHandleMustBeStored` checks both forms give one diagnostic
  with the local-binding workaround.
- [x] Resolve joined results in arguments without changing call syntax. The
  [parenthesized form](../../specs/21-concurrency.md#213-joining) is covered by `E2EOwnedMatchPayloads`.
  [0018](../decisions/0018-tasks.md#static-semantics) records why bare `consume` is the argument marker.
- [x] Keep `Any` reserved; no channel-only exception is needed. The
  [diagnostic decision](../decisions/0019-channels.md#diagnostic-follow-up) preserves the observed
  `lexer.unsupported-syntax` rejection for `Any` and the separate type-checking rejection for `Never`.
- [x] Decide the warning scope. [0019](../decisions/0019-channels.md#diagnostic-follow-up)
  defers deadlock warnings until a channel dependency can be identified and discarded-result
  warnings to a general must-use design. No warnings were added.

- [x] Commit moves from named linear places only after array or enum construction operands succeed.
  `E2ELinearAggregateCommit` covers array, contextual and qualified enum forms, aborted construction,
  sibling field access, and retry with `continue` at `-O0` through `-O3`.
  Ordinary calls and struct initializers retain the conservative guard.
- [x] Prevent a channel from carrying its own receiver. `TypeChannelRejectsOwnReceiver` rejects
  the measured leak before execution. Generic tests check substituted payload types and allow
  phantom type arguments, and `E2EChannelCycle` still passes. The conservative creation rule and
  separate distinct-queue limitation are recorded in [§21.9](../../specs/21-concurrency.md#219-channels).
- [x] Dispatch to an idle worker. The idle-dispatch regression reproduced the second request
  stalling with round-robin. `WebServer.IdleDispatchO0` and `WebServer.IdleDispatchO2` passed
  after adding per-worker signals and `waitForChannel`. `E2EChannelReadiness` covers shutdown
  and join at `-O0` through `-O3`; the runtime test covers waking on send and sender close.
  See [decision 0019](../decisions/0019-channels.md#waiting-for-an-available-worker).

### Networking

- [ ] Finish native Linux validation for `tcpSendWithin` and `tcpConnectWithin`. Windows x64
  loopback `SystemTests` verified timed success, invalid budgets, local refusal and send
  backpressure. A WSL2 AArch64 kernel probe checked `ioctl`/`ppoll`/`SO_ERROR`,
  loopback refusal, and send backpressure, but did not run the Joyeer implementation.
  Blocked: the local Linux environment has no configured LLVM/LLD/Clang development SDK.
- [x] Apply one five-second deadline to the entire HTTP request head. `WebServer.RequestHeadDeadline`
  exercises a complete request and a trickled incomplete one on loopback; partial reads cannot
  restart the budget.
- [ ] Finish native Linux validation for capped `tcpPoll`. Windows x64 loopback `SystemTests`
  passed, including distinct connection/listener indices, idle timeout, EOF and the 64-socket cap.
  Linux `ppoll` type-checks in `TheSystemLibraryChecksForEveryTargetPlatform`;
  the WSL2 AArch64 kernel probe verified idle timeout and distinct descriptor readiness.
  Native Joyeer acceptance needs the same SDK.
- [x] Define and verify scalable read readiness on Windows x64. The
  [owned queue](../decisions/0032-tcp-read-queue.md) uses IOCP rather than
  scanning a poll set. `SystemTests` selected a socket at index 79 of 80 and
  verified non-consuming readiness, rearm, EOF, and bounded sends. `E2ETcpReadQueueTransfer`
  and `E2ETcpReadQueueWindows` passed at `-O0` and `-O2`, covering task handoff,
  partial setup rollback, isolated read errors, and handle and allocation cleanup.
- [ ] Verify the Linux one-shot `epoll` backend natively. Cross-target System
  type checking passed, and kernel probes confirmed the event layout, rearm,
  and EOF on x86-64 and WSL2 AArch64 (16 bytes, data at offset 8). Blocked:
  full Joyeer validation needs a configured Linux SDK.
- [x] Native macOS ARM64 TCP acceptance. At `5c21924e`, Debug and Release each passed
  unfiltered CTest 2230/2230 and `fern test` 7/7 without reruns. `SystemTests` exercised
  libSystem sockets, capped `poll`, send/connect deadlines and backpressure, and the
  `kqueue` read queue (80 sockets, stable indices, rearm and EOF).
  `E2ETcpReadQueueTransferO0` and `E2ETcpReadQueueTransferO2` covered task handoff;
  `WebServer.IdleDispatchO0`, `WebServer.IdleDispatchO2`, and `WebServer.RequestHeadDeadline`
  passed in both builds.

## Out of scope

Deferred, not forgotten:

- Lifting the [process launch restriction](../../specs/21-concurrency.md#216-restrictions)
  for programs that call `spawn`. See [decision 0018](../decisions/0018-tasks.md#open-questions).
- Multi-sender channels, which need a runtime sender count and would be the one named exception to §4.1
  and §4.8; unbounded channels, `trySend` (returning the value, with a `SendError<T>`), `tryReceive`,
  timeouts on channel operations, and iterating a receiver.
- Structured scopes that lend read-only borrows to child tasks; `actor` syntax as sugar over a task and
  its channel; `async`/`await`, only if tens of thousands of simultaneous connections need it.
- Per-thread allocator caches with lock-free remote frees, and raw `clone`/`futex` threads if the C
  library is ever dropped on Linux.
- An IR `linear` bit with no scope-end cleanup for linear values, a verifier rule and a wire bump, as
  defence in depth beyond the run-time trap.

## Progress log

- 2026-11-26: Checked off native macOS ARM64 TCP after verifying the full validator
  and the loopback, task-transfer, and web-server gates at `5c21924e`.
- 2026-11-25: Closed the Windows ARM64 gate. At `5c21924` the original
  `fern` bootstrap failed in both profiles with MSVC 14.51.36231 because
  VCRuntime's ARM64 memory helpers pulled the C heap into the own-entry link.
  The replacement keeps that link CRT-free. With MSVC 14.52.37025, both
  builds and `fern test` runs passed. Debug initially exposed a CMake fixture
  issuing a lowercase unset and uppercase assignment for the same Windows
  environment variable. After correcting it, the final unfiltered Debug
  and Release CTest runs needed no retries. WSL2 AArch64 passed direct
  kernel probes for `epoll`, `ppoll`, nonblocking connect, and send
  backpressure; the guest still needs its build tools and SDK for the full validator.
- 2026-11-24: Replaced the macOS TCP stubs with libSystem sockets, capped `poll`,
  and a `kqueue` read queue. Enabled the native loopback, task-transfer, and web-server
  tests on macOS. System type-checking passed for all three OS targets; the Darwin
  `fcntl` binding emitted a real variadic call and passed symbol collision tests.
  Windows x64 Debug and Release each passed unfiltered CTest 2259/2259 and
  `fern test` 7/7.
  No native macOS run is recorded for this change.
- 2026-11-24: Implemented fixed-membership TCP read queues with Windows IOCP and
  Linux one-shot `epoll`. Checked off Windows x64 after the 80-connection loopback,
  task handoff, and rollback/cleanup regressions. The unfiltered Windows x64
  validator passed CTest 2257/2257 and `fern test` 7/7 in both Debug and Release.
  Native Linux verification and the macOS TCP/`kqueue` backend remain open.
- 2026-11-24: Verified named linear array/enum commits, direct channel self-receiver rejection,
  and idle-worker dispatch on Windows x64. Capped `tcpPoll` and bounded send/connect passed
  Windows loopback tests and cross-target type checking; native Linux runs still need a configured
  SDK. After fixing the implicit array-role visibility diagnostic, the final Windows x64 Debug
  and Release validators each passed unfiltered CTest 2252/2252 and `fern test` 7/7.
  `Fern.Workspace` passed on rerun in both configurations, as did `ValidationScriptTests` in Debug.
- 2026-11-23: Implemented lazy Windows CRT archive loading, qualified enum construction and consume,
  a single temporary-join diagnostic, owned match transfers and linear payload checks, and the HTTP
  request-head deadline. Recorded the existing joined-argument syntax and the decisions on reserved
  `Any` and warnings. Windows x64 validation passed: unfiltered CTest 2226/2226 and `fern test` 7/7 in
  Debug and Release; Release `Fern.Workspace` passed on rerun. The Debug full run preceded the final
  nested-marker fix, which passed the focused Debug regressions. The example returned HTTP 200 for
  a complete request and HTTP 408 after about five seconds for an incomplete head. Linux and macOS
  were not rerun for this batch.
- 2026-11-23: Recorded Linux x86-64 and macOS ARM64 acceptance and added the missing Windows ARM64
  gate. Checked off `Equatable`, qualified `ChannelClosed`, `Never` rejection, and the pending-move
  safeguards; kept full commit semantics open. Probed qualified builtin cases and temporary joins
  with an existing Windows Debug compiler. No new full platform suite was run.
- 2026-11-14: Plan 0010 Phase 4 moves the public task/channel types and operations into the source
  prelude without changing this plan's runtime contract. Its callback fixture covers qualified
  `ChannelClosed.Closed`; `ConversionError.OutOfRange` remains open. Conservative task-transfer
  checking adds regressions for pending arguments, consuming receivers, array construction and
  early exits; the broader gap/acceptance items above remain open until their own evidence is recorded.
- 2026-11-12: the function-used-as-a-value half of one compiler-gap item was done by plan 0010 Phase 1;
  the item now covers only `spawn(...).join()`.
- 2026-11-12: converted to the plan template; no items changed.
