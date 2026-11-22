# 0029: Prelude Contracts and Owned Host Resources

- Status: accepted (2026-11-22)
- Spec: [§9](../../specs/09-contracts.md), [§18.3](../../specs/18-host.md#183-child-processes),
  [§18.7–9](../../specs/18-host.md#187-process-environment-and-current-directory),
  [§21.6](../../specs/21-concurrency.md#216-restrictions)

## Context

The standard library needs executable contracts and access to native resources
that outlive one call. Neither should require new language syntax or escape
deterministic ownership.

## Decision

- Provide ordinary Joyeer prelude functions, not compiler-recognized call
  syntax. Both checks run in all profiles and require an explicit `message:`
  (which may be empty). The prior draft's default messages and assertion
  elision would require new call or evaluation rules. Fatal errors use the
  existing panic policy through a length-aware entry; compiler-generated
  traps keep their C-string entry.
- Keep environment and current-directory lookup read-only and use separate
  monotonic and wall clocks. Exact encoding and typed query errors distinguish
  a failed lookup from an absent or empty value. Millisecond results make
  rounding and range checks explicit without promising host precision.
- Use noncopyable `System` structs with hidden tokens, exclusive access to
  mutable cursors, and consuming close/wait operations. Destruction closes
  a file or directory, or waits for a child. Do not silently detach or kill a
  dropped child. Unlike a linear task `JoinHandle`, a `ChildProcess` may be
  dropped; its destructor can block. Keep process launch out of programs
  that spawn tasks, including references to launchers as function values.

## Consequences

Close explicitly to observe file or directory release errors, and wait
explicitly to observe a child's status. No detach, timeout, cancellation,
general async I/O, or authorization to mutate host state is implied. Add
`Collections` or `Testing` only when a consumer needs an actual module.

## Evidence

[Standard-library validation](../impl/supported-features.md#standard-library-validation)
records the native macOS Debug and Release results and the cross-target frontend checks.
