// kernel/net/net.c — network subsystem initialization
//
// Two-stage init:
//   Stage A (Phase 6 subsys): e1000 hardware init only (no scheduler)
//   Stage B (post-SMP, pre-task_init): lwIP stack + tcpip_thread

#include <net/net.h>
#include <driver/e1000.h>
#include <driver/pci.h>
#include <kernel/subsys.h>
#include <kernel/debug.h>
#include <kernel/memory.h>  // Phy_To_Virt
#include <kernel/slab.h>    // kmalloc, kfree
#include <string.h>
#include <errno.h>
#include "lwip/init.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"

// ── Static state ───────────────────────────────────────────────
static int  net_hw_ok = 0;
static uint8_t e1000_irq = 0;
static uint64_t e1000_bar = 0;

// The single OS01 network interface
struct netif os01_netif;

// ── Stage A: Hardware init (Phase 6 subsys, no scheduler) ────

int net_hw_init(void)
{
    uint8_t bus, dev, func;

    if (pci_find_device(PCI_CLASS_NETWORK, PCI_SUBCLASS_ETHERNET, 0x00,
                        &bus, &dev, &func) != 0) {
        debug_block("net: no e1000 NIC found\n");
        return -1;  // optional subsys — boot continues without net
    }

    int is_mmio, is_64bit;
    e1000_bar = pci_read_bar(bus, dev, func, 0, &is_mmio, &is_64bit);
    // Note: is_64bit is ignored — e1000 BAR0 is always MMIO on QEMU.
    // pci_read_bar() already handles the 64-bit read internally.
    pci_enable_bus_mastering(bus, dev, func);
    pci_enable_mmio(bus, dev, func);
    e1000_irq = pci_read_interrupt_line(bus, dev, func);

    if (e1000_init(e1000_bar, e1000_irq, 0) != 0) {
        debug_block("net: e1000 init failed\n");
        return -EIO;
    }

    net_hw_ok = 1;
    return 0;
}

// ── Stage B: lwIP stack init (post-SMP, pre-task_init) ────────

void net_lwip_init(void)
{
    if (!net_hw_ok) return;

    // tcpip_init() calls lwip_init() internally — do NOT call
    // lwip_init() here.  A second call re-initializes sys_timeouts
    // and hangs in sys_timeout() on the second pass through
    // sys_timeouts_init().

    // Start the tcpip_thread FIRST — lwIP's core processing thread.
    // DHCP responses arrive via tcpip_inpkt → tcpip_mbox → tcpip_thread.
    // If tcpip_init() runs after dhcp_start(), the mbox doesn't exist yet
    // and DHCP responses are silently dropped → timeout → no IP.
    tcpip_init(NULL, NULL);

    // Bring up the single netif
    // e1000_netif_init is an init callback exported from e1000.c.
    // It sets MAC, MTU, flags, and linkoutput — all mandatory for
    // lwIP's etharp_output to work.
    struct netif *nif = &os01_netif;
    if (!netif_add(nif, NULL, NULL, NULL, NULL, e1000_netif_init, tcpip_input)) {
        log_info("net: netif_add failed\n");
        return;
    }

    netif_set_default(nif);
    netif_set_up(nif);

    // Start DHCP — tcpip_thread is already running, can process responses
    // Static IP for QEMU user-mode: 10.0.2.15/24 gw 10.0.2.2
    {
        ip4_addr_t ip, mask, gw;
        IP4_ADDR(&ip,  10, 0, 2, 15);
        IP4_ADDR(&mask, 255, 255, 255, 0);
        IP4_ADDR(&gw,  10, 0, 2, 2);
        netif_set_addr(&os01_netif, &ip, &mask, &gw);
        netif_set_link_up(&os01_netif);
        log_info("net: static IP 10.0.2.15/24\n");
    }
    // Also try DHCP
    dhcp_start(nif);

    log_info("net: lwIP stack initialized, DHCP started\n");
}
