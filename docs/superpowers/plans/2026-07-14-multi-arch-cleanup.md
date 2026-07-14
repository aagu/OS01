# Multi-Arch Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate all x86_64 direct includes from architecture-agnostic kernel code, establish aarch64 stubs so `make ARCH=aarch64` compiles to link stage.

**Architecture:** Patch through a chain of `#ifdef __x86_64__`/`#elif defined(__aarch64__)` dispatch headers under `arch/` to route `arch/x86_64/` includes through a single architecture selection point. Split `task.c` — move `__switch_to`, `kernel_thread_func` asm, and x86 init into `arch/x86_64/`.

**Tech Stack:** C (clang), GNU as (.S), GNU Make

## Global Constraints

- `make clean && make` must produce 0 warnings after every task
- `make test-syscall` must pass 70/70 at checkpoints
- Every commit must be bisectable (clean build)
- `spinlock_T` type name must not change (backward compat)
- Test mirror files (`test/include/kernel/`) must stay in sync
- `kernel/kernel/main.c` is exempt (pure x86 boot sequence, principle 4)
- `apic/`, `pic/`, `pit.c`, `sched/smp.c` are exempt

---

### Task 1: Build system parameterization + aarch64 make.config

**Files:**
- Modify: `kernel/Makefile:1-5,164`
- Create: `kernel/arch/aarch64/make.config`

**Interfaces:**
- Produces: `ARCH ?= x86_64`, `OBJFORMAT` variable, `$(BUILD_DIR)` per-arch

- [ ] **Step 1: Parameterize Makefile header**

Replace lines 1-9 of `kernel/Makefile`:

```makefile
# ── Architecture selection ──────────────────────────────────
ARCH        ?= x86_64
ARCHDIR     := arch/$(ARCH)
# Build directory: all .o .d .elf .bin go here, separate from source
BUILD_DIR   := ../build/$(ARCH)/kernel
DEST_DIR    := ..                  # root of the project (for kernel.bin)

# ── Load architecture configuration ────────────────────────
include $(ARCHDIR)/make.config

# ── Compiler / Linker (inherited from root Makefile exports) ─
CC       ?= clang -target x86_64-unknown-none
LD       ?= ld.lld -m elf_x86_64
AR       ?= llvm-ar
OBJ_CPY  ?= llvm-objcopy
```

- [ ] **Step 2: Add OBJFORMAT variable to Makefile**

After `OBJ_CPY  ?= llvm-objcopy` line, add:

```makefile
OBJFORMAT ?= elf64-x86-64
```

- [ ] **Step 3: Replace hardcoded elf64-x86-64 at line 164**

Find:
```makefile
$(BUILD_DIR)/kernel.bin: $(BUILD_DIR)/kernel.elf
	$(OBJ_CPY) -I elf64-x86-64 -S -R ".eh_frame" -R ".comment" \
	    -O binary $< $@
```

Replace with:
```makefile
$(BUILD_DIR)/kernel.bin: $(BUILD_DIR)/kernel.elf
	$(OBJ_CPY) -I $(OBJFORMAT) -S -R ".eh_frame" -R ".comment" \
	    -O binary $< $@
```

- [ ] **Step 4: Create aarch64 make.config**

Create `kernel/arch/aarch64/make.config`:

```makefile
# ── Architecture: aarch64 ──────────────────────────────────
# Toolchain (no nasm — aarch64 uses GNU as via clang)
ARCH_LINKER    = linker.ld

# Arch-specific CFLAGS
ARCH_CFLAGS    = -march=armv8-a -ffreestanding -Wall -Wextra \
                 -mgeneral-regs-only -fpie
ARCH_ASMFLAGS  =

# Arch-specific linker flags
ARCH_LDFLAGS   = -Wl,-m -Wl,aarch64elf -static -Wl,-z,muldefs

# Arch-specific source files (auto-discovered)
KERNEL_ARCH_SOURCES := \
    $(wildcard $(ARCHDIR)/*.c) \
    $(wildcard $(ARCHDIR)/*.S)
```

- [ ] **Step 5: Build and verify**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

```bash
cd /home/aagu/OS01 && make test-syscall 2>&1 | tail -5
```

Expected: `70/70 PASS`

- [ ] **Step 6: Commit**

```bash
git add kernel/Makefile kernel/arch/aarch64/make.config
git commit -m "build: parameterize ARCH, add OBJFORMAT, create aarch64 make.config

- HOSTARCH := x86_64 → ARCH ?= x86_64
- OBJFORMAT variable replaces hardcoded elf64-x86-64 at :164
- kernel/arch/aarch64/make.config with aarch64 toolchain flags

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Add ARCH_PAGE_OFFSET + arch_get_page_table to arch/mmu.h

**Files:**
- Modify: `kernel/include/kernel/arch/mmu.h`

**Interfaces:**
- Produces: `#define ARCH_PAGE_OFFSET 0xffff800000000000ULL`, `arch_get_page_table()`
- Consumes: (none — pure additions, no consumers yet)

- [ ] **Step 1: Read current arch/mmu.h x86_64 branch**

Already read — has `arch_flush_tlb_all`, `arch_flush_tlb_page`, `arch_switch_mm`, `arch_virt_to_phys`. No `ARCH_PAGE_OFFSET` or `arch_get_page_table` yet.

- [ ] **Step 2: Add ARCH_PAGE_OFFSET and arch_get_page_table**

In `kernel/include/kernel/arch/mmu.h`, inside the `#ifdef __x86_64__` block, add before `arch_flush_tlb_all`:

```c
// Higher-half base address for direct physical memory mapping.
// All physical RAM is mapped at this offset (256th PML4 entry).
#define ARCH_PAGE_OFFSET 0xffff800000000000ULL

// Return the current page table base (CR3 on x86_64, TTBR0_EL1 on aarch64).
static inline uint64_t *arch_get_page_table(void) {
    uint64_t cr3;
    __asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)cr3;
}
```

- [ ] **Step 3: Build and verify**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

- [ ] **Step 4: Commit**

```bash
git add kernel/include/kernel/arch/mmu.h
git commit -m "feat: add ARCH_PAGE_OFFSET and arch_get_page_table to arch/mmu.h

x86_64 branch: ARCH_PAGE_OFFSET=0xffff800000000000, arch_get_page_table via CR3.
No consumers yet — memory.h will switch to these in next commit.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Update memory.h — PAGE_OFFSET → ARCH_PAGE_OFFSET, delete get_cr3()

**Files:**
- Modify: `kernel/include/kernel/memory.h`

**Interfaces:**
- Consumes: `ARCH_PAGE_OFFSET`, `arch_get_page_table()` from arch/mmu.h (Task 2)
- Produces: `#define PAGE_OFFSET ARCH_PAGE_OFFSET`, backward-compat `get_cr3()` macro

- [ ] **Step 1: Read current memory.h**

Already read — has `#define PAGE_OFFSET ((unsigned long)0xffff800000000000)` on line 9, `get_cr3()` inline function at line 21.

- [ ] **Step 2: Add arch/mmu.h include**

After `#include <kernel/bootinfo.h>`, add:
```c
#include <kernel/arch/mmu.h>
```

- [ ] **Step 3: Replace PAGE_OFFSET definition**

Change line 9 from:
```c
#define PAGE_OFFSET ((unsigned long)0xffff800000000000)
```
To:
```c
#define PAGE_OFFSET ARCH_PAGE_OFFSET
```

- [ ] **Step 4: Delete get_cr3() inline function and add backward-compat alias**

Delete the `get_cr3()` inline function (currently at line 21), replace with:
```c
// Backward-compatible alias — returns current page table base.
// New code should use arch_get_page_table() directly.
#define get_cr3() arch_get_page_table()
```

- [ ] **Step 5: Build and verify**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

- [ ] **Step 6: Commit**

```bash
git add kernel/include/kernel/memory.h
git commit -m "refactor: PAGE_OFFSET → ARCH_PAGE_OFFSET, replace get_cr3() with arch_get_page_table()

- PAGE_OFFSET now delegates to ARCH_PAGE_OFFSET from arch/mmu.h
- get_cr3() inline x86 asm replaced by macro wrapping arch_get_page_table()
- Backward compatible: all existing callers continue to work

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Create arch/spinlock.h dispatch header

**Files:**
- Create: `kernel/include/kernel/arch/spinlock.h`

**Interfaces:**
- Produces: `#include <kernel/arch/spinlock.h>` routes to correct arch's spinlock
- Consumes: (none — no consumers yet, added in Task 5)

- [ ] **Step 1: Create the dispatch header**

Create `kernel/include/kernel/arch/spinlock.h`:

```c
#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H

#ifdef __x86_64__
#include <kernel/arch/x86_64/spinlock.h>
#elif defined(__aarch64__)
#include <kernel/arch/aarch64/spinlock.h>
#else
#error "Unsupported architecture"
#endif

#endif /* _ARCH_SPINLOCK_H */
```

- [ ] **Step 2: Build and verify**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0` (header exists but no consumers yet, no behavioral change)

- [ ] **Step 3: Commit**

```bash
git add kernel/include/kernel/arch/spinlock.h
git commit -m "feat: add arch/spinlock.h dispatch header

Routes arch/x86_64/spinlock.h (or arch/aarch64/spinlock.h) based on __x86_64__/__aarch64__.
No consumers yet — migration follows in next commit.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: Migrate all 15 spinlock.h includes to arch/spinlock.h

**Files:**
- Modify: 15 files (see steps below)

**Interfaces:**
- Consumes: `arch/spinlock.h` dispatch header (Task 4)

- [ ] **Step 1: Header files — replace include paths**

In each file, change `#include <kernel/arch/x86_64/spinlock.h>` to `#include <kernel/arch/spinlock.h>`:

1. `kernel/include/kernel/file.h:5`
2. `kernel/include/kernel/printk.h:5`
3. `kernel/include/kernel/tty.h:7`
4. `kernel/include/kernel/wait.h:6`
5. `kernel/include/fs/ext2.h:8`

- [ ] **Step 2: C source files — replace include paths**

Same change in each file (`arch/x86_64/spinlock.h` → `arch/spinlock.h`):

6. `kernel/kernel/main.c:8`
7. `kernel/kernel/log.c:3`
8. `kernel/kernel/printk.c:6`
9. `kernel/memory/pmm.c:8`
10. `kernel/driver/keyboard.c:11`
11. `kernel/sched/deferred_free.c:6`
12. `kernel/sched/task.c:5`
13. `kernel/test/selftest.c:8`

- [ ] **Step 3: Test mirror files — replace include paths**

14. `test/include/kernel/file.h:5`
15. `test/include/kernel/printk.h:5`

- [ ] **Step 4: Build and verify**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

```bash
cd /home/aagu/OS01 && make test-syscall 2>&1 | tail -5
```

Expected: `70/70 PASS`

- [ ] **Step 5: Commit**

```bash
git add kernel/include/kernel/file.h kernel/include/kernel/printk.h \
        kernel/include/kernel/tty.h kernel/include/kernel/wait.h \
        kernel/include/fs/ext2.h kernel/kernel/main.c \
        kernel/kernel/log.c kernel/kernel/printk.c kernel/memory/pmm.c \
        kernel/driver/keyboard.c kernel/sched/deferred_free.c \
        kernel/sched/task.c kernel/test/selftest.c \
        test/include/kernel/file.h test/include/kernel/printk.h
git commit -m "refactor: migrate 15 spinlock.h includes to arch/spinlock.h dispatch

All arch/x86_64/spinlock.h includes in arch-agnostic code now go through
arch/spinlock.h dispatch header. Test mirrors synced.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: Create arch/gate.h dispatch header

**Files:**
- Create: `kernel/include/kernel/arch/gate.h`

**Interfaces:**
- Produces: `set_tss64` macro (x86_64: real, aarch64: no-op)
- Consumes: (none — used in Task 11, 13)

- [ ] **Step 1: Create dispatch header**

Create `kernel/include/kernel/arch/gate.h`:

```c
#ifndef _ARCH_GATE_H
#define _ARCH_GATE_H

#ifdef __x86_64__
#include <kernel/arch/x86_64/gate.h>
#elif defined(__aarch64__)
// aarch64 has no TSS — all TSS operations are no-ops
#define set_tss64(rsp0, rsp1, rsp2, ist1, ist2, ist3, ist4, ist5, ist6, ist7) \
    do { (void)(rsp0); (void)(rsp1); (void)(rsp2); \
         (void)(ist1); (void)(ist2); (void)(ist3); \
         (void)(ist4); (void)(ist5); (void)(ist6); \
         (void)(ist7); } while (0)
#else
#error "Unsupported architecture"
#endif

#endif /* _ARCH_GATE_H */
```

- [ ] **Step 2: Build and verify**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

- [ ] **Step 3: Commit**

```bash
git add kernel/include/kernel/arch/gate.h
git commit -m "feat: add arch/gate.h dispatch header (set_tss64)

x86_64 routes to real set_tss64 in gate.h. aarch64 provides no-op macro.
Used by upcoming task.c split (__switch_to and task_init).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: Delete asmlinkage

**Files:**
- Modify: `kernel/include/kernel/arch/x86_64/linkage.h`

**Interfaces:**
- Consumes: (none)
- Produces: linkage.h without `asmlinkage` definition

- [ ] **Step 1: Delete asmlinkage line**

In `kernel/include/kernel/arch/x86_64/linkage.h`, delete line 9:
```c
#define asmlinkage __attribute__((regparm(0)))	
```

- [ ] **Step 2: Build and verify**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0` (asmlinkage had 0 callers)

```bash
cd /home/aagu/OS01 && make test-syscall 2>&1 | tail -5
```

Expected: `70/70 PASS`

- [ ] **Step 3: Commit**

```bash
git add kernel/include/kernel/arch/x86_64/linkage.h
git commit -m "refactor: delete asmlinkage (0 callers, x86-only regparm attribute)

asmlinkage was __attribute__((regparm(0))) — x86-specific calling convention
that would produce -Wignored-attributes on aarch64 clang. Grep confirms
zero call sites. ENTRY/SYMBOL_NAME macros preserved.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: Fix mutex.c arch/x86_64/cpu.h → arch/atomic.h + rename

**Files:**
- Modify: `kernel/mutex.c`

**Interfaces:**
- Consumes: `arch/atomic.h`, `arch_atomic_cas()`, `arch_atomic_write()`

- [ ] **Step 1: Change include and rename calls**

In `kernel/mutex.c`, change line 3:
```c
#include <kernel/arch/x86_64/cpu.h>
```
To:
```c
#include <kernel/arch/atomic.h>
```

Then rename all `atomic_cas` → `arch_atomic_cas` and `atomic_write` → `arch_atomic_write`:

Line 14:
```c
    while (arch_atomic_cas((volatile uint64_t *)&m->owner, 0, (uint64_t)current->pid) == 0) {
```
Line 21:
```c
    return arch_atomic_cas((volatile uint64_t *)&m->owner, 0, (uint64_t)current->pid);
```
Line 26:
```c
    arch_atomic_write((volatile uint64_t *)&m->owner, 0);  // xchgq provides full barrier
```
Line 32:
```c
    while (arch_atomic_cas((volatile uint64_t *)&m->owner, 0, (uint64_t)current->pid) == 0) {
```

- [ ] **Step 2: Build and verify**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

```bash
cd /home/aagu/OS01 && make test-syscall 2>&1 | tail -5
```

Expected: `70/70 PASS`

- [ ] **Step 3: Commit**

```bash
git add kernel/mutex.c
git commit -m "refactor: mutex.c use arch/atomic.h dispatch + arch_atomic_* names

arch/x86_64/cpu.h → arch/atomic.h (dispatch header)
atomic_cas → arch_atomic_cas, atomic_write → arch_atomic_write
Unblocks aarch64 compilation of mutex.c.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 9: Fix futex.c — delete dead arch/x86_64/cpu.h include

**Files:**
- Modify: `kernel/futex.c`

**Interfaces:**
- Consumes: (none — removing dead include)

- [ ] **Step 1: Remove dead include**

In `kernel/futex.c`, delete line 8:
```c
#include <kernel/arch/x86_64/cpu.h>
```

Futex.c does not call any `atomic_*` functions. Its spinlock usage comes via `wait.h` (already migrated in Task 5).

- [ ] **Step 2: Build and verify**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

```bash
cd /home/aagu/OS01 && make test-syscall 2>&1 | tail -5
```

Expected: `70/70 PASS`

- [ ] **Step 3: Commit**

```bash
git add kernel/futex.c
git commit -m "fix: remove dead arch/x86_64/cpu.h include from futex.c

Futex.c does not call any atomic_* functions. Spinlock is obtained
indirectly via wait.h. Verified with grep — 0 atomic_* calls.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 10: Replace arch_virt_to_phys literals with ARCH_PAGE_OFFSET

**Files:**
- Modify: `kernel/include/kernel/arch/mmu.h`

**Interfaces:**
- Consumes: `ARCH_PAGE_OFFSET` (defined in same file, Task 2)

- [ ] **Step 1: Replace three literal occurrences**

In `kernel/include/kernel/arch/mmu.h`, inside `arch_virt_to_phys`, replace all three occurrences of `0xffff800000000000ULL` with `ARCH_PAGE_OFFSET`.

Line ~31 (PML3 walk):
```c
    uint64_t *pml3 = (uint64_t *)((pml4[l4] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
```

Line ~36 (PML2 walk):
```c
    uint64_t *pml2 = (uint64_t *)((pml3[l3] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
```

Line ~41 (PT walk):
```c
    uint64_t *pml1 = (uint64_t *)((pml2[l2] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
```

- [ ] **Step 2: Build and verify**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

```bash
cd /home/aagu/OS01 && make test-syscall 2>&1 | tail -5
```

Expected: `70/70 PASS`

- [ ] **Step 3: Commit**

```bash
git add kernel/include/kernel/arch/mmu.h
git commit -m "refactor: replace arch_virt_to_phys literals with ARCH_PAGE_OFFSET

Three 0xffff800000000000ULL literals in the page-table walk replaced
with ARCH_PAGE_OFFSET macro. No behavioral change for x86_64.
Makes the page-table walk code architecture-neutral.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 11: Create arch/x86_64/switch.c — move __switch_to

**Files:**
- Create: `kernel/arch/x86_64/switch.c`
- Modify: `kernel/sched/task.c` (delete __switch_to body, remove gate.h include)

**Interfaces:**
- Produces: `__switch_to` global symbol (linker resolves `jmp __switch_to` from task.h macro)
- Consumes: `arch/gate.h` (set_tss64), `kernel/task.h` (task_t, thread_t, percpu), `kernel/arch/spinlock.h`

- [ ] **Step 1: Create switch.c**

Create `kernel/arch/x86_64/switch.c`:

```c
#include <kernel/task.h>
#include <kernel/percpu.h>
#include <kernel/arch/spinlock.h>
#include <kernel/arch/gate.h>
#include <kernel/arch/cpu.h>

/**
 * __switch_to — architecture-specific context switch
 *
 * Called by the switch_to() macro (in task.h) via inline asm jmp.
 * Performs:
 *   1. Update per-CPU TSS rsp0 for ring-0 stack on next interrupt.
 *   2. Save/restore FS base selector.
 *   3. Switch page table (CR3) if needed.
 *   4. Save/restore FPU/SSE state for user tasks.
 */
void __switch_to(task_t *prev, task_t *next)
{
    percpu_t *cpu = this_cpu();
    cpu->tss->rsp0 = next->thread->rsp0;

    set_tss64(cpu->tss->rsp0, cpu->tss->rsp1, cpu->tss->rsp2,
              cpu->tss->ist1, cpu->tss->ist2, cpu->tss->ist3,
              cpu->tss->ist4, cpu->tss->ist5, cpu->tss->ist6,
              cpu->tss->ist7);

    // Save/restore FS selector (used by kernel threads).
    // GS base is per-CPU and set ONCE via MSR — never
    // touch it here (loading a non-null GS selector would
    // reload the base from the GDT, clobbering the MSR).
    __asm__ __volatile__("movq %%fs, %0 \n\t":"=a"(prev->thread->fs));
    __asm__ __volatile__("movq %0, %%fs \n\t"::"a"(next->thread->fs));

    // Switch page table if the next task has its own address space
    if (next->thread->cr3 && next->thread->cr3 != prev->thread->cr3) {
        __asm__ __volatile__("movq %0, %%cr3" :: "r"(next->thread->cr3) : "memory");
    }

    // Save/restore FPU/SSE state.  The kernel never uses FPU
    // (-mno-sse -mno-80387), but user programs may.  clts ensures
    // CR0.TS=0 so fxsave/fxrstor don't #NM.
    // fpu_save is a raw malloc ptr; align to 16 bytes for FXSAVE.
    if (prev->fpu_save) {
        uint64_t area = ((uint64_t)prev->fpu_save + 15) & ~15ULL;
        __asm__ __volatile__(
            "clts                \n\t"
            "fxsave64 (%0)       \n\t"
            :: "r"(area) : "memory"
        );
    }
    if (next->fpu_save) {
        uint64_t area = ((uint64_t)next->fpu_save + 15) & ~15ULL;
        __asm__ __volatile__(
            "clts                \n\t"
            "fxrstor64 (%0)      \n\t"
            :: "r"(area) : "memory"
        );
    }
}
```

- [ ] **Step 2: Remove __switch_to and gate.h include from task.c**

Delete the entire `__switch_to` function (lines 26–76) from `kernel/sched/task.c`.

Remove line 4:
```c
#include <kernel/arch/x86_64/gate.h>
```

- [ ] **Step 3: Build and verify**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

```bash
cd /home/aagu/OS01 && make test-syscall 2>&1 | tail -5
```

Expected: `70/70 PASS`

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/switch.c kernel/sched/task.c
git commit -m "refactor: move __switch_to from task.c to arch/x86_64/switch.c

Pure code move — no behavioral change. __switch_to is TSS/CR3/FPU
management, 100% x86-specific. New arch/x86_64/switch.c auto-discovered
by Makefile wildcard. gate.h include removed from task.c.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 12: Create arch/x86_64/thread_entry.S — move kernel_thread_func asm

**Files:**
- Create: `kernel/arch/x86_64/thread_entry.S`
- Modify: `kernel/sched/task.c` (delete __asm__ block, keep extern declaration)

**Interfaces:**
- Produces: `kernel_thread_func` global symbol
- Consumes: `do_exit` (linker-resolved)

- [ ] **Step 1: Create thread_entry.S**

Create `kernel/arch/x86_64/thread_entry.S`:

```asm
# kernel/arch/x86_64/thread_entry.S
# Thread entry trampoline for kernel threads (x86_64).
# Called by kernel_thread() — pops saved register state from
# the fake pt_regs frame, loads data segments, and calls fn(arg).
# On return, calls do_exit().

.globl kernel_thread_func
kernel_thread_func:
    sti                         # re-enable IRQs after first context switch
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbx                   # fn pointer
    popq %rcx
    popq %rdx                   # arg
    popq %rsi
    popq %rdi
    popq %rbp
    popq %rax                   # skip DS slot
    popq %rax                   # skip ES slot
    popq %rax                   # original RAX
    pushq %rax                  # save RAX temporarily
    movq $0x10, %rax
    movq %rax, %ds
    movq %rax, %es
    movq %rax, %fs
    # GS is NOT reloaded — its base is per-CPU, set via
    # IA32_GS_BASE MSR. Loading a selector would clobber
    # the per-CPU base with the GDT flat descriptor value.
    popq %rax                   # restore RAX
    addq $0x38, %rsp
    movq %rdx, %rdi             # arg
    callq *%rbx                 # call fn(arg)
    movq %rax, %rdi
    callq do_exit
```

- [ ] **Step 2: Remove inline asm block from task.c**

Delete the entire `extern void kernel_thread_func(void);` declaration and `__asm__(...)` block (approximately lines 610–650 of task.c).

**Keep** the `extern void kernel_thread_func(void);` declaration in task.c — it's still needed by `kernel_thread()` which sets `thd->rip = (uint64_t)kernel_thread_func`.

Actually, replace the `extern` + `__asm__` block with just:

```c
// kernel_thread_func is defined in arch/x86_64/thread_entry.S.
extern void kernel_thread_func(void);
```

- [ ] **Step 3: Build and verify**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

```bash
cd /home/aagu/OS01 && make test-syscall 2>&1 | tail -5
```

Expected: `70/70 PASS`

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/thread_entry.S kernel/sched/task.c
git commit -m "refactor: move kernel_thread_func asm to arch/x86_64/thread_entry.S

Inline asm moved to standalone .S file. task.c keeps only the extern
declaration used by kernel_thread() to set thread->rip.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 13: Create arch/x86_64/task_arch.c + add arch_task_init_platform hook

**Files:**
- Create: `kernel/arch/x86_64/task_arch.c`
- Modify: `kernel/include/kernel/arch/thread.h` (add declaration)
- Modify: `kernel/sched/task.c` (call hook, remove x86 init code, remove gate.h/regs.h includes)

**Interfaces:**
- Produces: `void arch_task_init_platform(void)`
- Consumes from task.c: `init_mm`, `init_thread`, `init_tss` (declared in task.h)

- [ ] **Step 1: Add declaration to arch/thread.h (x86_64 block only)**

In `kernel/include/kernel/arch/thread.h`, inside the `#ifdef __x86_64__` block, add after the existing `#include <kernel/arch/x86_64/regs.h>` line:

```c
// Architecture-specific task init (TSS, CR3 setup).
// Called once by task_init() during boot.
void arch_task_init_platform(void);
```

Do NOT touch the `#elif defined(__aarch64__)` block — its `#error` stays until Task 19 replaces it with a full aarch64 pt_regs_t + stub.

- [ ] **Step 2: Create task_arch.c**

Create `kernel/arch/x86_64/task_arch.c`:

```c
#include <kernel/task.h>
#include <kernel/arch/mmu.h>
#include <kernel/arch/gate.h>
#include <kernel/arch/thread.h>

void arch_task_init_platform(void)
{
    // Save current page table base (set up by head.S / EFI stub).
    init_mm.pml4 = (uint64_t *)arch_get_page_table();
    init_thread.cr3 = (uint64_t)init_mm.pml4;

    // Program BSP TSS with kernel stack pointers and IST entries.
    set_tss64(init_thread.rsp0, init_tss[0].rsp1, init_tss[0].rsp2,
              init_tss[0].ist1, init_tss[0].ist2, init_tss[0].ist3,
              init_tss[0].ist4, init_tss[0].ist5, init_tss[0].ist6,
              init_tss[0].ist7);

    init_tss[0].rsp0 = init_thread.rsp0;
}
```

- [ ] **Step 3: Update task.c — add hook call, remove x86 code, clean includes**

In `kernel/sched/task.c`, in the `task_init()` function:

Replace these lines:
```c
    init_mm.pml4 = get_cr3();
    init_thread.cr3 = (uint64_t)init_mm.pml4;

    init_mm.start_code = PMMngr.start_code;
    ...
    list_init(&init_mm.vma_list);
    init_mm.mmap_base = 0;

    set_tss64(init_thread.rsp0, init_tss[0].rsp1, init_tss[0].rsp2,
              init_tss[0].ist1, init_tss[0].ist2, init_tss[0].ist3,
              init_tss[0].ist4, init_tss[0].ist5, init_tss[0].ist6,
              init_tss[0].ist7);

    init_tss[0].rsp0 = init_thread.rsp0;
```

With:
```c
    arch_task_init_platform();

    init_mm.start_code = PMMngr.start_code;
    ...
    list_init(&init_mm.vma_list);
    init_mm.mmap_base = 0;
```

Remove remaining `arch/x86_64/` includes from task.c:
- Delete line 7: `#include <kernel/arch/x86_64/regs.h>`
- Delete line 8: `#include <kernel/arch/x86_64/linkage.h>`

(Note: line 5 `arch/x86_64/spinlock.h` was already migrated in Task 5. line 4 `arch/x86_64/gate.h` was already removed in Task 11.)

No new include needed — `arch_task_init_platform()` is declared in `arch/thread.h`, which is already included via `task.h`.

- [ ] **Step 4: Build and verify**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

```bash
cd /home/aagu/OS01 && make test-syscall 2>&1 | tail -5
```

Expected: `70/70 PASS`

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/x86_64/task_arch.c kernel/include/kernel/arch/thread.h \
        kernel/sched/task.c
git commit -m "refactor: extract arch_task_init_platform() to arch/x86_64/task_arch.c

task_init() now calls arch_task_init_platform() for CR3 save + TSS setup.
All arch/x86_64/ includes removed from task.c (was 4, now 0).
arch/thread.h declares the hook for both x86_64 (extern) and aarch64 (inline stub).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 14: Sync test/include/ mirror files

**Files:**
- Modify: `test/include/kernel/file.h` (already done in Task 5)
- Modify: `test/include/kernel/printk.h` (already done in Task 5)
- Modify: `test/include/kernel/interrupt.h`
- Modify: `test/include/kernel/task.h`
- Modify: `test/include/kernel/trace.h`

**Interfaces:**
- Consumes: `arch/spinlock.h`, `arch/thread.h`, `arch/cpu.h` dispatch headers (Tasks 4, 2, existing)

- [ ] **Step 1: Apply to test/include/kernel/interrupt.h**

Current:
```c
#include <kernel/arch/x86_64/regs.h>
#include <kernel/arch/x86_64/linkage.h>
```

Change `regs.h` line to `arch/thread.h` (pt_regs_t already available via thread.h dispatch):
```c
#include <kernel/arch/thread.h>
#include <kernel/arch/x86_64/linkage.h>
```

Note: `linkage.h` stays as is (decision #3 — no dispatch).

- [ ] **Step 2: Apply to test/include/kernel/task.h**

Current:
```c
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/arch/x86_64/regs.h>
#include <kernel/arch/x86_64/linkage.h>
```

Change to:
```c
#include <kernel/arch/cpu.h>
#include <kernel/arch/thread.h>
#include <kernel/arch/x86_64/linkage.h>
```

- [ ] **Step 3: Apply to test/include/kernel/trace.h**

Current:
```c
#include <kernel/arch/x86_64/regs.h>
```

Change to:
```c
#include <kernel/arch/thread.h>
```

- [ ] **Step 4: Build and verify (including test)**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

```bash
cd /home/aagu/OS01 && make test 2>&1 | tail -3
```

Expected: test compile succeeds

```bash
cd /home/aagu/OS01 && make test-syscall 2>&1 | tail -5
```

Expected: `70/70 PASS`

- [ ] **Step 5: Commit**

```bash
git add test/include/kernel/interrupt.h test/include/kernel/task.h \
        test/include/kernel/trace.h
git commit -m "refactor: sync test/include/ mirrors with kernel header migration

interrupt.h: regs.h → arch/thread.h
task.h: cpu.h → arch/cpu.h, regs.h → arch/thread.h
trace.h: regs.h → arch/thread.h
All linkage.h references preserved (no dispatch per decision #3).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 15: aarch64 cpu.h stub

**Files:**
- Modify: `kernel/include/kernel/arch/cpu.h`

**Interfaces:**
- Produces: aarch64 `arch_cpu_halt`, `arch_cpu_pause`, `arch_nop`, `arch_cycle_counter`, `arch_cpu_enable_nx`, `arch_set_percpu_base`

- [ ] **Step 1: Implement aarch64 branch**

In `kernel/include/kernel/arch/cpu.h`, replace the `#elif defined(__aarch64__)` block:

Replace:
```c
#elif defined(__aarch64__)
#error "aarch64 cpu.h not yet implemented"
```

With:
```c
#elif defined(__aarch64__)

#include <stdint.h>

#ifndef NR_CPUS
#define NR_CPUS 8
#endif

static inline void arch_cpu_halt(void)
{
    __asm__ __volatile__("wfi" ::: "memory");
}

static inline void arch_cpu_pause(void)
{
    __asm__ __volatile__("yield" ::: "memory");
}

static inline void arch_nop(void)
{
    __asm__ __volatile__("nop");
}

static inline uint64_t arch_cycle_counter(void)
{
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

// Enable No-eXecute: set SCTLR_EL1.WXN (bit 19)
static inline void arch_cpu_enable_nx(void)
{
    uint64_t sctlr;
    __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 19);
    __asm__ __volatile__("msr sctlr_el1, %0" :: "r"(sctlr) : "memory");
}

// Set per-CPU data base pointer (TPIDR_EL1 on aarch64)
static inline void arch_set_percpu_base(void *ptr)
{
    __asm__ __volatile__("msr tpidr_el1, %0" :: "r"((uint64_t)ptr) : "memory");
}
```

- [ ] **Step 2: Build for aarch64 (expected: compiles cpu.h, fails on next missing header)**

```bash
cd /home/aagu/OS01/kernel && make ARCH=aarch64 clean && make ARCH=aarch64 2>&1 | head -20
```

Expected: Gets past cpu.h, fails on next `#error` (irq.h or mmu.h)

- [ ] **Step 3: Verify x86_64 still builds**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

- [ ] **Step 4: Commit**

```bash
git add kernel/include/kernel/arch/cpu.h
git commit -m "feat: aarch64 cpu.h stub — WFI, YIELD, NOP, cntvct_el0, SCTLR_EL1, TPIDR_EL1

Replaces #error with working inline asm for all cpu.h operations.
Compile-tested with ARCH=aarch64.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 16: aarch64 irq.h stub

**Files:**
- Modify: `kernel/include/kernel/arch/irq.h`

**Interfaces:**
- Produces: aarch64 `arch_irq_state_t`, `arch_local_irq_enable/disable/save/restore`

- [ ] **Step 1: Read current irq.h**

Let me check the current dispatch header:

```bash
cat kernel/include/kernel/arch/irq.h
```

- [ ] **Step 2: Implement aarch64 branch**

Replace the `#elif defined(__aarch64__)` block with:

```c
#elif defined(__aarch64__)

typedef uint64_t arch_irq_state_t;

// DAIF: bit 7=Debug, bit 6=SError, bit 2=IRQ, bit 1=FIQ
// daifclr clears bits → enables IRQs; daifset sets bits → disables IRQs
#define DAIF_IRQ_BIT  (1UL << 2)

static inline void arch_local_irq_enable(void)
{
    __asm__ __volatile__("msr daifclr, %0" :: "i"(DAIF_IRQ_BIT) : "memory");
}

static inline void arch_local_irq_disable(void)
{
    __asm__ __volatile__("msr daifset, %0" :: "i"(DAIF_IRQ_BIT) : "memory");
}

static inline arch_irq_state_t arch_local_irq_save(void)
{
    uint64_t daif;
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
    arch_local_irq_disable();
    return daif;
}

static inline void arch_local_irq_restore(arch_irq_state_t state)
{
    __asm__ __volatile__("msr daif, %0" :: "r"(state) : "memory");
}
```

- [ ] **Step 3: Verify x86_64 still builds + aarch64 gets further**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

- [ ] **Step 4: Commit**

```bash
git add kernel/include/kernel/arch/irq.h
git commit -m "feat: aarch64 irq.h stub — DAIF-based IRQ control

arch_irq_state_t, local_irq_enable/disable/save/restore via
msr daifclr/daifset/mrs daif.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 17: aarch64 atomic.h stub

**Files:**
- Modify: `kernel/include/kernel/arch/atomic.h`

**Interfaces:**
- Produces: aarch64 `arch_atomic_fetch_add/sub`, `arch_atomic_inc`, `arch_atomic_read/write`, `arch_atomic_cas`, `arch_atomic_xchg`

- [ ] **Step 1: Implement aarch64 branch**

Replace the `#elif defined(__aarch64__)` block with:

```c
#elif defined(__aarch64__)

static inline uint64_t arch_atomic_fetch_add(volatile uint64_t *ptr, uint64_t val)
{
    uint64_t old, tmp;
    __asm__ __volatile__(
        "1: ldxr %0, [%2]       \n\t"
        "   add  %1, %0, %3     \n\t"
        "   stxr %w4, %1, [%2]  \n\t"
        "   cbnz %w4, 1b        \n\t"
        : "=&r"(old), "=&r"(tmp), "+r"(ptr)
        : "r"(val), "r"(0)
        : "memory"
    );
    return old;
}

static inline uint64_t arch_atomic_fetch_sub(volatile uint64_t *ptr, uint64_t val)
{
    return arch_atomic_fetch_add(ptr, -(int64_t)val);
}

static inline uint64_t arch_atomic_inc(volatile uint64_t *ptr)
{
    return arch_atomic_fetch_add(ptr, 1) + 1;
}

static inline uint64_t arch_atomic_read(volatile uint64_t *ptr)
{
    uint64_t val;
    __asm__ __volatile__("ldr %0, [%1]" : "=r"(val) : "r"(ptr) : "memory");
    return val;
}

static inline void arch_atomic_write(volatile uint64_t *ptr, uint64_t val)
{
    uint64_t tmp;
    __asm__ __volatile__(
        "1: ldxr %0, [%1]       \n\t"
        "   stxr %w0, %2, [%1]  \n\t"
        "   cbnz %w0, 1b        \n\t"
        : "=&r"(tmp) : "r"(ptr), "r"(val) : "memory"
    );
}

static inline int arch_atomic_cas(volatile uint64_t *ptr, uint64_t old, uint64_t new)
{
    uint64_t cur;
    int result;
    __asm__ __volatile__(
        "1: ldxr %0, [%2]       \n\t"
        "   cmp  %0, %3         \n\t"
        "   b.ne 2f             \n\t"
        "   stxr %w1, %4, [%2]  \n\t"
        "   cbnz %w1, 1b        \n\t"
        "   mov  %1, #1         \n\t"
        "   b    3f             \n\t"
        "2: mov  %1, #0         \n\t"
        "3:                     \n\t"
        : "=&r"(cur), "=&r"(result) : "r"(ptr), "r"(old), "r"(new) : "memory"
    );
    return result;
}

static inline uint64_t arch_atomic_xchg(volatile uint64_t *ptr, uint64_t val)
{
    uint64_t old;
    int tmp;
    __asm__ __volatile__(
        "1: ldxr %0, [%2]       \n\t"
        "   stxr %w1, %3, [%2]  \n\t"
        "   cbnz %w1, 1b        \n\t"
        : "=&r"(old), "=&r"(tmp) : "r"(ptr), "r"(val) : "memory"
    );
    return old;
}
```

- [ ] **Step 2: Verify x86_64 still builds**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

- [ ] **Step 3: Commit**

```bash
git add kernel/include/kernel/arch/atomic.h
git commit -m "feat: aarch64 atomic.h stub — ldxr/stxr exclusive loops

All 7 atomic operations: fetch_add, fetch_sub, inc, read, write, cas, xchg.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 18: aarch64 mmu.h stub

**Files:**
- Modify: `kernel/include/kernel/arch/mmu.h`

**Interfaces:**
- Produces: aarch64 `ARCH_PAGE_OFFSET`, `arch_get_page_table`, `arch_flush_tlb_all/page`, `arch_switch_mm`, `arch_virt_to_phys`

- [ ] **Step 1: Implement aarch64 branch**

Replace the `#elif defined(__aarch64__)` block with:

```c
#elif defined(__aarch64__)

#define ARCH_PAGE_OFFSET 0xffff000000000000ULL

static inline uint64_t *arch_get_page_table(void)
{
    uint64_t ttbr0;
    __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    return (uint64_t *)ttbr0;
}

static inline void arch_flush_tlb_all(void)
{
    __asm__ __volatile__("tlbi vmalle1 \n\t dsb sy \n\t isb" ::: "memory");
}

static inline void arch_flush_tlb_page(uintptr_t vaddr)
{
    __asm__ __volatile__("tlbi vae1, %0 \n\t dsb sy \n\t isb" :: "r"(vaddr >> 12) : "memory");
}

static inline void arch_switch_mm(uint64_t *pgtbl)
{
    __asm__ __volatile__("msr ttbr0_el1, %0 \n\t isb" :: "r"(pgtbl) : "memory");
}

// Walk 4-level (48-bit VA) page table: PGD→PUD→PMD→PTE.
// Uses ARCH_PAGE_OFFSET to convert physical entries to direct-mapped virtual.
static inline uintptr_t arch_virt_to_phys(void *pgtbl, uintptr_t va)
{
    uint64_t *pgd = (uint64_t *)pgtbl;
    uint64_t l0 = (va >> 39) & 0x1FF;
    if (!(pgd[l0] & 1)) return 0;
    uint64_t *pud = (uint64_t *)((pgd[l0] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
    uint64_t l1 = (va >> 30) & 0x1FF;
    if (!(pud[l1] & 1)) return 0;
    // 1GB block (table entry bit 1 set)
    if ((pud[l1] & 2) == 0)
        return (pud[l1] & 0xFFFFFC0000000ULL) | (va & 0x3FFFFFFF);
    uint64_t *pmd = (uint64_t *)((pud[l1] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
    uint64_t l2 = (va >> 21) & 0x1FF;
    if (!(pmd[l2] & 1)) return 0;
    // 2MB block
    if ((pmd[l2] & 2) == 0)
        return (pmd[l2] & 0xFFFFFFFE00000ULL) | (va & 0x1FFFFF);
    uint64_t *pte = (uint64_t *)((pmd[l2] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
    uint64_t l3 = (va >> 12) & 0x1FF;
    if (!(pte[l3] & 1)) return 0;
    return (pte[l3] & 0xFFFFFFFFFFFFF000ULL) | (va & 0xFFF);
}
```

- [ ] **Step 2: Verify x86_64 still builds**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

- [ ] **Step 3: Commit**

```bash
git add kernel/include/kernel/arch/mmu.h
git commit -m "feat: aarch64 mmu.h stub — TTBR0, TLBI, 4-level walk

ARCH_PAGE_OFFSET=0xffff000000000000, arch_get_page_table via TTBR0_EL1,
TLB ops via tlbi vmalle1/vae1, 4-level (PGD→PUD→PMD→PTE) walk
with 1GB/2MB block support.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 19: aarch64 thread.h stub

**Files:**
- Modify: `kernel/include/kernel/arch/thread.h`

**Interfaces:**
- Produces: aarch64 `pt_regs_t`, `arch_task_init_platform()` stub (already added in Task 13)

- [ ] **Step 1: Implement aarch64 branch**

Replace the `#elif defined(__aarch64__)` block with:

```c
#elif defined(__aarch64__)

// aarch64 exception frame as pushed by the exception vector handler.
// x0-x29 (30 regs), plus SP_EL0, ELR_EL1, SPSR_EL1.
typedef struct pt_regs
{
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;
    uint64_t x16, x17, x18, x19, x20, x21, x22, x23;
    uint64_t x24, x25, x26, x27, x28, x29;
    uint64_t sp_el0;
    uint64_t elr_el1;
    uint64_t spsr_el1;
} pt_regs_t;

// arch_task_init_platform placeholder — aarch64 doesn't need TSS setup.
// Declared in this header's x86_64 block. For aarch64 it's a no-op.
static inline void arch_task_init_platform(void) {}
```

- [ ] **Step 2: Verify x86_64 still builds**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

- [ ] **Step 3: Commit**

```bash
git add kernel/include/kernel/arch/thread.h
git commit -m "feat: aarch64 thread.h stub — pt_regs_t with 33-field frame

30 general-purpose registers + SP_EL0 + ELR_EL1 + SPSR_EL1.
arch_task_init_platform is a no-op (no TSS on aarch64).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 20: aarch64 cache.h + segment.h stubs

**Files:**
- Modify: `kernel/include/kernel/arch/cache.h`
- Modify: `kernel/include/kernel/arch/segment.h`

**Interfaces:**
- Produces: aarch64 dcache ops (cvac/ivac), aarch64 segment macros (all 0)

- [ ] **Step 1: Implement aarch64 branch in cache.h**

Replace the `#elif defined(__aarch64__)` block with:

```c
#elif defined(__aarch64__)

// Clean data cache by virtual address to point of coherency
static inline void arch_flush_dcache(void *va, size_t size)
{
    uintptr_t addr = (uintptr_t)va;
    uintptr_t end = addr + size;
    for (; addr < end; addr += 64) {
        __asm__ __volatile__("dc cvac, %0" :: "r"(addr) : "memory");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
}

// Invalidate data cache by virtual address to point of coherency
static inline void arch_inval_dcache(void *va, size_t size)
{
    uintptr_t addr = (uintptr_t)va;
    uintptr_t end = addr + size;
    for (; addr < end; addr += 64) {
        __asm__ __volatile__("dc ivac, %0" :: "r"(addr) : "memory");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
}
```

- [ ] **Step 2: Implement aarch64 branch in segment.h**

Replace the `#elif defined(__aarch64__)` block with:

```c
#elif defined(__aarch64__)

// aarch64 has no segmentation — all selectors are 0
#define KERNEL_CS 0
#define KERNEL_DS 0
#define USER_CS   0
#define USER_DS   0
```

- [ ] **Step 3: Verify x86_64 still builds + aarch64 gets to link stage**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

```bash
cd /home/aagu/OS01/kernel && make ARCH=aarch64 2>&1 | tail -10
```

Expected: Compiles all .c files, link fails with `_start` undefined (no aarch64 head.S) or similar linker error.

- [ ] **Step 4: Commit**

```bash
git add kernel/include/kernel/arch/cache.h kernel/include/kernel/arch/segment.h
git commit -m "feat: aarch64 cache.h + segment.h stubs

cache.h: dc cvac (clean) + dc ivac (invalidate) with dsb sy barrier.
segment.h: all CS/DS macros → 0 (no segmentation on aarch64).

With all 7 arch stub headers complete, ARCH=aarch64 builds to link stage.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 21: Final verification — full test pass + aarch64 compile

**No code changes — verification only.**

- [ ] **Step 1: Full x86_64 build**

```bash
cd /home/aagu/OS01 && make clean && make 2>&1 | grep -c warning
```

Expected: `0`

- [ ] **Step 2: Syscall test suite**

```bash
cd /home/aagu/OS01 && make test-syscall 2>&1 | tail -5
```

Expected: `70/70 PASS`

- [ ] **Step 3: Boot test**

```bash
cd /home/aagu/OS01 && timeout 5 make run 2>&1 | head -20 || true
```

Expected: boots to shell prompt

- [ ] **Step 4: aarch64 compile test**

```bash
cd /home/aagu/OS01/kernel && make ARCH=aarch64 clean && make ARCH=aarch64 2>&1 | tail -5
```

Expected: Compiles all .c and .S, reaches linker stage. Linker error about `_start` or missing symbol is expected (no aarch64 head.S / startup code).

- [ ] **Step 5: Commit verification results (if needed)**

No commit needed — this is a verification task. If everything passes, the plan is complete.
