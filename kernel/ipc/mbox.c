/* kernel/ipc/mbox.c — Mailbox wake IPC primitive.
 *
 * ARCH-9 Task 9 (round-1 fix): extraction to satisfy the plan's
 *   "新源目录/公开头目录对称" global constraint
 *   (kernel/include/ipc/mbox.h ↔ kernel/ipc/mbox.c).
 *
 * sys_mbox_wake() is the ISR-safe entry point used by every NIC
 * driver (kernel/driver/e1000.c, kernel/driver/virtio-net.c) to
 * wake the tcpip thread after RX has filled the ring.  It must
 * be safe to call from any IRQ context and a no-op before
 * tcpip_init() has created the core mailbox.
 *
 * The actual mailbox struct (os_mbox_t) and its global pointer
 * (g_tcpip_mbox) are private to kernel/net/sys_arch.c — exposing
 * them across the ipc/ boundary would drag lwIP OS-layer internals
 * into a public-facing module.  So this source delegates the
 * real work to sys_arch_mbox_set_idle_and_wake() (kernel/net/
 * sys_arch.c).  The delegation is a single function call: this
 * module owns the public symbol, sys_arch.c owns the state.
 */
#include <ipc/mbox.h>

/* Implemented in kernel/net/sys_arch.c. */
extern void sys_arch_mbox_set_idle_and_wake(void);

void sys_mbox_wake(void)
{
    sys_arch_mbox_set_idle_and_wake();
}
