// kernel/net/net.c — network subsystem initialization
//
// Two-stage init:
//   Stage A (Phase 6 subsys): hardware init (e1000 or virtio-net)
//   Stage B (post-SMP, pre-task_init): lwIP stack + tcpip_thread
//
// Supports: Intel e1000 and VirtIO-net (preferred).

#include <net/net.h>
#include <driver/e1000.h>
#include <driver/virtio-net.h>
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
int   net_hw_ok = 0;
int   is_virtio = 0;       // 1 = virtio-net, 0 = e1000
static uint8_t  nic_gsi = 0;
static uint64_t nic_bar = 0;
// Forward declaration
extern void e1000_process_rx(void);
static uint8_t  nic_bus = 0;      // for virtio init
static uint8_t  nic_dev = 0;
static uint8_t  nic_func = 0;

// The single OS01 network interface
struct netif os01_netif;

// ── Stage A: Hardware init (Phase 6 subsys, no scheduler) ────

int net_hw_init(void)
{
    uint8_t bus, dev, func;

    if (pci_find_device(PCI_CLASS_NETWORK, PCI_SUBCLASS_ETHERNET, 0x00,
                        &bus, &dev, &func) != 0) {
        debug_block("net: no network NIC found\n");
        return -1;
    }

    // Read vendor/device to determine NIC type
    uint32_t vendor_dev = pci_config_read(bus, dev, func, 0);
    uint32_t vendor = vendor_dev & 0xFFFF;
    uint32_t device = (vendor_dev >> 16) & 0xFFFF;

    int is_mmio, is_64bit;
    nic_bar = pci_read_bar(bus, dev, func, 0, &is_mmio, &is_64bit);
    pci_enable_bus_mastering(bus, dev, func);
    pci_enable_mmio(bus, dev, func);
    nic_gsi = pci_get_gsi(bus, dev, func);
    nic_bus = bus;
    nic_dev = dev;
    nic_func = func;

    if (vendor == VIRTIO_PCI_VENDOR_ID) {
        // VirtIO-net (0x1AF4:0x1000 or 0x1041)
        is_virtio = 1;
        log_info("net: VirtIO-net NIC found (vendor=0x%x device=0x%x)\n",
                 vendor, device);
        if (virtio_net_init(nic_bar, bus, dev, func, nic_gsi) != 0) {
            debug_block("net: virtio-net init failed\n");
            return -EIO;
        }
    } else {
        // Assume e1000 (0x8086:0x100E or similar)
        is_virtio = 0;
        if (e1000_init(nic_bar, nic_gsi, 0) != 0) {
            debug_block("net: e1000 init failed\n");
            return -EIO;
        }
    }

    net_hw_ok = 1;
    return 0;
}

// ── RX poll dispatcher (used by sys_arch mbox_fetch polling) ─
void net_poll_rx(void)
{
    if (!net_hw_ok) return;
    if (is_virtio)
        virtio_net_poll_rx();
    else {
        e1000_poll_rx();         // IRQ context: just buffer the packet
        e1000_process_rx();      // tcpip_thread context: process buffered packet
    }
}

// ── Stage B: lwIP stack init (post-SMP, pre-task_init) ────────

void net_lwip_init(void)
{
    if (!net_hw_ok) return;

    tcpip_init(NULL, NULL);

    // Choose the right netif_init callback
    netif_init_fn init_fn = is_virtio ? virtio_netif_init : e1000_netif_init;

    struct netif *nif = &os01_netif;
    if (!netif_add(nif, NULL, NULL, NULL, NULL, init_fn, tcpip_input)) {
        log_info("net: netif_add failed\n");
        return;
    }

    netif_set_default(nif);
    netif_set_up(nif);

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
    // Also try DHCP (works over virtio-net when everything is connected)
    dhcp_start(nif);

    log_info("net: lwIP stack initialized, DHCP started\n");
}
