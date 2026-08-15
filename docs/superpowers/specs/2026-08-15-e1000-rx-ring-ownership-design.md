# E1000 RX Ring Ownership Fix

## Problem

The E1000 receive ring currently has two software consumers:
`e1000_handler()` in IRQ context and `net_poll_rx()` in tcpip-thread context.
Both call `e1000_poll_rx()` and advance `e1000.rx_tail` without a shared
ownership protocol. Diagnostics observed duplicate and skipped descriptors.
The driver also writes fixed `RDT=30` then `RDT=31` values on every poll rather
than returning the descriptor actually consumed. Under load, a host-confirmed
TCP reply can remain unprocessed until later mailbox activity.

## Ownership Model

The tcpip thread will be the sole hardware RX-ring owner.

- `e1000_handler()` will acknowledge interrupt causes and request a persistent
  tcpip mailbox wake. It will not inspect, copy, clear, or advance RX
  descriptors. The handler's ICR read remains mandatory: besides acknowledging
  the interrupt, it clears the QEMU e1000e RX latch so the device may write
  subsequent descriptors. The equivalent ICR read is removed from
  `e1000_poll_rx()` only after this responsibility is retained in the handler.
- `net_poll_rx()` will call `e1000_poll_rx()` and then `e1000_process_rx()` in
  tcpip-thread context. No other caller may invoke `e1000_poll_rx()`.
- The existing software RX queue remains between descriptor copying and lwIP
  delivery, but both fill and drain occur in tcpip-thread context. It is kept
  to minimize the change surface; collapsing this now-redundant bounce queue
  into direct `pbuf` delivery is separate follow-up work.

This removes IRQ/task reentrancy and keeps allocation and packet copying out of
the interrupt handler.

## Descriptor Lifecycle

For each descriptor at software index `i` with `DD` set:

1. Read and validate its length.
2. Copy a valid packet into the software queue, or intentionally drop it when
   allocation fails, length is invalid, or the queue is full.
3. Clear the descriptor status only after the CPU has finished reading its DMA
   buffer.
4. Call `arch_wmb()` (the x86 implementation is `sfence` plus a compiler memory
   clobber) so the cleared descriptor is visible before MMIO advertises it to
   hardware. A compiler-only barrier is not sufficient.
5. Write `RDT=i`, returning exactly that descriptor to the device.
6. Advance the software index to `(i + 1) % E1000_NUM_RX_DESC`.

Steps 3–6 apply to every consumed DD descriptor without exception. A packet
that is dropped because of invalid length, allocation failure, or a full
software queue must still clear DD, execute `arch_wmb()`, and write `RDT=i`.
Merely advancing the software index on a drop would permanently withhold that
descriptor after the fixed-tail workaround is removed.

Initialization remains `RDH=0`, `RDT=E1000_NUM_RX_DESC-1`, and software index
zero. The fixed `30→31` writes and their workaround comments are removed.
The stale `e1000_poll_rx()` comment claiming that the MSI-X path is not wired
and no IRQ ever fires is also removed with the poll-side ICR read. The handler
comment must instead document that its ICR read acknowledges the interrupt and
clears QEMU's RX latch; no wording may imply that polling still owns this duty.

## Wakeup Contract

An RX interrupt can occur after tcpip-thread RX polling but before that thread
joins the mailbox wait queue. To close this window, `sys_mbox_wake()` must set
the mailbox's existing `idle_wakeup` flag using
`spin_lock_irqsave(&mb->lock)` / `spin_unlock_irqrestore()`, then—after the
unlock—call `wait_queue_wake_one(&mb->wq)`. This exact
set-under-lock → unlock → wake ordering mirrors `mbox_idle_callback()` and is
safe when called from `e1000_handler()` in IRQ context. The fetch side must
observe and clear the flag under the same lock before sleeping. This persistent
wake is required together with single ring ownership; it was previously
insufficient while descriptor bookkeeping was still ambiguous.

The comment at `kernel/net/net.c`'s `e1000_poll_rx()` call must explicitly say
that it runs in tcpip-thread context and is the sole hardware-ring consumer.
This comment is part of the ownership contract and replaces the misleading
existing `IRQ context` wording.

## Diagnostics and Scope

Temporary per-packet serial logging must not remain because it materially
changes timing. Failure-only counters or snapshots may be used during
investigation, but production changes stay limited to E1000 RX ownership,
descriptor return ordering, and the mailbox wake contract. TCP retry or result
parser relaxation is prohibited.

The existing `localhost` DNS prerequisite and fixed host-port limitation remain
unchanged. The pre-existing `nanosleep()` wake defect is outside this fix.
VirtIO-net currently has a similar IRQ/task dual-consumer shape; it is explicitly
out of scope and must be tracked as follow-up work rather than treated as fixed
by the E1000 change.

## Verification

- Use a named deterministic descriptor-return mutation: initialize `RDT=1`
  and suppress all per-consumption `RDT=i` writes, limiting hardware to the
  first two descriptors. Across 20 fresh QEMU iterations, every aggregate
  network `RESULT` must report at least one failure. The DHCP subtest is
  expected to pass because the guest starts with a static address; failure must
  appear in a later RX-dependent UDP, DNS, TCP, or wget subtest. This proves the
  regression observes a stalled RX ring; a single timing-dependent failure is
  not sufficient mutation evidence.
- Run at least 20 QEMU network iterations without per-packet serial logging;
  every first result must report five passed and zero failed.
- Run the host test suite with `make test`.
- Run `make OS01_SYSTEST=1 test-syscall` after a clean rebuild because driver
  state/layout and shared mailbox behavior changed.
- Run `git diff --check` and an independent driver-focused code review.
