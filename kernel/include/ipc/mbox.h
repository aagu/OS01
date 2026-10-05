/* kernel/include/ipc/mbox.h — Mailbox wake IPC primitives.
 *
 * ARCH-9 Task 9: replaces the block-scope `extern void sys_mbox_wake(void);`
 * redeclarations that used to live inside kernel/driver/virtio-net.c
 * and kernel/driver/e1000.c's IRQ handlers.  Centralising the prototype
 * in kernel/include/ipc/mbox.h follows the AGENTS.md source/header
 * symmetry rule (kernel/ipc/mbox.h ↔ kernel/net/sys_arch.c) and lets
 * any driver or subsystem wake the tcpip-thread mbox without copying
 * the extern into its own translation unit.
 *
 * Semantics: sys_mbox_wake() is ISR-safe and may be called from any
 * interrupt context.  It signals the tcpip_thread's core mbox
 * (g_tcpip_mbox in kernel/net/sys_arch.c) so the next sys_arch_mbox_fetch
 * returns promptly even if no API message is pending — the tcpip
 * thread then sweeps RX via net_poll_rx() before re-checking the mbox.
 *
 * Implementation lives in kernel/net/sys_arch.c next to the rest of
 * the lwIP OS-adaptation surface (sys_arch_protect, sys_mbox_post,
 * etc.).  A duplicate definition here would cause a link error.
 */
#ifndef _KERNEL_IPC_MBOX_H
#define _KERNEL_IPC_MBOX_H

#include <stdint.h>

/* Wake the core tcpip mbox.  Sets the lost-wakeup guard and wakes
 * one wait_queue_wake_one(&mb->wq) waiter.  No-op if the core mbox
 * has not been created yet (i.e. tcpip_init has not run). */
void sys_mbox_wake(void);

#endif /* _KERNEL_IPC_MBOX_H */
