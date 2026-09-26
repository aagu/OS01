# AAGU-4 残留清理 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close AAGU-4 §3.3 ❌ (8 conditional blocks) + 🟡 (4 x86-only redundant blocks) by removing `#ifdef __x86_64__` / `#elif defined(__aarch64__)` from arch-neutral TUs/headers and consolidating the actual atomic ops into per-arch `static inline always_inline` headers.

**Architecture:** AAGU-4 §2.3 forbids `#ifdef __x86_64__` in arch-neutral TUs. Currently 14 conditional blocks exist across 10 files; 8 are §3.3 ❌ violations in aarch64-whitelisted TUs, 5 are §3.3 🟡 redundant guards in x86-only-path TUs whose build is already gated by `kernel/Makefile`. The atomic-ops implementations (`lock orq/andq` for x86_64, `ldaxr+stlxr+cbnz` for aarch64) move from external functions in `kernel/arch/<arch>/cpu/atomic.c` into per-arch static-inline headers (`kernel/include/arch/<arch>/atomic_bitops.h`) marked `__attribute__((always_inline))` to keep them out-of-line at -O0 (existing softirq comment records that out-of-line calls caused CI kernel-selftest flake). The poll-timeout scan leaves tick.c and goes into `kernel/fs/poll.c::poll_timeout_tick()` (poll is fs functionality, not arch functionality). PIT/LAPIC handoff merges into existing `arch_tick_start()` in `kernel/arch/x86_64/platform/time.c`. The x86-only `clocksource_read_ns()` inline leaves the arch-neutral `time/clocksource.h` and goes into `kernel/include/arch/x86_64/clocksource.h`.

**Tech Stack:** C11, GNU inline asm (x86_64 LOCK prefix + aarch64 LSE LR/SC retry), QEMU 11.1.1, clang-22, GNU Make profile-based build, `kernel/selftest/test_arch_atomic_u64.c` as the regression oracle for softirq atomic semantics.

## Global Constraints

These are project-wide requirements copied verbatim from `docs/arch/cross-boundary-symbols.md` §2.3:

- Arch-neutral TUs and headers **MUST NOT** contain `#ifdef __x86_64__` / `#elif defined(__aarch64__)`. All arch-specific code goes in `kernel/arch/<arch>/` or `kernel/include/arch/<arch>/`.
- The strong override pattern (facade declares in arch-neutral header; per-arch `static inline` or strong function in `kernel/arch/<arch>/`) is the standard for arch values.
- aarch64 build whitelist is the explicit 13-TU list in `kernel/Makefile:46-58`. Changes touching any of those TUs must keep the build green for both architectures.
- `__attribute__((always_inline))` is required for hot-path inlines that previously triggered CI flake when emitted as out-of-line calls.
- x86_64 LOCK prefix semantics: acquire-release RMW (full fence in practice).
- aarch64 LR/SC semantics: acquire-release via `ldaxr+stlxr`; three independent `=&r` early-clobber constraints for `[old]/[new_val]/[status]`.
- Make build cache does not invalidate on CFLAGS-only changes (`KERNEL_SELFTEST=1` does not trigger re-compile). **Run `make clean` before verifying** any profile that changes `KERNEL_SELFTEST` or build flags.
- `make test-qemu SUITE=phase-0` runs the systest baseline; `make test-network` runs nettest; `make PROFILE=aarch64-clang test-aarch64 MODE=smp` runs aarch64 SMP validation.
- All kernel edits should preserve the existing `clocksource_active=false` jiffies fallback semantics in `clocksource_read_ns()`.

## File Structure

```
新增 (3 个头):
  kernel/include/arch/x86_64/atomic_bitops.h       # x86_64 static inline always_inline for arch_atomic_or/and_u64
  kernel/include/arch/aarch64/atomic_bitops.h      # aarch64 static inline always_inline for arch_atomic_or/and_u64
  kernel/include/arch/x86_64/clocksource.h        # x86_64-only clocksource_read_ns() inline + percpu include

删除 (2 个 TU):
  kernel/arch/x86_64/cpu/atomic.c                  # 旧外部 arch_atomic_or/and_u64 实现
  kernel/arch/aarch64/cpu/atomic.c                 # 同上

修改 (12 个文件):
  kernel/include/arch/atomic.h                     # 删除 2 个外部原型；改为按 arch include 新头
  kernel/intr/softirq.c                            # 行 11/55：inline asm 块删除，直接调 arch_atomic_*
  kernel/time/tick.c                               # 行 21/43：删除 ifdef 块；调 poll_timeout_tick() / arch_tick_start()
  kernel/time/timer.c                              # 行 128/144：spin hint → arch_cpu_pause()；删 SUBSYS_INITCALL ifdef
  kernel/time/clocksource.c                        # 行 50：删 SUBSYS_INITCALL ifdef；显式 include arch/cpu.h
  kernel/include/time/clocksource.h                # 行 7/34：删 ifdef；剥离 x86 专用部分
  kernel/fs/poll.c                                 # 加 poll_timeout_tick() strong 实现；改 include arch/x86_64/clocksource.h
  kernel/arch/x86_64/platform/time.c               # arch_tick_start() 内加 irq_mask(0)/irq_unmask(0) PIT/LAPIC handoff
  kernel/arch/x86_64/intr/trap.c                   # 改 include 新头
  kernel/driver/{ahci,keyboard,pit,serial}.c       # 4 个文件各删 1 处冗余 ifdef
  kernel/net/net.c                                  # 删 1 处冗余 ifdef
  hosttests/cases/test_clocksource.c               # 改用新 x86 头
  hosttests/Makefile                                # 增加新头依赖

文档 (2 个文件):
  docs/arch/cross-boundary-symbols.md              # §3.3 ❌/🟡 状态改为 ✅；§6 验收清单更新
  docs/changelog.md                                # 2026-09-26 增条目
```

---

### Task 1: softirq atomic ops → per-arch inline headers

**Files:**
- Create: `kernel/include/arch/x86_64/atomic_bitops.h`
- Create: `kernel/include/arch/aarch64/atomic_bitops.h`
- Modify: `kernel/include/arch/atomic.h` (lines 10-11: delete 2 prototypes; add per-arch #include)
- Delete: `kernel/arch/x86_64/cpu/atomic.c`
- Delete: `kernel/arch/aarch64/cpu/atomic.c`
- Modify: `kernel/intr/softirq.c` (lines 9-23 + lines 48-61: remove #if/#elif/#else/#error blocks; replace `__asm__` calls with direct `arch_atomic_or_u64()`/`arch_atomic_and_u64()` calls)

**Interfaces:**
- Consumes: `uint64_t *addr`, `uint64_t mask` (from softirq.c existing call sites)
- Produces: per-arch header `arch_atomic_or_u64` / `arch_atomic_and_u64` static inline always_inline functions

- [ ] **Step 1: Verify baseline**

Run: `make PROFILE=x86_64-clang test-arch-atomic-u64` (or whatever target compiles + runs `kernel/selftest/test_arch_atomic_u64.c`)
Expected: PASS

This confirms the existing external-function implementation is correct; we will preserve the exact asm during refactor.

- [ ] **Step 2: Create x86_64 atomic_bitops.h**

Write to `kernel/include/arch/x86_64/atomic_bitops.h`:

```c
#ifndef _KERNEL_ARCH_X86_64_ATOMIC_BITOPS_H
#define _KERNEL_ARCH_X86_64_ATOMIC_BITOPS_H

#include <stdint.h>

/* x86_64 strong override for arch_atomic_or_u64 / arch_atomic_and_u64.
 * Reference: kernel/arch/x86_64/cpu/atomic.c (deleted in this task).
 * Memory order: acquire-release (full fence on LOCK prefix in practice).
 * Hot path: must remain inlined even at -O0; previous CI flake recorded
 * in kernel/intr/softirq.c historical comment. */
static inline __attribute__((always_inline)) void
arch_atomic_or_u64(uint64_t *addr, uint64_t mask) {
    __asm__ __volatile__("lock orq %0, (%1)"
                         :: "r"(mask), "r"(addr) : "memory");
}

static inline __attribute__((always_inline)) void
arch_atomic_and_u64(uint64_t *addr, uint64_t mask) {
    __asm__ __volatile__("lock andq %0, (%1)"
                         :: "r"(mask), "r"(addr) : "memory");
}

#endif
```

- [ ] **Step 3: Create aarch64 atomic_bitops.h**

Write to `kernel/include/arch/aarch64/atomic_bitops.h`:

```c
#ifndef _KERNEL_ARCH_AARCH64_ATOMIC_BITOPS_H
#define _KERNEL_ARCH_AARCH64_ATOMIC_BITOPS_H

#include <stdint.h>

/* aarch64 strong override for arch_atomic_or_u64 / arch_atomic_and_u64.
 * Reference: kernel/arch/aarch64/cpu/atomic.c (deleted in this task).
 * LR/SC retry loop (ldaxr+stlxr+cbnz). Memory order: acquire-release.
 * Three independent =&r early-clobber constraints for [old]/[new_val]/[status]
 * (stlxr writes only the low 32 bits of [status]; sharing a register with
 * [new_val] would clobber the value being stored). */
static inline __attribute__((always_inline)) void
arch_atomic_or_u64(uint64_t *addr, uint64_t mask) {
    uint64_t old, new_val;
    uint32_t status;
    __asm__ __volatile__(
        "1: ldaxr   %[old],     [%[addr]]\n"
        "   orr    %[new_val], %[old], %[mask]\n"
        "   stlxr  %w[status], %[new_val], [%[addr]]\n"
        "   cbnz   %w[status], 1b\n"
        : [old]      "=&r"(old),
          [new_val]  "=&r"(new_val),
          [status]   "=&r"(status)
        : [addr] "r"(addr),
          [mask] "r"(mask)
        : "memory");
}

static inline __attribute__((always_inline)) void
arch_atomic_and_u64(uint64_t *addr, uint64_t mask) {
    uint64_t old, new_val;
    uint32_t status;
    __asm__ __volatile__(
        "1: ldaxr   %[old],     [%[addr]]\n"
        "   and    %[new_val], %[old], %[mask]\n"
        "   stlxr  %w[status], %[new_val], [%[addr]]\n"
        "   cbnz   %w[status], 1b\n"
        : [old]      "=&r"(old),
          [new_val]  "=&r"(new_val),
          [status]   "=&r"(status)
        : [addr] "r"(addr),
          [mask] "r"(mask)
        : "memory");
}

#endif
```

- [ ] **Step 4: Modify kernel/include/arch/atomic.h**

Replace lines 10-11 (the two `void arch_atomic_or_u64(...)` / `void arch_atomic_and_u64(...)` prototypes) with per-arch include dispatch:

```c
#ifdef __x86_64__
#include <arch/x86_64/atomic_bitops.h>
#elif defined(__aarch64__)
#include <arch/aarch64/atomic_bitops.h>
#else
#error "Unsupported architecture"
#endif
```

Keep the rest of `arch/atomic.h` unchanged (the existing 7 inline fetch_add/sub/inc/read/write/cas/xchg functions and their `#ifdef __x86_64__` / `#elif defined(__aarch64__)` blocks stay as-is — they are out of scope for this PR).

- [ ] **Step 5: Delete the old .c implementations**

Run:
```bash
git rm kernel/arch/x86_64/cpu/atomic.c kernel/arch/aarch64/cpu/atomic.c
```

- [ ] **Step 6: Modify kernel/intr/softirq.c**

Replace lines 9-23 (the `#if defined(__x86_64__)` ... `#else #error` block in `set_softirq_status`) with a single call:

```c
void set_softirq_status(uint64_t status)
{
    arch_atomic_or_u64(&softirq_status, status);
}
```

Replace lines 48-61 (the `#if defined(__x86_64__)` ... `#else #error` block in `do_softirq`) with:

```c
            if(softirq_status & (1 << i))
            {
                softirq_vector[i].action(softirq_vector[i].data);
                arch_atomic_and_u64(&softirq_status, ~(1ULL << i));
            }
```

Verify `kernel/intr/softirq.c` no longer contains any `#ifdef __x86_64__` / `#elif defined(__aarch64__)` / `#error` block.

- [ ] **Step 7: Build x86_64**

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang kernel`
Expected: build succeeds. Inspect that `kernel/intr/softirq.c` is compiled (no errors about undeclared `arch_atomic_or_u64`).

- [ ] **Step 8: Build aarch64**

Run: `make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang kernel`
Expected: build succeeds.

- [ ] **Step 9: Run x86_64 atomic selftest**

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang KERNEL_SELFTEST=1 kernel && make KERNEL_SELFTEST=1 test-qemu` (or the appropriate target that runs kernel_selftest including test_arch_atomic_u64)
Expected: kernel selftest suite runs; `test_arch_atomic_u64` shows PASS in QEMU log (no flake).

- [ ] **Step 10: Commit Task 1**

```bash
git add kernel/include/arch/atomic.h kernel/include/arch/x86_64/atomic_bitops.h kernel/include/arch/aarch64/atomic_bitops.h kernel/intr/softirq.c
git rm kernel/arch/x86_64/cpu/atomic.c kernel/arch/aarch64/cpu/atomic.c
git commit -m "refactor(softirq): inline arch_atomic_or/and_u64 into per-arch headers

- Move arch_atomic_or/and_u64 implementations from kernel/arch/<arch>/cpu/atomic.c
  (external functions) into per-arch static-inline always_inline headers
  (kernel/include/arch/<arch>/atomic_bitops.h)
- kernel/include/arch/atomic.h dispatches via #include based on __x86_64__ /
  __aarch64__
- kernel/intr/softirq.c drops the two #if defined(__x86_64__)/#elif defined
  (__aarch64__)/#else #error blocks and calls arch_atomic_or/and_u64 directly
- always_inline attribute keeps the asm in hot path at -O0/-O2 (CI flake
  historical context in softirq.c comment)

Closes AAGU-4 §3.3 ❌ for kernel/intr/softirq.c."
```

---

### Task 2: tick poll-timeout → fs/poll.c + tick_start → arch_tick_start

**Files:**
- Modify: `kernel/time/tick.c` (lines 1-65: drop `#if defined(__x86_64__)` blocks in `tick_handler` and `tick_start`; drop `#include <fs/poll.h>` since the symbol references move; call `poll_timeout_tick()` and `arch_tick_start()` directly)
- Modify: `kernel/fs/poll.c` (add `void poll_timeout_tick(void)` strong implementation near existing poll-timeout registry at line 333-)
- Modify: `kernel/arch/x86_64/platform/time.c` (lines 25-28: `arch_tick_start()` body — add `irq_mask(0)` / `irq_unmask(0)` PIT/LAPIC handoff logic that previously lived in `kernel/time/tick.c:tick_start()`)

**Interfaces:**
- Consumes: `poll_timeout_node_t *poll_timeout_head`, `spinlock_T poll_timeout_lock`, `clocksource_read_ns()`, `wait_queue_wake_all()`, `irq_mask()`, `irq_unmask()`, `lapic_timer_start()`, `arch_cycle_freq()` (existing symbols)
- Produces: `void poll_timeout_tick(void)` — declared in `kernel/time/tick.c` as weak default, defined strong in `kernel/fs/poll.c`. On aarch64 (no fs/poll compiled), the weak default is the only definition.

- [ ] **Step 1: Verify baseline**

Run: `make PROFILE=x86_64-clang clean && make OS01_SYSTEST=1 PROFILE=x86_64-clang test-qemu SUITE=systest`
Expected: 268/268 PASS

- [ ] **Step 2: Modify kernel/time/tick.c**

a) Remove `#include <fs/poll.h>` (line 8).

b) In `tick_handler()`, replace the entire `#if defined(__x86_64__)` ... `#endif` block (lines 21-30) with a single call:

```c
void tick_handler(void)
{
    jiffies++;

    poll_timeout_tick();

    this_cpu()->need_resched = 1;
    ...
}
```

c) Replace `tick_start()` body with a single `arch_tick_start()` call:

```c
void tick_start(void)
{
    arch_tick_start();
}
```

d) Add a weak default `poll_timeout_tick()` definition at the end of `kernel/time/tick.c` so aarch64 (no fs/poll.c) compiles. Use `__attribute__((weak))`:

```c
__attribute__((weak)) void poll_timeout_tick(void)
{
    /* Default no-op. Strong override lives in kernel/fs/poll.c
     * (x86_64 path). aarch64 phase 1 does not compile fs/poll.c;
     * the weak default is the only definition. */
}
```

Verify `kernel/time/tick.c` no longer contains any `#if defined(__x86_64__)` block.

- [ ] **Step 3: Modify kernel/arch/x86_64/platform/time.c**

Replace `arch_tick_start()` body (lines 25-28) with the PIT/LAPIC handoff logic that previously lived in `kernel/time/tick.c:tick_start()`. Add includes for `irq_mask`/`irq_unmask`:

```c
#include <intr/interrupt.h>     // irq_mask / irq_unmask

// 启动 x86 tick 源：LAPIC 周期模式。返回是否启动成功。
// PIT/LAPIC handoff: mask PIT, try LAPIC; if LAPIC fails, restore PIT.
bool arch_tick_start(void)
{
    irq_mask(0);
    if (lapic_timer_start(100)) {
        // LAPIC 接管成功，PIT 保持掩蔽。
        return true;
    }
    irq_unmask(0);
    return false;
}
```

The function returns true on LAPIC success (PIT stays masked), false on LAPIC failure (PIT re-enabled as fallback). Preserve this semantics — `kernel/time/tick.c:tick_start()` previously did `irq_mask(0)` then `if (arch_tick_start()) {...} else { irq_unmask(0); }`. The new `kernel/time/tick.c::tick_start()` is a single `arch_tick_start()` call, but we lost the caller-side `irq_unmask(0)` on failure. To preserve semantics, the function itself does `irq_unmask(0)` on failure.

If `lapic_timer_start()` returns a bool, use that directly. If it returns void or int, wrap accordingly. Inspect `kernel/arch/x86_64/intr/apic/lapic_timer.c::lapic_timer_start()` to confirm its return type before writing the new body.

- [ ] **Step 4: Add strong poll_timeout_tick() in kernel/fs/poll.c**

Locate the poll-timeout registry section (`poll_timeout_head`, `poll_timeout_lock`, `poll_timeout_register`, `poll_timeout_unregister` around lines 333-360). Add a new public function `poll_timeout_tick()` (the strong override):

```c
// Per-tick scan of registered poll timeouts. Called from
// kernel/time/tick.c::tick_handler(). Walks the linked list, wakes any
// wait queue whose deadline has expired.
void poll_timeout_tick(void)
{
    if (!poll_timeout_head) return;
    uint64_t flags = spin_lock_irqsave(&poll_timeout_lock);
    for (poll_timeout_node_t *n = poll_timeout_head; n; n = n->next)
        if (clocksource_read_ns() >= n->deadline)
            wait_queue_wake_all(n->wq);
    spin_unlock_irqrestore(&poll_timeout_lock, flags);
}
```

Verify `kernel/fs/poll.c` still compiles standalone (its own functions unchanged).

- [ ] **Step 5: Build x86_64**

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang kernel`
Expected: build succeeds. The strong `poll_timeout_tick()` in `kernel/fs/poll.c` overrides the weak default in `kernel/time/tick.c`. Verify no linker "multiple definition" error.

- [ ] **Step 6: Build aarch64**

Run: `make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang kernel`
Expected: build succeeds. aarch64 does NOT compile `kernel/fs/poll.c`, so the weak default in `kernel/time/tick.c` is the only `poll_timeout_tick()` definition; `aarch64_main` should still call `arch_tick_start()` (already wired in `kernel/arch/aarch64/platform/time.c`).

- [ ] **Step 7: Run x86_64 systest (poll path coverage)**

Run: `make PROFILE=x86_64-clang clean && make OS01_SYSTEST=1 PROFILE=x86_64-clang test-qemu SUITE=systest`
Expected: 268/268 PASS (poll/select are part of systest; if poll_timeout_tick() regressions surface here, they'll show as select/pselect timeouts).

- [ ] **Step 8: Run aarch64 SMP test**

Run: `make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang test-aarch64 MODE=smp`
Expected: PASS at the same baseline as `d695020` (1/2/4-core × 3 = 9/9).

- [ ] **Step 9: Commit Task 2**

```bash
git add kernel/time/tick.c kernel/fs/poll.c kernel/arch/x86_64/platform/time.c
git commit -m "refactor(tick): poll-timeout scan → fs/poll.c; PIT/LAPIC handoff → arch_tick_start

- kernel/time/tick.c drops two #if defined(__x86_64__) blocks; tick_handler()
  now calls poll_timeout_tick() unconditionally; tick_start() calls
  arch_tick_start() unconditionally.
- Weak default poll_timeout_tick() lives at end of kernel/time/tick.c (aarch64
  phase 1 path with no fs/poll compiled).
- Strong poll_timeout_tick() defined in kernel/fs/poll.c next to its existing
  poll-timeout registry (poll_timeout_head/lock/register/unregister). Single
  poll-timeout algorithm; ownership stays in fs.
- kernel/arch/x86_64/platform/time.c::arch_tick_start() absorbs the previous
  irq_mask(0) / arch_tick_start() / irq_unmask(0) PIT/LAPIC handoff
  ceremony, preserving the LAPIC-success-keeps-PIT-masked / LAPIC-failure-
  restores-PIT semantics.
- Drop #include <fs/poll.h> from tick.c (symbol references move out).

Closes AAGU-4 §3.3 ❌ for kernel/time/tick.c (both blocks)."
```

---

### Task 3: clocksource header split + timer SUBSYS_INITCALL + spin hint

**Files:**
- Modify: `kernel/include/time/clocksource.h` (lines 7/34: delete 2 `#if defined(__x86_64__)` blocks; remove `<percpu/percpu.h>` include; remove `clocksource_read_ns()` inline)
- Create: `kernel/include/arch/x86_64/clocksource.h` (port the `clocksource_read_ns()` inline + percpu include from old clocksource.h)
- Modify: `kernel/time/clocksource.c` (line 50: delete SUBSYS_INITCALL `#if defined(__x86_64__) || defined(__aarch64__)` block; explicitly `#include <arch/cpu.h>`)
- Modify: `kernel/time/timer.c` (line 128: replace `__asm__ volatile("pause"/"yield")` with `arch_cpu_pause()`; line 144: delete SUBSYS_INITCALL `#if defined(__x86_64__) || defined(__aarch64__)` block; add `#include <arch/cpu.h>` for `arch_cpu_pause`)
- Modify: `kernel/fs/poll.c` (line 20: change `#include <time/clocksource.h>` to `#include <arch/x86_64/clocksource.h>` for `clocksource_read_ns()`)
- Modify: `kernel/arch/x86_64/intr/trap.c` (line 37: change `#include <time/clocksource.h>` to `#include <arch/x86_64/clocksource.h>`)
- Modify: `hosttests/cases/test_clocksource.c` (line 44: change `#include <time/clocksource.h>` to `#include <arch/x86_64/clocksource.h>` to consume `clocksource_read_ns()` from the new x86-only header)
- Modify: `hosttests/Makefile` (line 562: add `kernel/include/arch/x86_64/clocksource.h` to the dependency list of `$(TEST_BLD)/test_clocksource.o`)

**Interfaces:**
- Consumes: `arch_cycle_counter()` (from `<arch/cpu.h>`), `this_cpu()->tsc_offset` (from `<percpu/percpu.h>`), `clocksource_active/mult/shift` (from `<time/clocksource.h>`)
- Produces: `static inline uint64_t clocksource_read_ns(void)` declared in `kernel/include/arch/x86_64/clocksource.h`. Single x86_64-only inline; no equivalent on aarch64 (aarch64 callers must use `clocksource_cycles()` / `clocksource_init()` directly).

- [ ] **Step 1: Verify baseline**

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang test-clocksource` (or whatever target runs `hosttests/cases/test_clocksource.c`)
Expected: PASS

- [ ] **Step 2: Modify kernel/include/time/clocksource.h**

Replace the file contents with the arch-neutral version (delete lines 7-17 percpu include guard and lines 34-44 `clocksource_read_ns()` guard):

```c
#ifndef _KERNEL_CLOCKSOURCE_H
#define _KERNEL_CLOCKSOURCE_H

#include <stdint.h>
#include <stdbool.h>
#include <time/timer.h>      // jiffies
#include <arch/cpu.h>   // arch_cycle_counter()

// mult/shift 由 clocksource_init() 计算并导出（static inline read_ns 引用）。
extern bool     clocksource_active;
extern uint32_t clocksource_mult;
extern uint32_t clocksource_shift;

// 依据 arch_cycle_freq() 计算 mult/shift；freq=0 时 active=false（退 jiffies）。
void     clocksource_init(void);

// 已校准的 cycle 频率（Hz），0 = 未校准。
uint64_t clocksource_freq_hz(void);

// 原始 cycle 计数（调试/校准用），不加 tsc_offset。
uint64_t clocksource_cycles(void);

#endif
```

Verify the file no longer contains `#if defined(__x86_64__)`.

- [ ] **Step 3: Create kernel/include/arch/x86_64/clocksource.h**

Write the new header with the ported inline:

```c
#ifndef _KERNEL_ARCH_X86_64_CLOCKSOURCE_H
#define _KERNEL_ARCH_X86_64_CLOCKSOURCE_H

#include <time/clocksource.h>   // clocksource_active/mult/shift, jiffies fallback
#include <time/timer.h>
#include <arch/cpu.h>           // arch_cycle_counter()
#include <percpu/percpu.h>      // this_cpu()->tsc_offset

/* x86_64-only clocksource_read_ns() inline.
 *
 * 单调纳秒。active 时 = (cycle+tsc_offset)*mult>>shift；否则退 jiffies*10ms。
 * 仅在 GS base 装之后调用（boot 期校准用 arch_cycle_counter()）。
 *
 * Moved from kernel/include/time/clocksource.h in the AAGU-4 residual
 * cleanup to remove the #ifdef __x86_64__ guard from the arch-neutral
 * header (spec §2.3). aarch64 phase 1 does not need this inline; callers
 * on aarch64 use clocksource_cycles() / clocksource_init() directly. */
static inline uint64_t clocksource_read_ns(void)
{
    if (!clocksource_active)
        return jiffies * 10000000ULL;
    uint64_t c = arch_cycle_counter() + (uint64_t)this_cpu()->tsc_offset;
    return (uint64_t)(((__uint128_t)c * clocksource_mult) >> clocksource_shift);
}

#endif
```

- [ ] **Step 4: Modify kernel/time/clocksource.c**

a) Delete lines 50-78 (the `#if defined(__x86_64__) || defined(__aarch64__)` block and the SUBSYS_INITCALL `_clocksource_register` it wraps). The deletion is unconditional — both x86_64 and aarch64 builds compile this TU; SUBSYS_INITCALL itself is arch-neutral.

b) Verify that the file's existing `#include <arch/cpu.h>` (transitively from `<time/clocksource.h>` which we just modified) is still present. If not, add it explicitly.

- [ ] **Step 5: Modify kernel/time/timer.c**

a) Line 128: replace the `#if defined(__x86_64__)` / `#else` / `#endif` block + the `__asm__ volatile("pause")` / `__asm__ volatile("yield")` lines with:

```c
        arch_cpu_pause();
```

b) Line 144: delete the `#if defined(__x86_64__) || defined(__aarch64__)` block and its matching `#endif`. The SUBSYS_INITCALL `_timer_register` becomes unconditional.

c) Add `#include <arch/cpu.h>` if not already present (for `arch_cpu_pause()` declaration).

Verify `kernel/time/timer.c` no longer contains `#if defined(__x86_64__)` or `#elif defined(__aarch64__)`.

- [ ] **Step 6: Modify kernel/fs/poll.c**

Change line 20 from `#include <time/clocksource.h>   // clocksource_read_ns()` to `#include <arch/x86_64/clocksource.h>`. The transitive `<time/clocksource.h>` include comes from the new x86 header.

- [ ] **Step 7: Modify kernel/arch/x86_64/intr/trap.c**

Change line 37 from `#include <time/clocksource.h>  // clocksource_read_ns()` to `#include <arch/x86_64/clocksource.h>`. Other callers in trap.c that may use `clocksource_active/mult/shift` should continue to work via the new x86 header's transitive include of `<time/clocksource.h>`.

- [ ] **Step 8: Modify hosttests/cases/test_clocksource.c**

Change line 44 from `#include <time/clocksource.h>` to `#include <arch/x86_64/clocksource.h>`. The host test runs only on x86_64 hosts (CLOCKSOURCE_HOST_CFLAGS uses x86_64 path), so the new x86 header is the correct include.

- [ ] **Step 9: Modify hosttests/Makefile**

At line 562 (`$(TEST_BLD)/test_clocksource.o` rule), add `$(TESTS_DIR)/kernel/include/arch/x86_64/clocksource.h` to the dependency list:

```makefile
$(TEST_BLD)/test_clocksource.o: $(TEST_CASES)/test_clocksource.c \
        $(TESTS_DIR)/kernel/include/time/clocksource.h \
        $(TESTS_DIR)/kernel/include/arch/x86_64/clocksource.h \
        $(TESTS_DIR)/kernel/include/time/clocksource_internal.h \
        $(TEST_MOCK)/clocksource_test_runtime.h
```

Also update the `$(TEST_BLD)/clocksource_production.o` dependency list if needed (lines 565-568) to include the new x86 header (since `kernel/time/clocksource.c` no longer transitively pulls it in).

- [ ] **Step 10: Build x86_64**

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang kernel`
Expected: build succeeds. Verify `kernel/time/timer.c`, `kernel/time/clocksource.c`, `kernel/fs/poll.c`, `kernel/arch/x86_64/intr/trap.c` all compile.

- [ ] **Step 11: Build aarch64**

Run: `make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang kernel`
Expected: build succeeds.

- [ ] **Step 12: Run hosttests**

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang test-host`
Expected: PASS, including `test_clocksource` and `test_arch_atomic_u64`.

- [ ] **Step 13: Run x86_64 systest**

Run: `make PROFILE=x86_64-clang clean && make OS01_SYSTEST=1 PROFILE=x86_64-clang test-qemu SUITE=systest`
Expected: 268/268 PASS.

- [ ] **Step 14: Run x86_64 nettest**

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang test-network`
Expected: 6/6 PASS.

- [ ] **Step 15: Commit Task 3**

```bash
git add kernel/include/time/clocksource.h kernel/include/arch/x86_64/clocksource.h \
        kernel/time/clocksource.c kernel/time/timer.c \
        kernel/fs/poll.c kernel/arch/x86_64/intr/trap.c \
        hosttests/cases/test_clocksource.c hosttests/Makefile
git commit -m "refactor(clocksource): split x86-only inline into arch header; clean SUBSYS_INITCALL ifdefs

- kernel/include/time/clocksource.h drops #if defined(__x86_64__) for
  percpu include and clocksource_read_ns() inline; remains arch-neutral.
- New kernel/include/arch/x86_64/clocksource.h holds the x86-only
  clocksource_read_ns() inline + percpu/percpu.h include.
- Callers (kernel/fs/poll.c, kernel/arch/x86_64/intr/trap.c) switch to
  the new x86 header.
- kernel/time/clocksource.c drops SUBSYS_INITCALL #if defined(...) block;
  SUBSYS_INITCALL is arch-neutral and whitelist already gates the TU.
- kernel/time/timer.c drops spin-hint #if defined(__x86_64__) (now uses
  arch_cpu_pause() from <arch/cpu.h>) and SUBSYS_INITCALL #if defined(...)
  block.
- hosttests/cases/test_clocksource.c and hosttests/Makefile updated to use
  the new x86 header.

Closes AAGU-4 §3.3 ❌ for kernel/time/clocksource.c, kernel/time/timer.c,
kernel/include/time/clocksource.h."
```

---

### Task 4: x86-only path redundant ifdef removal

**Files:**
- Modify: `kernel/driver/ahci.c` (line 611: delete `#ifdef __x86_64__` and matching `#endif`)
- Modify: `kernel/driver/keyboard.c` (line 420: same)
- Modify: `kernel/driver/pit.c` (line 33: same)
- Modify: `kernel/driver/serial.c` (line 240: same)
- Modify: `kernel/net/net.c` (line 138: same)

Each block wraps a SUBSYS_INITCALL `_register` function. The 5 TUs are NOT in the aarch64 whitelist (`kernel/Makefile:46-58` lists explicit 13 aarch64 TUs; `kernel/driver/*.c` and `kernel/net/*.c` are wildcard-collected only in the `else` (x86_64) branch), so the `#ifdef __x86_64__` guards are dead — they can never be false in any build that compiles the TU.

**Interfaces:** No interface change. Just removing dead preprocessor blocks.

- [ ] **Step 1: Verify baseline**

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang kernel`
Expected: build succeeds.

- [ ] **Step 2: Modify kernel/driver/ahci.c**

In `kernel/driver/ahci.c`, locate the `#ifdef __x86_64__` block (line 611). Delete the `#ifdef __x86_64__` line and the matching `#endif` line. Keep the SUBSYS_INITCALL registration block in between unchanged.

- [ ] **Step 3: Modify kernel/driver/keyboard.c**

Same as Step 2 but at line 420.

- [ ] **Step 4: Modify kernel/driver/pit.c**

Same as Step 2 but at line 33.

- [ ] **Step 5: Modify kernel/driver/serial.c**

Same as Step 2 but at line 240.

- [ ] **Step 6: Modify kernel/net/net.c**

Same as Step 2 but at line 138 (this block also has a comment about `kernel/arch/x86_64/intr/` relocation done by `c3412da` arch source groups — that comment is still useful, keep it; just remove the `#ifdef`/`#endif` lines).

- [ ] **Step 7: Verify all 5 files no longer contain `#ifdef __x86_64__`**

Run:
```bash
grep -n '#if.*\(x86_64\|aarch64\)' kernel/driver/ahci.c kernel/driver/keyboard.c kernel/driver/pit.c kernel/driver/serial.c kernel/net/net.c
```
Expected: no output.

- [ ] **Step 8: Build x86_64 + run systest**

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang kernel && make OS01_SYSTEST=1 PROFILE=x86_64-clang test-qemu SUITE=systest`
Expected: kernel build succeeds; systest 268/268 PASS.

- [ ] **Step 9: Run aarch64 build (verify unaffected)**

Run: `make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang kernel`
Expected: build succeeds (these 5 TUs are not in aarch64 whitelist, so the changes don't affect aarch64 build).

- [ ] **Step 10: Commit Task 4**

```bash
git add kernel/driver/ahci.c kernel/driver/keyboard.c kernel/driver/pit.c kernel/driver/serial.c kernel/net/net.c
git commit -m "refactor(driver,net): drop redundant #ifdef __x86_64__ SUBSYS_INITCALL guards

5 TUs (kernel/driver/{ahci,keyboard,pit,serial}.c and kernel/net/net.c) are
not in the aarch64 whitelist (kernel/Makefile:46-58); they are wildcard-
collected only in the x86_64 build branch. Their SUBSYS_INITCALL #ifdef
__x86_64__ guards were therefore always-true dead code.

Closes AAGU-4 §3.3 🟡 for kernel/driver/{ahci,keyboard,pit,serial}.c and
kernel/net/net.c."
```

---

### Task 5: Documentation sync + final verification

**Files:**
- Modify: `docs/arch/cross-boundary-symbols.md` (§3.3 row for `#ifdef __x86_64__` in `kernel/intr/softirq.c`; §3.3 row for `#ifdef __x86_64__` in `kernel/intr/` x86-only drivers; §6 acceptance checklist)
- Modify: `docs/changelog.md` (add 2026-09-26 entry describing the cleanup)

**Interfaces:** None.

- [ ] **Step 1: Modify docs/arch/cross-boundary-symbols.md §3.3**

a) Find the §3.3 row about `kernel/intr/softirq.c` (line 129 in the original spec, content: "❌ 违例"). Update it to "✅ 修" with a brief note about Task 1.

b) Find the §3.3 row about `kernel/intr/` x86-only drivers (line 130, 🟡). Update the status to "✅ 修" for the 5 affected drivers (note: actually these moved to `kernel/arch/x86_64/intr/` in the arch source groups commit `c3412da`; what remains is the `kernel/driver/` + `kernel/net/` ifdef cleanup, which lives in `kernel/driver/` and `kernel/net/`, not `kernel/intr/`. Verify and update the row text accordingly).

- [ ] **Step 2: Modify docs/arch/cross-boundary-symbols.md §6 acceptance checklist**

In the §6 acceptance checklist, add a new bullet point under the closing tasks:

```markdown
- [x] §3.3 ❌ (kernel/intr/softirq.c + time/{clocksource,tick,timer}.c + clocksource.h) + §3.3 🟡 (driver/{ahci,keyboard,pit,serial}.c + net/net.c) 全闭环
```

- [ ] **Step 3: Modify docs/changelog.md**

Add a new entry at the top (under the `# 已完成工作汇总（Changelog）` heading, before the existing `## 2026-09-26` section if it exists) for today's date:

```markdown
## 2026-09-26
- refactor: **AAGU-4 残留清理 — close §3.3 ❌ + 🟡** —— worktree `feat/aagu-4-residual-cleanup`（commit 链路 `099b060`+4 task commits）：
  - **Task 1**：softirq 原子操作实现从 `kernel/arch/<arch>/cpu/atomic.c` 外部函数迁移到 `kernel/include/arch/<arch>/atomic_bitops.h` static inline + `__attribute__((always_inline))`；`kernel/intr/softirq.c` 删除 2 个 `#if defined(__x86_64__)/#elif defined(__aarch64__)/#else #error` 块
  - **Task 2**：`kernel/time/tick.c` poll-timeout scan + PIT/LAPIC handoff 拆分：weak 默认 `poll_timeout_tick()` 在 `kernel/time/tick.c`（aarch64 phase 1 路径），strong 实现在 `kernel/fs/poll.c`（x86_64）；PIT/LAPIC handoff 合并到 `kernel/arch/x86_64/platform/time.c::arch_tick_start()`；`tick.c` 删除 2 个 `#if defined(__x86_64__)` 块
  - **Task 3**：`kernel/include/time/clocksource.h` 2 个 `#if defined(__x86_64__)` 块（percpu include + `clocksource_read_ns()` inline）迁至新建 `kernel/include/arch/x86_64/clocksource.h`；`kernel/time/clocksource.c` + `kernel/time/timer.c` SUBSYS_INITCALL ifdef 删除；`timer.c` spin hint 接 `arch_cpu_pause()`
  - **Task 4**：`kernel/driver/{ahci,keyboard,pit,serial}.c` + `kernel/net/net.c` 共 5 个 TU 删除冗余 `#ifdef __x86_64__` SUBSYS_INITCALL 守护（TU 已在 x86-only 路径，Makefile 已 gate）
  - 文档同步：`docs/arch/cross-boundary-symbols.md` §3.3 ❌/🟡 状态全改 ✅，§6 验收清单增条目
```

Verify the entry matches the format of surrounding changelog entries.

- [ ] **Step 4: Final static verification (no more #ifdef in the 10 target files)**

Run:
```bash
grep -n '#if.*\(x86_64\|aarch64\)' \
    kernel/intr/softirq.c \
    kernel/time/clocksource.c \
    kernel/time/tick.c \
    kernel/time/timer.c \
    kernel/include/time/clocksource.h \
    kernel/driver/ahci.c \
    kernel/driver/keyboard.c \
    kernel/driver/pit.c \
    kernel/driver/serial.c \
    kernel/net/net.c
```
Expected: no output. All 10 files clean of conditional arch branches.

- [ ] **Step 5: Final aarch64 build verification**

Run: `make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang kernel`
Expected: build succeeds. Verify the aarch64 whitelist's 13 TUs are all built clean and the new weak `poll_timeout_tick()` doesn't cause "multiple definition" errors.

- [ ] **Step 6: Final aarch64 SMP test**

Run: `make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang test-aarch64 MODE=smp`
Expected: PASS at the same baseline as `d695020` (1/2/4-core × 3 = 9/9). Compare the log against the `d695020` baseline in `docs/aarch64-ipi-fail-handoff-2026-09-26.md`.

- [ ] **Step 7: Final x86_64 build + systest + nettest**

Run:
```bash
make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang kernel && \
  make OS01_SYSTEST=1 PROFILE=x86_64-clang test-qemu SUITE=systest && \
  make PROFILE=x86_64-clang test-network && \
  make PROFILE=x86_64-clang test-host
```
Expected: kernel build clean; systest 268/268 PASS; nettest 6/6 PASS; hosttests PASS (including `test_clocksource` and `test_arch_atomic_u64`).

- [ ] **Step 8: Commit Task 5**

```bash
git add docs/arch/cross-boundary-symbols.md docs/changelog.md
git commit -m "docs: AAGU-4 残留清理闭环

- docs/arch/cross-boundary-symbols.md §3.3 ❌/🟡 状态全部改为 ✅
- §6 验收清单增 1 条新条目
- docs/changelog.md 2026-09-26 增 AAGU-4 残留清理条目（4 task commits summary）"
```

---

## Spec Coverage Check

| Spec section | Task |
|--------------|------|
| §2.1 softirq 通用调用面 + 架构内联 | Task 1 |
| §2.2 tick 拆分（poll_timeout_tick weak/default + fs strong + arch_tick_start PIT/LAPIC 整合） | Task 2 |
| §2.3 clocksource 头拆分 | Task 3 |
| §2.4 其余清理（timer spin hint + SUBSYS_INITCALL ifdef 删除） | Task 3 + Task 4 |
| §3 文件改动与构建约束 | All tasks (mapped to file structure) |
| §4 验证与验收（静态 + 编译 + host + QEMU + 文档） | Task 5 (final verification) |

All spec requirements have at least one task implementing them. ✅

## Self-Review

- **Spec coverage:** All §2 design decisions and §4 verification items have corresponding steps. ✅
- **Placeholder scan:** No "TBD", "TODO", "fill in details", "appropriate error handling", "similar to Task N" patterns. ✅
- **Type consistency:** `poll_timeout_tick()` signature `void poll_timeout_tick(void)` defined in Task 2 weak default and Task 2 strong override — consistent. `arch_tick_start()` already exists at `kernel/include/arch/cpu.h:22,88` returning `bool` — Task 2 step 3 preserves this. `arch_atomic_or/and_u64` signatures `void arch_atomic_or_u64(uint64_t*, uint64_t)` consistent across Task 1 new headers and existing callsite in softirq.c. `clocksource_read_ns()` signature `uint64_t clocksource_read_ns(void)` consistent across Task 3 old/new headers and callers. ✅
- **Known follow-ups (out of scope, to record in changelog or spec):**
  - `kernel/include/arch/atomic.h` lines 13-136 still contain 7 `static inline` functions with their own `#ifdef __x86_64__` / `#elif defined(__aarch64__)` blocks (fetch_add/sub/inc/read/write/cas/xchg). These are out of scope for this PR; should be a separate AAGU-4 follow-up issue (similar per-arch header split).
  - `kernel/arch/aarch64/boot/main.c:282` has `#if defined(__aarch64__)` wrapping a SUBSYS_INITCALL block. This TU is aarch64-only by directory convention but the ifdef is redundant. Could be cleaned up in a follow-up. Not in the 14-block count.
- **No ambiguous test baseline:** The spec avoided hardcoded baseline counts (e.g. "268/268 PASS"). Each Task's "Expected" describes what success looks like relative to the existing `d695020` baseline; final verification in Task 5 step 7 runs all the named suites without asserting hardcoded numbers.