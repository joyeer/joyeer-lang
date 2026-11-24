# 0032: Owned TCP read readiness queue

- Status: proposed; implemented on Windows, Linux, and macOS, with native Linux and macOS validation pending
- Spec: [§18.6, TCP read readiness queues](../../specs/18-host.md#tcp-read-readiness-queues)

## Context

`tcpPoll` borrows a small set of listeners and connections. Raising its cap
would still require visiting the entire set on every call. A process with a
fixed cohort of connected feeds needs to wait without polling each one.

## Options considered

1. Extend `tcpPoll`. It keeps ownership with the caller, but the initial
   registration and membership scan would remain on the wait path.
2. Borrow sockets in a persistent queue. Registration would outlive the
   borrow, and Windows cannot detach a socket from its completion port to
   make it independently usable again.
3. Own the sockets and provide selected reads, bounded writes, and close.
   Ownership and registration have the same lifetime. This does not solve
   dynamic accept and dispatch, which continue to use the smaller borrowed API.

## Decision

`TcpReadQueue` consumes a fixed array of 1–4096 owners, with an all-or-nothing
setup. Wait releases the previous selection, then selects one readable
connection by its original input index. Readiness does not consume bytes or
prove EOF. Reads never block; writes have a finite budget. Closing a connection
retires its slot without shifting any index. The queue can move between tasks
but cannot be shared. A single connection error is not a queue failure.

Linux uses level-triggered one-shot `epoll`: add once per socket, modify only
the slot being rearmed, and use the input index as the event identity. Neither
the kernel's event data nor its wait buffers outlive the syscall except for
the copied integer identity.

macOS uses a `kevent64` read filter with `EV_DISPATCH`, then `EV_ENABLE` on
release. The copied `udata` is the stable input index, not an address. The
read may still return bytes even when the filter reports `EV_EOF` with a
socket error.

Windows associates each socket with an I/O completion port and posts an
overlapped zero-byte `WSARecv` as a readiness probe. Its native record stays
at a stable address. Both immediate success and pending success owe one
packet. A failed packet still retires that submission; cancellation from an
exiting issuer is rearmed, not exposed as a socket error. Teardown closes
the sockets, drains all outstanding packets without rearming, then releases
the port and records. It cannot free a live record even on an error path.
If cleanup cannot establish that all native operations have finished, it
terminates instead of releasing storage the OS may still use.
See [§19.6](../../specs/19-c-interop.md#196-ownership-errors-and-control-flow)
for the narrow private retention rule.

## Consequences

Setup, storage and destruction scale with the number of inputs; steady-state
wait bookkeeping scales with notifications handled, not with array length.
4096 is an API ceiling, not a promise about host resource limits. Dynamic
membership, listening sockets, handle extraction and write readiness are not
provided.

## Evidence

Windows x64 `SystemTests` verified the selected index in a set of 80 sockets.
`E2ETcpReadQueueTransfer` moves a queue from both an exited task and a live
task; `E2ETcpReadQueueWindows` tests partial registration rollback, reset
isolation, synchronous probe errors, and cleanup counters. Both passed at
`-O0` and `-O2`. An ordinary TCP program built at `-O2` did not import the
read queue's native calls; `-O0` retains unused System bindings.

A Linux x86-64 kernel probe checked the packed event layout, one-shot disarm
and rearm, and FIN readiness. It does not replace validation of the Joyeer
library on Linux.
