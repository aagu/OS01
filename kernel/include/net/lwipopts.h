// kernel/include/net/lwipopts.h — OS01 lwIP compile-time configuration
#ifndef LWIP_OPTS_H
#define LWIP_OPTS_H

#define NO_SYS                  0   // OS mode
#define MEM_LIBC_MALLOC         1   // use OS01 kmalloc() instead of internal heap
#define LWIP_NETCONN            1   // netconn API (needed for socket layer)

// Core locking OFF: netconn API functions run inside the tcpip_thread
// via message passing (tcpip_api_call posts a TCPIP_MSG_API_CALL and
// waits on a semaphore).  With LWIP_TCPIP_CORE_LOCKING=1 the API call
// executes in the CALLER's context while taking the core lock — but
// tcpip_thread holds that lock while processing timers/messages, so a
// caller blocked on LOCK_TCPIP_CORE() can never post its message:
// classic deadlock (socktest's connect() hangs forever).  Message
// passing is also what our sys_arch mbox implementation expects.
#define LWIP_TCPIP_CORE_LOCKING     0
#define LWIP_TCPIP_CORE_LOCKING_INPUT 0
#define LWIP_SOCKET             0   // don't use lwIP's own socket layer
#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define LWIP_TCP                1
#define LWIP_UDP                1
#define LWIP_DHCP               1
// Disable RFC5227 address conflict detection during DHCP.  It is enabled by
// default (LWIP_DHCP_DOES_ACD_CHECK == LWIP_DHCP) and, with our NIC's
// MAC-derived probe delays, takes ~10.6s of PROBE/ANNOUNCE traffic before
// dhcp_bind() sets the lease.  That races the guest's 10s DHCP wait in
// nettest and intermittently loses the bind.  QEMU user-mode NAT serves a
// single guest on 10.0.2.20 (dhcpstart=10.0.2.20), so no conflict can occur;
// bind immediately after ACK instead.
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_DNS                1
#define LWIP_ARP                1
#define LWIP_ICMP               1
#define LWIP_RAW                0
#define MEM_ALIGNMENT           8
// MEM_SIZE not needed — MEM_LIBC_MALLOC uses OS01 kmalloc()
#define MEMP_NUM_TCP_SEG        32
#define MEMP_NUM_TCP_PCB        8
#define MEMP_NUM_UDP_PCB        4
#define MEMP_NUM_NETCONN        12
#define PBUF_POOL_SIZE          16
#define TCP_MSS                 1460
#define TCP_SND_BUF             (8 * TCP_MSS)
#define TCP_WND                 (8 * TCP_MSS)
// LWIP_NETIF_HOSTNAME: 0 = disable hostname field (avoids DNS code path
// that tries to access netif->hostname)
#define LWIP_NETIF_HOSTNAME     0

// Prevent lwIP from including OS userspace headers (<unistd.h> etc.)
// that would conflict with kernel headers (struct timeval, etc.)
#define LWIP_NO_UNISTD_H        1

// ── Debug output ──────────────────────────────────────────
#define LWIP_DEBUG 1
// LWIP_PLATFORM_DIAG maps to log_info (see arch/cc.h), so these
// are always visible regardless of NDEBUG.
// Note: numeric value 0x80 = LWIP_DBG_ON (lwip/debug.h isn't
// included yet at lwipopts.h parse time, so the macro isn't available).
#define DHCP_DEBUG                      (LWIP_DBG_ON | LWIP_DBG_TRACE | LWIP_DBG_STATE)
#define LWIP_NETIF_DEBUG    0x80
#define LWIP_ETHARP_DEBUG   (0x80 | 0x08 | 0x10)  // ON | TRACE | STATE
#define LWIP_ARP_DEBUG      0x80

#endif // LWIP_OPTS_H