/* kernel/include/net/lwip.h — Unified lwIP stack adapter and readiness interface */
#ifndef _NET_LWIP_H
#define _NET_LWIP_H

#include <stdint.h>
#include <stdbool.h>

enum net_service_state {
    NET_OFF = 0,
    NET_STARTING,
    NET_ONLINE,
    NET_FAILED,
};

/* Stack readiness: true only when tcpip core is confirmed and >=1 adapter is bound */
bool net_service_ready(void);

/* Default interface IPv4 address in network byte order (0 if not ready or no interface) */
uint32_t net_default_ipv4(void);

/* Post-SMP network stack bring-up */
void net_lwip_start(void);

/* Query internal service state */
enum net_service_state net_service_get_state(void);

/* Reset state for tests or re-initialization */
void net_lwip_reset_state(void);

#ifdef OS01_HOST_TEST
/* ── Test-only hooks (Task 9 mailbox + readiness state) ──────── */
/* These expose the core mailbox and application mailbox paths so
 * the host test fixture can drive them without standing up a real
 * tcpip_thread.  They are weak-instrumented in production code via
 * OS01_HOST_TEST. */
extern int fake_core_mailbox_msg_count;
extern int fake_app_mailbox_msg_count;
extern int fake_core_fetch_rx_sweeps;
extern int fake_core_mailbox_lock_check;
extern int fake_pci_drivers_count;

void net_service_force_state_for_test(enum net_service_state s);
void net_service_set_default_ipv4_for_test(uint32_t ip);

void net_service_core_mbox_post_for_test(void *msg);
void net_service_app_mbox_post_for_test(void *msg);
int  net_service_drain_core_mailbox_for_test(void);
int  net_service_drain_app_mailbox_for_test(void);
int  net_service_core_mbox_fetch_one_for_test(void);

/* Refresh + return the count of declared PCI_DRIVER_DECLARE entries. */
int fake_pci_drivers_count_for_test(void);

/* Drive pci_bind_all() with two NICs in the fixture and return the
 * ndev registration count.  The real matcher is used. */
int test_pci_bind_dual_nic_count_for_test(void);
#endif

#endif /* _NET_LWIP_H */