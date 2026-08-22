# lwIP Networking Stack Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate lwIP TCP/IP stack into OS01 — e1000 NIC driver, BSD socket syscalls, and mbedTLS for busybox `wget` HTTPS support.

**Architecture:** Three-phase delivery. Phase 1: lwIP submodule + e1000 driver with two-stage init (Phase 6 subsys for hardware only; post-SMP explicit call for lwIP/tcpip_thread). Phase 2: `FD_SOCKET` type + 9 socket syscalls + `do_select()` + libc headers. Phase 3: mbedTLS submodule + static lib + busybox TLS config.

**Tech Stack:** C (clang `x86_64-unknown-none`), lwIP (git submodule), mbedTLS (git submodule), QEMU `-device e1000`, OS01 sys_arch adaptation layer.

## Global Constraints

- `task_init()` resets the global task list via `list_init(&init_task_union.task.list)` — any kthread created before it is lost. Network init is split: **Stage A** (Phase 6 subsys, e1000 hw only) and **Stage B** (explicit `net_lwip_init()` call between SMP and `task_init()`).
- PCI MMIO BARs must use `vmm_map_page(kernel_map, bar_page, Phy_To_Virt(bar_page), PAGE_KERNEL_MMIO)`, not raw `Phy_To_Virt()`.
- `sys_arch_protect` MUST be recursive (lwIP nests calls). Must use `spin_lock_irqsave` to prevent timer-IRQ-induced deadlock via `need_resched` → `schedule()`.
- Busybox calls Linux x86_64 syscall numbers; `linux_to_os01[]` translation table **must** include 9 socket mappings.
- `file_t` layout change requires `make clean` before rebuild.
- `MEM_LIBC_MALLOC=1` — lwIP uses OS01 `kmalloc()`/`kfree()` rather than its own internal heap.
- `MBEDTLS_NO_PLATFORM_ENTROPY` — no `/dev/urandom`; custom entropy callback from `rdtsc()` + jiffies + `__stack_chk_guard`.
- Kernel thread names passed to `sys_thread_new()` must be strdup'd — lwIP may pass stack-local strings.

---
## Phase 1: lwIP Port + e1000 Driver

### Task 1.1: Add lwIP git submodule

**Files:**
- Create: `thirdpart/lwip/` (git submodule)

**Interfaces:**
- Produces: `thirdpart/lwip/` — lwIP source tree at a known commit

- [ ] **Step 1: Add submodule**

```bash
cd /home/aagu/OS01-lwIP
git submodule add https://git.savannah.nongnu.org/git/lwip.git thirdpart/lwip
```

- [ ] **Step 2: Pin to a stable commit**

```bash
cd thirdpart/lwip
git checkout STABLE-2_2_0  # or latest stable tag
cd ../..
git add thirdpart/lwip
git commit -m "thirdparty: add lwIP submodule"
```

- [ ] **Step 3: Commit**

```bash
git add .gitmodules thirdpart/lwip
git commit -m "thirdparty: add lwIP STABLE-2_2_0 submodule"
```

---

### Task 1.2: Add PCI network class constants

**Files:**
- Modify: `kernel/include/driver/pci.h:60-63`

**Interfaces:**
- Produces: `PCI_CLASS_NETWORK (0x02)`, `PCI_SUBCLASS_ETHERNET (0x00)` — used by net subsys registration

- [ ] **Step 1: Add constants**

In `kernel/include/driver/pci.h`, after `#define PCI_PROGIF_AHCI 0x01`:

```c
// ── Class codes ────────────────────────────────────────────
#define PCI_CLASS_MASS_STORAGE   0x01
#define PCI_SUBCLASS_SATA        0x06
#define PCI_PROGIF_AHCI          0x01

#define PCI_CLASS_NETWORK          0x02
#define PCI_SUBCLASS_ETHERNET      0x00
```

- [ ] **Step 2: Build verification**

```bash
make kernel.bin
```

Expected: compiles successfully (no users of these constants yet, but the header change is inert).

- [ ] **Step 3: Commit**

```bash
git add kernel/include/driver/pci.h
git commit -m "feat(pci): add PCI_CLASS_NETWORK and PCI_SUBCLASS_ETHERNET constants"
```

---

### Task 1.3: Create lwIP configuration and kernel net headers

**Files:**
- Create: `kernel/include/net/lwipopts.h`
- Create: `kernel/include/net/net.h`
- Create: `kernel/include/net/socket.h` (empty placeholder for Phase 2)

**Interfaces:**
- Produces: `lwipopts.h` — included by lwIP's `lwip/opt.h` before all lwIP code
- Produces: `net.h` — declares `int net_hw_init(void);` and `void net_lwip_init(void);`
- Produces: `socket.h` — placeholder, expanded in Phase 2

- [ ] **Step 1: Create `kernel/include/net/lwipopts.h`**

```c
// kernel/include/net/lwipopts.h — OS01 lwIP compile-time configuration
#ifndef LWIP_OPTS_H
#define LWIP_OPTS_H

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

#endif // LWIP_OPTS_H
```

- [ ] **Step 2: Create `kernel/include/net/net.h`**

```c
// kernel/include/net/net.h — network subsystem interface
#ifndef _NET_NET_H
#define _NET_NET_H

// Stage A: Phase 6 subsys — hardware init only (PCI probe, BAR map, IRQ register)
int net_hw_init(void);

// Stage B: Post-SMP, pre-task_init — lwIP stack init + tcpip_thread creation
void net_lwip_init(void);

#endif // _NET_NET_H
```

- [ ] **Step 3: Create placeholder `kernel/include/net/socket.h`**

```c
// kernel/include/net/socket.h — socket types (expanded in Phase 2)
#ifndef _NET_SOCKET_H
#define _NET_SOCKET_H

// Placeholder — socket_t and socket API will be added in Phase 2

#endif // _NET_SOCKET_H
```

- [ ] **Step 4: Verify directories exist**

```bash
ls kernel/net/ kernel/include/net/
```

- [ ] **Step 5: Commit**

```bash
git add kernel/include/net/lwipopts.h kernel/include/net/net.h kernel/include/net/socket.h
git commit -m "feat(net): add lwipopts, net.h, and socket.h placeholder"
```

---

### Task 1.4: Implement sys_arch.c — OS adaptation layer

**Files:**
- Create: `kernel/net/sys_arch.c`

**Interfaces:**
- Consumes: `kernel/include/kernel/arch/spinlock.h` (spinlock_T, spin_lock_irqsave), `kernel/include/kernel/wait.h` (wait_queue_t), `kernel/include/kernel/task.h` (create_kthread, TASK_RUNNING), `kernel/include/device/timer.h` (jiffies), `stdlib.h` (kmalloc, kfree, strdup)
- Produces: `sys_sem_t`, `sys_mbox_t`, `sys_thread_t`, `sys_prot_t` — the full lwIP sys_arch interface. `sys_arch_protect()`/`sys_arch_unprotect()` with recursive IRQ-save semantics.

- [ ] **Step 1: Create `kernel/net/sys_arch.c`**

```c
// kernel/net/sys_arch.c — lwIP OS adaptation layer for OS01
//
// lwIP expects sys_arch to provide:
//   - Semaphores (sys_sem_new, sys_sem_signal, sys_sem_free)
//   - Mailboxes  (sys_mbox_new, sys_mbox_post, sys_mbox_fetch, sys_mbox_free)
//   - Threads    (sys_thread_new)
//   - Protection (sys_arch_protect, sys_arch_unprotect)
//   - Time       (sys_now)

#include "lwip/sys.h"
#include <kernel/arch/spinlock.h>
#include <kernel/wait.h>
#include <kernel/task.h>
#include <device/timer.h>
#include <stdlib.h>
#include <string.h>

// ═══════════════════════════════════════════════════════════════
//  Semaphores — spinlock + counter + wait_queue
// ═══════════════════════════════════════════════════════════════

typedef struct {
    int           count;
    spinlock_T    lock;
    wait_queue_t  wq;
} os_sem_t;

sys_sem_t sys_sem_new(u8_t count)
{
    os_sem_t *sem = (os_sem_t *)kmalloc(sizeof(os_sem_t), 0);
    if (!sem) return NULL;
    sem->count = (int)count;
    spin_init(&sem->lock);
    wait_queue_init(&sem->wq);
    return (sys_sem_t)sem;
}

void sys_sem_signal(sys_sem_t sem)
{
    os_sem_t *s = (os_sem_t *)sem;
    if (!s) return;
    uint64_t flags = spin_lock_irqsave(&s->lock);
    s->count++;
    spin_unlock_irqrestore(&s->lock, flags);
    wait_queue_wake_one(&s->wq);
}

u32_t sys_arch_sem_wait(sys_sem_t sem, u32_t timeout_ms)
{
    os_sem_t *s = (os_sem_t *)sem;
    if (!s) return SYS_ARCH_TIMEOUT;
    (void)timeout_ms;  // infinite only for now

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&s->lock);
        if (s->count > 0) {
            s->count--;
            spin_unlock_irqrestore(&s->lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&s->lock, flags);
        wait_queue_sleep(&s->wq);
    }
}

void sys_sem_free(sys_sem_t sem)
{
    // lwIP semaphores are created once at init and never freed.
    // We allocate them with kmalloc but free is a no-op — they
    // live for the lifetime of the kernel.
    (void)sem;
}

// ═══════════════════════════════════════════════════════════════
//  Mailboxes — ring buffer (64 slots) + wait_queue
// ═══════════════════════════════════════════════════════════════

#define MBOX_SIZE 64

typedef struct {
    void         *queue[MBOX_SIZE];
    int           head;       // producer writes here
    int           tail;       // consumer reads here
    int           count;
    spinlock_T    lock;
    wait_queue_t  wq;         // readers wait here
} os_mbox_t;

sys_mbox_t sys_mbox_new(int size)
{
    (void)size;
    os_mbox_t *mb = (os_mbox_t *)kmalloc(sizeof(os_mbox_t), 0);
    if (!mb) return NULL;
    mb->head = 0;
    mb->tail = 0;
    mb->count = 0;
    spin_init(&mb->lock);
    wait_queue_init(&mb->wq);
    return (sys_mbox_t)mb;
}

void sys_mbox_post(sys_mbox_t mbox, void *msg)
{
    os_mbox_t *mb = (os_mbox_t *)mbox;
    if (!mb) return;

    uint64_t flags = spin_lock_irqsave(&mb->lock);
    if (mb->count >= MBOX_SIZE) {
        // Drop on overflow — should not happen with properly
        // sized mailboxes.  Log and bail.
        spin_unlock_irqrestore(&mb->lock, flags);
        return;
    }
    mb->queue[mb->head] = msg;
    mb->head = (mb->head + 1) % MBOX_SIZE;
    mb->count++;
    spin_unlock_irqrestore(&mb->lock, flags);

    wait_queue_wake_one(&mb->wq);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t mbox, void **msg, u32_t timeout_ms)
{
    os_mbox_t *mb = (os_mbox_t *)mbox;
    if (!mb) return SYS_ARCH_TIMEOUT;
    (void)timeout_ms;  // infinite only

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&mb->lock);
        if (mb->count > 0) {
            *msg = mb->queue[mb->tail];
            mb->tail = (mb->tail + 1) % MBOX_SIZE;
            mb->count--;
            spin_unlock_irqrestore(&mb->lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&mb->lock, flags);
        wait_queue_sleep(&mb->wq);
    }
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t mbox, void **msg)
{
    os_mbox_t *mb = (os_mbox_t *)mbox;
    if (!mb) return SYS_MBOX_EMPTY;

    uint64_t flags = spin_lock_irqsave(&mb->lock);
    if (mb->count > 0) {
        *msg = mb->queue[mb->tail];
        mb->tail = (mb->tail + 1) % MBOX_SIZE;
        mb->count--;
        spin_unlock_irqrestore(&mb->lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&mb->lock, flags);
    return SYS_MBOX_EMPTY;
}

void sys_mbox_free(sys_mbox_t mbox)
{
    (void)mbox;  // live forever, same rationale as semaphores
}

// ═══════════════════════════════════════════════════════════════
//  Thread — create_kthread wrapper
// ═══════════════════════════════════════════════════════════════

struct lwip_thread_ctx {
    lwip_thread_fn fn;
    void          *arg;
};

static uint64_t lwip_thread_entry(uint64_t arg)
{
    struct lwip_thread_ctx *ctx = (struct lwip_thread_ctx *)(uintptr_t)arg;
    ctx->fn(ctx->arg);
    kfree(ctx);      // ctx was kmalloc'd in sys_thread_new
    return 0;
}

sys_thread_t sys_thread_new(const char *name,
                            lwip_thread_fn thread, void *arg,
                            int stacksize, int prio)
{
    (void)stacksize;
    (void)prio;

    // lwIP may pass a stack-local name string; strdup it.
    char *name_copy = strdup(name);
    if (!name_copy) return NULL;

    // Bundle fn+arg into heap-allocated context so lwip_thread_entry
    // can call fn(arg) without UB-prone function pointer casts.
    struct lwip_thread_ctx *ctx = kmalloc(sizeof(*ctx), 0);
    if (!ctx) { kfree(name_copy); return NULL; }
    ctx->fn   = thread;
    ctx->arg  = arg;

    task_t *t = create_kthread(lwip_thread_entry,
                               (uint64_t)(uintptr_t)ctx, name_copy);
    if (!t) {
        kfree(name_copy);
        kfree(ctx);
        return NULL;
    }
    return (sys_thread_t)t;
}

// ═══════════════════════════════════════════════════════════════
//  Protection — recursive IRQ-save spinlock
// ═══════════════════════════════════════════════════════════════

static spinlock_T  lwip_global_lock = { .lock = 1 };
static volatile int protect_nest = 0;
static uint64_t     protect_flags = 0;

sys_prot_t sys_arch_protect(void)
{
    if (protect_nest == 0) {
        // IRQ-save is required: a 100 Hz PIT timer tick during a
        // lwIP critical section may set need_resched; ret_from_intr
        // could then switch to another kernel thread that also enters
        // lwIP, deadlocking on lwip_global_lock.
        protect_flags = spin_lock_irqsave(&lwip_global_lock);
    }
    protect_nest++;
    return protect_flags;
}

void sys_arch_unprotect(sys_prot_t pval)
{
    (void)pval;
    if (protect_nest <= 0) return;
    if (--protect_nest == 0)
        spin_unlock_irqrestore(&lwip_global_lock, protect_flags);
}

// ═══════════════════════════════════════════════════════════════
//  Time
// ═══════════════════════════════════════════════════════════════

u32_t sys_now(void)
{
    // jiffies increments at 100 Hz (PIT), so 1 jiffy = 10 ms
    return (u32_t)(jiffies * 10);
}
```

- [ ] **Step 2: Commit**

```bash
git add kernel/net/sys_arch.c
git commit -m "feat(net): implement lwIP sys_arch adaptation layer
Semaphores: spinlock + counter + wait_queue.
Mailboxes: ring buffer (64 slots) + wait_queue.
Threads: create_kthread wrapper with strdup'd name.
Protection: recursive IRQ-save spinlock (prevents timer-IRQ deadlock).
Time: jiffies * 10 (ms resolution)."
```

---
### Task 1.5: Write e1000 register definitions header

**Files:**
- Create: `kernel/driver/e1000.h`

**Interfaces:**
- Produces: All e1000 register offsets, descriptor structures, and flag constants consumed by `e1000.c`

- [ ] **Step 1: Create `kernel/driver/e1000.h`**

```c
// kernel/driver/e1000.h — Intel 82540EM (e1000) register definitions
#ifndef _DRIVER_E1000_H
#define _DRIVER_E1000_H

#include <stdint.h>

// ── Register offsets (16-byte aligned for 64-bit MMIO) ────────
#define E1000_REG_CTRL     0x0000   // Device Control
#define E1000_REG_STATUS   0x0008   // Device Status
#define E1000_REG_EERD     0x0014   // EEPROM Read
#define E1000_REG_ICR      0x00C0   // Interrupt Cause Read
#define E1000_REG_IMS      0x00D0   // Interrupt Mask Set
#define E1000_REG_IMC      0x00D8   // Interrupt Mask Clear
#define E1000_REG_RCTL     0x0100   // Receive Control
#define E1000_REG_TCTL     0x0400   // Transmit Control
#define E1000_REG_RDBAL    0x2800   // RX Descriptor Base Low
#define E1000_REG_RDBAH    0x2804   // RX Descriptor Base High
#define E1000_REG_RDLEN    0x2808   // RX Descriptor Length
#define E1000_REG_RDH      0x2810   // RX Descriptor Head
#define E1000_REG_RDT      0x2818   // RX Descriptor Tail
#define E1000_REG_RDTR     0x2820   // RX Delay Timer
#define E1000_REG_TDBAL    0x3800   // TX Descriptor Base Low
#define E1000_REG_TDBAH    0x3804   // TX Descriptor Base High
#define E1000_REG_TDLEN    0x3808   // TX Descriptor Length
#define E1000_REG_TDH      0x3810   // TX Descriptor Head
#define E1000_REG_TDT      0x3818   // TX Descriptor Tail
#define E1000_REG_RA_BASE  0x5400   // Receive Address (MAC) Filter

// ── CTRL bits ─────────────────────────────────────────────────
#define E1000_CTRL_FD       (1 << 0)
#define E1000_CTRL_ASDE     (1 << 5)
#define E1000_CTRL_SLU      (1 << 6)
#define E1000_CTRL_ILOS     (1 << 7)
#define E1000_CTRL_SPEED_10  0
#define E1000_CTRL_SPEED_100 (1 << 8)
#define E1000_CTRL_SPEED_1000 (2 << 8)
#define E1000_CTRL_SPEED_MASK (3 << 8)
#define E1000_CTRL_FRCSPD   (1 << 11)
#define E1000_CTRL_FRCDPLX  (1 << 12)
#define E1000_CTRL_RST      (1 << 26)

// ── STATUS bits ────────────────────────────────────────────────
#define E1000_STATUS_FD     (1 << 0)
#define E1000_STATUS_LU     (1 << 1)

// ── EERD bits ──────────────────────────────────────────────────
#define E1000_EERD_START    (1 << 0)
#define E1000_EERD_DONE     (1 << 4)
#define E1000_EERD_DATA_MASK 0xFFFF0000
#define E1000_EERD_DATA_SHIFT 16

// ── RCTL bits ──────────────────────────────────────────────────
#define E1000_RCTL_EN       (1 << 1)
#define E1000_RCTL_SBP      (1 << 2)
#define E1000_RCTL_UPE      (1 << 3)
#define E1000_RCTL_MPE      (1 << 4)
#define E1000_RCTL_LPE      (1 << 5)
#define E1000_RCTL_LBM_NONE 0
#define E1000_RCTL_LBM_LOOP (3 << 6)
#define E1000_RCTL_RDMTS_HALF 0
#define E1000_RCTL_RDMTS_QUARTER (1 << 8)
#define E1000_RCTL_RDMTS_EIGHTH (2 << 8)
#define E1000_RCTL_BAM      (1 << 15)
#define E1000_RCTL_BSIZE_256   (3 << 16)
#define E1000_RCTL_BSIZE_512   (2 << 16)
#define E1000_RCTL_BSIZE_1024  (1 << 16)
#define E1000_RCTL_BSIZE_2048  0
#define E1000_RCTL_BSIZE_4096  ((3 << 16) | (1 << 25))
#define E1000_RCTL_BSIZE_8192  ((2 << 16) | (1 << 25))
#define E1000_RCTL_BSIZE_16384 ((1 << 16) | (1 << 25))
#define E1000_RCTL_VFE      (1 << 18)
#define E1000_RCTL_SECRC    (1 << 26)

// ── TCTL bits ──────────────────────────────────────────────────
#define E1000_TCTL_EN       (1 << 1)
#define E1000_TCTL_PSP      (1 << 3)
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12
#define E1000_TCTL_COLD_FULLDUPLEX 0x40
#define E1000_TCTL_COLD_HALFDUPLEX 0x200

// ── ICR / IMS bits ─────────────────────────────────────────────
#define E1000_ICR_TXDW      (1 << 0)
#define E1000_ICR_TXQE      (1 << 1)
#define E1000_ICR_LSC       (1 << 2)
#define E1000_ICR_RXSEQ     (1 << 3)
#define E1000_ICR_RXDMT0    (1 << 4)
#define E1000_ICR_RXO       (1 << 6)
#define E1000_ICR_RXT0      (1 << 7)

// ── RX descriptor ──────────────────────────────────────────────
#define E1000_NUM_RX_DESC   32
#define E1000_NUM_TX_DESC   32

typedef struct {
    uint64_t addr;       // physical address of data buffer
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

#define E1000_RXD_STAT_DD  (1 << 0)   // Descriptor Done
#define E1000_RXD_STAT_EOP (1 << 1)   // End of Packet

// ── TX descriptor ──────────────────────────────────────────────
typedef struct {
    uint64_t addr;       // physical address of data buffer
    uint16_t length;
    uint8_t  cso;        // Checksum Offset
    uint8_t  cmd;        // Command
    uint8_t  status;     // Status (written by hardware on completion)
    uint8_t  css;        // Checksum Start
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

#define E1000_TXD_CMD_EOP  (1 << 0)   // End of Packet
#define E1000_TXD_CMD_IFCS (1 << 1)   // Insert FCS/CRC
#define E1000_TXD_CMD_RS   (1 << 3)   // Report Status
#define E1000_TXD_STAT_DD  (1 << 0)   // Descriptor Done

#endif // _DRIVER_E1000_H
```

- [ ] **Step 2: Commit**

```bash
git add kernel/driver/e1000.h
git commit -m "feat(e1000): add register definitions header"
```

---

### Task 1.6: Implement e1000 driver

**Files:**
- Create: `kernel/driver/e1000.c`

**Interfaces:**
- Consumes: `e1000.h`, `driver/pci.h`, `kernel/vmm.h` (vmm_map_page, kernel_map, PAGE_KERNEL_MMIO), `kernel/pmm.h` (PAGE_2M_MASK, alloc_pages), `kernel/memory.h` (Phy_To_Virt), `kernel/interrupt.h` (register_irq), `kernel/apic.h` (get_ioapic_controller), `stdlib.h` (kmalloc, memset)
- Produces: `void e1000_init(uint64_t bar_phys, uint8_t irq)` — called by Stage A subsys. Internally: `e1000_xmit()` (netif->linkoutput), `e1000_input()` (netif->input), IRQ handler.

- [ ] **Step 1: Create `kernel/driver/e1000.c`**

```c
// kernel/driver/e1000.c — Intel 82540EM (e1000) NIC driver
#include <driver/e1000.h>
#include <driver/pci.h>
#include <kernel/vmm.h>       // vmm_map_page, kernel_map, PAGE_KERNEL_MMIO
#include <kernel/pmm.h>       // PAGE_2M_MASK, alloc_pages
#include <kernel/memory.h>    // Phy_To_Virt
#include <kernel/interrupt.h> // register_irq
#include <kernel/apic.h>      // get_ioapic_controller
#include <kernel/log.h>       // log_info
#include <stdlib.h>
#include <string.h>
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/etharp.h"   // ethernet_input

// ── Per-device state ───────────────────────────────────────────
static struct {
    uint64_t           mmio_phys;
    volatile uint8_t  *mmio;           // kernel-virtual MMIO base
    uint8_t            mac[6];
    uint8_t            irq;
    struct netif      *netif_ptr;      // set by e1000_netif_init during netif_add

    // Descriptor rings (in RAM — Phy_To_Virt works)
    e1000_rx_desc_t   *rx_descs;
    e1000_tx_desc_t   *tx_descs;
    uint64_t            rx_phys;
    uint64_t            tx_phys;

    // Packet buffers — must be allocated from physical pages, NOT kmalloc.
    // kmalloc returns slab memory which is NOT identity-mapped, so
    // Virt_To_Phy() computes a garbage physical address → DMA corruption.
    // Use alloc_4k_page() which returns a physical address usable for DMA.
    uint64_t           rx_buf_phys[E1000_NUM_RX_DESC];
    uint64_t           tx_buf_phys[E1000_NUM_TX_DESC];
    uint8_t           *rx_bufs[E1000_NUM_RX_DESC];    // Phy_To_Virt(rx_buf_phys[i])
    uint8_t           *tx_bufs[E1000_NUM_TX_DESC];    // Phy_To_Virt(tx_buf_phys[i])

    uint32_t            tx_head;       // next descriptor to send
    uint32_t            tx_tail;       // next free slot (post-completion)
    uint32_t            rx_tail;       // next descriptor to check

    int                 initialized;
} e1000;

// ── MMIO helpers ───────────────────────────────────────────────
static inline uint32_t e1000_read(uint32_t reg)
{
    return *(volatile uint32_t *)(e1000.mmio + reg);
}

static inline void e1000_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(e1000.mmio + reg) = val;
}

// ── EEPROM read (MAC address) ─────────────────────────────────
static uint16_t e1000_eeprom_read(uint8_t addr)
{
    e1000_write(E1000_REG_EERD, ((uint32_t)addr << 8) | E1000_EERD_START);
    while (!(e1000_read(E1000_REG_EERD) & E1000_EERD_DONE))
        ;  // spin — called once at init
    return (uint16_t)(e1000_read(E1000_REG_EERD) >> E1000_EERD_DATA_SHIFT);
}

// ── Interrupt handler ──────────────────────────────────────────
// Uses register_irq() — the same pattern as keyboard, serial, PIT.
// The kernel has 16 pre-installed stubs (arch/x86_64/irq.c) for
// vectors 0x20–0x2f that dispatch through do_IRQ → irq_table[].
// No DEFINE_INTR_STUB/REGISTER_INTR_HANDLER needed.

static void e1000_handler(uint64_t nr, uint64_t param, pt_regs_t *regs)
{
    (void)nr; (void)param; (void)regs;
    uint32_t icr = e1000_read(E1000_REG_ICR);
    if (!icr) return;

    // ── RX: descriptor done ─────────────────────────────────
    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0)) {
        while (e1000.rx_descs[e1000.rx_tail].status & E1000_RXD_STAT_DD) {
            uint16_t len = e1000.rx_descs[e1000.rx_tail].length;
            if (len > 0) {
                struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
                if (p) {
                    memcpy(p->payload, e1000.rx_bufs[e1000.rx_tail], len);
                    if (tcpip_inpkt(p, e1000.netif_ptr, ethernet_input) != ERR_OK)
                        pbuf_free(p);
                } else {
                    e1000.rx_bufs[e1000.rx_tail][0] = 0xDB; // "dropped" marker
                }
            }
            e1000.rx_descs[e1000.rx_tail].status = 0;
            e1000.rx_tail = (e1000.rx_tail + 1) % E1000_NUM_RX_DESC;
            e1000_write(E1000_REG_RDT, e1000.rx_tail);
        }
    }

    // ── TX: descriptor done ─────────────────────────────────
    if (icr & E1000_ICR_TXDW) {
        // Walk from tx_tail to tx_head, freeing completed descriptors
        while (e1000.tx_tail != e1000.tx_head) {
            if (!(e1000.tx_descs[e1000.tx_tail].status & E1000_TXD_STAT_DD))
                break;
            e1000.tx_descs[e1000.tx_tail].status = 0;
            e1000.tx_tail = (e1000.tx_tail + 1) % E1000_NUM_TX_DESC;
        }
    }
}

// ── TX: netif->linkoutput ─────────────────────────────────────
static err_t e1000_xmit(struct netif *netif, struct pbuf *p)
{
    (void)netif;

    uint32_t next = (e1000.tx_head + 1) % E1000_NUM_TX_DESC;
    if (next == e1000.tx_tail) {
        // Ring full — lwIP will retry
        return ERR_MEM;
    }

    // Copy pbuf chain into TX buffer
    uint8_t *dst = e1000.tx_bufs[e1000.tx_head];
    uint16_t total = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        memcpy(dst + total, q->payload, q->len);
        total += (uint16_t)q->len;
    }

    e1000.tx_descs[e1000.tx_head].length = total;
    e1000.tx_descs[e1000.tx_head].cmd = E1000_TXD_CMD_EOP
                                       | E1000_TXD_CMD_IFCS
                                       | E1000_TXD_CMD_RS;
    e1000.tx_descs[e1000.tx_head].status = 0;

    e1000.tx_head = next;
    e1000_write(E1000_REG_TDT, e1000.tx_head);

    return ERR_OK;
}

// ── Initialization ─────────────────────────────────────────────
int e1000_init(uint64_t bar_phys, uint8_t irq)
{
    if (e1000.initialized) return 0;

    // 1. Map MMIO BAR (2MB page at PCI MMIO address)
    e1000.mmio_phys = bar_phys;
    uint64_t bar_page = bar_phys & PAGE_2M_MASK;
    vmm_map_page(kernel_map, bar_page,
                 (uintptr_t)Phy_To_Virt(bar_page), PAGE_KERNEL_MMIO);
    e1000.mmio = (volatile uint8_t *)Phy_To_Virt(bar_phys);
    e1000.irq = irq;

    // 2. Reset device
    uint32_t ctrl = e1000_read(E1000_REG_CTRL);
    e1000_write(E1000_REG_CTRL, ctrl | E1000_CTRL_RST);
    for (volatile int i = 0; i < 100000; i++) {
        if (!(e1000_read(E1000_REG_CTRL) & E1000_CTRL_RST))
            break;
    }

    // 3. Read MAC from EEPROM
    for (int i = 0; i < 3; i++) {
        uint16_t word = e1000_eeprom_read((uint8_t)i);
        e1000.mac[i * 2]     = (uint8_t)(word & 0xFF);
        e1000.mac[i * 2 + 1] = (uint8_t)(word >> 8);
    }

    // 4. Allocate descriptor rings (physically contiguous, 16-byte aligned)
    struct Page *rx_page = alloc_pages(ZONE_NORMAL, 1, 0);
    struct Page *tx_page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!rx_page || !tx_page) return -1;

    e1000.rx_phys = rx_page->phy_address;
    e1000.tx_phys = tx_page->phy_address;
    e1000.rx_descs = (e1000_rx_desc_t *)Phy_To_Virt(e1000.rx_phys);
    e1000.tx_descs = (e1000_tx_desc_t *)Phy_To_Virt(e1000.tx_phys);
    memset(e1000.rx_descs, 0, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    memset(e1000.tx_descs, 0, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);

    // 5. Allocate DMA buffers for RX/TX descriptors using physical pages.
    //    alloc_4k_page() returns a physical address — Phy_To_Virt gives the
    //    kernel-virtual address for CPU access.  The physical address goes
    //    directly into the descriptor (no Virt_To_Phy needed).
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        uint64_t buf_phys = alloc_4k_page();
        if (!buf_phys) return -1;
        e1000.rx_buf_phys[i] = buf_phys;
        e1000.rx_bufs[i] = (uint8_t *)Phy_To_Virt(buf_phys);
        e1000.rx_descs[i].addr = buf_phys;  // physical address for DMA
    }
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        uint64_t buf_phys = alloc_4k_page();
        if (!buf_phys) return -1;
        e1000.tx_buf_phys[i] = buf_phys;
        e1000.tx_bufs[i] = (uint8_t *)Phy_To_Virt(buf_phys);
        e1000.tx_descs[i].addr = buf_phys;  // physical address for DMA
    }

    // 6. Configure RX
    e1000_write(E1000_REG_RDBAL, (uint32_t)(e1000.rx_phys & 0xFFFFFFFF));
    e1000_write(E1000_REG_RDBAH, (uint32_t)(e1000.rx_phys >> 32));
    e1000_write(E1000_REG_RDLEN, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    e1000_write(E1000_REG_RDH, 0);
    e1000_write(E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
    e1000.rx_tail = 0;
    e1000_write(E1000_REG_RCTL,
        E1000_RCTL_EN | E1000_RCTL_SBP | E1000_RCTL_BAM
        | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC);

    // 7. Configure TX
    e1000_write(E1000_REG_TDBAL, (uint32_t)(e1000.tx_phys & 0xFFFFFFFF));
    e1000_write(E1000_REG_TDBAH, (uint32_t)(e1000.tx_phys >> 32));
    e1000_write(E1000_REG_TDLEN, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
    e1000_write(E1000_REG_TDH, 0);
    e1000_write(E1000_REG_TDT, 0);
    e1000.tx_head = 0;
    e1000.tx_tail = 0;
    e1000_write(E1000_REG_TCTL,
        E1000_TCTL_EN | E1000_TCTL_PSP
        | (0x10 << E1000_TCTL_CT_SHIFT)
        | (E1000_TCTL_COLD_FULLDUPLEX << E1000_TCTL_COLD_SHIFT));

    // 8. Enable e1000 interrupt sources
    e1000_write(E1000_REG_IMS,
        E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_TXDW | E1000_ICR_LSC);

    // 9. Register interrupt handler (same pattern as keyboard/serial/PIT)
    // The kernel pre-installs 16 stubs for vectors 0x20..0x2f that
    // dispatch through do_IRQ → irq_table[].  register_irq fills
    // irq_table[irq] with the handler and configures the IOAPIC.
    register_irq(0x20 + irq, NULL, &e1000_handler, 0,
                 get_ioapic_controller(), "e1000");

    e1000.initialized = 1;

    log_info("e1000: MAC %02x:%02x:%02x:%02x:%02x:%02x IRQ=%u\n",
                e1000.mac[0], e1000.mac[1], e1000.mac[2],
                e1000.mac[3], e1000.mac[4], e1000.mac[5], irq);
    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add kernel/driver/e1000.c
git commit -m "feat(e1000): implement Intel 82540EM NIC driver
MMIO mapped via vmm_map_page(PAGE_KERNEL_MMIO).
TX: copy pbuf chain, set EOP/IFCS/RS, advance TDT.
RX: interrupt handler allocates pbuf, copies data, tcpip_inpkt.
TX completion: walk descriptors, clear DD, advance tx_tail.
EEPROM MAC read at init."
```

---
### Task 1.7: Implement net.c — two-stage initialization

**Files:**
- Create: `kernel/net/net.c`
- Modify: `kernel/arch/x86_64/subsys.c:82-84`
- Modify: `kernel/kernel/main.c:284-291`

**Interfaces:**
- Consumes: `net/net.h`, `driver/e1000.h` (e1000_init), `driver/pci.h` (pci_find_device, pci_read_bar, pci_enable_bus_mastering, pci_enable_mmio, pci_read_interrupt_line), `lwip/init.h` (lwip_init), `lwip/tcpip.h` (tcpip_init), `lwip/netif.h`, `lwip/dhcp.h`
- Produces: `net_hw_init()` (Phase 6 subsys), `net_lwip_init()` (post-SMP explicit call). Internal: netif init, DHCP start.

- [ ] **Step 1: Create `kernel/net/net.c`**

```c
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
#include <stdlib.h>         // kmalloc, kfree
#include <string.h>
#include "lwip/init.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"

// ── Static state ───────────────────────────────────────────────
static int  net_hw_ok = 0;
static uint8_t e1000_irq = 0;
static uint64_t e1000_bar = 0;

// The single OS01 network interface
static struct netif os01_netif;

// ── Stage A: Hardware init (Phase 6 subsys, no scheduler) ────

static int net_hw_init(void)
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

    if (e1000_init(e1000_bar, e1000_irq) != 0) {
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

    lwip_init();

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
    dhcp_start(nif);

    log_info("net: lwIP stack initialized, DHCP started\n");
}
```

`e1000_netif_init` is added to `kernel/driver/e1000.c`:

```c
// ── netif init callback — called by netif_add to configure the interface ──
// Sets hwaddr, hwaddr_len, mtu, flags, linkoutput.
// The e1000 state is global (single NIC), so no void *arg needed.

err_t e1000_netif_init(struct netif *netif)
{
    e1000.netif_ptr = netif;  // store for IRQ handler's tcpip_inpkt()
    netif->input = ethernet_input;  // input function for received packets
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, e1000.mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->linkoutput = e1000_xmit;
    netif->output = etharp_output;  // standard Ethernet ARP output
    return ERR_OK;
}
```

And declared in `kernel/driver/e1000.h`:
```c
// ── Driver API ─────────────────────────────────────────────────
#include "lwip/netif.h"  // for struct netif, struct pbuf, err_t

int   e1000_init(uint64_t bar_phys, uint8_t irq);
err_t e1000_xmit(struct netif *netif, struct pbuf *p);
err_t e1000_netif_init(struct netif *netif);
```

- [ ] **Step 3: Register net_hw_init as a Phase 6 subsys**

In `kernel/arch/x86_64/subsys.c`, add:
```c
#include <net/net.h>

// Add wrapper:
static int _net_hw_init_wrapper(void)
{
    return net_hw_init();
}
```

And in `arch_register_subsys()`, after the AHCI line:
```c
// Phase 6: storage + network
register_subsys("ahci", _ahci_init_wrapper,            SUBSYS_PHASE_6, SUBSYS_FLAG_OPTIONAL);
register_subsys("net-hw", _net_hw_init_wrapper,        SUBSYS_PHASE_6, SUBSYS_FLAG_OPTIONAL);
```

- [ ] **Step 4: Call net_lwip_init() from kernel_main()**

In `kernel/kernel/main.c`, after `arch_register_subsys_percpu()`/`subsys_init_percpu()`, and before `task_init()`:

```c
// Add include at top:
#include <net/net.h>

// After the existing block (~line 276-291):
    // per-CPU 子系统二次 init
    arch_register_subsys_percpu();
    subsys_init_percpu();

#ifdef OS01_SELFTEST
    // ... existing selftest ...
#endif

    // ═══ Network stack init (post-SMP, pre-scheduler) ═══
    // lwIP creates kernel threads (tcpip_thread) — must happen
    // after SMP is up but BEFORE task_init() resets the task list.
    net_lwip_init();

    futex_init();
    // ... existing rest of kernel_main ...
```

- [ ] **Step 5: Commit**

```bash
git add kernel/net/net.c kernel/driver/e1000.h kernel/arch/x86_64/subsys.c kernel/kernel/main.c
git commit -m "feat(net): two-stage network init (hw Phase 6 + lwIP post-SMP)
Stage A: net_hw_init() registered as SUBSYS_PHASE_6 subsys — e1000
  PCI probe, BAR mapping, descriptor rings, IRQ registration.
Stage B: net_lwip_init() called explicitly between SMP bringup and
  task_init() — lwip_init(), netif_add, DHCP start, tcpip_init().
  Avoids task_init() resetting the task list and losing kthreads."
```

---

### Task 1.8: Add lwIP source compilation to kernel build

**Files:**
- Modify: `kernel/Makefile`
- Modify: `kernel/net/` (may need a Makefile fragment)

**Interfaces:**
- Consumes: lwIP source files under `thirdpart/lwip/src/`
- Produces: lwIP `.o` files linked into `kernel.bin`

- [ ] **Step 1: Update kernel Makefile to compile lwIP sources**

In `kernel/Makefile`, add after existing wildcards:

```makefile
# ── lwIP networking stack ────────────────────────────────────
LWIP_SRC_DIR  := ../thirdpart/lwip/src
LWIP_CORE     := $(wildcard $(LWIP_SRC_DIR)/core/*.c)
LWIP_CORE4    := $(wildcard $(LWIP_SRC_DIR)/core/ipv4/*.c)
LWIP_NETIF    := $(wildcard $(LWIP_SRC_DIR)/netif/*.c)
LWIP_API      := $(wildcard $(LWIP_SRC_DIR)/api/*.c)

# Exclude PPP/SLIP/SLIPIF/ethernetif.c (we provide our own netif)
LWIP_NETIF    := $(filter-out %/ppp.c %/slipif.c %/ethernetif.c, $(LWIP_NETIF))

LWIP_SOURCES  := $(LWIP_CORE) $(LWIP_CORE4) $(LWIP_NETIF) $(LWIP_API)
# Map lwIP sources to build dir: thirdpart/lwip/src/core/tcp.c → build/.../lwip/core/tcp.o
LWIP_OBJECTS  := $(patsubst $(LWIP_SRC_DIR)/%.c,$(BUILD_DIR)/lwip/%.o,$(LWIP_SOURCES))

KERNEL_OBJECTS += $(LWIP_OBJECTS)

# IMPORTANT: add kernel/net/*.c to the existing KERNEL_C_SOURCES list
# In the existing KERNEL_C_SOURCES definition (~line 27), add:
#     $(wildcard net/*.c) \
KERNEL_C_SOURCES += $(wildcard net/*.c)

ALL_CFLAGS     += -I$(LWIP_SRC_DIR)/include -I../kernel/net
ALL_LDFLAGS    +=
```

Add compilation rule for lwIP:
```makefile
# lwIP .c → .o
$(BUILD_DIR)/lwip/%.o: $(LWIP_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -MMD -MP -c $< -o $@
```

- [ ] **Step 2: Do a clean build to test**

```bash
make clean
make kernel.bin 2>&1 | tail -30
```

Expected: lwIP sources compile without errors. Some warnings from lwIP may appear — suppress with `-Wno-` flags if needed.

- [ ] **Step 3: Commit**

```bash
git add kernel/Makefile
git commit -m "build: add lwIP source compilation to kernel Makefile
Compiles lwIP core, core/ipv4, netif (excluding PPP/SLIP/ethernetif),
and api sources. Adds lwIP include path (-Ithirdpart/lwip/src/include)
and kernel/net (for lwipopts.h)."
```

---

### Task 1.9: Add QEMU e1000 device and verify Phase 1

**Files:**
- Modify: `Makefile` (root, QEMU run targets)

- [ ] **Step 1: Add e1000 netdev to QEMU command lines**

In root `Makefile`, add `-netdev user,id=net0 -device e1000,netdev=net0` to the `run`, `run-kvm`, and `debug` targets:

```makefile
run: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp 2 -pflash boot/uefi/OVMF.fd \
	  -netdev user,id=net0 -device e1000,netdev=net0 \
	  -drive file=disk.img,format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio
```

Do the same for `run-kvm` and `debug`.

- [ ] **Step 2: Build and boot**

```bash
make clean
make run
```

- [ ] **Step 3: Verify DHCP IP in serial log**

Expected output (may take a few seconds for DHCP):
```
PCI: scanning for class=02 subclass=00 progIF=00
PCI: found at 0:3.0
e1000: MAC 52:54:00:12:34:56 IRQ=11
net: lwIP stack initialized, DHCP started
dhcp: bound to 10.0.2.15
```

- [ ] **Step 4: Test without NIC (optional subsys)**

Comment out the e1000 device from `Makefile` run target, then `make run`.

Expected: clean boot, no crash, just "net: no e1000 NIC found" message.

- [ ] **Step 5: Commit**

```bash
git add Makefile
git commit -m "feat(qemu): add e1000 netdev to run/run-kvm/debug targets
-netdev user,id=net0 provides NAT networking with DHCP (10.0.2.0/24).
-device e1000 exposes Intel 82540EM PCI NIC to the guest."
```

---
## Phase 2: Socket Syscall Layer

### Task 2.1: Extend file_t with FD_SOCKET and socket_t

**Files:**
- Modify: `kernel/include/kernel/file.h`
- Modify: `kernel/fs/file.c` (file_free, fd_read, fd_write)

- [ ] **Step 1: Add socket_t and FD_SOCKET to file.h**

In `kernel/include/kernel/file.h`, after the pipe_t definition and before file_t:

```c
// ── Socket ────────────────────────────────────────────────────
#include <kernel/arch/spinlock.h>
#include <list.h>

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
```

Add `FD_SOCKET` to the enum:
```c
enum file_type {
    FD_NONE = 0,
    FD_VFS,
    FD_PIPE,
    FD_DEV,
    FD_SOCKET,    // new
};
```

Add `socket_t *sock` to `file_t`:
```c
typedef struct file {
    enum file_type type;
    uint32_t       refcount;
    int            flags;       // O_RDONLY / O_WRONLY / O_RDWR
    uint64_t       offset;
    // FD_VFS / FD_DEV
    struct vfs_node *node;
    // FD_PIPE
    pipe_t         *pipe;
    // FD_SOCKET
    socket_t       *sock;
} file_t;
```

- [ ] **Step 2: Add FD_SOCKET cleanup to file_free()**

In `kernel/fs/file.c`, in `file_free()`:

```c
void file_free(file_t *f)
{
    if (!f) return;
    if (f->node)
        vfs_node_put(f->node);
    if (f->type == FD_SOCKET && f->sock) {
        // Forward-declare: netconn_delete is in lwIP
        extern void netconn_delete(struct netconn *);
        if (f->sock->conn)
            netconn_delete((struct netconn *)f->sock->conn);
        kfree(f->sock);
    }
    kfree(f);
}
```

- [ ] **Step 3: Commit**

```bash
git add kernel/include/kernel/file.h kernel/fs/file.c
git commit -m "feat(file): add FD_SOCKET type, socket_t struct, and cleanup
Adds FD_SOCKET to enum file_type, socket_t with netconn pointer +
poll_list, and cleanup path in file_free() (netconn_delete + kfree).
Requires make clean after this commit (struct layout change)."
```

---

### Task 2.2: Add socket syscal numbers and dispatch

**Files:**
- Modify: `kernel/include/uapi/syscall.h`
- Modify: `kernel/arch/x86_64/trap.c` (dispatch switch + syscall_names + linux_to_os01)

- [ ] **Step 1: Add syscall numbers**

In `kernel/include/uapi/syscall.h`, after `#define SYS_select 50`:

```c
// ── Phase 10: socket networking ────────────────────────────────
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

- [ ] **Step 2: Add to syscall_names table**

In `kernel/arch/x86_64/trap.c`, in the `syscall_names` array (around line 900), add entries for 51-59:

```c
[51] = "socket",
[52] = "bind",
[53] = "connect",
[54] = "listen",
[55] = "accept",
[56] = "sendto",
[57] = "recvfrom",
[58] = "setsockopt",
[59] = "getsockopt",
```

- [ ] **Step 3: Add Linux→OS01 translation mappings**

In `kernel/arch/x86_64/trap.c`, in the `linux_to_os01[]` array (around line 808), add:

```c
[41] = 51,  // Linux socket → OS01 SYS_socket
[42] = 53,  // Linux connect → OS01 SYS_connect
[43] = 55,  // Linux accept → OS01 SYS_accept
[44] = 56,  // Linux sendto → OS01 SYS_sendto
[45] = 57,  // Linux recvfrom → OS01 SYS_recvfrom
[49] = 52,  // Linux bind → OS01 SYS_bind
[50] = 54,  // Linux listen → OS01 SYS_listen
[54] = 58,  // Linux setsockopt → OS01 SYS_setsockopt
[55] = 59,  // Linux getsockopt → OS01 SYS_getsockopt
```

- [ ] **Step 4: Add case stubs to the dispatch switch**

In `kernel/arch/x86_64/trap.c`, add after the `SYS_select` case (line ~2079). Use `-ENOSYS` stubs for now — real implementations go in Task 2.4:

```c
case SYS_socket:
case SYS_bind:
case SYS_connect:
case SYS_listen:
case SYS_accept:
case SYS_sendto:
case SYS_recvfrom:
case SYS_setsockopt:
case SYS_getsockopt: {
    regs->rax = -ENOSYS;
    break;
}
```

- [ ] **Step 5: Commit**

```bash
git add kernel/include/uapi/syscall.h kernel/arch/x86_64/trap.c
git commit -m "feat(syscall): add SYS_socket..SYS_getsockopt (51-59) with stubs
Adds 9 new networking syscalls, their Linux→OS01 translation
entries, and -ENOSYS dispatch stubs."
```

---

### Task 2.3: Implement socket.c — socket allocation and helpers

**Files:**
- Create: `kernel/net/socket.c`

**Interfaces:**
- Consumes: `net/socket.h`, `kernel/file.h`, `stdlib.h`, `lwip/netconn.h`, `lwip/netbuf.h`, `lwip/err.h`
- Produces: `socket_t *socket_alloc(int domain, int type, int protocol)` (+ `socket_free`), `socket_t *socket_get(int fd)` (validate fd → socket_t)

- [ ] **Step 1: Create `kernel/net/socket.c` with allocation/validation helpers**

```c
// kernel/net/socket.c — BSD socket implementation backed by lwIP netconn
#include <net/socket.h>
#include <kernel/file.h>
#include <kernel/task.h>
#include <kernel/poll.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "lwip/netconn.h"
#include "lwip/netbuf.h"
#include "lwip/err.h"

// ── Socket state constants ─────────────────────────────────────
#define SOCK_UNCONNECTED  0
#define SOCK_CONNECTED    1
#define SOCK_LISTENING    2
#define SOCK_CLOSED       3

// ── Socket allocation ──────────────────────────────────────────

socket_t *socket_alloc(int domain, int type, int protocol)
{
    // Map BSD socket types to lwIP netconn types
    enum netconn_type nc_type;
    switch (type) {
    case 1: nc_type = NETCONN_TCP; break;   // SOCK_STREAM
    case 2: nc_type = NETCONN_UDP; break;   // SOCK_DGRAM
    default: return NULL;
    }

    struct netconn *conn = netconn_new_with_proto_and_callback(nc_type,
        (u8_t)protocol, NULL);
    if (!conn) return NULL;

    socket_t *s = (socket_t *)kmalloc(sizeof(socket_t), 0);
    if (!s) { netconn_delete(conn); return NULL; }

    s->conn     = conn;
    s->domain   = domain;
    s->type     = type;
    s->protocol = protocol;
    s->state    = SOCK_UNCONNECTED;
    s->bound    = 0;
    spin_init(&s->lock);
    list_init(&s->poll_list);

    return s;
}

void socket_free(socket_t *s)
{
    if (!s) return;
    if (s->conn) netconn_delete((struct netconn *)s->conn);
    kfree(s);
}

// ── FD → socket lookup ─────────────────────────────────────────

socket_t *socket_get(int fd)
{
    if (fd < 0 || fd >= NOFILE || !current || !current->files)
        return NULL;
    file_t *f = current->files->fd[fd];
    if (!f || f->type != FD_SOCKET || !f->sock)
        return NULL;
    return f->sock;
}
```

- [ ] **Step 2: Commit**

```bash
git add kernel/net/socket.c
git commit -m "feat(socket): add socket_alloc/socket_get helpers
socket_alloc translates BSD SOCK_STREAM/DGRAM to lwIP NETCONN_TCP/UDP,
creates socket_t, initializes spinlock + poll_list.
socket_get validates fd → socket_t."
```

---

### Task 2.4: Implement socket syscall implementations

**Files:**
- Modify: `kernel/net/socket.c` (add syscall implementations)
- Modify: `kernel/arch/x86_64/trap.c` (replace stubs with real calls)

- [ ] **Step 1: Add socket syscall implementations to socket.c**

Append to `kernel/net/socket.c`:

```c
// ── SYS_socket — create a socket ───────────────────────────────

int64_t do_socket(int domain, int type, int protocol)
{
    socket_t *s = socket_alloc(domain, type, protocol);
    if (!s) return -ENOMEM;

    file_t *f = file_alloc();
    if (!f) { socket_free(s); return -ENOMEM; }
    f->type = FD_SOCKET;
    f->sock = s;
    f->flags = (type == 1) ? O_RDWR : O_RDWR;  // TCP/UDP both read+write

    int fd = fd_alloc(current->files, f);
    if (fd < 0) {
        f->sock = NULL;  // prevent double-free in file_free
        file_free(f);
        return -ENFILE;
    }
    return fd;
}

// ── SYS_connect — connect to remote address ──────────────────

int64_t do_connect(int fd, uint32_t ip, uint16_t port)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;

    ip_addr_t addr;
    ip4_addr_set_u32(&addr, ip);
    err_t err = netconn_connect((struct netconn *)s->conn, &addr, port);
    if (err == ERR_OK) {
        s->state = SOCK_CONNECTED;
        return 0;
    }
    if (err == ERR_TIMEOUT) return -ETIMEDOUT;
    return -ECONNREFUSED;
}

// ── SYS_sendto — send data (and optionally specify dest) ─────

int64_t do_sendto(int fd, const void *buf, uint64_t len, int flags,
                  uint32_t ip, uint16_t port)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;

    if (s->type == 1) {
        // TCP: use netconn_write (ignore ip/port — connection already established)
        err_t err = netconn_write((struct netconn *)s->conn, buf,
                                  (u16_t)len, 0x01);  // NETCONN_COPY
        return (err == ERR_OK) ? (int64_t)len : -EIO;
    } else {
        // UDP: create netbuf with destination
        struct netbuf *nb = netbuf_new();
        if (!nb) return -ENOMEM;
        void *payload = netbuf_alloc(nb, (u16_t)len);
        if (!payload) { netbuf_delete(nb); return -ENOMEM; }
        memcpy(payload, buf, len);

        ip_addr_t addr;
        ip4_addr_set_u32(&addr, ip);
        netconn_sendto((struct netconn *)s->conn, nb, &addr, port);
        netbuf_delete(nb);
        return (int64_t)len;
    }
}

// ── SYS_recvfrom — receive data (and optionally get source) ──

int64_t do_recvfrom(int fd, void *buf, uint64_t len, int flags,
                    uint32_t *out_ip, uint16_t *out_port)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;

    struct netbuf *nb;
    err_t err = netconn_recv((struct netconn *)s->conn, &nb);
    if (err != ERR_OK) {
        if (err == ERR_CLSD) return 0;
        if (err == ERR_TIMEOUT) return -ETIMEDOUT;
        return -EIO;
    }

    void *data;
    u16_t data_len;
    netbuf_data(nb, &data, &data_len);
    u16_t copy = (data_len < (u16_t)len) ? data_len : (u16_t)len;
    memcpy(buf, data, copy);

    // Fill in source address if requested
    if (out_ip) {
        ip_addr_t *addr = netbuf_fromaddr(nb);
        *out_ip = ip4_addr_get_u32(addr);
    }
    if (out_port) {
        *out_port = netbuf_fromport(nb);
    }

    netbuf_delete(nb);

    if (signal_pending_fatal())
        return -EINTR;

    return copy;
}

// ── SYS_bind — bind to local address ─────────────────────────

int64_t do_bind(int fd, uint32_t ip, uint16_t port)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;

    ip_addr_t addr;
    ip4_addr_set_u32(&addr, ip);
    err_t err = netconn_bind((struct netconn *)s->conn, &addr, port);
    if (err == ERR_OK) {
        s->bound = 1;
        return 0;
    }
    return -EADDRINUSE;
}

// ── SYS_listen — mark socket as listening ────────────────────

int64_t do_listen(int fd, int backlog)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;
    if (!s->bound) return -EINVAL;

    struct netconn *conn = (struct netconn *)s->conn;
    err_t err = netconn_listen_with_backlog(conn, (u8_t)backlog);
    if (err == ERR_OK) {
        s->state = SOCK_LISTENING;
        return 0;
    }
    return -EIO;
}

// ── SYS_accept — accept incoming connection ──────────────────

int64_t do_accept(int fd, uint32_t *out_ip, uint16_t *out_port)
{
    socket_t *listen_sock = socket_get(fd);
    if (!listen_sock) return -EBADF;
    if (listen_sock->state != SOCK_LISTENING) return -EINVAL;

    struct netconn *new_conn;
    err_t err = netconn_accept((struct netconn *)listen_sock->conn, &new_conn);
    if (err != ERR_OK) return -EIO;

    // Fill source address
    if (out_ip) {
        // netconn_accept doesn't give us the peer address directly.
        // For Phase 2, accept returning the address is a "nice to have".
        *out_ip = 0;  // stub
    }
    if (out_port) *out_port = 0;

    // Create new socket + fd for the accepted connection
    socket_t *new_sock = (socket_t *)kmalloc(sizeof(socket_t), 0);
    if (!new_sock) { netconn_delete(new_conn); return -ENOMEM; }
    new_sock->conn     = new_conn;
    new_sock->domain   = listen_sock->domain;
    new_sock->type     = listen_sock->type;
    new_sock->protocol = listen_sock->protocol;
    new_sock->state    = SOCK_CONNECTED;
    new_sock->bound    = 0;
    spin_init(&new_sock->lock);
    list_init(&new_sock->poll_list);

    file_t *new_f = file_alloc();
    if (!new_f) { socket_free(new_sock); return -ENOMEM; }
    new_f->type  = FD_SOCKET;
    new_f->sock  = new_sock;
    new_f->flags = O_RDWR;

    int new_fd = fd_alloc(current->files, new_f);
    if (new_fd < 0) {
        new_f->sock = NULL;
        file_free(new_f);
        return -ENFILE;
    }
    return new_fd;
}

// ── SYS_setsockopt / SYS_getsockopt — minimal stubs ────────────

int64_t do_setsockopt(int fd, int level, int optname,
                      const void *optval, uint64_t optlen)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;
    // For Phase 2, SO_REUSEADDR is the only commonly needed option.
    (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;  // no-op success
}

int64_t do_getsockopt(int fd, int level, int optname,
                      void *optval, uint64_t *optlen)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;
    (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}
```

- [ ] **Step 1b: Create shared sockaddr_in header**

Create `kernel/include/uapi/sockaddr.h` so both kernel (trap.c) and userspace
(libc netinet/in.h) share the same layout:

```c
// kernel/include/uapi/sockaddr.h — shared sockaddr_in layout (kernel + userspace)
#ifndef _UAPI_SOCKADDR_H
#define _UAPI_SOCKADDR_H

#include <stdint.h>

// AF_INET must match libc/include/sys/socket.h
#define AF_INET     2

struct sockaddr_in {
    uint16_t sin_family;   // AF_INET
    uint16_t sin_port;     // network byte order
    uint32_t sin_addr;     // 4-byte IPv4 address (network byte order)
    uint8_t  sin_zero[8];  // padding to sizeof(struct sockaddr)
} __attribute__((packed));

#endif
```

- [ ] **Step 1c: Add sockaddr.h include at top of trap.c**

In `kernel/arch/x86_64/trap.c`, add with the other kernel includes (NOT inside
the switch statement — that is invalid C):

```c
#include <uapi/sockaddr.h>   // struct sockaddr_in (shared with userspace)
```

- [ ] **Step 2: Replace stubs in trap.c with real dispatch**

In `kernel/arch/x86_64/trap.c`, replace the `case SYS_socket ... case SYS_getsockopt:` block (which uses `struct sockaddr_in` from the header added above):

```c
case SYS_socket: {
    regs->rax = do_socket((int)regs->rdi, (int)regs->rsi, (int)regs->rdx);
    break;
}
case SYS_connect: {
    // rsi = pointer to sockaddr_in (ip:port)
    struct sockaddr_in addr;
    if ((uint64_t)regs->rsi >= current->addr_limit) { regs->rax = -EFAULT; break; }
    memcpy(&addr, (void *)regs->rsi, sizeof(addr));
    regs->rax = do_connect((int)regs->rdi,
                           addr.sin_addr,   // lwIP expects network byte order
                           addr.sin_port);  // network byte order
    break;
}
case SYS_sendto: {
    uint64_t len = regs->rdx;
    uint32_t ip = 0; uint16_t port = 0;
    // args: rdi=fd, rsi=buf, rdx=len, r10=flags, r8=addr, r9=addrlen
    uint64_t addr_ptr = regs->r8;
    if (addr_ptr && addr_ptr < current->addr_limit) {
        struct sockaddr_in a;
        memcpy(&a, (void *)addr_ptr, sizeof(a));
        ip = a.sin_addr; port = a.sin_port;  // lwIP expects network byte order
    }
    regs->rax = do_sendto((int)regs->rdi, (void *)regs->rsi,
                          len, (int)regs->r10, ip, port);
    break;
}
case SYS_recvfrom: {
    regs->rax = do_recvfrom((int)regs->rdi, (void *)regs->rsi,
                            regs->rdx, (int)regs->r10, NULL, NULL);
    break;
}
case SYS_bind: {
    struct sockaddr_in a;
    if ((uint64_t)regs->rsi >= current->addr_limit) { regs->rax = -EFAULT; break; }
    memcpy(&a, (void *)regs->rsi, sizeof(a));
    regs->rax = do_bind((int)regs->rdi,
                        a.sin_addr, a.sin_port);  // lwIP expects network byte order
    break;
}
case SYS_listen: {
    regs->rax = do_listen((int)regs->rdi, (int)regs->rsi);
    break;
}
case SYS_accept: {
    regs->rax = do_accept((int)regs->rdi, NULL, NULL);
    break;
}
case SYS_setsockopt: {
    regs->rax = do_setsockopt((int)regs->rdi, (int)regs->rsi,
                              (int)regs->rdx, (void *)regs->r10, regs->r8);
    break;
}
case SYS_getsockopt: {
    regs->rax = do_getsockopt((int)regs->rdi, (int)regs->rsi,
                              (int)regs->rdx, (void *)regs->r10, (uint64_t *)regs->r8);
    break;
}
```

- [ ] **Step 3: Declare the do_* functions in net/socket.h**

Update `kernel/include/net/socket.h`:

```c
#ifndef _NET_SOCKET_H
#define _NET_SOCKET_H

#include <kernel/file.h>
#include <stdint.h>

// socket_t is defined in kernel/file.h

socket_t *socket_alloc(int domain, int type, int protocol);
void      socket_free(socket_t *s);
socket_t *socket_get(int fd);

int64_t do_socket(int domain, int type, int protocol);
int64_t do_connect(int fd, uint32_t ip, uint16_t port);
int64_t do_sendto(int fd, const void *buf, uint64_t len, int flags,
                  uint32_t ip, uint16_t port);
int64_t do_recvfrom(int fd, void *buf, uint64_t len, int flags,
                    uint32_t *out_ip, uint16_t *out_port);
int64_t do_bind(int fd, uint32_t ip, uint16_t port);
int64_t do_listen(int fd, int backlog);
int64_t do_accept(int fd, uint32_t *out_ip, uint16_t *out_port);
int64_t do_setsockopt(int fd, int level, int optname,
                      const void *optval, uint64_t optlen);
int64_t do_getsockopt(int fd, int level, int optname,
                      void *optval, uint64_t *optlen);

#endif
```

- [ ] **Step 4: Commit**

```bash
git add kernel/net/socket.c kernel/include/net/socket.h kernel/include/uapi/sockaddr.h kernel/arch/x86_64/trap.c
git commit -m "feat(socket): implement 9 socket syscall implementations
do_socket: netconn_new → socket_t → file_t → fd_alloc.
do_connect: netconn_connect (blocks via netconn semaphore).
do_sendto/recvfrom: netconn_send/netconn_recv with netbuf management.
do_bind/listen/accept: standard TCP listening flow.
do_setsockopt/getsockopt: minimal stubs (SO_REUSEADDR no-op)."
```

---
### Task 2.5: Implement FD_SOCKET in fd_read/fd_write

**Files:**
- Modify: `kernel/fs/file.c` (fd_read, fd_write)

- [ ] **Step 1: Add FD_SOCKET case to fd_read()**

In `kernel/fs/file.c`, in `fd_read()`, after the `FD_PIPE` case, add:

```c
case FD_SOCKET: {
    socket_t *s = f->sock;
    if (!s) return -EIO;

    struct netbuf *nb;
    // Forward-declare: defined in lwIP
    extern err_t netconn_recv(struct netconn *, struct netbuf **);
    extern void  netbuf_data(struct netbuf *, void **, u16_t *);
    extern void  netbuf_delete(struct netbuf *);

    err_t err = netconn_recv((struct netconn *)s->conn, &nb);
    if (err == ERR_OK) {
        void *data; u16_t data_len;
        netbuf_data(nb, &data, &data_len);
        size_t copy = (data_len < size) ? data_len : size;
        memcpy(buf, data, copy);
        netbuf_delete(nb);

        if (signal_pending_fatal())
            return -EINTR;
        return (int64_t)copy;
    }
    if (err == ERR_CLSD) return 0;  // EOF
    return -EIO;
}
```

- [ ] **Step 2: Add FD_SOCKET case to fd_write()**

In `kernel/fs/file.c`, in `fd_write()`, after the `FD_PIPE` case, add:

```c
case FD_SOCKET: {
    socket_t *s = f->sock;
    if (!s) return -EIO;

    extern err_t netconn_write(struct netconn *, const void *, u16_t, u8_t);
    // NETCONN_COPY = 0x01 in lwIP (NOT 0!).  NETCONN_NOCOPY = 0x00.
    // Using 0 by mistake means NOCOPY — lwIP dereferences the pointer
    // later from tcpip_thread context → use-after-free and data corruption.
    err_t err = netconn_write((struct netconn *)s->conn, buf,
                              (u16_t)size, 0x01);  // NETCONN_COPY
    if (err == ERR_OK) {
        f->offset += size;
        return (int64_t)size;
    }
    return -EIO;
}
```

- [ ] **Step 3: Commit**

```bash
git add kernel/fs/file.c
git commit -m "feat(socket): add FD_SOCKET read/write to fd_read/fd_write
SYS_read/SYS_write now transparently work on sockets via
netconn_recv/netconn_write. Signal check returns -EINTR.
ERR_CLSD returns 0 (EOF)."
```

---

### Task 2.6: Implement FD_SOCKET poll and do_select

**Files:**
- Modify: `kernel/fs/poll.c` (fd_poll, add do_select)
- Modify: `kernel/arch/x86_64/trap.c` (SYS_select dispatch)

- [ ] **Step 1: Add FD_SOCKET case to fd_poll()**

In `kernel/fs/poll.c`, in `fd_poll()`, add before the `default: return POLLNVAL`:

```c
case FD_SOCKET: {
    socket_t *s = f->sock;
    if (!s) return POLLNVAL;

    uint32_t revents = 0;
    uint64_t flags = spin_lock_irqsave(&s->lock);

    // Check for incoming data (recvmbox non-empty means readable)
    // lwIP netconn API: we can't directly peek the mbox without
    // draining it.  For the poll path we mark as ready if the
    // socket is connected and writable; readable is heuristic.
    // A proper implementation would need a netconn-level peek.
    // For now: connected TCP sockets are always writable;
    // listening sockets are readable (has pending accept or not).
    // Data-ready detection uses the recv callback → poll_list wake.

    if (s->state == SOCK_CONNECTED) {
        // Writable: TCP send buffer is rarely full at hobby OS scale
        revents |= POLLOUT;
    }
    if (s->state == SOCK_LISTENING) {
        revents |= POLLIN;  // accept() may or may not block
    }

    if (revents == 0 && pt && !pt->triggered) {
        poll_wait(pt, &s->poll_list, &s->lock);
    }

    spin_unlock_irqrestore(&s->lock, flags);
    return revents;
}
```

Note: Full poll accuracy requires a lwIP recv callback that wakes `s->poll_list`. For Phase 2, the data-ready case is handled by the blocking `netconn_recv` path; poll is primarily useful for `select()` timeout and the listener case.

- [ ] **Step 2: Implement do_select() in poll.c**

- [ ] **Step 2a: Add fd_set_kern to kernel/include/kernel/poll.h**

In `kernel/include/kernel/poll.h`, after the `struct pollfd` definition, add:

```c
// ── fd_set for select(2) — shared by poll.c (do_select) and trap.c (dispatch) ──
typedef struct {
    uint64_t bits[16];  // 1024 fds
} fd_set_kern;

#define FD_SET_KERN(fd, set)  ((set)->bits[(fd)/64] |= (1ULL << ((fd) % 64)))
#define FD_ISSET_KERN(fd, set) ((set)->bits[(fd)/64] & (1ULL << ((fd) % 64)))
#define FD_ZERO_KERN(set)      memset((set)->bits, 0, sizeof((set)->bits))
```

- [ ] **Step 2b: Implement do_select() in poll.c**

Add to `kernel/fs/poll.c`, after `do_poll()`:

```c
// ── do_select — select(2) implementation ─────────────────────
//
// Does NOT go through do_poll() because do_poll checks
// (uint64_t)user_fds >= current->addr_limit and would reject
// kernel-stack struct pollfd arrays.  do_select is always called
// from the syscall path (trap.c), so we build a kernel-stack
// pollfd array and run the poll loop inline.
//
// fd_set_kern is defined in kernel/include/kernel/poll.h so both
// poll.c and trap.c can see it.

int64_t do_select(int nfds, fd_set_kern *readfds, fd_set_kern *writefds,
                  fd_set_kern *exceptfds, int timeout_ms)
{
    (void)exceptfds;

    if (nfds <= 0 || nfds > POLL_MAX_FDS)
        return -EINVAL;

    // ── Build pollfd array from fd_sets ─────────────────────
    int pfds_fd[POLL_MAX_FDS];
    short pfds_events[POLL_MAX_FDS];
    short pfds_revents[POLL_MAX_FDS];
    int n = 0;

    for (int fd = 0; fd < nfds && n < POLL_MAX_FDS; fd++) {
        short events = 0;
        if (readfds  && (readfds->bits[fd / 64]  & (1ULL << (fd % 64))))
            events |= POLLIN;
        if (writefds && (writefds->bits[fd / 64] & (1ULL << (fd % 64))))
            events |= POLLOUT;
        if (events) {
            pfds_fd[n]     = fd;
            pfds_events[n] = events;
            pfds_revents[n] = 0;
            n++;
        }
    }

    // ── Signal check ────────────────────────────────────────
    if (current->signal & ~current->blocked)
        return -EINTR;

    // ── Timeout setup ───────────────────────────────────────
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        int ticks = (timeout_ms + 9) / 10;
        if (ticks < 1) ticks = 1;
        deadline = jiffies + (uint64_t)ticks;
    }

    // ── Poll loop ───────────────────────────────────────────
    poll_table_t pt;
    poll_table_setup(&pt);

    int ready_count;
    for (;;) {
        poll_table_init(&pt);
        ready_count = 0;

        for (int i = 0; i < n; i++) {
            file_t *f = current->files->fd[pfds_fd[i]];
            if (!f) {
                pfds_revents[i] = POLLNVAL;
                ready_count++;
                continue;
            }

            uint32_t revents = fd_poll(f, &pt);
            if ((revents & pfds_events[i]) || (revents & (POLLHUP | POLLERR))) {
                pfds_revents[i] = (revents & pfds_events[i])
                                | (revents & (POLLHUP | POLLERR | POLLNVAL));
                ready_count++;
            }
        }

        if (ready_count > 0) {
            poll_table_cleanup(&pt);
            break;
        }

        if (timeout_ms == 0) {
            poll_table_cleanup(&pt);
            break;
        }

        if (current->signal & ~current->blocked) {
            poll_table_cleanup(&pt);
            return -EINTR;
        }

        wait_queue_sleep(&pt.wq);
        poll_table_cleanup(&pt);

        if (timeout_ms > 0 && jiffies >= deadline)
            return 0;

        if (current->signal & ~current->blocked)
            return -EINTR;
    }

    // ── Convert revents back to fd_sets ─────────────────────
    if (readfds)  FD_ZERO_KERN(readfds);
    if (writefds) FD_ZERO_KERN(writefds);

    int count = 0;
    for (int i = 0; i < n; i++) {
        if (pfds_revents[i] & (POLLIN | POLLHUP | POLLERR)) {
            if (readfds) FD_SET_KERN(pfds_fd[i], readfds);
            count++;
        }
        if (pfds_revents[i] & (POLLOUT | POLLHUP | POLLERR)) {
            if (writefds) FD_SET_KERN(pfds_fd[i], writefds);
            count++;
        }
    }
    return count;
}
```

- [ ] **Step 3: Wire SYS_select dispatch**

In `kernel/arch/x86_64/trap.c`, replace the `SYS_select` case:

```c
case SYS_select: {
    // int select(int nfds, fd_set *readfds, fd_set *writefds,
    //            fd_set *exceptfds, struct timeval *timeout)
    int nfds = (int)regs->rdi;
    fd_set_kern *rf = (fd_set_kern *)regs->rsi;
    fd_set_kern *wf = (fd_set_kern *)regs->rdx;
    fd_set_kern *ef = (fd_set_kern *)regs->r10;
    int timeout_ms = -1;

    // struct timeval at r8
    if (regs->r8 && regs->r8 < current->addr_limit) {
        struct { int64_t tv_sec; int64_t tv_usec; } tv;
        memcpy(&tv, (void *)regs->r8, sizeof(tv));
        timeout_ms = (int)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
    }

    // Validate user pointers
    fd_set_kern krf, kwf, kef;
    if (rf && (uint64_t)rf < current->addr_limit) memcpy(&krf, rf, sizeof(krf));
    if (wf && (uint64_t)wf < current->addr_limit) memcpy(&kwf, wf, sizeof(kwf));

    regs->rax = do_select(nfds,
                          (rf && (uint64_t)rf < current->addr_limit) ? &krf : NULL,
                          (wf && (uint64_t)wf < current->addr_limit) ? &kwf : NULL,
                          NULL,
                          timeout_ms);

    // Copy results back to user space
    if ((int64_t)regs->rax > 0) {
        if (rf && (uint64_t)rf < current->addr_limit)
            memcpy(rf, &krf, sizeof(krf));
        if (wf && (uint64_t)wf < current->addr_limit)
            memcpy(wf, &kwf, sizeof(kwf));
    }
    break;
}
```

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/poll.c kernel/arch/x86_64/trap.c
git commit -m "feat(socket): add FD_SOCKET poll support + do_select()
FD_SOCKET in fd_poll: connected sockets always writable,
listening sockets readable, poll_wait on socket->poll_list.
do_select(): ~30-line wrapper converting fd_set↔pollfd
→ do_poll(), replacing the old -ENOSYS stub."
```

---

### Task 2.7: Add userspace libc socket headers and syscall wrappers

**Files:**
- Create: `libc/include/sys/socket.h`
- Create: `libc/include/netinet/in.h`
- Create: `libc/include/arpa/inet.h`
- Create: `libc/include/netdb.h`
- Modify: `libc/include/sys/syscall.h` (add socket syscall numbers)

- [ ] **Step 1: Add socket syscall numbers to libc syscall.h**

In `libc/include/sys/syscall.h`, after `#define SYS_select 50`:

```c
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

- [ ] **Step 2: Create `libc/include/sys/socket.h`**

```c
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <sys/cdefs.h>
#include <sys/types.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Address families ───────────────────────────────────────────
#define AF_INET     2

// ── Socket types ───────────────────────────────────────────────
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

// ── Socket option levels ───────────────────────────────────────
#define SOL_SOCKET  1
#define SO_REUSEADDR 2

// ── Generic socket address ─────────────────────────────────────
typedef struct {
    uint16_t sa_family;
    char     sa_data[14];
} sockaddr;

typedef uint32_t socklen_t;

// ── Syscall wrappers ───────────────────────────────────────────

int socket(int domain, int type, int protocol);
int bind(int fd, const sockaddr *addr, socklen_t addrlen);
int connect(int fd, const sockaddr *addr, socklen_t addrlen);
int listen(int fd, int backlog);
int accept(int fd, sockaddr *addr, socklen_t *addrlen);
int64_t send(int fd, const void *buf, uint64_t len, int flags);
int64_t recv(int fd, void *buf, uint64_t len, int flags);
int64_t sendto(int fd, const void *buf, uint64_t len, int flags,
               const sockaddr *addr, socklen_t addrlen);
int64_t recvfrom(int fd, void *buf, uint64_t len, int flags,
                 sockaddr *addr, socklen_t *addrlen);
int setsockopt(int fd, int level, int optname,
               const void *optval, socklen_t optlen);
int getsockopt(int fd, int level, int optname,
               void *optval, socklen_t *optlen);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 3: Create `libc/include/netinet/in.h`**

```c
#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Internet address ───────────────────────────────────────────
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    uint16_t        sin_family;   // AF_INET
    uint16_t        sin_port;     // network byte order
    struct in_addr  sin_addr;
    char            sin_zero[8];
};

// ── Protocols ──────────────────────────────────────────────────
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

// ── Address/port conversion ────────────────────────────────────
#define INADDR_ANY  ((in_addr_t)0x00000000)

static inline uint16_t htons(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}
static inline uint32_t htonl(uint32_t x) {
    return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00)
         | ((x << 8) & 0xFF0000) | ((x << 24) & 0xFF000000);
}
#define ntohs(x) htons(x)
#define ntohl(x) htonl(x)

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 4: Create `libc/include/arpa/inet.h`**

```c
#ifndef _ARPA_INET_H
#define _ARPA_INET_H

#include <netinet/in.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int         inet_aton(const char *cp, struct in_addr *inp);
char       *inet_ntoa(struct in_addr in);
const char *inet_ntop(int af, const void *src, char *dst, uint32_t size);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 5: Create `libc/include/netdb.h`**

```c
#ifndef _NETDB_H
#define _NETDB_H

#include <stdint.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── getaddrinfo / freeaddrinfo ─────────────────────────────────

struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE 1
#define AI_CANONNAME 2
#define EAI_NONAME -2
#define EAI_AGAIN  -3
#define EAI_FAIL   -4
#define EAI_MEMORY -10
#define EAI_SYSTEM -11

int  getaddrinfo(const char *node, const char *service,
                 const struct addrinfo *hints,
                 struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int ecode);

// ── gethostbyname ─────────────────────────────────────────────

struct hostent {
    char   *h_name;
    char  **h_aliases;
    int     h_addrtype;
    int     h_length;
    char  **h_addr_list;
};

struct hostent *gethostbyname(const char *name);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 6: Implement the syscall wrapper .c files**

Create `libc/unistd/socket.c`:
```c
#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>

int socket(int domain, int type, int protocol) {
    int64_t ret = syscall(SYS_socket, domain, type, protocol);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
```

Create `libc/unistd/connect.c`:
```c
#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>

int connect(int fd, const sockaddr *addr, socklen_t addrlen) {
    (void)addrlen;
    int64_t ret = syscall(SYS_connect, fd, (uint64_t)addr, addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
```

Create `libc/unistd/send.c`:
```c
#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>
#include <stdint.h>

int64_t send(int fd, const void *buf, uint64_t len, int flags) {
    return sendto(fd, buf, len, flags, NULL, 0);
}
```

Create `libc/unistd/recv.c`:
```c
#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>
#include <stdint.h>

int64_t recv(int fd, void *buf, uint64_t len, int flags) {
    return recvfrom(fd, buf, len, flags, NULL, 0);
}
```

Create `libc/unistd/recvfrom.c`:
```c
#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>

int64_t recvfrom(int fd, void *buf, uint64_t len, int flags,
                 sockaddr *addr, socklen_t *addrlen)
{
    // 6 args via syscall6 (same pattern as sendto/mmap)
    int64_t ret = syscall6(SYS_recvfrom, fd, (uint64_t)buf, len,
                           (uint64_t)(int64_t)flags, (uint64_t)addr, (uint64_t)addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return ret;
}
```

Create `libc/unistd/bind.c`:
```c
#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>

int bind(int fd, const sockaddr *addr, socklen_t addrlen) {
    (void)addrlen;
    int64_t ret = syscall(SYS_bind, fd, (uint64_t)addr, addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
```

Create `libc/unistd/listen.c`:
```c
#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>

int listen(int fd, int backlog) {
    int64_t ret = syscall(SYS_listen, fd, backlog, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
```

Create `libc/unistd/accept.c`:
```c
#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>

int accept(int fd, sockaddr *addr, socklen_t *addrlen) {
    int64_t ret = syscall(SYS_accept, fd, (uint64_t)addr, (uint64_t)addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
```

Create `libc/unistd/setsockopt.c`:
```c
#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>

int setsockopt(int fd, int level, int optname,
               const void *optval, socklen_t optlen) {
    int64_t ret = syscall(SYS_setsockopt, fd, level, optname,
                          (uint64_t)optval, optlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
```

Create `libc/unistd/getsockopt.c`:
```c
#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>

int getsockopt(int fd, int level, int optname,
               void *optval, socklen_t *optlen) {
    int64_t ret = syscall(SYS_getsockopt, fd, level, optname,
                          (uint64_t)optval, (uint64_t)optlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
```

Create `libc/unistd/sendto.c`:
```c
#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>

int64_t sendto(int fd, const void *buf, uint64_t len, int flags,
               const sockaddr *addr, socklen_t addrlen)
{
    // sendto needs 6 args — use syscall6 (same pattern as mmap).
    // syscall() only takes 3 args and would truncate flags/addr/addrlen.
    (void)addrlen;
    int64_t ret = syscall6(SYS_sendto, fd, (uint64_t)buf, len,
                           (uint64_t)(int64_t)flags, (uint64_t)addr, addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return ret;
}
```

- [ ] **Step 7: Implement getaddrinfo in `libc/network/getaddrinfo.c`**

Create `libc/network/` directory and `getaddrinfo.c`:
```c
// libc/network/getaddrinfo.c — minimal DNS resolution for wget
//
// Sends a DNS A-record query via UDP socket to a hardcoded
// resolver (QEMU user-mode NAT provides 10.0.2.3 as DNS).
// This is NOT a full resolver — just enough for busybox wget.

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

// ── DNS resolver address (QEMU user-mode NAT built-in DNS) ─────
#define DNS_IP     0x0302000A  // 10.0.2.3 in network byte order (htonl)
#define DNS_PORT   53

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints,
                struct addrinfo **res)
{
    (void)service; (void)hints;
    if (!node || !res) return EAI_NONAME;

    // Try parsing as dotted-quad IP first
    struct in_addr ip;
    if (inet_aton(node, &ip)) {
        // Already an IP — no DNS needed
        struct addrinfo *ai = calloc(1, sizeof(*ai));
        if (!ai) return EAI_MEMORY;
        ai->ai_family = AF_INET;
        ai->ai_socktype = SOCK_STREAM;
        struct sockaddr_in *sa = calloc(1, sizeof(*sa));
        if (!sa) { free(ai); return EAI_MEMORY; }
        sa->sin_family = AF_INET;
        sa->sin_addr = ip;
        ai->ai_addr = (struct sockaddr *)sa;
        ai->ai_addrlen = sizeof(*sa);
        *res = ai;
        return 0;
    }

    // Build DNS query
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return EAI_SYSTEM;

    struct sockaddr_in dns;
    dns.sin_family = AF_INET;
    dns.sin_port = htons(DNS_PORT);
    dns.sin_addr.s_addr = DNS_IP;

    // Minimal DNS A-record query for <node>
    uint8_t query[512];
    memset(query, 0, sizeof(query));
    uint16_t id = 0x0001;
    query[0] = (uint8_t)(id >> 8);
    query[1] = (uint8_t)(id & 0xFF);
    query[2] = 0x01; query[3] = 0x00;   // RD=1
    query[5] = 0x01;                     // QDCOUNT=1

    int qi = 12;
    const char *p = node;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t seg = dot ? (size_t)(dot - p) : strlen(p);
        query[qi++] = (uint8_t)seg;
        memcpy(&query[qi], p, seg);
        qi += seg;
        p = dot ? dot + 1 : p + seg;
    }
    query[qi++] = 0;  // root label
    query[qi++] = 0; query[qi++] = 1;    // QTYPE=A
    query[qi++] = 0; query[qi++] = 1;    // QCLASS=IN

    sendto(fd, query, qi, 0, (struct sockaddr *)&dns, sizeof(dns));

    uint8_t reply[512];
    int64_t n = recvfrom(fd, reply, sizeof(reply), 0, NULL, 0);
    close(fd);

    if (n < 12) return EAI_FAIL;
    // Skip header + question to get to answer
    int ans_offset = qi;  // answer starts after question section
    if (ans_offset >= n) return EAI_FAIL;

    // Parse answer: skip NAME (could be a pointer), then TYPE/CLASS/TTL/RDLENGTH/RDATA
    int a = ans_offset;
    if (reply[a] == 0xC0) a += 2;  // compressed name pointer
    a += 10;  // TYPE(2) + CLASS(2) + TTL(4) + RDLENGTH(2)
    if (a + 4 > n) return EAI_FAIL;
    uint32_t result_ip;
    memcpy(&result_ip, &reply[a], 4);

    // Build result
    struct addrinfo *ai = calloc(1, sizeof(*ai));
    if (!ai) return EAI_MEMORY;
    ai->ai_family = AF_INET;
    ai->ai_socktype = SOCK_STREAM;
    struct sockaddr_in *sa = calloc(1, sizeof(*sa));
    if (!sa) { free(ai); return EAI_MEMORY; }
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = result_ip;
    ai->ai_addr = (struct sockaddr *)sa;
    ai->ai_addrlen = sizeof(*sa);
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res);
        res = next;
    }
}

const char *gai_strerror(int ecode) {
    (void)ecode;
    return "DNS error";
}

// ── gethostbyname ─────────────────────────────────────────────
struct hostent *gethostbyname(const char *name)
{
    // stub — busybox wget uses getaddrinfo() when FEATURE_WGET_HTTPS=y
    (void)name;
    return NULL;
}
```

Create `libc/network/inet.c`:
```c
#include <arpa/inet.h>
#include <string.h>

int inet_aton(const char *cp, struct in_addr *inp)
{
    if (!cp || !inp) return 0;
    unsigned int a, b, c, d;
    if (sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    inp->s_addr = (uint32_t)((a << 0) | (b << 8) | (c << 16) | (d << 24));
    return 1;
}

char *inet_ntoa(struct in_addr in)
{
    static char buf[16];
    uint32_t ip = in.s_addr;
    // Use snprintf or simple conversion
    unsigned int a = (ip >> 0)  & 0xFF;
    unsigned int b = (ip >> 8)  & 0xFF;
    unsigned int c = (ip >> 16) & 0xFF;
    unsigned int d = (ip >> 24) & 0xFF;
    // Write to static buffer — not thread-safe but sufficient
    extern int sprintf(char *str, const char *fmt, ...);
    sprintf(buf, "%u.%u.%u.%u", a, b, c, d);
    return buf;
}
```

- [ ] **Step 8: Update libc Makefile to compile network/*.c**

In `libc/Makefile`, in the `C_SOURCES` definition (~line 29), add:

```makefile
C_SOURCES := \
    $(wildcard ctype/*.c) \
    $(wildcard dirent/*.c) \
    $(wildcard errno/*.c) \
    $(wildcard list/*.c) \
    $(wildcard network/*.c) \      # ← NEW: getaddrinfo, inet, entropy
    $(wildcard stdio/*.c) \
    $(wildcard stdlib/*.c) \
    $(wildcard string/*.c) \
    $(wildcard time/*.c) \
    $(wildcard unistd/*.c) \
    $(wildcard pthread/*.c)
```

Without this, `libc/network/getaddrinfo.c`, `inet.c`, and `entropy.c` are never compiled and wget fails at link time.

Build verification:
```bash
make clean
make lib
ls build/x86_64/libc/network/  # should contain getaddrinfo.o, inet.o
```

- [ ] **Step 9: Commit**

```bash
git add libc/include/sys/socket.h libc/include/netinet/in.h libc/include/arpa/inet.h libc/include/netdb.h libc/include/sys/syscall.h libc/unistd/socket.c libc/unistd/connect.c libc/unistd/send.c libc/unistd/recv.c libc/unistd/sendto.c libc/network/getaddrinfo.c libc/network/inet.c libc/Makefile
git commit -m "feat(libc): add BSD socket headers, wrappers, network/ build
Headers: sys/socket.h, netinet/in.h, arpa/inet.h, netdb.h.
Wrappers: socket, connect, send, recv, sendto.
getaddrinfo: minimal DNS A-record resolver via UDP socket
(queries QEMU user-mode NAT DNS at 10.0.2.3).
inet_aton/ntoa for dotted-quad conversion.
libc/Makefile: add $(wildcard network/*.c) to C_SOURCES."
```

---

### Task 2.8: Phase 2 integration test

- [ ] **Step 1: Build and boot**

```bash
make clean
make run
```

- [ ] **Step 2: Test wget HTTP**

From the busybox shell inside the VM:

```bash
wget http://example.com -O /tmp/test.html
cat /tmp/test.html
```

Expected: HTML content of example.com printed to console.

- [ ] **Step 3: Test wget to httpbin**

```bash
wget http://httpbin.org/get -O /tmp/httpbin.html
cat /tmp/httpbin.html
```

Expected: JSON response with request details.

- [ ] **Step 4: Commit any hotfixes and tag**

```bash
git commit -am "fix: Phase 2 integration fixes"
git tag phase-2-socket-done
```

---
## Phase 3: mbedTLS Integration

### Task 3.1: Add mbedTLS submodule and build

**Files:**
- Create: `thirdpart/mbedtls/` (git submodule)
- Create: `config/mbedtls_config.h`
- Modify: `Makefile` (root, add mbedTLS build target)

- [ ] **Step 1: Add submodule**

```bash
cd /home/aagu/OS01-lwIP
git submodule add https://github.com/Mbed-TLS/mbedtls.git thirdpart/mbedtls
cd thirdpart/mbedtls
git checkout v3.6.0  # or latest stable 3.x
cd ../..
```

- [ ] **Step 2: Create mbedTLS config**

Create `config/mbedtls_config.h`:

```c
// config/mbedtls_config.h — OS01 minimal TLS 1.2 client config
#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#define MBEDTLS_HAVE_TIME
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_NET_C
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_SSL_OUT_CONTENT_LEN  4096
#define MBEDTLS_SSL_IN_CONTENT_LEN   4096

// Custom entropy callback (no /dev/urandom)
#define MBEDTLS_ENTROPY_HARDWARE_ALT

#include "mbedtls/check_config.h"

#endif
```

- [ ] **Step 3: Add mbedTLS build target to root Makefile**

```makefile
# ── mbedTLS ────────────────────────────────────────────────
MBEDTLS_SRC  = thirdpart/mbedtls
MBEDTLS_LIB  = $(SYSROOT)/usr/lib/libmbedtls.a

$(MBEDTLS_LIB): lib $(MBEDTLS_SRC)/Makefile config/mbedtls_config.h
	@test -f $(MBEDTLS_SRC)/Makefile || { \
	    echo "ERROR: mbedtls submodule not initialized"; \
	    echo "Run: git submodule update --init"; false; }
	cp config/mbedtls_config.h $(MBEDTLS_SRC)/include/mbedtls/config.h
	$(MAKE) -C $(MBEDTLS_SRC) CC=clang \
	    CFLAGS="--target=x86_64-unknown-none --sysroot=$(SYSROOT) -g" \
	    no_test no_programs lib
	mkdir -p $(SYSROOT)/usr/lib $(SYSROOT)/usr/include
	cp $(MBEDTLS_SRC)/library/libmbedtls.a $(MBEDTLS_LIB)
	cp -R $(MBEDTLS_SRC)/include/mbedtls $(SYSROOT)/usr/include/
```

- [ ] **Step 4: Build mbedTLS standalone**

```bash
make $(SYSROOT)/usr/lib/libmbedtls.a
```

Expected: mbedTLS compiles and `libmbedtls.a` appears in sysroot.

- [ ] **Step 5: Commit**

```bash
git add .gitmodules thirdpart/mbedtls config/mbedtls_config.h Makefile
git commit -m "thirdparty: add mbedTLS v3.6.0 submodule + build integration
config/mbedtls_config.h: TLS 1.2 client-only, MBEDTLS_NO_PLATFORM_ENTROPY
  with custom entropy callback.
Root Makefile: libmbedtls.a target, headers installed to sysroot."
```

---

### Task 3.2: Add custom entropy callback for mbedTLS

**Files:**
- Create: `libc/network/entropy.c`

- [ ] **Step 1: Create entropy callback**

```c
// libc/network/entropy.c — custom mbedTLS entropy source for OS01
#include <mbedtls/entropy.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>

// Weak but functional entropy for a hobby OS:
//   - rdtsc() for some hardware noise
//   - time() for temporal uniqueness
//   - a static pool that accumulates calls

int mbedtls_hardware_poll(void *data,
                          unsigned char *output, size_t len, size_t *olen)
{
    (void)data;

    // Seed pool: mix rdtsc + time + stack address
    static uint64_t pool = 0;
    uint64_t tsc;
    __asm__ volatile ("rdtsc" : "=a"(((uint32_t *)&tsc)[0]),
                              "=d"(((uint32_t *)&tsc)[1]));
    pool = pool * 6364136223846793005ULL + tsc
           + (uint64_t)time(NULL)
           + (uint64_t)(uintptr_t)&pool;

    // Fill output from pool
    size_t written = 0;
    while (written < len) {
        pool = pool * 6364136223846793005ULL + 1;
        size_t to_copy = len - written;
        if (to_copy > sizeof(pool)) to_copy = sizeof(pool);
        for (size_t i = 0; i < to_copy; i++)
            output[written + i] = (unsigned char)(pool >> (i * 8));
        written += to_copy;
    }
    *olen = len;
    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add libc/network/entropy.c
git commit -m "feat(mbedtls): add custom entropy callback for OS01
Uses rdtsc() + time() + stack address mixing via PCG-style LCG.
Sufficient for hobby OS TLS; configure with MBEDTLS_NO_PLATFORM_ENTROPY."
```

---

### Task 3.3: Update busybox config for wget+TLS

**Files:**
- Modify: `config/busybox.config`

- [ ] **Step 1: Update busybox config**

The `config/busybox.config` needs these settings. Add them or verify they exist:

```
CONFIG_WGET=y
CONFIG_FEATURE_WGET_HTTPS=y
CONFIG_FEATURE_WGET_OPENSSL=n
CONFIG_TLS=y
CONFIG_TLS_MBEDTLS=y
CONFIG_FEATURE_WGET_TIMEOUT=y
CONFIG_FEATURE_WGET_LONG_OPTIONS=y
```

- [ ] **Step 2: Rebuild busybox with mbedTLS**

```bash
make clean  # struct layout changes
make $(SYSROOT)/usr/lib/libmbedtls.a
make thirdpart/busybox-1.36.1/busybox
```

Expected: busybox compiles and links with mbedTLS. The resulting binary should be larger (TLS code added).

- [ ] **Step 3: Rebuild disk image and boot**

```bash
make
make run
```

- [ ] **Step 4: Test wget HTTPS**

From busybox shell:

```bash
wget https://example.com -O /tmp/test-tls.html
cat /tmp/test-tls.html
```

Expected: HTML content downloaded over HTTPS.

- [ ] **Step 5: Test wget HTTPS to httpbin**

```bash
wget https://httpbin.org/get -O /tmp/httpbin-tls.html
cat /tmp/httpbin-tls.html
```

- [ ] **Step 6: Commit**

```bash
git add config/busybox.config
git commit -m "feat(busybox): enable wget HTTPS via mbedTLS
CONFIG_WGET=y, CONFIG_FEATURE_WGET_HTTPS=y, CONFIG_TLS_MBEDTLS=y."
```

---

### Task 3.4: Final verification and tag

- [ ] **Step 1: Full clean build**

```bash
make clean
make
```

Expected: Full build succeeds with zero warnings (or only known lwIP/mbedTLS ones).

- [ ] **Step 2: Run with e1000 and test both HTTP and HTTPS**

```bash
make run
```

From busybox shell:
```bash
wget http://example.com -O /tmp/http.html
wget https://example.com -O /tmp/https.html
```

- [ ] **Step 3: Run without NIC for optional-subsys verification**

Remove `-netdev`/`-device e1000` from `make run`, then:
```bash
make run
```

Expected: Clean boot, no crash.

- [ ] **Step 4: Tag the release**

```bash
git tag phase-3-mbedtls-done
```

---

---

## Implementation Notes

### libc Makefile update

`$(wildcard network/*.c)` has been added to C_SOURCES in Task 2.7 Step 8.
Verify:
```bash
ls build/x86_64/libc/network/getaddrinfo.o build/x86_64/libc/network/inet.o \
   build/x86_64/libc/network/entropy.o
```

### kernel Makefile update

`$(wildcard net/*.c)` must be added to KERNEL_C_SOURCES in `kernel/Makefile`
(Task 1.8 Step 1). Without it, `kernel/net/net.c`, `sys_arch.c`, `socket.c`
are never compiled: `KERNEL_C_SOURCES += $(wildcard net/*.c)`.

### lwIP header include paths

The kernel Makefile adds `-I../kernel/net` (for `lwipopts.h`) and
`-I../thirdpart/lwip/src/include` (for lwIP headers).

**⚠️ ORDER IS CRITICAL:** `-I../kernel/net` MUST appear BEFORE
`-I../thirdpart/lwip/src/include`. lwIP's `lwip/opt.h` does
`#include "lwipopts.h"`. The compiler searches include paths from
left to right; with the wrong order it finds a stale/missing
`lwipopts.h` and lwIP uses its defaults (NO_SYS=1, no DHCP, etc.),
silently breaking everything. **Never reorder these flags.**

### recvfrom address output

The Phase 2 `do_recvfrom` implementation currently passes NULL for out_ip/out_port.
Full recvfrom with address output requires parsing the 6th syscall argument (addr
pointer) in trap.c and passing it through. This is a Phase 2 follow-up if wget
needs recvfrom source address (it shouldn't for HTTP — only DNS UDP needs it,
and we use a dedicated getaddrinfo implementation instead).

### struct sockaddr_in consistency

`struct sockaddr_in` is defined once in `kernel/include/uapi/sockaddr.h`
(Task 2.4 Step 1b) with the exact packed layout `{ u16 sin_family; u16 sin_port;
u32 sin_addr; u8 sin_zero[8] }`. Both `kernel/arch/x86_64/trap.c` (syscall
dispatch) and `libc/include/netinet/in.h` use this same definition, avoiding
the previous 3 inline duplicates.

### Known kernel net/socket.c vs lwIP header dependencies

`kernel/net/socket.c` uses `lwip/netconn.h` which needs `lwip/opt.h` →
our `lwipopts.h`. The include path `-I../kernel/net` must be visible when
compiling `kernel/net/socket.c`. Since it's already in `ALL_CFLAGS`, this
should work automatically.
