/* kernel/include/ipc/mbox.h — Mailbox wake IPC primitives.
 *
 * ARCH-9 Task 9: replaces the block-scope `extern void sys_mbox_wake(void);`
 * redeclarations that used to live inside kernel/driver/virtio-net.c
 * and kernel/driver/e1000.c's IRQ handlers.  Centralising the prototype
 * in kernel/include/ipc/mbox.h follows the AGENTS.md source/header
 * symmetry rule (kernel/include/ipc/mbox.h ↔ kernel/ipc/mbox.c) and lets
 * any driver or subsystem wake the tcpip-thread mbox without copying
 * the extern into its own translation unit.
 *
 * Semantics: sys_mbox_wake() is ISR-safe and may be called from any
 * interrupt context.  It signals the tcpip_thread's core mbox
 * (g_tcpip_mbox in kernel/net/sys_arch.c) so the next sys_arch_mbox_fetch
 * returns promptly even if no API message is pending — the tcpip
 * thread then sweeps RX via net_poll_rx() before re-checking the mbox.
 *
 * Implementation lives in kernel/ipc/mbox.c.  The actual lock/flag
 * dance runs in kernel/net/sys_arch.c (where the mailbox state lives)
 * via sys_arch_mbox_set_idle_and_wake(); the ipc source owns the
 * public symbol and delegates the state access to sys_arch.c.
 */
#ifndef _KERNEL_IPC_MBOX_H
#define _KERNEL_IPC_MBOX_H

#include <stdint.h>

/* Wake the core tcpip mbox.  Sets the lost-wakeup guard and wakes
 * one wait_queue_wake_one(&mb->wq) waiter.  No-op if the core mbox
 * has not been created yet (i.e. tcpip_init has not run). */
void sys_mbox_wake(void);

#endif /* _KERNEL_IPC_MBOX_H */
