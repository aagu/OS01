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
  descriptors.
- `net_poll_rx()` will call `e1000_poll_rx()` and then `e1000_process_rx()` in
  tcpip-thread context. No other caller may invoke `e1000_poll_rx()`.
- The existing software RX queue remains between descriptor copying and lwIP
  delivery, but both fill and drain occur in tcpip-thread context.

This removes IRQ/task reentrancy and keeps allocation and packet copying out of
the interrupt handler.

## Descriptor Lifecycle

For each descriptor at software index `i` with `DD` set:

1. Read and validate its length.
2. Copy a valid packet into the software queue, or intentionally drop it when
   allocation fails, length is invalid, or the queue is full.
3. Clear the descriptor status only after the CPU has finished reading its DMA
   buffer.
4. Execute an explicit compiler/DMA ordering barrier so the cleared descriptor
   is visible before MMIO advertises it to hardware.
5. Write `RDT=i`, returning exactly that descriptor to the device.
6. Advance the software index to `(i + 1) % E1000_NUM_RX_DESC`.

Initialization remains `RDH=0`, `RDT=E1000_NUM_RX_DESC-1`, and software index
zero. The fixed `30→31` writes and their workaround comments are removed.

## Wakeup Contract

An RX interrupt can occur after tcpip-thread RX polling but before that thread
joins the mailbox wait queue. To close this window, `sys_mbox_wake()` must set
the mailbox's existing `idle_wakeup` flag while holding `mb->lock`, then wake
the queue. The fetch side must observe and clear this flag under the same lock
before sleeping. This persistent wake is required together with single ring
ownership; it was previously insufficient while descriptor bookkeeping was
still ambiguous.

## Diagnostics and Scope

Temporary per-packet serial logging must not remain because it materially
changes timing. Failure-only counters or snapshots may be used during
investigation, but production changes stay limited to E1000 RX ownership,
descriptor return ordering, and the mailbox wake contract. TCP retry or result
parser relaxation is prohibited.

The existing `localhost` DNS prerequisite and fixed host-port limitation remain
unchanged. The pre-existing `nanosleep()` wake defect is outside this fix.

## Verification

- A deliberately broken descriptor-return mutation must make the QEMU network
  regression fail, proving the test observes RX loss.
- Run at least 20 QEMU network iterations without per-packet serial logging;
  every first result must report five passed and zero failed.
- Run the host test suite with `make test`.
- Run `make OS01_SYSTEST=1 test-syscall` after a clean rebuild because driver
  state/layout and shared mailbox behavior changed.
- Run `git diff --check` and an independent driver-focused code review.
