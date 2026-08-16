# Requested-Event-Aware Poll Design

## Problem

The E1000 ownership change at `9f83283` reliably carries the TCP echo reply
through QEMU, the hardware RX ring, the software RX queue, the tcpip mailbox,
lwIP, and into the correct socket. The remaining first-run TCP failure is in
the kernel poll registration contract.

A connected socket reports `POLLOUT` even when a caller requests only
`POLLIN`. The current `fd_poll(file, table)` registers a socket waiter only
when its complete readiness mask is zero. An unrelated ready output direction
therefore suppresses registration for the unavailable requested input
direction. If receive data arrives after that scan, `rx_pending` is set but
the empty socket poll list cannot wake the sleeping call.

The confirmed failure signature is:

```text
requested=POLLIN, initial mask=POLLOUT, registrations=0,
deadline mask=POLLIN|POLLOUT|POLLRDNORM
```

## Interface Contract

Change the kernel interface to:

```c
uint32_t fd_poll(struct file *f, uint32_t requested,
                 struct poll_table *pt);
```

`requested` is the caller's event mask for this descriptor. `fd_poll()` still
returns the descriptor's complete current readiness mask. The core poll layer
continues to publish only requested readiness plus the unconditional
`POLLERR`, `POLLHUP`, and `POLLNVAL` events required by poll semantics.

Wait registration is based on requested-but-not-ready directions, not on
whether any readiness bit exists. Readiness computation and registration for a
given wait list must occur under the same object lock used by its wake path.
This prevents an event from arriving between the readiness check and waiter
enqueue.

## Descriptor Behavior

### Sockets

While holding `s->lock`, compute socket readiness exactly as today. If the
caller requests any read-class event (`POLLIN`, `POLLRDNORM`, `POLLPRI`, or
`POLLRDBAND`) and no requested read-class event is ready, register
`s->poll_list`. A ready `POLLOUT` must not suppress that registration.

The current socket implementation has one poll list and no blocked-send
readiness callback. This change therefore does not invent write-side socket
backpressure. A caller requesting only the currently available `POLLOUT`
returns immediately without retaining a waiter. A caller requesting both
directions registers for receive only when output is ready but input is not;
the poll core then returns immediately for `POLLOUT` and cleans that temporary
entry before returning.

### Pipes and PTYs

Pipe and PTY read/write wait lists remain separate. Register only a direction
that the caller requested and that is not currently ready. Preserve existing
EOF, `POLLHUP`, `POLLERR`, and reader/writer-count semantics. An unrelated
ready direction must not prevent registration for a requested unavailable
direction.

Readiness and registration use different inputs. Readiness computation remains
keyed by the descriptor's legal open directions (`f->flags`: `O_RDONLY`,
`O_WRONLY`, or `O_RDWR`) and the endpoint's capabilities. Registration uses
`requested & legal_directions` and occurs only when that requested legal
direction is unavailable. Requested bits must never make an `O_RDONLY`
descriptor report writable or an `O_WRONLY` descriptor report readable.

### VFS and Devices

Plain VFS files remain immediately ready according to their open mode. Device
polling continues to delegate to the existing device-specific implementation,
but the requested mask is propagated explicitly through
`devfs_poll(node, requested, table)` and the `devfs_ops.poll` callback. The TTY
callback computes the same full readiness mask as today and registers its read
wait list only when a read-class event was requested and input is unavailable.
TTY's current input-empty registration is already correct; this signature
change is a consistency refinement, not a repair of a TTY lost wake. Default
always-ready devices keep their current readiness mask and do not acquire new
registration behavior.

## Poll and Select Flow

`do_poll_core()` passes each `pollfd.events` value into `fd_poll()`. It filters
the returned full readiness mask against requested events while always
retaining `POLLERR`, `POLLHUP`, and `POLLNVAL`.

`select()` and `pselect()` already translate read, write, and exception fd sets
into poll-style requested masks. They use the same event-aware `fd_poll()` path
and therefore receive identical directional registration semantics. The
translation and result mapping remain unchanged.

Before returning zero because a finite deadline has been reached,
`do_poll_core()` performs one final readiness scan with registration disabled.
If requested readiness is present, it returns that readiness instead of zero.
This closes the deadline-boundary race as defense in depth; it does not replace
correct wait registration. The concrete mechanism is
`fd_poll(file, requested, NULL)`: a null poll table computes readiness but
cannot add an entry. The final scan runs after existing entries are cleaned and
does not restore `current_poll_wq`, change the PIT deadline contract, or reuse
the previously active poll table for registration.

Every scan cleans its prior poll-table entries before reinitializing or
returning. Temporary entries created for a descriptor whose other requested
direction is already ready must therefore not survive the immediate return.

## Deterministic Regression

Extend the controlled host TCP echo endpoint with a test-only delay of exactly
250 ms. The guest connects and writes the exact payload, then calls
`poll(POLLIN)` before the host sends its reply. The regression must prove:

- the socket is already writable when the input wait begins;
- `poll(POLLIN)` returns readable well before its five-second deadline;
- the guest reads the exact echoed payload;
- the first guest aggregate result passes without relying on PID 1 respawn.

Add focused coverage for `POLLIN`, `POLLOUT`, both directions together, and
the equivalent `select()` read/write sets. The required test carriers are:

- `test/` host tests (`make test`): pipe, PTY, and devfs directional
  readiness/registration; entry cleanup; EOF, error, and hangup propagation.
- `user/systest.c` guest syscall E2E: pipe `POLLIN`, `POLLOUT`, and combined
  requests plus equivalent `select()` read/write fd sets.
- QEMU network harness and its guest network test: real connected-socket
  `POLLIN`, `POLLOUT`, combined requests, and `select()` equivalence, including
  the exact 250 ms delayed reply.

Tests must distinguish a genuine guest result from build, boot, bind, and
host-service failures. No retry or result-parser relaxation is permitted.

## Verification

- Demonstrate the delayed-reply regression fails on `9f83283` before the poll
  fix and passes afterward.
- Run at least 20 fresh no-delay QEMU network boots; every first result must be
  `5 passed, 0 failed`.
- Run a repeated delayed-reply cold-boot series; every run must return before
  the five-second timeout and report `5 passed, 0 failed`.
- Run focused host tests for poll/select requested-mask filtering, registration
  cleanup, pipe directionality, and error/hangup propagation.
- Run `make test`.
- Run `make clean && make OS01_SYSTEST=1 test-syscall`.
- Run `git diff --check` and an independent poll/select-focused code review.

## Scope

The E1000 ownership commit `9f83283` remains part of the branch and is not
reverted: diagnostics prove its descriptor lifecycle and receive path are
working. This design does not change TCP retry behavior, the network result
parser, VirtIO RX ownership, the accepted localhost resolver prerequisite, or
fixed host ports.

The existing global/single-CPU assumptions around `current_poll_wq` and poll
deadlines are separate SMP scalability work. They are documented but not
refactored by this fix.

Socket readiness does not currently produce `POLLPRI` or `POLLRDBAND`.
Read-class classification may use those requested bits to decide whether a
receive waiter is relevant, but it must not synthesize either readiness bit.
Consequently socket `select()` exception sets remain unsupported existing
behavior and are outside this fix.
