# 0010: Generics

- Status: in progress (updated 2026-11-12)
- Decisions: none yet; each phase records one (see "Open questions"). Background:
  [0009](../decisions/0009-runtime-core-capabilities.md), [0015](../decisions/0015-noncopyable-and-linear-types.md),
  [0018](../decisions/0018-tasks.md), [0019](../decisions/0019-channels.md)
- Spec: [§2.3](../../specs/02-types.md), [§2.6](../../specs/02-types.md), [§3.2.3](../../specs/03-declarations.md),
  [§20](../../specs/20-protocols.md), [§21](../../specs/21-concurrency.md)

## Goal

Add function values, generic code and type-layout knowledge to the language, so that the compiler's
hard-coded generic builtins become ordinary Joyeer library types. Today the compiler supplies type
knowledge the runtime cannot express (element layouts, destructors, the task entry thunk, `Result`
construction) through dedicated IR opcodes (`taskSpawn`, `taskJoin`, `channelCreate`, `channelSend`,
`channelReceive`), although the algorithms are already Joyeer in [lib/Runtime/core/](../../lib/Runtime/core/).

## Definition of done

- A user can pass a top-level function as a value, write generic functions bounded by protocols, and
  declare generic structs and enums.
- `JoinHandle<R>`, `ChannelSender<T>` and `ChannelReceiver<T>` are declared in Joyeer over the existing
  `joyeer_thread_*` and `joyeer_channel_*` runtime functions, with the same public API and diagnostics as
  §21.
- The task and channel opcodes, `TypeKind::task`, `channelSender` and `channelReceiver`, and their
  backend cases in [native_collections.cpp](../../compiler/backend/native_collections.cpp) are removed.
- Tasks and channels are no slower than today, measured in both optimization profiles.

## Current state

| Feature | State |
|---|---|
| Function types `(T): R` | Syntax described in §2.3; no `TypeKind`, not accepted. A function used as a value gives a late or misleading diagnostic ([concurrency plan](0009-concurrency.md)). |
| Indirect calls | Runtime-only and untyped: `joyeer_intrinsic_call_void1/2` (decision 0009, D2). |
| Protocols and generic functions | Proposed in §20, not implemented; `protocol` and `trait` are reserved. |
| Generic structs, enums, methods | Deferred by §20.10; v0.1 forbids them (§2.6, §3.2.3). |
| Layout intrinsics | None; runtime code sees only `Int` addresses ([decision 0005](../decisions/0005-runtime-unsafe-memory.md)). |

## Todo

Each phase follows the [feature workflow](../decisions/README.md): decision record, spec, failing
tests, implementation, role reviewers run separately from the author, then
`python3 scripts/validate.py --llvm-root <sdk>` in Debug and Release.

### Phase 1: function values

Non-capturing references to top-level functions; access effects are part of the type and must match
exactly. No closures, methods or generic functions as values.

- [ ] Decision record: function values without captures, and why captures stay out. Drafted as
  [0020](../decisions/0020-function-values.md) and reviewed (language-designer, ir-backend-reviewer);
  open until accepted.
- [ ] Spec: make §2.3 function types normative; mark what is not accepted.
- [ ] Failing GoogleTests and stage fixtures for each access effect and each rejected form.
- [ ] Name resolution and type checking: a function `TypeKind`, a function name used as a value.
- [ ] IR: a function-address opcode and an indirect-call opcode; verifier checks signature and ownership.
- [ ] Backend: function pointers, indirect calls with the Joyeer calling convention, debug info.
- [ ] End-to-end fixture: store, pass and call function values, including `consuming` and `inout`.
- [ ] Close the "function used as a value" diagnostic gap in the [concurrency plan](0009-concurrency.md).
- [ ] Reviews (language-designer, type-system-reviewer, ir-backend-reviewer, adversarial-tester) and validation.
- [ ] Update [supported features](../impl/supported-features.md).

### Phase 2: protocols and generic functions (§20 v1)

- [ ] Decision record: reviewability of monomorphized generics, ownership of a type parameter, and
  where instantiation happens.
- [ ] Spec: settle §20 v1 and update §2.6, §3.2.3 and the reserved-keyword appendix.
- [ ] Failing GoogleTests and stage fixtures for protocols, conformances, constraints and `Copyable`.
- [ ] Parsing and name resolution: `protocol`, conformance extensions, generic parameters.
- [ ] Type checking: check a generic body once against its constraints; call-site errors name the
  unmet constraint.
- [ ] Instantiation with a cache keyed by function and type arguments; spans and debug scopes point at
  the generic source.
- [ ] IR, verifier and backend see only concrete functions; end-to-end fixtures.
- [ ] Measure compile time and code size on fern and the web-server example.
- [ ] Reviews and validation; update supported features.

### Phase 3: generic types and layout intrinsics

- [ ] Decision record: generic structs and enums (lifting part of §20.10), and the runtime-only layout
  intrinsics' surface.
- [ ] Spec for generic structs, enums and their methods.
- [ ] Failing tests, then parsing, type checking and instantiation of generic types.
- [ ] Runtime-only intrinsics: size and alignment of `T`, moving a `T` into and out of an address, and
  destroying a `T` at an address.
- [ ] Fixture: a runtime-source generic struct owns a raw buffer of `T` and destroys queued noncopyable
  values correctly.
- [ ] Reviews and validation; update supported features.

### Phase 4: concurrency builtins in the library

- [ ] Benchmark today's tasks and channels as the baseline.
- [ ] Declare `JoinHandle<R>`, `ChannelSender<T>`, `ChannelReceiver<T>`, `spawn` and `channel` in Joyeer,
  keeping §21: linear handles, `Result` from `send` and `receive`, destruction of unsent and unreceived
  values, the 256-task limit and its traps.
- [ ] Existing §21 fixtures and diagnostics pass unchanged.
- [ ] Benchmark again; no regression in either profile.
- [ ] Delete the opcodes, type kinds, backend cases and compiler-known names.
- [ ] Reviews and validation; update supported features, runtime notes and §21.
- [ ] Write a follow-up plan for `Optional` and `Result` as library enums, and later `Array` and `Dict`.

## Open questions

1. **Reviewability.** The preamble wants code that humans review; monomorphization compiles one body many
   times. How do bounded, statically dispatched generics keep behavior predictable, and what does the
   compiler report (instantiations, size) to keep it visible? Phase 2 record.
2. **Ownership of a type parameter.** `Copyable` covers copying; how does a generic body treat a `T` that
   may be noncopyable or `linear` (decision 0015)? Phase 2 record.
3. **Where instantiation happens:** on the typed model after type checking, or during IR lowering.
   Phase 2 record.
4. **Captures.** What would justify closures later, given ownership of captured state? Phase 1 record.

## Out of scope

Closures and captures, existential values (`any P`) and dynamic dispatch, associated types, default
protocol methods, protocol inheritance, explicit type arguments at call sites, and variadic generics.
They stay deferred per §20.10 unless a phase's record shows they are needed.

## Progress log

- 2026-11-12: decision record 0020 drafted and reviewed by language-designer and ir-backend-reviewer;
  awaiting acceptance. No items done.
- 2026-11-12: plan written; no items done.
