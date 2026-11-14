# 0023: Concurrency as a source-library prelude

- Status: accepted (2026-11-14), implemented and validated on Windows x64 and Linux x86_64.
- Spec: §2.6.3, §4.1.1/§4.7.1, §5.6.1, §19.4.5, §20.5/.6/.12,
  [§21](../../specs/21-concurrency.md), and Appendix C.

## Context

Phase 4 of [plan 0010](../plan/0010-generics.md) replaces compiler-defined task and channel
types and operations with Joyeer declarations. The public calls, ownership rules, stable diagnostics,
task limit, traps, and close protocol must stay compatible with §21 and decisions 0018 and 0019.

Phase 3 supplies concrete generic layouts and raw typed moves, but not everything this migration
needs. A named generic function cannot currently be a task target. Generic functions and types reject
linear type arguments, whereas an existing task may take or return a `JoinHandle`. There is no
runtime-only typed stack slot or way to obtain the address of a specialized generic callback.

Removing only the backend operations is insufficient: declaring the public names as builtins or
dispatching on their spelling would keep the compiler coupled to the library.

## Options considered

1. **Keep compiler-known wrappers, move only their algorithms.** Lowest migration risk, but does not
   satisfy the plan's requirement to remove the builtin types, names, and operations.
2. **Ordinary source declarations with narrow, authorized compatibility contracts.** The declarations
   own signatures, generic layouts, destruction, and executable bodies. Compiler metadata preserves
   the existing contextual diagnostics and restrictions without recognizing the public names.
3. **Ordinary source declarations with only ordinary generic diagnostics.** Simplest compiler surface,
   but changes the diagnostic IDs and accepts function-value variables where §21 rejects them. This
   does not meet the unchanged-fixture gate.

## Decision

Option 2, with the following closed source-library boundary.

### Source prelude and compatibility

The concurrency declarations are a toolchain-owned Joyeer source prelude. Its exported declarations
enter the existing prelude scope as ordinary structures, enums, functions, and methods. The original
root module remains the root, private helpers stay private, and current shadowing/import precedence
and native-binding authorization remain unchanged. Bundling the source at compiler build time is
preferred to a new runtime file-search dependency; source spans must still identify the library.

Closed, compiler-bundled prelude declaration metadata identifies compatibility contracts, rather
than names. Ordinary user units and explicit `--runtime-module` units cannot opt into it:

- a named, unary, consuming task target, with the existing `spawn-target-*` diagnostics;
- channel creation with matching initializing outputs, rejecting an integer literal (optionally
  negated) below 1; nonliteral `Int` capacities retain the runtime capacity/overflow traps;
- a channel element that must be affine and have a representation;
- a consuming member whose receiver must be stored, preserving the existing temporary and
  array-element restrictions;
- a task-result owner whose linear diagnostic help names its ending method.

Contracts validate ordinary calls and types. They do not generate task/channel IR, construct results,
select layouts, or implement destruction. User declarations cannot opt into them.

### Generic task targets and linear arguments

A public task target remains a named **nongeneric** top-level function. The current rejection of a
generic target, including its existing fixture, stays unchanged. The handoff's missing capability
is instead supplied by an equivalent runtime-only specialization of the library's generic entry
callback. The library `spawn<I, R>` receives a monomorphic `(consuming I): R`; its ordinary signature
determines `I` and `R`, including contextual typing of literals passed to a `Double` target.

Only explicitly marked, compiler-bundled task declarations may accept linear type arguments.
Their unconstrained parameters are checked conservatively as must-transfer opaque values: owning
values must be forwarded through a consuming call, typed move, or return on every returning path,
even for affine substitutions. They may not be dropped, copied, or stored in an implicitly
destructible field. This keeps the single checked-body model and does not permit arbitrary
per-instance re-type-checking. User generic declarations and channel parameters retain the
§20.12 restriction. A task may still take or return a linear handle.

### Runtime-only memory and callback requirements

Three additional typed runtime operations implement the source-library boundary:

- `joyeer_intrinsic_stack_slot<T>(): Int`, an uninitialized slot in the enclosing invocation, with
  the complete size/alignment of `T` and no automatic destructor;
- `joyeer_intrinsic_callback_address<T...>(function: entry): Int`, a named callback specialized by
  explicitly supplied runtime type arguments, one per type parameter of the entry in declaration
  order. The entry must have the exact noncapturing, by-value
  unary `consuming Int` / explicit `Void` signature, be declared in an authorized runtime unit,
  and be compatible with `void(void*)` on supported 64-bit targets;
- `joyeer_intrinsic_needs_destroy<T>(): Bool`, a compile-time destruction query, so a channel passes
  a null callback for a trivial element instead of allocating a useless pending-close buffer.

These operations are unavailable outside authorized runtime code. They reuse ordinary allocation, typed moves, function
addresses, and address conversion in verified IR; they are not concurrency opcodes. Raw slots have
no implicit ownership: library code must move a value into each slot, transfer it, move it out, or
destroy it exactly once. A stack slot avoids allocating per channel message and is allocated once
per enclosing invocation, not on every loop backedge. Size, alignment, stack slots, `move_in`, and
`move_out` may serve must-transfer task types; `destroy_at` remains forbidden for linear types.

An address-only callback reference enters the same discovery/cache/report/recursion-limit machinery
as a generic call; it must not bypass instantiation just because it is never directly called.

### Library implementation

`JoinHandle<R>` is a linear generic structure over an owned task block. A non-linear private block
owner supplies the existing `task dropped without join` backstop through `deinit`; successful join
consumes the block-owner field out of `self` and clears it before destruction. One allocation holds
the thread record, exact free size, result, typed target function, and input. A specialized Joyeer entry moves the input out, calls the target,
and moves the result into the block. The thread primitive keeps the 256-unjoined-task limit and
8 MiB stacks.

Channel ends are generic structures with ordinary `deinit` and consuming `close` methods. `send`
moves its consuming argument into a typed stack slot and transfers the bytes through the existing
channel ABI; failure destroys the unsent slot. `receive` moves a successfully received slot into
`Result<T, ChannelClosed>`. A specialized Joyeer callback destroys unreceived elements. The current
locked ring and outside-the-lock close protocol remain in `core/channels.joyeer`.

Creation uses raw `Int` output slots for the C ABI, then ordinary end construction initializes both
the private handle and the hidden live flag. The ABI must not write a raw pointer directly into an
ordinary deinitializable structure.

The compiler's thread-link requirement must follow references to the primitive runtime ABI, not
the removed opcodes or public type names. The textual `runProcess` exclusion follows the authorized
task-call contract and remains unchanged.

## Consequences

The migration removes six concurrency opcodes, three type kinds, backend task thunks and ownership
cases, and prelude builtin declarations. Any new address-conversion operation requires a wire-version
bump and verifier coverage, including rejecting unauthorized or malformed callbacks.

The library uses an indirect target call and may have larger end/handle representations because
ordinary `deinit` structures have a hidden live flag. Neither is assumed free: interleaved benchmarks
must establish the plan's no-regression gate in both profiles.

Independent implementation reviews and failing regressions checked the compatibility metadata,
must-transfer analysis, and verified-IR boundary. Acceptance evidence is recorded below; native
macOS and AArch64 verification remain open platform gates, not inferred from x86_64 results.

### Language review

The independent language-designer review requested changes before implementation. Folded in here:
preserve the obsolete-to-the-first-draft generic-target rejection by specializing only the internal
callback; accept nonliteral capacities; replace broad concrete-instance analysis with closed
must-transfer parameters; explicitly amend the layout rules; preserve null destroy callbacks for
trivial queued values; construct ends after raw ABI output; and require callback references to enter
instance discovery and recursion checks. The review did not approve an implementation or acceptance.

### Implementation reviews

Separate language, type-system, and IR/backend implementation reviews found issues that were
reproduced before correction. Focused regressions now cover target-derived optional task inputs
and ordinary mismatch diagnostics; single channel-element diagnostics at every type occurrence,
including invalid creation/wrapper calls; callback markers, explicit result and target authority;
closed metadata shape/multiplicity; and role-based ending help using a renamed `Ticket.finish`.

Must-transfer regressions reject branch-only transfer, copy, overwrite, destructible fields, and
early exits after pending arguments, consuming receivers, and array elements. A nested-loop case
keeps unrelated outer pending moves live without a false positive. Dictionary and enum construction
use the same pending-value mechanism.

The verifier independently checks callback signature, authority, non-native status and named-address
provenance at address erasure, bounds-checking malformed duplicate-block lookups. A reverse-edge
worklist propagates noncopyability from `deinit` owners through stored fields, enum payloads, and
containers, but not function signature arguments or phantom nominal arguments; `copy` rejects those
owners. Wire-round-trip and malformed-IR regressions cover both boundaries.

The adversarial reviewer independently prepared 52 programs / 56 cases but lacked a process tool.
The parent executed 64 case instances of those programs (the O0/O2 runs of the runnable subset); three initial mismatches were invalid probe syntax
(`result`, semicolons, and ownership markers), corrected without changing the compiler. All cases
then passed across the saved run/re-run records. This is separate test authorship, not a claim that
the reviewer executed the programs. Net allocation balance is not treated as a transient-allocation
measurement; the durable callback fixture inspects the actual creation ABI's null/non-null destroy
argument and the construction of eight live ends.

## Evidence

Unchanged tree `c6b5b4d`, Windows x64, LLVM 22.1.8: focused task/channel/concurrency/spawn CTest
selection passed 98 of 98 in both Debug and Release. Baseline workloads are
`tests/perf/task_churn.joyeer` (2048 tasks, batches of 64) and
`tests/perf/channel_throughput.joyeer` (100000 messages, capacity 64).

The reproducible runner is `tests/perf/benchmark_concurrency.py`. Fifteen process-wall-time
samples per workload/profile, with output checked on every run:

| Workload/profile | Minimum | Median | Executable |
|---|---:|---:|---:|
| task churn, Debug runtime / `-O0` | 0.163249 s | 0.180246 s | 93184 B |
| channel throughput, Debug runtime / `-O0` | 0.036592 s | 0.038330 s | 101376 B |
| task churn, Release runtime / `-O2` | 0.157060 s | 0.180460 s | 45568 B |
| channel throughput, Release runtime / `-O2` | 0.020632 s | 0.021747 s | 48640 B |

These are a baseline, not a comparison or acceptance result. The unchanged executables are preserved
outside the repository for interleaved comparison with the eventual implementation.

The comparison criterion, fixed before timing the candidate, is the paired median
candidate/baseline ratio with a deterministic 10000-resample 95% bootstrap interval. A lower bound
above 1 is a regression outside sampled noise and blocks acceptance. Run no build, validation, or
adversarial workload while timing; retain every sample and increase repetitions when noise makes
the result inconclusive.

### Final comparison and validation

The final candidate was built with the compilers/runtimes that passed `scripts/validate.py`,
then compared with the preserved unchanged executables in 31 alternating-order pairs. No builds,
validation, or adversarial programs ran during timing. Each invocation checked its complete output
and empty stderr.

| Workload/profile | Baseline median | Candidate median | Paired median ratio / 95% interval |
|---|---:|---:|---|
| task churn, Debug / `-O0` | 0.189092 s | 0.188155 s | 1.0213 / [0.9501, 1.0423] |
| channel throughput, Debug / `-O0` | 0.038325 s | 0.039343 s | 1.0246 / [0.9949, 1.0381] |
| task churn, Release / `-O2` | 0.176452 s | 0.178068 s | 1.0052 / [0.9850, 1.0281] |
| channel throughput, Release / `-O2` | 0.021365 s | 0.021892 s | 1.0201 / [0.9832, 1.0703] |

All four intervals include 1: no regression outside the sampled noise was resolved by the
predeclared criterion. This does not claim zero overhead on every machine. Ratios of paired samples
differ from ratios of the two unpaired medians because process timings drift within a group.
Executable sizes (baseline -> candidate): task O0 93184 -> 96256 B, channel O0 101376 -> 105984 B,
task O2 45568 -> 45568 B, channel O2 48640 -> 49152 B.

Full acceptance used `python scripts/validate.py --llvm-root <sdk> --jobs <n>
--define BUILD_TESTING=ON --define JOYEER_BUILD_FERN=ON --define JOYEER_BUILD_COMPRESSION=ON`
in an MSVC x64 Developer shell on Windows (`n=4`) and an isolated Git archive plus exact patch/new
files on WSL Ubuntu (`n=12`). LLVM was the developer-provided 22.1.8 SDK; no toolchains were installed.

| Platform | Debug | Release |
|---|---|---|
| Windows x64 | build, unfiltered CTest 1665/1665, fern 7/7 | build, unfiltered CTest 1665/1665, fern 7/7 |
| Linux x86_64 (WSL Ubuntu) | build, unfiltered CTest 1641/1641, fern 7/7 | build, unfiltered CTest 1641/1641, fern 7/7 |

Native macOS, AArch64, and the earlier debugger-breakpoint gate were not run. Future removal of
`Optional`, `Result`, `Array`, and `Dict` is tracked in [0011](../plan/0011-library-containers.md).
