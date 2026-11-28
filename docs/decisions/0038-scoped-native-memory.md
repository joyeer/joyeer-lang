# 0038: Scope of the toolchain native-memory boundary

- Status: proposed; no new native representation or compiler behavior
- Plan: [0002](../plan/0002-c-interop.md#native-memory-implemented-boundary-and-remaining-design)
- Existing contract: [§19.4.3–4](../../specs/19-c-interop.md#1943-restricted-buffer-imports),
  [§19.6](../../specs/19-c-interop.md#196-ownership-errors-and-control-flow)

## Context

The toolchain needs to call native services with explicit bounds and
ownership. Existing authorized buffer forms and small wrappers cover most
uses without exposing an address type or transferring foreign storage into a
Joyeer container.

## Proposal

| Need | First boundary |
|---|---|
| Writable bytes and partial writes | Pass an initialized `[UInt8]` exclusively with `pointer:` and a separate length no larger than its count. A wrapper checks native status and returned count before publishing that prefix. No uninitialized tail is exposed. |
| Scoped address and length | Borrowed buffer adapters already provide a pointer and a count for the duration of a call. Do not add a storable pointer, a generic region layout, or automatic lowering with another parameter order. Use an explicit scalar length and a bare byte pointer or an adapter for a native prototype that differs. |
| Nullable pointers | An empty buffer may map to a null data pointer. It does not generally represent optional storage. Where a native API distinguishes absent from empty, use an explicit adapter with a presence flag; do not lower `Optional` as a C pointer. |
| Native result ownership | Return into an initialized runtime-owned buffer or copy the completed result into one. Keep foreign allocator ownership on the native side; the adapter releases its allocation exactly once, including error paths. Arbitrary owned pointer results remain out of scope. |
| Native resource handles | Wrap tokens behind private fields in a noncopyable owner. Publish an owner only after successful creation, invalidate it on consuming close, and release at most once. `System` file and process owners provide the existing pattern. |
| C strings | Select encoding explicitly, reject interior NUL before calling a terminating-string API, and append one NUL to a new byte buffer. Borrow the resulting bytes for the call only, unless a separately reviewed native owner makes a copy or owns a stable buffer. |
| Callbacks and foreign threads | No change to the already specified runtime intrinsics and runtime-created thread entry. Public callbacks, retention of managed buffers, and foreign-thread entry require their own contracts and are not implied by this proposal. |

Keep the distinction between an authorized wrapper's obligations and a
compiler-enforced lifetime. In particular, the compiler cannot verify that
a C function matches the declared ABI, uses the declared extent, or does
not retain the address. The proposal does not legitimize disguised address
arithmetic in ordinary code or untrusted native modules.

## Evidence and open work

`JoyeerCInterop.Buffers*`, `JoyeerCInterop.Pointers*`, and the `System` owned
resource tests exercise current wrapper patterns. Native x86-64 acceptance
evidence and formal adoption of this boundary remain tracked in plan 0002.
