#if AARCH64_EL0_TEST

#include <arch/aarch64/el0.h>
#include <arch/aarch64/dtb.h>
#include <arch/cpu.h>
#include <arch/aarch64/reg.h>
#include <arch/aarch64/timer.h>
#include <arch/aarch64/uaccess.h>
#include <arch/aarch64/user_elf.h>
#include <fs/elf.h>
#include <arch/aarch64/page_table.h>
#include <arch/mmu.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/slab.h>
#include <stddef.h>
#include <errno.h>

#define CODE_VA 0x400000
#define DATA_VA (CODE_VA + PAGE_4K_SIZE)
#define CROSS_VA (DATA_VA + PAGE_4K_SIZE - 1)
#define STACK_TOP (CODE_VA + 3 * PAGE_4K_SIZE)
extern const char _binary_aarch64_el0_elf_start[], _binary_aarch64_el0_elf_end[];

static size_t fixture_size(void)
{
    return (uintptr_t)_binary_aarch64_el0_elf_end -
           (uintptr_t)_binary_aarch64_el0_elf_start;
}

struct run {
    uint64_t value;
    uint64_t action;
    unsigned traps;
    bool passed;
    uint64_t esr, far, pc, x0;
    uint64_t irq_start, wait_start;
};

static int exercise_uaccess(uint64_t addr, uint64_t len)
{
    unsigned char bytes[2] = {0};
    if (len != sizeof(bytes))
        return -EINVAL;
    int rc = aarch64_copy_from_user(bytes, addr, len);
    if (rc)
        return rc;
    if (addr != CROSS_VA || bytes[0] != 0x12 || bytes[1] != 0x34)
        return 2;
    const unsigned char sentinel = 0x7f;
    unsigned char last;
    if (aarch64_copy_to_user(STACK_TOP - 1, &sentinel, 1) ||
        aarch64_copy_from_user(bytes, STACK_TOP - 1, 2) != -EFAULT ||
        bytes[0] != 0x12 || bytes[1] != 0x34 ||
        aarch64_copy_to_user(STACK_TOP - 1, bytes, 2) != -EFAULT ||
        aarch64_copy_from_user(&last, STACK_TOP - 1, 1) || last != sentinel ||
        aarch64_copy_to_user(CODE_VA, bytes, 1) != -EFAULT ||
        aarch64_copy_from_user(bytes, (uintptr_t)&bytes, 1) != -EFAULT ||
        aarch64_copy_from_user(bytes, UINT64_MAX, 2) != -EFAULT)
        return 3;

    /* Skip preflight only here, to execute both actual faulting opcodes. */
    if (aarch64_copy_from_user_raw(bytes, STACK_TOP, 1) != -EFAULT ||
        aarch64_copy_to_user_raw(CODE_VA, bytes, 1) != -EFAULT)
        return 4;

    const unsigned char swapped[] = {0x34, 0x12};
    return aarch64_copy_to_user(addr, swapped, sizeof(swapped)) ? 5 : 0;
}

static bool handle_sync(pt_regs_t *regs, uint64_t esr, uint64_t far, void *arg)
{
    struct run *run = arg;
    const uint64_t ec = esr >> 26;
    const uint64_t iss = esr & 0x1ffffff;

    run->traps++;
    if (run->action == 3) {
        if (ec == 0x15 && iss == 4 && regs->sp_el0 == STACK_TOP - 16) {
            uint64_t now = arch_cycle_counter();
            uint64_t ticks = aarch64_el0_timer_ticks();
            if (run->traps == 1) {
                run->wait_start = now;
                run->irq_start = ticks;
            }
            if (ticks - run->irq_start >= 2)
                regs->x0 = 1;
            else if (now - run->wait_start >= cntfrq_el0() / 2)
                regs->x0 = (uint64_t)-ETIMEDOUT;
            else
                regs->x0 = 0;
            return true;
        }
        run->passed = ec == 0x15 && iss == 1 && regs->sp_el0 == STACK_TOP &&
                      !regs->x0 && aarch64_el0_timer_ticks() - run->irq_start >= 2;
        goto done;
    }
    if (run->action == 2) {
        if (run->traps <= 2 && ec == 0x15 && iss == 3) {
            regs->x0 = exercise_uaccess(regs->x0, regs->x1);
            return true;
        }
        run->passed = run->traps == 3 && ec == 0x15 && iss == 1 && !regs->x0;
        goto done;
    }
    if (run->traps == 1 && ec == 0x15 && iss == 0 &&
        regs->x0 == run->value && regs->sp_el0 == STACK_TOP - 16 &&
        regs->x12 == run->action) {
        regs->x0++;
        return true;
    }

    if (run->traps == 2 && regs->sp_el0 == STACK_TOP) {
        if (!run->action) {
            run->passed = ec == 0x15 && iss == 1 &&
                          regs->x0 == run->value + 1;
        } else if (ec == 0x24 && !(iss & (1u << 10))) {
            unsigned dfsc = iss & 0x3f;
            bool write = (iss & (1u << 6)) != 0;
            run->passed = run->action == 1
                ? far == CODE_VA && write && dfsc == 0x0f
                : far == run->action && !write && dfsc >= 0x0d && dfsc <= 0x0f;
        }
    }
done:
    /* Publish diagnostics after the probe restores the boot context. */
    run->esr = esr;
    run->far = far;
    run->pc = regs->elr_el1;
    run->x0 = regs->x0;
    return false;
}

struct space {
    struct aarch64_user_elf elf;
    uint64_t stack_pa;
};

static bool setup(struct space *space, uint64_t value)
{
    int rc = aarch64_user_elf_load(_binary_aarch64_el0_elf_start,
                                   fixture_size(), &space->elf);
    if (rc) {
        log_err("M4.3: fixture load rc=%d\n", rc);
        return false;
    }
    uint64_t pa;
    uint32_t perm;
    if (aarch64_pt_query_4k(space->elf.root, DATA_VA, &pa, &perm) ||
        perm != AARCH64_PT_USER_RW)
        return false;
    *(uint64_t *)Phy_To_Virt(pa) = value;

    space->stack_pa = alloc_4k_page();
    if (!space->stack_pa)
        return false;
    __builtin_memset((void *)Phy_To_Virt(space->stack_pa), 0, PAGE_4K_SIZE);
    return aarch64_el0_map(space->elf.root, STACK_TOP - PAGE_4K_SIZE,
                           space->stack_pa, AARCH64_PT_USER_RW) == 0;
}

static void teardown(struct space *space)
{
    aarch64_user_elf_destroy(&space->elf);
    free_4k_page(space->stack_pa);
}

static bool exercise(struct space *space, uint64_t value, uint64_t action)
{
    uint64_t before0, before1, after0, after1;
    __asm__ volatile("mrs %0, ttbr0_el1\n\tmrs %1, ttbr1_el1"
                     : "=r"(before0), "=r"(before1));
    struct run run = { .value = value, .action = action };
    int rc = aarch64_el0_run(space->elf.root, space->elf.entry, STACK_TOP, action,
                            action == 3, handle_sync, &run);
    __asm__ volatile("mrs %0, ttbr0_el1\n\tmrs %1, ttbr1_el1"
                     : "=r"(after0), "=r"(after1));
    if (rc || before0 != after0 || before1 != after1) {
        log_err("M4.1: run rc=%d TTBR0 restored=%u TTBR1 preserved=%u\n",
                rc, before0 == after0, before1 == after1);
        return false;
    }
    if (!run.passed)
        log_err("M4: trap=%u esr=%lx far=%lx pc=%lx x0=%lx action=%lx\n",
                run.traps, run.esr, run.far, run.pc, run.x0, action);
    return run.passed;
}

/* Boot SP is an identity alias. Use its TTBR1 alias for the probe so
 * its EL1 stack and callback state are reachable under either root.
 * Restore the original stack and FP chain before returning to boot. */
__asm__(
    ".pushsection .text, \"ax\"\n"
    ".balign 4\n"
    ".global aarch64_el0_test_high_stack\n"
    ".type aarch64_el0_test_high_stack, %function\n"
    "aarch64_el0_test_high_stack:\n"
    "    mov x9, sp\n"
    "    orr x10, x9, #0xffff000000000000\n"
    "    mov sp, x10\n"
    "    stp x9, x30, [sp, #-16]!\n"
    "    blr x0\n"
    "    ldp x9, x30, [sp], #16\n"
    "    mov sp, x9\n"
    "    ret\n"
    ".size aarch64_el0_test_high_stack, . - aarch64_el0_test_high_stack\n"
    ".popsection\n");

static bool reject_bad_entry(void)
{
    size_t size = fixture_size();
    elf64_ehdr_t *bad = kmalloc(size);
    if (!bad)
        return false;
    __builtin_memcpy(bad, _binary_aarch64_el0_elf_start, size);
    bad->e_entry = DATA_VA; /* mapped by the ELF but not executable */
    uint64_t before = pmm_probe_4k_allocated_count();
    struct aarch64_user_elf elf = {0};
    int rc = aarch64_user_elf_load(bad, size, &elf);
    bool ok = rc == -ENOEXEC && !elf.root && !elf.page_count &&
              before == pmm_probe_4k_allocated_count();
    aarch64_user_elf_destroy(&elf);
    kfree(bad);
    if (!ok)
        log_err("M4.3: bad entry rejection rc=%d\n", rc);
    return ok;
}

/* Dirty the reusable pages first so missing BSS clearing cannot be
 * hidden by fresh QEMU RAM. Root, tables and ELF leaves fit in 8 pages. */
static bool dirty_free_pages(void)
{
    uint64_t pages[8] = {0};
    bool ok = true;
    for (unsigned i = 0; i < 8; i++) {
        pages[i] = alloc_4k_page();
        if (!pages[i]) {
            ok = false;
            break;
        }
        __builtin_memset((void *)Phy_To_Virt(pages[i]), 0xa5, PAGE_4K_SIZE);
    }
    for (unsigned i = 0; i < 8; i++)
        free_4k_page(pages[i]);
    return ok;
}

static int test_aarch64_el0_on_high_stack(void)
{
    if (!reject_bad_entry() || !dirty_free_pages())
        return -1;
    uint64_t baseline = pmm_probe_4k_allocated_count();
    for (unsigned round = 0; round < 8; round++) {
        struct space a = {0}, b = {0};
        bool ok = setup(&a, 0xa1 + round) && setup(&b, 0xb1 + round);
        if (ok)
            ok = exercise(&a, 0xa1 + round, 0) &&
                 exercise(&b, 0xb1 + round, 0) &&
                 exercise(&a, 0xa1 + round, 1) &&
                 exercise(&b, 0xb1 + round, (uintptr_t)&b) &&
                 (round != 0 || exercise(&a, 0xa1 + round, 2));
        teardown(&a);
        teardown(&b);
        bool freed = pmm_probe_4k_allocated_count() == baseline;
        if (!ok || !freed) {
            log_err("M4: round=%u execution=%u reclaim=%u\n", round, ok, freed);
            return -1;
        }
    }
    return 0;
}
extern int aarch64_el0_test_high_stack(int (*test)(void));

int test_aarch64_el0(void)
{
    return aarch64_el0_test_high_stack(test_aarch64_el0_on_high_stack);
}

struct pair_run {
    struct space spaces[2];
    unsigned step;
    unsigned switches;
    bool ready;
    bool timed_out;
    uint64_t start;
    uint64_t esr;
};

static uint64_t pair_stack(unsigned index)
{
    return STACK_TOP - 32 * index;
}

static bool handle_pair_sync(pt_regs_t *regs, uint64_t esr, uint64_t far,
                             void *arg)
{
    (void)far;
    struct pair_run *pair = arg;
    unsigned index = pair->step % 2;
    bool yield = pair->step < 2;
    pair->esr = esr;
    if (pair->step >= 4 || esr >> 26 != 0x15 ||
        (esr & 0x1ffffff) != (yield ? 5u : 1u) ||
        regs->x0 != 4 + index ||
        regs->sp_el0 != pair_stack(index) - (yield ? 16 : 0) ||
        aarch64_el0_active_root() != pair->spaces[index].elf.root)
        return false;
    pair->step++;
    return pair->step < 4 && aarch64_el0_swap(regs);
}

static bool handle_preempt_sync(pt_regs_t *regs, uint64_t esr, uint64_t far,
                                void *arg)
{
    (void)far;
    struct pair_run *pair = arg;
    unsigned index = pair->switches % 2;
    bool reported = pair->step > index;
    pair->esr = esr;
    if (esr >> 26 != 0x15 || regs->x0 != 6 + index ||
        aarch64_el0_active_root() != pair->spaces[index].elf.root)
        return false;
    if ((esr & 0x1ffffff) == 6 &&
        regs->sp_el0 == pair_stack(index) - (reported ? 0 : 16)) {
        if (arch_cycle_counter() - pair->start >= cntfrq_el0() / 2) {
            pair->timed_out = true;
            return false;
        }
        pair->ready = true;
        regs->x1 = !reported && pair->switches >= 2;
        return true;
    }
    if ((esr & 0x1ffffff) == 1 && regs->sp_el0 == pair_stack(index) &&
        pair->switches == 2 + index && pair->step == index) {
        pair->step++;
        return index == 0; /* A waits for the final timer handoff. */
    }
    return false;
}

static void handle_preempt_tick(pt_regs_t *regs, void *arg)
{
    struct pair_run *pair = arg;
    /* Do not leave until the current context has set up its canaries.
     * At the third handoff A must already have reported its checks. */
    if (pair->ready && pair->switches < 3 &&
        (pair->switches != 2 || pair->step == 1) && aarch64_el0_swap(regs)) {
        pair->switches++;
        pair->ready = false;
    }
}

static int test_pair(bool preempt)
{
    uint64_t baseline = pmm_probe_4k_allocated_count();
    struct pair_run pair = {0};
    struct space *a = &pair.spaces[0], *b = &pair.spaces[1];
    int rc = -1;
    uint64_t before0, before1, after0, after1;
    __asm__ volatile("mrs %0, ttbr0_el1\n\tmrs %1, ttbr1_el1"
                     : "=r"(before0), "=r"(before1));
    if (setup(a, 0xa1) && setup(b, 0xb1)) {
        unsigned action = preempt ? 6 : 4;
        const struct aarch64_el0_start starts[] = {
            { a->elf.root, a->elf.entry, pair_stack(0), action },
            { b->elf.root, b->elf.entry, pair_stack(1), action + 1 },
        };
        if (preempt) {
            pair.start = arch_cycle_counter();
            rc = aarch64_el0_run_pair(starts, handle_preempt_sync,
                                     handle_preempt_tick, &pair);
        } else {
            rc = aarch64_el0_run_pair(starts, handle_pair_sync, NULL, &pair);
        }
    }
    __asm__ volatile("mrs %0, ttbr0_el1\n\tmrs %1, ttbr1_el1"
                     : "=r"(after0), "=r"(after1));
    teardown(a);
    teardown(b);
    bool restored = before0 == after0 && before1 == after1;
    bool freed = baseline == pmm_probe_4k_allocated_count();
    bool complete = preempt ? pair.switches == 3 && pair.step == 2 : pair.step == 4;
    if (rc || !complete || !restored || !freed) {
        log_err("M4.%u: run=%d step=%u switches=%u timeout=%u esr=%lx restored=%u reclaim=%u\n",
                preempt ? 6u : 5u, rc, pair.step, pair.switches, pair.timed_out,
                pair.esr, restored, freed);
        return -1;
    }
    return 0;
}

static int test_switch_on_high_stack(void)
{
    return test_pair(false);
}

int test_aarch64_el0_switch(void)
{
    return aarch64_el0_test_high_stack(test_switch_on_high_stack);
}

static int test_preempt_on_high_stack(void)
{
    return test_pair(true);
}

int test_aarch64_el0_preempt(void)
{
    if (dtb_cpu_count() != 1) {
        log_err("M4.6: use SMP=1 for the EL0 preemption probe\n");
        return -1;
    }
    return aarch64_el0_test_high_stack(test_preempt_on_high_stack);
}

static int test_irq_on_high_stack(void)
{
    uint64_t baseline = pmm_probe_4k_allocated_count();
    struct space space = {0};
    bool ok = setup(&space, 0xa1) && exercise(&space, 0xa1, 3);
    teardown(&space);
    return ok && pmm_probe_4k_allocated_count() == baseline ? 0 : -1;
}

int test_aarch64_el0_irq(void)
{
    if (dtb_cpu_count() != 1) {
        log_err("M4.4: use SMP=1 for the EL0 IRQ probe\n");
        return -1;
    }
    return aarch64_el0_test_high_stack(test_irq_on_high_stack);
}
#endif
