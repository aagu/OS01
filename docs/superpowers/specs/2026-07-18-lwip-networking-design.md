# lwIP Networking Stack Integration Design

**Date:** 2026-07-18
**Status:** Draft

## Overview

Integrate lwIP TCP/IP stack into OS01 to provide BSD-socket networking for userspace
applications, targeting busybox `wget` with both HTTP and HTTPS (mbedTLS) support.

The work is delivered in three independent phases, each with its own verification
milestone:
1. lwIP port + e1000 NIC driver → kernel log shows DHCP IP
2. Socket syscall layer → `wget http://example.com` succeeds
3. mbedTLS integration → `wget https://example.com` succeeds

## Architecture

```
userspace:  busybox wget
              ├─ HTTP  → tcp_socket / connect / read / write
              └─ HTTPS → mbedTLS → tcp_socket / connect / read / write
            ════════════ syscall interface ════════════
            libc wrappers: socket(2) / connect(2) / sendto(2) / recvfrom(2) ...

kernel:     file_t (FD_SOCKET) → socket_t → lwIP netconn → tcpip_thread
              └─ fd_poll integration (poll/select)

            lwIP core:  IP / TCP / UDP / DHCP / DNS
              └─ netif → e1000 driver → PCI MMIO → hardware IRQ

hardware:   QEMU -device e1000 (Intel PRO/1000 PCI NIC)
```

**Key decisions:**
- lwIP runs in **tcpip_thread mode** (dedicated kernel thread) — the standard "lwIP with OS" model
- Socket fd management via OS01's own `file_t` structure, **not** lwIP's built-in `sockets.c`
- mbedTLS is **userspace only** — static library linked into busybox, using the same socket syscalls as plain HTTP
- DNS resolution handled by busybox's built-in DNS client (UDP → `sendto`/`recvfrom`)

## Directory Layout

```
thirdpart/
  lwip/                     # git submodule — lwIP source
  mbedtls/                  # git submodule — mbedTLS source

kernel/
  net/                      # new directory
    net.c                   # subsystem registration, lwIP init, tcpip_thread start
    sys_arch.c              # lwIP OS adaptation layer (semaphore, mbox, thread, protect)
    socket.c                # socket_t allocation, syscall implementations
    lwipopts.h              # lwIP compile-time configuration
  driver/
    e1000.c                 # e1000 NIC driver
    e1000.h                 # register definitions, driver interface
  include/
    net/
      net.h                 # net subsystem header
      socket.h              # socket_t struct + socket API declarations

libc/include/
  sys/socket.h              # AF_INET / SOCK_STREAM / sockaddr / socket() wrappers
  netinet/in.h              # sockaddr_in / htons / ntohs / IPPROTO_TCP
  arpa/inet.h               # inet_aton / inet_ntoa
  netdb.h                   # gethostbyname stub (delegated to busybox DNS)

config/
  busybox-wget.config       # busybox config fragment for wget + mbedTLS TLS
  mbedtls.config            # mbedTLS minimal config for TLS 1.2 client
```

## Phase 1: lwIP Port + e1000 Driver

### 1.1 Submodule and Build

- `git submodule add https://git.savannah.nongnu.org/git/lwip.git thirdpart/lwip`
- lwIP sources compiled into kernel via `kernel/net/` — either a local Makefile fragment
  or by listing lwIP `.c` files directly. lwIP headers added to `ALL_CFLAGS`.
- `kernel/driver/e1000.c` auto-discovered by existing wildcard: `$(wildcard driver/*.c)`
- `kernel/net/*.c` — either add a new wildcard `$(wildcard net/*.c)` to `KERNEL_C_SOURCES`
  or add a dedicated Makefile in `kernel/net/`.

### 1.2 Init Sequence — Two-Stage Design (critical)

**`task_init()` in `kernel/sched/task.c:1285` calls `list_init(&init_task_union.task.list)`,
resetting the global task list.** Any kernel threads created via `tcpip_init()` →
`sys_thread_new()` → `create_kthread()` during Phase 6 would be lost when
`task_init()` wipes the list.

Therefore network init is split into two stages:

**Stage A — Phase 6 subsys (hardware only, no scheduler dependency):**
```c
// kernel/net/net.c
static int net_hw_init(void) {
    uint8_t bus, dev, func;
    if (pci_find_device(PCI_CLASS_NETWORK, PCI_SUBCLASS_ETHERNET, 0x00,
                        &bus, &dev, &func) != 0)
        return -1;  // no NIC → optional subsys

    int is_mmio, is_64bit;
    uint64_t bar_phys = pci_read_bar(bus, dev, func, 0, &is_mmio, &is_64bit);
    pci_enable_bus_mastering(bus, dev, func);
    pci_enable_mmio(bus, dev, func);
    uint8_t irq = pci_read_interrupt_line(bus, dev, func);

    e1000_init(bar_phys, irq);   // MMIO map + descriptor rings + IRQ registration
    return 0;
}

// In arch/x86_64/subsys.c or equivalent registration point:
register_subsys("net-hw", net_hw_init, SUBSYS_PHASE_6, SUBSYS_FLAG_OPTIONAL);
```

**Stage B — Between SMP and task_init (lwIP stack, scheduler required):**

In `kernel/kernel/main.c`, after `smp_boot_aps()` and `subsys_init_percpu()`,
but before `task_init()`:

```c
// kernel/kernel/main.c — in kernel_main(), after smp_boot_aps():
//   (existing: arch_register_subsys_percpu(); subsys_init_percpu();)

#ifdef OS01_SELFTEST
    // ... existing selftest block ...
#endif

    // ═══ Network stack init (post-SMP, pre-scheduler) ═══
    // lwIP creates kernel threads (tcpip_thread) — must happen
    // after SMP is up but BEFORE task_init() resets the task list.
    net_lwip_init();   // lwip_init() + tcpip_init(NULL, NULL)
```

`net_lwip_init()` is a separate function (in `kernel/net/net.c`) that calls
`lwip_init()` followed by `tcpip_init(NULL, NULL)`. It is called explicitly from
`kernel_main()`, NOT registered as a subsys. The tcpip_thread is created and
parked; `task_init()` then initializes the scheduler task list and the thread
starts running when `scheduler_ok = 1`.

### 1.3 e1000 Driver

**MMIO mapping — use vmm_map_page(), NOT Phy_To_Virt():**

PCI MMIO BAR addresses (typically 0xE0000000–0xFE000000 or above 4GB on x86_64)
are outside the kernel's identity-mapped RAM range. `Phy_To_Virt(x) = x + 0xffff800000000000`
only maps installed RAM. The correct approach follows the AHCI pattern
(`kernel/driver/ahci.c:124-128`):

```c
// Map the 2MB-aligned page containing the BAR into the kernel page table
uint64_t bar_page = bar_phys & PAGE_2M_MASK;
vmm_map_page(kernel_map, bar_page,
             (uintptr_t)Phy_To_Virt(bar_page), PAGE_KERNEL_MMIO);
// Then access registers via Phy_To_Virt(bar_phys) + offset
```

`PAGE_KERNEL_MMIO` includes `PAGE_PCD | PAGE_PWT` to disable caching for MMIO space.

**Initialization sequence:**
1. Map MMIO base via `vmm_map_page()` with `PAGE_KERNEL_MMIO` (see above).
2. Read MAC from EEPROM (`EERD` register at offset `0x14`).
3. Allocate RX/TX descriptor rings via `alloc_pages()`. Descriptors must be
   physically contiguous and 16-byte aligned. The physical address is passed to
   `RDBAL`/`RDBAH` and `TDBAL`/`TDBAH`; the driver uses `Phy_To_Virt()` to
   access them since they are in RAM.
4. Configure RX (`RDBAL`/`RDBAH`/`RDLEN`/`RDH`/`RDT`) and TX
   (`TDBAL`/`TDBAH`/`TDLEN`/`TDH`/`TDT`).
5. Configure `RCTL` (enable, broadcast, BSIZE=2048, strip CRC) and `TCTL`
   (enable, CT=0x10, COLD=0x40).
6. Enable interrupts via `IMS` (RX timer, RX descriptor minimum, transmitter
   empty, link status change).
7. Register interrupt handler via `register_irq()` +
   `DEFINE_INTR_STUB`/`REGISTER_INTR_HANDLER`.

**Transmit path:** `e1000_xmit(struct pbuf *p)` — lwIP calls this from
tcpip_thread context:

1. Find a free TX descriptor: check `TDT` (tail pointer) vs `TDH` (head pointer).
   If the ring is full (TDT + 1 == TDH modulo ring size), return `ERR_MEM` —
   lwIP will retry.
2. Copy `pbuf` chain data into the descriptor buffer.
3. Set descriptor flags: `E1000_TXD_CMD_RS | E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS`.
   `RS` (Report Status) ensures a TX completion interrupt fires.
4. Advance `TDT`, write `E1000_REG_TDT` to trigger the hardware.

**TX completion interrupt:** When ICR shows `E1000_ICR_TXDW`, walk descriptors
from `TDH` to `TDT`: for each with DD (Descriptor Done) set, clear DD and mark
the descriptor free. Freed descriptors become available for `e1000_xmit`.

**Receive path:** Interrupt handler → check `ICR` → while `RDT`+1 != `RDH`
and `RXD_STAT_DD` is set → `pbuf_alloc(PBUF_RAW, length, PBUF_POOL)` →
memcpy from descriptor buffer to pbuf → `tcpip_inpkt(pbuf, netif, e1000_input)`
→ advance `RDT`, write `E1000_REG_RDT`.

**RX descriptor starvation:** lwIP `pbuf_alloc` may fail under memory pressure.
Count dropped packets and continue — the descriptor stays owned by hardware
(DD cleared) and will be reused when the hardware wraps around.

**QEMU command line addition:**
```
-netdev user,id=net0 -device e1000,netdev=net0
```

**PCI class constants:** Add to `kernel/include/driver/pci.h`:
```c
#define PCI_CLASS_NETWORK          0x02
#define PCI_SUBCLASS_ETHERNET      0x00
```

QEMU `-device e1000` presents as Intel 82540EM: class=0x02, subclass=0x00,
prog-if=0x00. The existing `pci_find_device()` scan correctly identifies it.

### 1.4 lwIP sys_arch Adaptation

lwIP requires OS primitives via `sys_arch.c`. Mapping to OS01:

| lwIP function | OS01 implementation |
|---|---|
| `sys_sem_new()` | spinlock + counter + wait_queue (custom semaphore struct) |
| `sys_sem_signal()` | increment counter, wake one waiter via wait_queue_wake_one |
| `sys_sem_free()` | no-op (statically allocated) |
| `sys_mbox_new()` | ring buffer (64 slots) + wait_queue for blocking fetch |
| `sys_mbox_post()` | enqueue message + wait_queue_wake_one; if full, spin briefly then fail |
| `sys_mbox_fetch()` | dequeue; block via wait_queue_sleep if empty |
| `sys_thread_new()` | `sched_task_create(name, fn, arg)` → kernel thread. **Name must be strdup'd** — lwIP may pass stack-local strings. |
| `sys_now()` | `jiffies * 10` (100 Hz tick → ms). **Must return 0 before PIT is running** — this is fine; lwIP timers don't fire until `sys_now()` advances. The PIT starts in Phase 4, well before `net_lwip_init()` in Stage B. |
| `sys_arch_protect()` | Recursive IRQ-disable (see below) |

**sys_arch_protect MUST be recursive.** lwIP may call `sys_arch_protect()`
multiple times on a single code path (e.g., `tcp_input()` acquires protection,
calls into `tcp_output()` which also acquires it). A plain spinlock would deadlock
on the second acquisition:

```c
// kernel/net/sys_arch.c
static spinlock_T  lwip_global_lock = SPINLOCK_RELEASED;
static volatile int protect_nest = 0;
static uint64_t     protect_flags = 0;

sys_prot_t sys_arch_protect(void) {
    if (protect_nest == 0) {
        // IRQ-save is required: a 100 Hz timer tick during a lwIP critical
        // section may set need_resched; ret_from_intr could then switch to
        // another kernel thread that also enters lwIP, deadlocking on
        // lwip_global_lock. Disabling preemption prevents this.
        protect_flags = spin_lock_irqsave(&lwip_global_lock);
    }
    protect_nest++;
    return protect_flags;
}

void sys_arch_unprotect(sys_prot_t pval) {
    (void)pval;
    if (protect_nest <= 0) return;  // safety
    if (--protect_nest == 0)
        spin_unlock_irqrestore(&lwip_global_lock, protect_flags);
}
```

**Rationale for IRQ-save:** The e1000 RX interrupt handler only calls
`tcpip_inpkt()` → mbox enqueue (its own lock), so it never contends on
`lwip_global_lock`. However, the 100 Hz PIT timer interrupt can set
`need_resched` inside a lwIP critical section; `ret_from_intr` →
`schedule()` could then switch to another kernel thread that also enters
lwIP, deadlocking on a plain `spin_lock`. `spin_lock_irqsave` disables
interrupts on this CPU, preventing preemption while the lock is held.

### 1.5 lwIP Configuration (`lwipopts.h`)

Key settings:
```c
#define NO_SYS                  0   // OS mode
#define MEM_LIBC_MALLOC         1   // use OS01 kmalloc() instead of internal heap
#define LWIP_NETCONN            1   // netconn API (needed for socket layer)
#define LWIP_SOCKET             0   // don't use lwIP's own socket layer
#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define LWIP_TCP                1
#define LWIP_UDP                1
#define LWIP_DHCP               1
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
#define LWIP_NETIF_HOSTNAME     "os01"
```

**Verification:** Boot kernel, watch serial log for:
```
PCI: scanning for class=02 subclass=00 progIF=00
PCI: found at 0:3.0
e1000: MAC xx:xx:xx:xx:xx:xx
netif: link up
dhcp: bound to 10.0.2.15
```

## Phase 2: Socket Syscall Layer

### 2.1 File Descriptor Extension

```c
// kernel/include/kernel/file.h — additions

enum file_type {
    FD_NONE = 0,
    FD_VFS,
    FD_PIPE,
    FD_DEV,
    FD_SOCKET,    // new
};

typedef struct socket {
    void        *conn;         // lwIP struct netconn *
    int          domain;       // AF_INET (2)
    int          type;         // SOCK_STREAM (1) or SOCK_DGRAM (2)
    int          protocol;     // IPPROTO_TCP (6) or IPPROTO_UDP (17)
    int          state;        // UNCONNECTED/CONNECTED/LISTENING/CLOSED
    int          bound;        // 1 if bind() was called
    spinlock_T   lock;
    list_t       poll_list;    // poll_wait_entry_t chain
} socket_t;

// file_t gets new field:
typedef struct file {
    // ... existing fields ...
    socket_t       *sock;      // valid when type == FD_SOCKET
} file_t;
```

**Socket lifecycle and cleanup:** `file_free()` and `fd_close()` must handle
`FD_SOCKET`:

```c
// In file_free() (kernel/fs/file.c:22-28):
// After the existing vfs_node_put() for FD_VFS/FD_DEV, add:
if (f->type == FD_SOCKET && f->sock) {
    if (f->sock->conn)
        netconn_delete(f->sock->conn);  // release lwIP netconn
    free(f->sock);                       // free socket_t
}

// In fd_close() (kernel/fs/file.c:118-150):
// The existing path (fd=NULL, refcount--, file_free) already reaches
// file_free(), so the above cleanup suffices. The pipe-specific
// reader/writer decrement path is untouched — FD_SOCKET files don't
// enter that branch.
```

**`make clean` mandatory** after adding `FD_SOCKET` to `enum file_type` and
`socket_t *sock` to `file_t` — struct layout changes, stale `.o` files will
have wrong `sizeof()`.

### 2.2 New Syscalls (51–59)

```c
// kernel/include/uapi/syscall.h — additions

#define SYS_socket        51
#define SYS_bind          52
#define SYS_connect       53
#define SYS_listen        54
#define SYS_accept        55
#define SYS_sendto        56
#define SYS_recvfrom      57
#define SYS_setsockopt    58
#define SYS_getsockopt    59
```

Each syscall maps directly to lwIP's netconn API:

| Syscall | lwIP netconn API | Notes |
|---|---|---|
| `socket(domain, type, protocol)` | `netconn_new_with_proto_and_callback(type, proto, NULL)` → `socket_t` → `file_t` → `fd_alloc` | Returns fd |
| `bind(fd, addr, addrlen)` | `netconn_bind(sock->conn, ip, port)` | |
| `connect(fd, addr, addrlen)` | `netconn_connect(sock->conn, ip, port)` | Blocks via netconn semaphore |
| `listen(fd, backlog)` | `netconn_listen(sock->conn)` | `sock->state = LISTENING` |
| `accept(fd, addr, addrlen)` | `netconn_accept(sock->conn, &new_conn)` | New `socket_t` + `file_t` + `fd_alloc` |
| `sendto(fd, buf, len, flags, addr, addrlen)` | `netconn_send(sock->conn, netbuf)` or `netconn_write` for TCP | |
| `recvfrom(fd, buf, len, flags, addr, addrlen)` | `netconn_recv(sock->conn, &netbuf)` | Extract data from netbuf |
| `setsockopt(fd, ...)` | `netconn_set_*` or lwIP `ip_set_option` | SO_REUSEADDR, etc. |
| `getsockopt(fd, ...)` | lwIP query APIs | |

### 2.3 Linux → OS01 Syscall Translation (critical for busybox)

Busybox is compiled for Linux x86_64 and issues Linux syscall numbers. OS01
translates them via `linux_to_os01[256]` in `kernel/arch/x86_64/trap.c:808-851`.
Socket syscalls must be added:

```c
// kernel/arch/x86_64/trap.c — linux_to_os01[] additions

[41] = SYS_socket,          // Linux socket    → OS01 51
[42] = SYS_connect,         // Linux connect   → OS01 53
[43] = SYS_accept,          // Linux accept    → OS01 55
[44] = SYS_sendto,          // Linux sendto    → OS01 56
[45] = SYS_recvfrom,        // Linux recvfrom  → OS01 57
[49] = SYS_bind,            // Linux bind      → OS01 52
[50] = SYS_listen,          // Linux listen    → OS01 54
[54] = SYS_setsockopt,      // Linux setsockopt → OS01 58
[55] = SYS_getsockopt,      // Linux getsockopt → OS01 59
```

Without these mappings, busybox `wget` calls Linux socket syscall numbers
directly and gets `-ENOSYS`.

### 2.4 Transparent read/write on Sockets

Existing `fd_read`/`fd_write` get a `FD_SOCKET` case so `SYS_read`/`SYS_write` work
without new syscalls. wget uses `read()`/`write()` for TCP streams after connect.

```c
// In fd_read():
case FD_SOCKET: {
    struct netbuf *nb;
    err_t err = netconn_recv(f->sock->conn, &nb);
    if (err == ERR_OK) {
        void *data; u16_t len;
        netbuf_data(nb, &data, &len);
        size_t copy = (len < size) ? len : size;
        memcpy(buf, data, copy);
        netbuf_delete(nb);
        return (int64_t)copy;
    }
    if (err == ERR_CLSD) return 0;  // EOF
    // ERR_TIMEOUT / other → return -1 (or block)
    // After netconn_recv returns, check for fatal signals:
    if (signal_pending_fatal())
        return -EINTR;
}

// In fd_write():
case FD_SOCKET: {
    err_t err = netconn_write(f->sock->conn, buf, (u16_t)size, NETCONN_COPY);
    return (err == ERR_OK) ? (int64_t)size : -1;
}
```

### 2.5 Poll / Select Integration

Extend `fd_poll()` in `kernel/fs/poll.c`:

```c
case FD_SOCKET: {
    socket_t *s = f->sock;
    uint32_t revents = 0;

    // Check readable: data available or listening socket has pending accept
    // lwIP netconn has internal recvmbox — non-empty → readable
    // Check writable: TCP connected & send buffer not full

    if (!revents) {
        // Not ready — register on socket's poll_list for wakeup
        poll_wait(pt, &s->poll_list, &s->lock);
    }
    return revents;
}
```

When lwIP delivers data to a netconn, a callback wakes poll waiters on `s->poll_list`.

**Select:** `SYS_select` (50) currently returns `-ENOSYS`. Phase 2 includes a
`do_select()` implementation — a thin wrapper that converts `fd_set` bitmaps
to `struct pollfd[]`, calls `do_poll()`, then converts `revents` back to
`fd_set`. Approximately 30 lines:

```c
int64_t do_select(int nfds, fd_set *readfds, fd_set *writefds,
                  fd_set *exceptfds, struct timeval *timeout) {
    struct pollfd pfds[POLL_MAX_FDS];
    int n = 0;
    for (int fd = 0; fd < nfds && n < POLL_MAX_FDS; fd++) {
        short events = 0;
        if (readfds  && (readfds->bits[fd / 64]  & (1ULL << (fd % 64))))  events |= POLLIN;
        if (writefds && (writefds->bits[fd / 64] & (1ULL << (fd % 64)))) events |= POLLOUT;
        if (events) { pfds[n].fd = fd; pfds[n].events = events; n++; }
    }
    int timeout_ms = (timeout) ? (timeout->tv_sec * 1000 + timeout->tv_usec / 1000) : -1;
    int64_t ret = do_poll(pfds, n, timeout_ms);
    if (ret > 0 && readfds)  FD_ZERO(readfds);
    if (ret > 0 && writefds) FD_ZERO(writefds);
    if (ret > 0) {
        for (int i = 0; i < n; i++) {
            if (pfds[i].revents & (POLLIN|POLLHUP|POLLERR))
                if (readfds)  FD_SET(pfds[i].fd, readfds);
            if (pfds[i].revents & (POLLOUT|POLLHUP|POLLERR))
                if (writefds) FD_SET(pfds[i].fd, writefds);
        }
    }
    return ret;
}
```

This is included in Phase 2 regardless of whether busybox wget uses poll or
select — it's trivial once `do_poll()` exists, and avoids a late-discovery
build failure at the `make test` stage.
logic; `do_select()` would be ~30 lines of fd_set ↔ pollfd conversion.

### 2.6 Userspace libc Headers

Minimal BSD socket headers in `libc/include/`:
- `sys/socket.h` — `socket()`, `connect()`, `bind()`, `listen()`, `accept()`, `sendto()`,
  `recvfrom()`, `send()`, `recv()`, `AF_INET`, `SOCK_STREAM`, `SOCK_DGRAM`, `struct sockaddr`
- `netinet/in.h` — `struct sockaddr_in`, `IPPROTO_TCP`, `IPPROTO_UDP`, `htons`/`ntohs`/`htonl`/`ntohl`
- `arpa/inet.h` — `inet_aton()`, `inet_ntoa()`
- `netdb.h` — `getaddrinfo()` / `gethostbyname()` wrappers using OS01 socket syscalls

**DNS resolution note:** busybox `wget` with `FEATURE_WGET_HTTPS` uses
`getaddrinfo()` (not direct UDP DNS). The `netdb.h` wrapper must implement
a working `getaddrinfo()` by sending DNS queries via UDP sockets:
`socket(AF_INET, SOCK_DGRAM) → sendto(DNS server) → recvfrom() → parse reply`.
A stub returning NULL will cause wget HTTPS to fail at name resolution.

System call wrappers follow the existing pattern in OS01 libc (inline asm `int $0x80`).

**Verification:** `wget http://example.com -O /tmp/test.html` downloads the page.

## Phase 3: mbedTLS Integration

### 3.1 Architecture

mbedTLS is a **userspace static library**. It compiles with the OS01 cross toolchain
and links into busybox at build time. No kernel changes needed.

### 3.2 Submodule and Build

- `git submodule add https://github.com/Mbed-TLS/mbedtls.git thirdpart/mbedtls`
- Build as static library: `thirdpart/mbedtls/library/*.c` → `$(SYSROOT)/usr/lib/libmbedtls.a`
- Headers installed to `$(SYSROOT)/usr/include/mbedtls/`
- Root `Makefile` gains a `thirdpart/mbedtls/libmbedtls.a` target (similar to busybox build)

### 3.3 Configuration (`config/mbedtls.config`)

Minimal TLS 1.2 client-only config:

```c
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_SSL_CLI_C            // TLS client
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_X509_CRT_PARSE_C     // certificate parsing
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_AES_C                // AES-GCM for TLS 1.2 cipher suites
#define MBEDTLS_GCM_C
#define MBEDTLS_SHA256_C             // SHA-256
#define MBEDTLS_ECDSA_C              // ECDSA cert verification
#define MBEDTLS_ECP_C                // ECDHE key exchange
#define MBEDTLS_ECDH_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CTR_DRBG_C           // random number generator
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_NO_PLATFORM_ENTROPY  // use custom entropy callback (no /dev/urandom)
#define MBEDTLS_NET_C                // uses OS01 socket syscalls
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C

// Everything else #undef
```

### 3.4 Busybox Configuration

Update `config/busybox.config`:
```
CONFIG_WGET=y
CONFIG_FEATURE_WGET_HTTPS=y
CONFIG_FEATURE_WGET_OPENSSL=n
CONFIG_TLS=y
CONFIG_TLS_MBEDTLS=y
```

### 3.5 TLS Data Flow

```
wget https://example.com/foo
  → socket(AF_INET, SOCK_STREAM, 0)    → fd=3
  → connect(3, 93.184.216.34:443)       → TCP handshake via lwIP
  → mbedtls_ssl_init()
  → mbedtls_ssl_setup(SSL_IS_CLIENT)
  → mbedtls_ssl_handshake()            → TLS 1.2 handshake over fd 3
  → mbedtls_ssl_write("GET /foo HTTP/1.0\r\n...")
  → mbedtls_ssl_read(response)
  → close(3)
```

All mbedTLS socket I/O goes through `mbedtls_net_send()`/`mbedtls_net_recv()` which
call OS01's `send()`/`recv()` syscall wrappers.

**mbedTLS entropy source:** OS01 has no `/dev/urandom`. Configure mbedTLS
to use `MBEDTLS_NO_PLATFORM_ENTROPY` and provide a custom entropy callback that
collects from: `rdtsc()` readings, jiffies, and the kernel's `__stack_chk_guard`
(which was seeded from `rdtsc()` at boot). This is acceptable for a hobby OS
where cryptographic strength is not the primary concern.

**Verification:** `wget https://example.com -O /tmp/test.html` downloads the page.

## Error Handling

| Scenario | Behavior |
|---|---|
| No e1000 NIC found | Subsys registered with `SUBSYS_FLAG_OPTIONAL`; boot continues |
| e1000 IRQ registration fails | `net_hw_init()` returns -1; subsys skipped |
| DHCP timeout / no IP | lwIP keeps retrying; socket operations return errors |
| connect() to unreachable host | lwIP TCP timeout → `ERR_TIMEOUT` → `-ETIMEDOUT` to userspace |
| Socket fd exhausted | `fd_alloc()` returns -1 → `-ENFILE` |
| mbedTLS cert verify failure | mbedTLS returns error → wget exits with message |

## Backward Compatibility

- No existing syscall numbers changed. New syscalls 51-59 are net-new.
- Existing `file_t` layout unchanged; `sock` field is NULL for all existing fd types.
- No change to boot flow when NIC is absent (optional subsys).
- Phase 1 does not require any userspace changes — kernel-only.

## Testing Strategy

| Phase | Test |
|---|---|
| Phase 1 | Kernel boot with `-device e1000`; verify DHCP IP in serial log |
| Phase 1 | Boot without NIC (`-device e1000` omitted); verify clean boot (optional subsys) |
| Phase 2 | `wget http://example.com` from busybox shell |
| Phase 2 | `wget http://httpbin.org/get` (validates HTTP response parsing) |
| Phase 3 | `wget https://example.com` from busybox shell |
| Phase 3 | `wget https://httpbin.org/get` (validates TLS 1.2) |

Future: systest cases for socket/bind/connect/send/recv loopback pattern.
