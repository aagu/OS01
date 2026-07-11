# Multi-Arch Abstraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create an `arch/` header-level abstraction layer that decouples x86_64-specific code from generic kernel code, establishing a clean porting contract for future aarch64 support.

**Architecture:** 11 new generic headers in `kernel/include/kernel/arch/` dispatch to x86_64 implementations via `#ifdef __x86_64__`. Generic `.c` files switch from `<kernel/arch/x86_64/*.h>` to `<kernel/arch/*.h>` and replace bare `__asm__` with `arch_`-prefixed semantic functions. x86-specific code (IDT, TSS, trampoline, switch_to) stays in `arch/x86_64/` unabstracted. Zero runtime behavior change.

**Tech Stack:** C (clang -target x86_64-unknown-none), GNU Make, QEMU x86_64

**Spec:** `docs/superpowers/specs/2026-07-11-multi-arch-abstraction-design.md`

## Global Constraints

- `make clean && make` must produce 0 warnings after every task
- Zero runtime behavior change — this is a pure refactor
- Generic `.c` files (outside `arch/x86_64/`) must contain zero bare `__asm__` by final verification
- `make test-syscall` must remain 70/70 PASS
- Each task commits independently (git bisectable)
- Do NOT abstract: `get_current_task()`, `switch_to`/`__switch_to`, IDT/GDT, INTR_SAVE_ALL, TSS, trampoline, spinlock.h, cpuid.h, msr.h
- All `arch_` interface names are semantic, not instruction names (e.g. `arch_flush_tlb_page`, not `arch_invlpg`)
- `spinlock.h`: stays in `arch/x86_64/` — headers that include `<kernel/arch/x86_64/spinlock.h>` keep the direct path unchanged
- `flush_tlb()` / `switch_tlb()`: kept as backward-compat aliases in `vmm.h` pointing to `arch_flush_tlb_all()` / `arch_switch_mm()`. All existing callers (vma.c, ahci.c, printk.c, task.c, trap.c) continue to work without per-file changes
- `NR_CPUS`: defined in `arch/cpu.h` x86_64 branch (not in a separate cpumask.h) — keeps the change minimal

---

### Task 1: Create batch-1 arch headers (no dependencies)

**Files:**
- Create: `kernel/include/kernel/arch/cache.h`
- Create: `kernel/include/kernel/arch/elf.h`
- Create: `kernel/include/kernel/arch/segment.h`
- Create: `kernel/include/kernel/arch/barrier.h`
- Create: `kernel/include/kernel/arch/thread.h`
- Create: `kernel/include/kernel/arch/io.h`

**Interfaces:**
- Produces: `arch_flush_dcache(void*, size_t)`, `arch_inval_dcache(void*, size_t)` (x86 no-ops)
- Produces: `ARCH_ELF_MACHINE` (0x3E for x86_64)
- Produces: `ARCH_KERNEL_CS`, `ARCH_KERNEL_DS`, `ARCH_USER_CS`, `ARCH_USER_DS`
- Produces: `arch_mb()`, `arch_rmb()`, `arch_wmb()`
- Produces: `pt_regs_t` (via `#include <kernel/arch/x86_64/regs.h>`)
- Produces: `arch_readb/w/l/q()`, `arch_writeb/w/l/q()`, `arch_inb/outb/inw/outw/ind/outd()`

- [ ] **Step 1: Create `kernel/include/kernel/arch/cache.h`**

```c
#ifndef _ARCH_CACHE_H
#define _ARCH_CACHE_H

#include <stddef.h>

static inline void arch_flush_dcache(void *addr, size_t len) { (void)addr; (void)len; }
static inline void arch_inval_dcache(void *addr, size_t len) { (void)addr; (void)len; }

#endif
```

- [ ] **Step 2: Create `kernel/include/kernel/arch/elf.h`**

```c
#ifndef _ARCH_ELF_H
#define _ARCH_ELF_H

#ifdef __x86_64__
#define ARCH_ELF_MACHINE  0x3E    // EM_X86_64
#elif defined(__aarch64__)
#define ARCH_ELF_MACHINE  0xB7    // EM_AARCH64
#else
#error "Unsupported architecture"
#endif

#endif
```

- [ ] **Step 3: Create `kernel/include/kernel/arch/segment.h`**

```c
#ifndef _ARCH_SEGMENT_H
#define _ARCH_SEGMENT_H

#ifdef __x86_64__
#define ARCH_KERNEL_CS  0x08
#define ARCH_KERNEL_DS  0x10
#define ARCH_USER_CS    0x23
#define ARCH_USER_DS    0x2B
#else
#define ARCH_KERNEL_CS  0
#define ARCH_KERNEL_DS  0
#define ARCH_USER_CS    0
#define ARCH_USER_DS    0
#endif

// Legacy aliases for existing code that uses bare names
#ifndef KERNEL_CS
#define KERNEL_CS  ARCH_KERNEL_CS
#endif
#ifndef KERNEL_DS
#define KERNEL_DS  ARCH_KERNEL_DS
#endif
#ifndef USER_CS
#define USER_CS    ARCH_USER_CS
#endif
#ifndef USER_DS
#define USER_DS    ARCH_USER_DS
#endif

#endif
```

- [ ] **Step 4: Create `kernel/include/kernel/arch/barrier.h`**

```c
#ifndef _ARCH_BARRIER_H
#define _ARCH_BARRIER_H

#ifdef __x86_64__
#define arch_mb()  __asm__ __volatile__("mfence" ::: "memory")
#define arch_rmb() __asm__ __volatile__("lfence" ::: "memory")
#define arch_wmb() __asm__ __volatile__("sfence" ::: "memory")
#elif defined(__aarch64__)
#define arch_mb()  __asm__ __volatile__("dmb sy" ::: "memory")
#define arch_rmb() __asm__ __volatile__("dmb ld" ::: "memory")
#define arch_wmb() __asm__ __volatile__("dmb st" ::: "memory")
#else
#error "Unsupported architecture"
#endif

#endif
```

- [ ] **Step 5: Create `kernel/include/kernel/arch/thread.h`**

```c
#ifndef _ARCH_THREAD_H
#define _ARCH_THREAD_H

#ifdef __x86_64__
#include <kernel/arch/x86_64/regs.h>   // provides pt_regs_t
#elif defined(__aarch64__)
#error "aarch64 thread.h not yet implemented"
#else
#error "Unsupported architecture"
#endif

#endif
```

- [ ] **Step 6: Create `kernel/include/kernel/arch/io.h`**

```c
#ifndef _ARCH_IO_H
#define _ARCH_IO_H

#include <stdint.h>

#ifdef __x86_64__
#include <kernel/arch/x86_64/hw.h>

// MMIO
static inline uint8_t  arch_readb(volatile void *a) { return *(volatile uint8_t  *)a; }
static inline uint16_t arch_readw(volatile void *a) { return *(volatile uint16_t *)a; }
static inline uint32_t arch_readl(volatile void *a) { return *(volatile uint32_t *)a; }
static inline uint64_t arch_readq(volatile void *a) { return *(volatile uint64_t *)a; }
static inline void     arch_writeb(volatile void *a, uint8_t  v) { *(volatile uint8_t  *)a = v; }
static inline void     arch_writew(volatile void *a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline void     arch_writel(volatile void *a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void     arch_writeq(volatile void *a, uint64_t v) { *(volatile uint64_t *)a = v; }

// Port I/O — alias existing hw.h functions
#define arch_inb   inb
#define arch_outb  outb
#define arch_inw   inw
#define arch_outw  outw
#define arch_ind   ind
#define arch_outd  outd

#elif defined(__aarch64__)
static inline uint8_t  arch_readb(volatile void *a) { return *(volatile uint8_t  *)a; }
static inline uint16_t arch_readw(volatile void *a) { return *(volatile uint16_t *)a; }
static inline uint32_t arch_readl(volatile void *a) { return *(volatile uint32_t *)a; }
static inline uint64_t arch_readq(volatile void *a) { return *(volatile uint64_t *)a; }
static inline void     arch_writeb(volatile void *a, uint8_t  v) { *(volatile uint8_t  *)a = v; }
static inline void     arch_writew(volatile void *a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline void     arch_writel(volatile void *a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void     arch_writeq(volatile void *a, uint64_t v) { *(volatile uint64_t *)a = v; }
static inline uint8_t  arch_inb(uint16_t p)  { (void)p; __builtin_trap(); return 0; }
static inline void     arch_outb(uint16_t p, uint8_t d) { (void)p; (void)d; __builtin_trap(); }
static inline uint16_t arch_inw(uint16_t p)  { (void)p; __builtin_trap(); return 0; }
static inline void     arch_outw(uint16_t p, uint16_t d) { (void)p; (void)d; __builtin_trap(); }
static inline uint32_t arch_ind(uint16_t p)  { (void)p; __builtin_trap(); return 0; }
static inline void     arch_outd(uint16_t p, uint32_t d) { (void)p; (void)d; __builtin_trap(); }
#else
#error "Unsupported architecture"
#endif

#endif
```

- [ ] **Step 7: Verify `make clean && make` compiles with 0 warnings**

These headers are pure additions — nothing includes them yet, so no compilation impact. The build must succeed unchanged.

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c 'warning'
# Expected: 0
```

- [ ] **Step 8: Commit**

```bash
cd /home/aagu/OS01
git add kernel/include/kernel/arch/cache.h \
        kernel/include/kernel/arch/elf.h \
        kernel/include/kernel/arch/segment.h \
        kernel/include/kernel/arch/barrier.h \
        kernel/include/kernel/arch/thread.h \
        kernel/include/kernel/arch/io.h
git commit -m "feat(arch): add batch-1 arch-generic headers (io, cache, elf, segment, barrier, thread)

New headers in kernel/include/kernel/arch/ dispatch to x86_64
implementations via #ifdef __x86_64__. Pure additions — nothing
includes them yet, zero compilation impact.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Create batch-2 arch headers (irq, mmu, cpu, atomic, percpu)

**Files:**
- Create: `kernel/include/kernel/arch/irq.h`
- Create: `kernel/include/kernel/arch/mmu.h`
- Create: `kernel/include/kernel/arch/cpu.h`
- Create: `kernel/include/kernel/arch/atomic.h`
- Create: `kernel/include/kernel/arch/percpu.h`

**Interfaces:**
- Consumes: `pt_regs_t` from `arch/thread.h` (Task 1)
- Produces: `arch_irq_state_t`, `arch_local_irq_enable/disable/save/restore()`, `arch_intr_handler_fn`, `arch_install_intr_gate()`, `arch_irq_install()`
- Produces: `arch_flush_tlb_all()`, `arch_flush_tlb_page()`, `arch_switch_mm()`, `arch_virt_to_phys()`
- Produces: `arch_cpu_halt()`, `arch_cpu_pause()`, `arch_nop()`, `arch_cycle_counter()`, `arch_cpu_enable_nx()`, `arch_set_percpu_base()`
- Produces: `arch_atomic_fetch_add/sub()`, `arch_atomic_inc/read/write()`, `arch_atomic_cas()`, `arch_atomic_xchg()`
- Produces: `arch_this_cpu_ptr()` returning `void *` (casted by percpu.h's `this_cpu()`)

- [ ] **Step 1: Create `kernel/include/kernel/arch/irq.h`**

```c
#ifndef _ARCH_IRQ_H
#define _ARCH_IRQ_H

#include <stdint.h>
#include <kernel/arch/thread.h>   // for pt_regs_t

// IRQ state type: 64-bit for RFLAGS (x86) and DAIF (aarch64).
// aarch64 only needs 4 bits, but uint64_t keeps the save/restore
// interface uniform and avoids truncation bugs.
typedef uint64_t arch_irq_state_t;

#ifdef __x86_64__
#include <kernel/arch/x86_64/asm.h>

static inline void arch_local_irq_enable(void)  { sti(); }
static inline void arch_local_irq_disable(void) { cli(); }

static inline arch_irq_state_t arch_local_irq_save(void) {
    arch_irq_state_t flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void arch_local_irq_restore(arch_irq_state_t flags) {
    // Use pushfq/popfq to restore ALL flags (IF, DF, AC, etc.)
    // This is the correct match for arch_local_irq_save() which
    // captures full RFLAGS via pushfq.
    __asm__ __volatile__("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

// Handler table (shared between arch and generic intr/)
typedef void (*arch_intr_handler_fn)(uint64_t nr, uint64_t param, pt_regs_t *regs);
extern arch_intr_handler_fn intr_handler_table[256];
extern void *intr_handler_param[256];

// Architecture-specific IRQ setup
void arch_install_intr_gate(uint8_t vector, void *stub, uint8_t ist);
void arch_irq_install(void);

#elif defined(__aarch64__)
#error "aarch64 irq.h not yet implemented"
#else
#error "Unsupported architecture"
#endif

#endif
```

- [ ] **Step 2: Create `kernel/include/kernel/arch/mmu.h`**

```c
#ifndef _ARCH_MMU_H
#define _ARCH_MMU_H

#include <stdint.h>

#ifdef __x86_64__

// Reload CR3 to flush entire TLB
static inline void arch_flush_tlb_all(void) {
    uint64_t cr3;
    __asm__ __volatile__("movq %%cr3, %0; movq %0, %%cr3" : "=r"(cr3) :: "memory");
}

// Invalidate a single 4KB page mapping
static inline void arch_flush_tlb_page(uintptr_t vaddr) {
    __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr) : "memory");
}

// Switch address space (load page table base)
static inline void arch_switch_mm(uint64_t *pml4) {
    __asm__ __volatile__("movq %0, %%cr3" : : "r"(pml4) : "memory");
}

// Walk 4-level page table (PML4→PDPT→PD→PT), return full physical
// address (page base + in-page offset), or 0 if unmapped.
// Does NOT interpret PTE flags — that stays in arch/x86_64/trap.c.
static inline uintptr_t arch_virt_to_phys(void *pgtbl, uintptr_t va) {
    uint64_t *pml4 = (uint64_t *)pgtbl;
    uint64_t l4 = (va >> 39) & 0x1FF;
    if (!(pml4[l4] & 1)) return 0;
    uint64_t *pml3 = (uint64_t *)((pml4[l4] & ~(uint64_t)0xFFF) + 0xffff800000000000ULL);
    uint64_t l3 = (va >> 30) & 0x1FF;
    if (!(pml3[l3] & 1)) return 0;
    if (pml3[l3] & 0x80)  // 1GB huge page
        return (pml3[l3] & 0xFFFFFC0000000ULL) | (va & 0x3FFFFFFF);
    uint64_t *pml2 = (uint64_t *)((pml3[l3] & ~(uint64_t)0xFFF) + 0xffff800000000000ULL);
    uint64_t l2 = (va >> 21) & 0x1FF;
    if (!(pml2[l2] & 1)) return 0;
    if (pml2[l2] & 0x80)  // 2MB huge page
        return (pml2[l2] & 0xFFFFFFFE00000ULL) | (va & 0x1FFFFF);
    uint64_t *pml1 = (uint64_t *)((pml2[l2] & ~(uint64_t)0xFFF) + 0xffff800000000000ULL);
    uint64_t l1 = (va >> 12) & 0x1FF;
    if (!(pml1[l1] & 1)) return 0;
    return (pml1[l1] & 0xFFFFFFFFFFFFF000ULL) | (va & 0xFFF);
}

#elif defined(__aarch64__)
#error "aarch64 mmu.h not yet implemented"
#else
#error "Unsupported architecture"
#endif

#endif
```

- [ ] **Step 3: Create `kernel/include/kernel/arch/atomic.h`**

```c
#ifndef _ARCH_ATOMIC_H
#define _ARCH_ATOMIC_H

#include <stdint.h>

#ifdef __x86_64__

static inline uint64_t arch_atomic_fetch_add(volatile uint64_t *ptr, uint64_t val) {
    __asm__ __volatile__("lock xaddq %0, %1" : "+r"(val), "+m"(*ptr) : : "memory");
    return val;
}

static inline uint64_t arch_atomic_fetch_sub(volatile uint64_t *ptr, uint64_t val) {
    return arch_atomic_fetch_add(ptr, -(int64_t)val);
}

static inline uint64_t arch_atomic_inc(volatile uint64_t *ptr) {
    return arch_atomic_fetch_add(ptr, 1) + 1;
}

static inline uint64_t arch_atomic_read(volatile uint64_t *ptr) {
    uint64_t val;
    __asm__ __volatile__("movq %1, %0" : "=r"(val) : "m"(*ptr) : "memory");
    return val;
}

static inline void arch_atomic_write(volatile uint64_t *ptr, uint64_t val) {
    __asm__ __volatile__("xchgq %0, %1" : "+r"(val), "+m"(*ptr) : : "memory");
}

static inline int arch_atomic_cas(volatile uint64_t *ptr, uint64_t old, uint64_t new) {
    uint8_t result;
    __asm__ __volatile__("lock cmpxchgq %3, %1; sete %0"
                         : "=a"(result), "+m"(*ptr) : "a"(old), "r"(new) : "memory");
    return result;
}

static inline uint64_t arch_atomic_xchg(volatile uint64_t *ptr, uint64_t val) {
    __asm__ __volatile__("xchgq %0, %1" : "+r"(val), "+m"(*ptr) : : "memory");
    return val;
}

#elif defined(__aarch64__)
#error "aarch64 atomic.h not yet implemented"
#else
#error "Unsupported architecture"
#endif

#endif
```

- [ ] **Step 4: Create `kernel/include/kernel/arch/cpu.h`**

```c
#ifndef _ARCH_CPU_H
#define _ARCH_CPU_H

#include <stdint.h>

#ifdef __x86_64__
#include <kernel/arch/x86_64/asm.h>
#include <kernel/arch/x86_64/cpu.h>    // for rdtsc(), NR_CPUS

static inline void     arch_cpu_halt(void)      { hlt(); }
static inline void     arch_cpu_pause(void)     { __asm__ __volatile__("pause"); }
static inline void     arch_nop(void)           { __asm__ __volatile__("nop"); }
static inline uint64_t arch_cycle_counter(void) { return rdtsc(); }

// NR_CPUS — architecture-specific max CPU count.
// Defined here (as well as in arch/x86_64/cpu.h for backward compat)
// so generic headers like task.h and percpu.h can find it.
#ifndef NR_CPUS
#define NR_CPUS 8
#endif

// Enable No-eXecute: set EFER.NXE (bit 11)
static inline void arch_cpu_enable_nx(void) {
    uint32_t eax, edx;
    __asm__ __volatile__("rdmsr" : "=a"(eax), "=d"(edx) : "c"(0xC0000080));
    if (!(eax & (1 << 11))) {
        eax |= (1 << 11);
        __asm__ __volatile__("wrmsr" : : "a"(eax), "d"(edx), "c"(0xC0000080));
    }
}

// Set per-CPU data base pointer (GS on x86, tpidr_el1 on aarch64)
static inline void arch_set_percpu_base(void *ptr) {
    uint32_t lo = (uint32_t)(uintptr_t)ptr;
    uint32_t hi = (uint32_t)((uintptr_t)ptr >> 32);
    __asm__ __volatile__("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000101) : "memory");
}

#elif defined(__aarch64__)
#error "aarch64 cpu.h not yet implemented"
#else
#error "Unsupported architecture"
#endif

#endif
```

- [ ] **Step 5: Create `kernel/include/kernel/arch/percpu.h`**

```c
#ifndef _ARCH_PERCPU_H
#define _ARCH_PERCPU_H

#include <stdint.h>

// Returns the raw per-CPU data pointer for the current CPU.
// Caller (percpu.h's this_cpu()) casts to percpu_t *.
// Returns void * to avoid circular dependency with percpu_t definition.
#ifdef __x86_64__
static inline void *arch_this_cpu_ptr(void) {
    void *ptr;
    __asm__ __volatile__("movq %%gs:0, %0" : "=r"(ptr));
    return ptr;
}
#elif defined(__aarch64__)
static inline void *arch_this_cpu_ptr(void) {
    void *ptr;
    __asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(ptr));
    return ptr;
}
#else
#error "Unsupported architecture"
#endif

#endif
```

> **Note**: `percpu.h`'s `this_cpu()` calls `arch_this_cpu_ptr()` and casts the `void *` to `percpu_t *`.  This breaks the cycle: `percpu.h` includes `arch/percpu.h`, but `arch/percpu.h` does NOT include `percpu.h` — it only returns an opaque pointer.

- [ ] **Step 6: Verify `make clean && make` compiles with 0 warnings**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -i 'warning\|error'
# Expected: no output (0 warnings, 0 errors)
```

- [ ] **Step 7: Commit**

```bash
cd /home/aagu/OS01
git add kernel/include/kernel/arch/irq.h \
        kernel/include/kernel/arch/mmu.h \
        kernel/include/kernel/arch/cpu.h \
        kernel/include/kernel/arch/atomic.h \
        kernel/include/kernel/arch/percpu.h
git commit -m "feat(arch): add batch-2 arch-generic headers (irq, mmu, cpu, atomic, percpu)

All 11 arch/ headers now created. Each dispatches to x86_64
implementation via #ifdef __x86_64__. Zero compilation impact —
nothing includes them yet.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Update x86_64 headers + create arch/x86_64/irq.c

**Files:**
- Modify: `kernel/include/kernel/arch/x86_64/hw.h` — add MMIO accessors
- Modify: `kernel/include/kernel/arch/x86_64/cpu.h` — add `arch_pause()`
- Modify: `kernel/include/kernel/arch/x86_64/asm.h` — no changes needed (nop exists)
- Modify: `kernel/intr/irq.c` — extract x86-specific IDT setup
- Create: `kernel/arch/x86_64/irq.c` — IDT IRQ gate installation

**Interfaces:**
- Produces: `readb/w/l/q()`, `writeb/w/l/q()` (in hw.h)
- Produces: `arch_pause()` (in cpu.h)
- Produces: `arch_irq_install()`, `arch_install_intr_gate()` (in new irq.c)

- [ ] **Step 1: Add MMIO accessors to `kernel/include/kernel/arch/x86_64/hw.h`**

Read the existing file, then append the following before the final `#endif`:

```c
// ── MMIO accessors ─────────────────────────────────────────
static inline uint8_t  readb(volatile void *a)  { return *(volatile uint8_t  *)a; }
static inline uint16_t readw(volatile void *a)  { return *(volatile uint16_t *)a; }
static inline uint32_t readl(volatile void *a)  { return *(volatile uint32_t *)a; }
static inline uint64_t readq(volatile void *a)  { return *(volatile uint64_t *)a; }
static inline void     writeb(volatile void *a, uint8_t  v) { *(volatile uint8_t  *)a = v; }
static inline void     writew(volatile void *a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline void     writel(volatile void *a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void     writeq(volatile void *a, uint64_t v) { *(volatile uint64_t *)a = v; }
```

- [ ] **Step 2: Add `arch_pause()` to `kernel/include/kernel/arch/x86_64/cpu.h`**

Append before the final `#endif`:

```c
// Spin-wait hint (defined here so arch/cpu.h can forward it)
static inline void arch_pause(void) { __asm__ __volatile__("pause"); }
```

- [ ] **Step 3: Create `kernel/arch/x86_64/irq.c` — minimal (no Build_IRQ yet)**

Build_IRQ will cause duplicate symbol errors if both intr/irq.c AND arch/x86_64/irq.c define the same IRQ stubs. To keep each commit bisectable, create the file NOW with only `arch_install_intr_gate()`. The Build_IRQ code stays in `intr/irq.c` until Task 6 when it's moved.

```c
// kernel/arch/x86_64/irq.c — x86-specific IRQ gate installation
//
// Build_IRQ expansions and arch_irq_install() are still in
// intr/irq.c at this stage.  They will be moved here in Task 6
// when intr/irq.c is cleaned up.

#include <kernel/arch/x86_64/gate.h>

void arch_install_intr_gate(uint8_t vector, void *stub, uint8_t ist) {
    set_intr_gate_raw(vector, ist, stub);
}
```

- [ ] **Step 4: Verify `make clean && make` compiles with 0 warnings**

No duplicate symbols — only `arch_install_intr_gate` is new and is not called yet.

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -ci 'warning'
# Expected: 0
```

- [ ] **Step 5: Commit**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -ci 'warning'
# Expected: 0
```

```bash
cd /home/aagu/OS01
git add kernel/include/kernel/arch/x86_64/hw.h \
        kernel/include/kernel/arch/x86_64/cpu.h \
        kernel/arch/x86_64/irq.c
git commit -m "refactor(arch): add MMIO accessors, arch_pause(), arch/x86_64/irq.c

- hw.h: add readb/w/l/q, writeb/w/l/q MMIO inline functions
- cpu.h: add arch_pause() spin-wait hint
- arch/x86_64/irq.c: extract IDT IRQ gate setup from intr/irq.c
  (Build_IRQ expansions temporarily disabled until intr/irq.c cleanup)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Update kernel headers

**Files:**
- Modify: `kernel/include/kernel/task.h`
- Modify: `kernel/include/kernel/percpu.h`
- Modify: `kernel/include/kernel/vmm.h`
- Modify: `kernel/include/kernel/interrupt.h`
- Modify: `kernel/include/kernel/apic.h`
- Modify: `kernel/include/kernel/trace.h`
- Modify: `kernel/include/kernel/wait.h`

**Interfaces:**
- Consumes: All arch/ headers from Tasks 1-2
- Produces: Updated task.h (uses arch/cpu.h, arch/thread.h, arch/segment.h)
- Produces: Updated percpu.h (uses arch/percpu.h, `this_cpu()` wraps `arch_this_cpu()`)
- Produces: Updated vmm.h (flush_tlb/switch_tlb aliases to arch/mmu.h)
- Produces: Updated interrupt.h, trace.h, wait.h (use arch/irq.h, arch/thread.h)

- [ ] **Step 1: Read current `kernel/include/kernel/task.h` to find all x86 references**

```bash
cd /home/aagu/OS01
grep -n 'x86_64\|KERNEL_CS\|KERNEL_DS\|USER_CS\|USER_DS\|#include.*arch/' kernel/include/kernel/task.h
```

- [ ] **Step 2: Update `kernel/include/kernel/task.h`**

Replace:
- `#include <kernel/arch/x86_64/regs.h>` → `#include <kernel/arch/thread.h>`
- `#include <kernel/arch/x86_64/cpu.h>` → `#include <kernel/arch/cpu.h>`
- Add: `#include <kernel/arch/segment.h>`
- Remove bare `#define KERNEL_CS ... #define USER_DS ...` (now provided by arch/segment.h legacy aliases)

Verify by searching for the exact lines to change:

```bash
grep -n '#include.*arch/x86_64' kernel/include/kernel/task.h
grep -n '#define KERNEL_CS\|#define KERNEL_DS\|#define USER_CS\|#define USER_DS' kernel/include/kernel/task.h
```

Apply the edits — remove the bare defines and update includes to arch/ headers.

- [ ] **Step 3: Update `kernel/include/kernel/percpu.h`**

Replace the inline `this_cpu()` function body with a call to `arch_this_cpu()`:

```c
#include <kernel/arch/percpu.h>

static inline percpu_t *this_cpu(void)
{
    return (percpu_t *)arch_this_cpu_ptr();
}
```

Also rename `apic_id` to `arch_processor_id` in the `percpu_t` struct:

Old:
```c
uint32_t apic_id;           // Local APIC ID (from MADT)
```

New:
```c
uint32_t arch_processor_id; // APIC ID (x86) / MPIDR_EL1 (aarch64)
```

Remove the bare `#ifndef NR_CPUS #error ...` check — NR_CPUS is now guaranteed by `arch/cpu.h` (included via task.h chain).

- [ ] **Step 4: Update `kernel/include/kernel/vmm.h`**

Add `#include <kernel/arch/mmu.h>` and add backward-compatible aliases:

```c
#include <kernel/arch/mmu.h>

// Backward-compatible aliases for existing callers
#define flush_tlb()    arch_flush_tlb_all()
#define switch_tlb(p)  arch_switch_mm((uint64_t *)(p))
```

If `flush_tlb()` / `switch_tlb()` are currently macros in vmm.h, replace their definitions with these aliases pointing to `arch/mmu.h`.

- [ ] **Step 5: Update `kernel/include/kernel/interrupt.h`**

Replace `#include <kernel/arch/x86_64/gate.h>` with `#include <kernel/arch/irq.h>`.

The `arch_intr_handler_fn` typedef and `intr_handler_table` array are now declared in `arch/irq.h`. If `interrupt.h` also declares these, remove the duplicate declarations.

- [ ] **Step 5b: Update `kernel/include/kernel/apic.h`**

Replace `#include <kernel/arch/x86_64/cpu.h>` with `#include <kernel/arch/cpu.h>`.
Only change needed — apic.h includes x86_64/cpu.h solely for NR_CPUS.

- [ ] **Step 6: Update `kernel/include/kernel/trace.h`**

Replace `#include <kernel/arch/x86_64/regs.h>` with `#include <kernel/arch/thread.h>`.

- [ ] **Step 7: Update `kernel/include/kernel/wait.h`**

Replace `#include <kernel/arch/x86_64/regs.h>` with `#include <kernel/arch/thread.h>` (if present).

- [ ] **Step 8: Verify `make clean && make` compiles with 0 warnings**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | tail -20
# Check for errors. Warnings should be 0.
```

- [ ] **Step 9: Commit**

```bash
cd /home/aagu/OS01
git add kernel/include/kernel/task.h \
        kernel/include/kernel/percpu.h \
        kernel/include/kernel/vmm.h \
        kernel/include/kernel/interrupt.h \
        kernel/include/kernel/trace.h \
        kernel/include/kernel/wait.h
git commit -m "refactor(arch): switch kernel headers to arch/ interface

- task.h: use arch/cpu.h, arch/thread.h, arch/segment.h
- percpu.h: delegate this_cpu() to arch_this_cpu();
  rename apic_id → arch_processor_id
- vmm.h: flush_tlb()/switch_tlb() → arch_flush_tlb_all()/
  arch_switch_mm() aliases
- interrupt.h, trace.h, wait.h: use arch/irq.h, arch/thread.h

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5A: Migrate Group A — pure IO + CPU (no IRQ/MMU dependency)

**Files:**
- Modify: `kernel/fs/elf.c`
- Modify: `kernel/memory/pmm.c`
- Modify: `kernel/fs/devfs.c`
- Modify: `kernel/kernel/panic.c`
- Modify: `kernel/driver/pci.c`
- Modify: `kernel/driver/ahci.c`
- Modify: `kernel/percpu/percpu.c`

**Interfaces:**
- Consumes: `arch/elf.h`, `arch/cpu.h`, `arch/io.h`
- No dependency on `arch/irq.h` or `arch/mmu.h`

- [ ] **Step 1: Migrate `kernel/fs/elf.c`**

Change the hardcoded `EM_X86_64` (0x3E) check to use `ARCH_ELF_MACHINE`:

Add at the top: `#include <kernel/arch/elf.h>`

Replace `0x3E` with `ARCH_ELF_MACHINE` in the ELF header validation check.

- [ ] **Step 2: Migrate `kernel/memory/pmm.c`**

Remove `#include <kernel/arch/x86_64/string.h>`.
Ensure `#include <string.h>` (the generic libc version) is present.

- [ ] **Step 3: Migrate `kernel/fs/devfs.c`**

Replace `#include <kernel/arch/x86_64/cpu.h>` with `#include <kernel/arch/cpu.h>`.
Replace `rdtsc()` with `arch_cycle_counter()` in the `/dev/random` read handler.

- [ ] **Step 4: Migrate `kernel/kernel/panic.c`**

Replace bare `cli; hlt`:

Add at top:
```c
#include <kernel/arch/cpu.h>
#include <kernel/arch/irq.h>
```

Replace:
```c
__asm__ __volatile__("cli; hlt");
```
With:
```c
arch_local_irq_disable();
arch_cpu_halt();
```

- [ ] **Step 5: Migrate `kernel/driver/pci.c`**

Replace `ind()`/`outd()` with `arch_ind()`/`arch_outd()`.

Add at top: `#include <kernel/arch/io.h>`

- [ ] **Step 6: Migrate `kernel/driver/ahci.c`**

The `WAIT_WHILE` macro uses `nop()`. Replace `#include <kernel/arch/x86_64/asm.h>` with `#include <kernel/arch/io.h>` and `#include <kernel/arch/cpu.h>`.

Replace `nop()` → `arch_nop()`, `__asm__ __volatile__("pause")` → `arch_cpu_pause()`.

- [ ] **Step 7: Migrate `kernel/percpu/percpu.c`**

Replace:
- `#include <kernel/arch/x86_64/msr.h>` → `#include <kernel/arch/cpu.h>`
- `wrmsr(IA32_GS_BASE, (uint64_t)&percpu_data[cpu])` → `arch_set_percpu_base(&percpu_data[cpu])`
- All `apic_id` field accesses → `arch_processor_id`

- [ ] **Step 8: Verify compile**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -ci 'warning'
# Expected: 0
```

- [ ] **Step 9: Commit**

```bash
cd /home/aagu/OS01
git add kernel/fs/elf.c kernel/memory/pmm.c kernel/fs/devfs.c \
        kernel/kernel/panic.c kernel/driver/pci.c kernel/driver/ahci.c \
        kernel/percpu/percpu.c
git commit -m "refactor(arch): migrate group-A .c files (io, cpu, no irq)

- elf.c: EM_X86_64 → ARCH_ELF_MACHINE
- pmm.c: drop arch/x86_64/string.h → generic <string.h>
- devfs.c: rdtsc() → arch_cycle_counter()
- panic.c: cli/hlt → arch_local_irq_disable/arch_cpu_halt
- pci.c: ind/outd → arch_ind/arch_outd
- ahci.c: nop/pause → arch_nop/arch_cpu_pause
- percpu.c: wrmsr(IA32_GS_BASE) → arch_set_percpu_base; apic_id → arch_processor_id

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5B: Migrate Group B — needs irq.h + io.h

**Files:**
- Modify: `kernel/driver/serial.c`
- Modify: `kernel/driver/keyboard.c`
- Modify: `kernel/apic/ioapic.c`
- Modify: `kernel/apic/lapic.c`

**Interfaces:**
- Consumes: `arch/irq.h`, `arch/io.h`, `arch/cpu.h` (all from prior tasks)

- [ ] **Step 1: Migrate `kernel/driver/serial.c`**

Replace bare `__asm__` with arch calls:

- `__asm__ __volatile__("pushfq; popq %0; cli" : "=r"(irqf) :: "memory")` → `irqf = arch_local_irq_save()`
- `__asm__ __volatile__("sti" ::: "memory")` → `arch_local_irq_restore(irqf)` (or `arch_local_irq_enable()`)
- `__asm__ __volatile__("pause")` → `arch_cpu_pause()`
- `inb()`/`outb()` → `arch_inb()`/`arch_outb()`

Add at top: `#include <kernel/arch/io.h>`, `#include <kernel/arch/cpu.h>`, `#include <kernel/arch/irq.h>`

- [ ] **Step 2: Migrate `kernel/driver/keyboard.c`**

Replace:
- `inb(0x64)` → `arch_inb(0x64)`
- `outb(0x60, ...)` → `arch_outb(0x60, ...)`
- `__asm__ __volatile__("pause")` → `arch_cpu_pause()`

Add at top: `#include <kernel/arch/io.h>`, `#include <kernel/arch/cpu.h>`

- [ ] **Step 3: Migrate `kernel/apic/ioapic.c`**

Replace `#include <kernel/arch/x86_64/asm.h>` and `<kernel/arch/x86_64/hw.h>` with `#include <kernel/arch/io.h>`.

- [ ] **Step 4: Migrate `kernel/apic/lapic.c`**

Replace generic-use arch includes:
- `#include <kernel/arch/x86_64/asm.h>` → `#include <kernel/arch/irq.h>`
- `#include <kernel/arch/x86_64/regs.h>` → `#include <kernel/arch/thread.h>`
- `#include <kernel/arch/x86_64/gate.h>` → keep (x86-specific macro dependencies)

Keep `#include <kernel/arch/x86_64/msr.h>` and `#include <kernel/arch/x86_64/cpuid.h>` — lapic.c is x86-specific and uses MSR/CPUID directly.

- [ ] **Step 5: Verify compile**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -ci 'warning'
# Expected: 0
```

- [ ] **Step 6: Commit**

```bash
cd /home/aagu/OS01
git add kernel/driver/serial.c kernel/driver/keyboard.c \
        kernel/apic/ioapic.c kernel/apic/lapic.c
git commit -m "refactor(arch): migrate group-B .c files (irq + io)

- serial.c: pushfq/cli/sti/pause/inb/outb → arch_* equivalents
- keyboard.c: inb/outb/pause → arch_inb/arch_outb/arch_cpu_pause
- ioapic.c: x86_64/asm.h + hw.h → arch/io.h
- lapic.c: asm.h/regs.h/gate.h → arch/irq.h + arch/thread.h;
  msr.h + cpuid.h kept (x86-specific driver)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5C: Migrate Group C — needs mmu.h + irq.h

**Files:**
- Modify: `kernel/memory/tlb.c`
- Modify: `kernel/memory/vmm.c`
- Modify: `kernel/sched/smp.c`
- Modify: `kernel/apic/lapic_timer.c`
- Modify: `kernel/apic/ipi.c`
- Modify: `kernel/kernel/main.c`

**Interfaces:**
- Consumes: `arch/mmu.h`, `arch/irq.h`, `arch/cpu.h` (all from prior tasks)

- [ ] **Step 1: Migrate `kernel/memory/tlb.c`**

Replace:
- `__asm__ __volatile__("pause")` → `arch_cpu_pause()`
- `__asm__ __volatile__("invlpg (%0)" ...)` → `arch_flush_tlb_page(vaddr)`

Add: `#include <kernel/arch/cpu.h>`, `#include <kernel/arch/mmu.h>`

- [ ] **Step 2: Migrate `kernel/memory/vmm.c`**

Replace:
- `__asm__ __volatile__("invlpg (%0)" ...)` → `arch_flush_tlb_page(vaddr)`

`flush_tlb()` / `switch_tlb()` are already handled by the backward-compat alias in `vmm.h` (Task 4) — no code change needed here for those.

Add: `#include <kernel/arch/mmu.h>`

- [ ] **Step 3: Migrate `kernel/sched/smp.c`**

Replace:
- `__asm__ __volatile__("sti")` → `arch_local_irq_enable()`
- `__asm__ __volatile__("hlt")` → `arch_cpu_halt()`
- `__asm__ __volatile__("pause")` → `arch_cpu_pause()`

**Keep** the `lgdt`/`lidt`/`lretq` sequence — these are x86-specific AP GDT/IDT reload protocol (spec Section 5, NOT abstracted). Add comment: `// x86-specific: AP GDT/IDT reload`.

Add: `#include <kernel/arch/cpu.h>`, `#include <kernel/arch/irq.h>`

- [ ] **Step 4: Migrate `kernel/apic/lapic_timer.c`**

Replace:
- `#include <kernel/arch/x86_64/gate.h>` + `<kernel/arch/x86_64/regs.h>` → `#include <kernel/arch/irq.h>` + `#include <kernel/arch/thread.h>`

Keep `#include <kernel/arch/x86_64/gate.h>` if `DEFINE_INTR_STUB`/`REGISTER_INTR_HANDLER` macros are used (they are x86-specific).

- [ ] **Step 5: Migrate `kernel/apic/ipi.c`**

Replace `#include <kernel/arch/x86_64/gate.h>` with `#include <kernel/arch/irq.h>`.
Keep `#include <kernel/arch/x86_64/gate.h>` for `DEFINE_INTR_STUB`/`REGISTER_INTR_HANDLER`.

- [ ] **Step 6: Migrate `kernel/kernel/main.c`**

Replace:
- `__asm__ __volatile__("cli")` → `arch_local_irq_disable()`
- `__asm__ __volatile__("1: hlt; jmp 1b")` → `while(1) arch_cpu_halt();`
- `asm volatile("rdmsr"...)` + `asm volatile("wrmsr"...)` for NXE enable → `arch_cpu_enable_nx()`
- `wrmsr(IA32_GS_BASE, ...)` → `arch_set_percpu_base(...)`

Add: `#include <kernel/arch/cpu.h>`, `#include <kernel/arch/irq.h>`

- [ ] **Step 7: Verify compile**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -ci 'warning'
# Expected: 0
```

- [ ] **Step 8: Commit**

```bash
cd /home/aagu/OS01
git add kernel/memory/tlb.c kernel/memory/vmm.c kernel/sched/smp.c \
        kernel/apic/lapic_timer.c kernel/apic/ipi.c kernel/kernel/main.c
git commit -m "refactor(arch): migrate group-C .c files (mmu + irq)

- tlb.c, vmm.c: invlpg/pause → arch_flush_tlb_page/arch_cpu_pause
- smp.c: sti/hlt/pause → arch_*; lgdt/lidt kept (x86 GDT/IDT reload)
- lapic_timer.c, ipi.c: gate.h → arch/irq.h
- main.c: cli/hlt → arch_*, rdmsr/wrmsr NXE → arch_cpu_enable_nx,
  wrmsr(GS_BASE) → arch_set_percpu_base

flush_tlb()/switch_tlb() remain as aliases in vmm.h — no caller
changes needed.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5D: Migrate Group D — header re-include + final cleanup

**Files:**
- Modify: `kernel/intr/dispatch.c`
- Modify: `kernel/intr/irq.c` + `kernel/arch/x86_64/irq.c`
- Modify: `kernel/arch/x86_64/trap.c`

**Interfaces:**
- Consumes: all arch/ headers from prior tasks
- Produces: final state — zero bare `__asm__` in generic `.c` files

- [ ] **Step 1: Migrate `kernel/intr/dispatch.c`**

Replace `#include <kernel/arch/x86_64/gate.h>` with `#include <kernel/arch/irq.h>`.

`intr_handler_table` and `intr_handler_param` are now in `arch/irq.h` — verify they're accessible.

- [ ] **Step 2: Migrate `kernel/intr/irq.c` + `kernel/arch/x86_64/irq.c`**

**In `kernel/arch/x86_64/irq.c`** (replacing the minimal version from Task 3):

Move the full Build_IRQ macro, all 16 expansions, and `arch_irq_install()` here from `intr/irq.c`:

```c
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/linkage.h>

extern void ret_from_intr(void);
extern void generic_intr_dispatch(pt_regs_t *regs, uint64_t vector);

#define Build_IRQ(nr)                                                    \
    __asm__(                                                             \
        ".globl " SYMBOL_NAME_STR(IRQ) #nr "_interrupt\n\t"              \
        SYMBOL_NAME_STR(IRQ) #nr "_interrupt:\n\t"                       \
        "pushq  $0\n\t"                   /* dummy error code */         \
        "cld;\n\t"                                                        \
        "pushq  %rax;\n\t"                                               \
        "pushq  %rax;\n\t"                                               \
        "movq   %es,    %rax;\n\t"                                       \
        "pushq  %rax;\n\t"                                               \
        "movq   %ds,    %rax;\n\t"                                       \
        "pushq  %rax;\n\t"                                               \
        "xorq   %rax,   %rax;\n\t"                                       \
        "pushq  %rbp;\n\t"                                               \
        "pushq  %rdi;\n\t"                                               \
        "pushq  %rsi;\n\t"                                               \
        "pushq  %rdx;\n\t"                                               \
        "pushq  %rcx;\n\t"                                               \
        "pushq  %rbx;\n\t"                                               \
        "pushq  %r8;\n\t"                                                \
        "pushq  %r9;\n\t"                                                \
        "pushq  %r10;\n\t"                                               \
        "pushq  %r11;\n\t"                                               \
        "pushq  %r12;\n\t"                                               \
        "pushq  %r13;\n\t"                                               \
        "pushq  %r14;\n\t"                                               \
        "pushq  %r15;\n\t"                                               \
        "movq   $0x10,  %rdx;\n\t"                                       \
        "movq   %rdx,   %ds;\n\t"                                        \
        "movq   %rdx,   %es;\n\t"                                        \
        "movq   %rsp,   %rdi;\n\t"       /* pt_regs* (arg 1) */          \
        "movq   $" #nr ", %rsi;\n\t"     /* vector number (arg 2) */     \
        "leaq   ret_from_intr(%rip), %rax;\n\t"                          \
        "pushq  %rax;\n\t"               /* return via ret_from_intr */  \
        "jmp    generic_intr_dispatch\n\t"                                \
    );                                                                   \
    static inline void __attribute__((always_inline))                    \
    _irq_install_##nr(void) {                                            \
        set_intr_gate_raw(0x20 + (nr), 0,                                \
                          (void *)(uintptr_t)IRQ##nr##_interrupt);       \
    }

Build_IRQ(0);  Build_IRQ(1);  Build_IRQ(2);  Build_IRQ(3);
Build_IRQ(4);  Build_IRQ(5);  Build_IRQ(6);  Build_IRQ(7);
Build_IRQ(8);  Build_IRQ(9);  Build_IRQ(10); Build_IRQ(11);
Build_IRQ(12); Build_IRQ(13); Build_IRQ(14); Build_IRQ(15);

#undef Build_IRQ

void arch_install_intr_gate(uint8_t vector, void *stub, uint8_t ist) {
    set_intr_gate_raw(vector, ist, stub);
}

void arch_irq_install(void) {
    _irq_install_0();  _irq_install_1();  _irq_install_2();  _irq_install_3();
    _irq_install_4();  _irq_install_5();  _irq_install_6();  _irq_install_7();
    _irq_install_8();  _irq_install_9();  _irq_install_10(); _irq_install_11();
    _irq_install_12(); _irq_install_13(); _irq_install_14(); _irq_install_15();
}
```

**In `kernel/intr/irq.c`:**
1. Remove Build_IRQ macro + all 16 expansions + `irq_install()` function body
2. Remove `#include <kernel/arch/x86_64/gate.h>` and `#include <kernel/arch/x86_64/linkage.h>`
3. Add `#include <kernel/arch/irq.h>`
4. Keep `register_irq()` — it's architecture-neutral

- [ ] **Step 3: Migrate `kernel/arch/x86_64/trap.c`** (architecture file — minimal changes)

Replace bare `KERNEL_CS`/`USER_CS` with `ARCH_KERNEL_CS`/`ARCH_USER_CS` (from `arch/segment.h` legacy aliases).
Keep bare `movq %%cr2, %0` — this is an arch file, not generic code.
`user_va_to_phys()` stays in this file — x86-specific, per spec Section 5.

Add: `#include <kernel/arch/segment.h>`

- [ ] **Step 4: Verify compile + `__asm__` check**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -ci 'warning'
# Expected: 0

# Check: zero bare __asm__ in generic .c files
# Exceptions per spec Section 5: arch/x86_64/, pic/, sched/smp.c, sched/task.c
grep -r '__asm__' kernel/ --include='*.c' \
    | grep -v 'arch/x86_64/' \
    | grep -v 'pic/' \
    | grep -v 'sched/smp.c' \
    | grep -v 'sched/task.c' \
    | grep -v '.d:'
# Expected: empty (no bare asm in generic code)
```

- [ ] **Step 5: Commit**

```bash
cd /home/aagu/OS01
git add kernel/intr/dispatch.c kernel/intr/irq.c \
        kernel/arch/x86_64/irq.c kernel/arch/x86_64/trap.c
git commit -m "refactor(arch): migrate group-D — intr/ dispatch + trap.c

- dispatch.c: gate.h → arch/irq.h
- intr/irq.c: move Build_IRQ + irq_install() → arch/x86_64/irq.c;
  keep register_irq() (arch-neutral)
- trap.c: KERNEL_CS/USER_CS → ARCH_KERNEL_CS/ARCH_USER_CS;
  bare cr2 kept (arch file)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: Final verification

**Files:** None (verification only)

- [ ] **Step 1: Full clean build with 0 warnings**

```bash
cd /home/aagu/OS01
make clean
make 2>&1 | tee /tmp/os01_build.log
grep -ci 'warning' /tmp/os01_build.log
# Expected: 0
```

- [ ] **Step 2: Boot test**

```bash
cd /home/aagu/OS01
timeout 10 make run 2>&1 | tee /tmp/os01_boot.log
grep -c 'Hello, World' /tmp/os01_boot.log
# Expected: 1 (boot message)
```

- [ ] **Step 3: Syscall regression test**

```bash
cd /home/aagu/OS01
make test-syscall 2>&1 | tee /tmp/os01_systest.log
tail -5 /tmp/os01_systest.log
# Expected: "70/70 PASS"
```

- [ ] **Step 4: SMP + debug channels stress test**

```bash
cd /home/aagu/OS01
timeout 10 make DEBUG_CHANNELS=sched,irq,mm run 2>&1 | tee /tmp/os01_smp.log
grep -c 'SMP: AP.*online' /tmp/os01_smp.log
# Expected: at least 1 (AP booted)
grep -ci 'stack smashing' /tmp/os01_smp.log
# Expected: 0
```

- [ ] **Step 5: Zero bare asm in generic code**

```bash
cd /home/aagu/OS01
# Verify zero bare __asm__ in generic .c files.
# Exceptions (spec Section 5 "不抽象的内容"):
#   - arch/x86_64/  — architecture files
#   - pic/          — pure x86 8259A driver
#   - sched/smp.c   — lgdt/lidt AP GDT/IDT reload (x86-specific protocol)
#   - sched/task.c  — switch_to/__switch_to, get_current_task() RSP mask
grep -r '__asm__' kernel/ --include='*.c' \
    | grep -v 'arch/x86_64/' \
    | grep -v 'pic/' \
    | grep -v 'sched/smp.c' \
    | grep -v 'sched/task.c' \
    | grep -v '.d:'
# Expected: empty (no bare asm in generic code)
```

- [ ] **Step 5: Known v1 gap — tty/tty.c still includes `<kernel/arch/x86_64/trap.h>`**

`kernel/tty/tty.c` uses `do_signal_delivery()` and `signal_pending_fatal()` whose declarations live in `arch/x86_64/trap.h`. These functions are part of the x86 syscall/signal dispatch path (`do_system_call` → `ret_from_intr` → signal delivery). No attempt is made to abstract them in v1 — they are inherently architecture-specific. This is documented, not a bug. A future aarch64 port will need its own signal delivery path with a different ABI (different `pt_regs_t` layout, different syscall convention).

```bash
grep -n 'arch/x86_64/trap.h' kernel/tty/tty.c
# Expected: 1 match — documented v1 gap
```

- [ ] **Step 6: Commit verification results**

```bash
cd /home/aagu/OS01
git add -A
git diff --cached --stat
git commit -m "verify(arch): multi-arch abstraction — all tests pass

Clean build: 0 warnings
Boot test: reaches shell
Syscall test: 70/70 PASS
SMP test: APs boot, no stack smashing
Generic asm check: 0 bare __asm__ outside arch/x86_64/

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Summary

| Task | What | Files | Length |
|------|------|-------|--------|
| 1 | Create batch-1 arch headers | 6 new | ~15 min |
| 2 | Create batch-2 arch headers | 5 new | ~15 min |
| 3 | Update x86_64 headers + irq.c (minimal) | 3 modify + 1 new | ~15 min |
| 4 | Update kernel headers | 7 modify | ~20 min |
| 5A | Migrate Group A (io + cpu, no irq) | 7 modify | ~20 min |
| 5B | Migrate Group B (irq + io) | 4 modify | ~15 min |
| 5C | Migrate Group C (mmu + irq) | 6 modify | ~20 min |
| 5D | Migrate Group D (intr + trap) | 4 modify | ~20 min |
| 6 | Final verification | 0 modify | ~15 min |
| **Total** | | **~36 files** | **~2.5 hours** |
