#if AARCH64_EL0_TEST

#include <arch/aarch64/el0.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/vmm_backend.h>
#include <arch/mmu.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <stddef.h>

#define CODE_VA 0x100000
#define STACK_TOP (CODE_VA + 3 * PAGE_4K_SIZE)
#define STR1(x) #x
#define STR(x) STR1(x)

/* Relocation-free EL0 code: load the root's data, use the EL0 stack,
 * and continue after the first SVC. x0 on entry selects the final action:
 * 0 exits, 1 writes to the read-only code page, otherwise read from x0. */
extern const char el0_blob[], el0_blob_end[];
__asm__(
    ".pushsection .rodata, \"a\"\n"
    ".balign 4\n"
    "el0_blob:\n"
    "    mov x12, x0\n"
    "    mov x9, #" STR(CODE_VA) "\n"
    "    ldr x0, [x9, #4096]\n"
    "    str x0, [sp, #-16]!\n"
    "    mov x30, #0x1234\n"
    "    svc #0\n"
    "    ldr x10, [sp], #16\n"
    "    add x10, x10, #1\n"
    "    cmp x0, x10\n"
    "    b.ne 2f\n"
    "    mov x10, #0x1234\n"
    "    cmp x30, x10\n"
    "    b.ne 2f\n"
    "    cbz x12, 1f\n"
    "    cmp x12, #1\n"
    "    b.ne 3f\n"
    "    str x0, [x9]\n"
    "    b 2f\n"
    "3:  ldr x0, [x12]\n"
    "    b 2f\n"
    "1:  svc #1\n"
    "2:  svc #2\n"
    "    b 2b\n"
    "el0_blob_end:\n"
    ".popsection\n");

struct run {
    uint64_t value;
    uint64_t action;
    unsigned traps;
    bool passed;
    uint64_t esr, far, pc, x0;
};

static bool handle_sync(pt_regs_t *regs, uint64_t esr, uint64_t far, void *arg)
{
    struct run *run = arg;
    const uint64_t ec = esr >> 26;
    const uint64_t iss = esr & 0x1ffffff;

    run->traps++;
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
    /* PL011 still relies on the boot TTBR0. Report only after restore. */
    run->esr = esr;
    run->far = far;
    run->pc = regs->elr_el1;
    run->x0 = regs->x0;
    return false;
}

static void copy_code(uint64_t pa)
{
    char *code = (char *)Phy_To_Virt(pa);
    size_t size = (uintptr_t)el0_blob_end - (uintptr_t)el0_blob;
    __builtin_memcpy(code, el0_blob, size);
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = 4UL << ((ctr >> 16) & 0xf);
    for (size_t i = 0; i < size; i += line)
        __asm__ volatile("dc cvau, %0" :: "r"(code + i) : "memory");
    /* The instruction fetch uses a different VA from the copy. */
    __asm__ volatile("dsb ish\n\tic iallu\n\tdsb ish\n\tisb" ::: "memory");
}

struct space {
    uint64_t *root;
    uint64_t leaf[3];
};

static bool setup(struct space *space, uint64_t value)
{
    space->root = aarch64_el0_root_create();
    if (!space->root)
        return false;
    for (unsigned i = 0; i < 3; i++) {
        uint64_t pa = alloc_4k_page();
        if (!pa)
            return false;
        space->leaf[i] = pa;
        __builtin_memset((void *)Phy_To_Virt(pa), 0, PAGE_4K_SIZE);
        uint32_t perm = i ? AARCH64_PT_USER_RW
                          : AARCH64_PT_USER_RO | AARCH64_PT_EXEC;
        int rc = aarch64_el0_map(space->root, CODE_VA + i * PAGE_4K_SIZE, pa, perm);
        if (rc) {
            log_err("M4.1: map slot=%u rc=%d\n", i, rc);
            return false;
        }
    }
    copy_code(space->leaf[0]);
    *(uint64_t *)Phy_To_Virt(space->leaf[1]) = value;
    return true;
}

static bool teardown(struct space *space)
{
    /* All three VAs share one path to L3. Record every allocated table
     * before teardown, including intermediates from a partial setup. */
    uint64_t tables[4] = {0};
    uint64_t *table = space->root;
    const uint64_t bits = AARCH64_PT_DESC_VALID | AARCH64_PT_DESC_TABLE;
    for (unsigned i = 0; table && i < 4; i++) {
        tables[i] = (uintptr_t)table - ARCH_PAGE_OFFSET;
        if (i == 3)
            break;
        unsigned index = (CODE_VA >> (39 - 9 * i)) & 0x1ff;
        uint64_t desc = table[index];
        table = (desc & bits) == bits
            ? (uint64_t *)Phy_To_Virt(desc & AARCH64_TTBR_BASE_MASK) : NULL;
    }
    aarch64_el0_root_destroy(space->root);
    bool ok = true;
    for (unsigned i = 0; i < 4; i++)
        if (tables[i] && pmm_4k_page_allocated(tables[i]))
            ok = false;
    for (unsigned i = 0; i < 3; i++) {
        uint64_t pa = space->leaf[i];
        if (pa) {
            if (!pmm_4k_page_allocated(pa))
                ok = false;
            free_4k_page(pa);
            if (pmm_4k_page_allocated(pa))
                ok = false;
        }
    }
    return ok;
}

static bool exercise(struct space *space, uint64_t value, uint64_t action)
{
    uint64_t before0, before1, after0, after1;
    __asm__ volatile("mrs %0, ttbr0_el1\n\tmrs %1, ttbr1_el1"
                     : "=r"(before0), "=r"(before1));
    struct run run = { .value = value, .action = action };
    int rc = aarch64_el0_run(space->root, CODE_VA, STACK_TOP, action,
                            handle_sync, &run);
    __asm__ volatile("mrs %0, ttbr0_el1\n\tmrs %1, ttbr1_el1"
                     : "=r"(after0), "=r"(after1));
    if (rc || before0 != after0 || before1 != after1) {
        log_err("M4.1: run rc=%d TTBR0 restored=%u TTBR1 preserved=%u\n",
                rc, before0 == after0, before1 == after1);
        return false;
    }
    if (!run.passed)
        log_err("M4.1: trap=%u esr=%lx far=%lx pc=%lx x0=%lx action=%lx\n",
                run.traps, run.esr, run.far, run.pc, run.x0, action);
    return run.passed;
}

/* Boot SP is an identity alias. Use its TTBR1 alias for the probe so
 * its EL1 stack and callback state are reachable under either root.
 * Restore the original stack and FP chain before returning to boot. */
__asm__(
    ".pushsection .text, \"ax\"\n"
    ".balign 4\n"
    ".global test_aarch64_el0\n"
    ".type test_aarch64_el0, %function\n"
    "test_aarch64_el0:\n"
    "    mov x9, sp\n"
    "    orr x10, x9, #0xffff000000000000\n"
    "    mov sp, x10\n"
    "    stp x9, x30, [sp, #-16]!\n"
    "    bl test_aarch64_el0_on_high_stack\n"
    "    ldp x9, x30, [sp], #16\n"
    "    mov sp, x9\n"
    "    ret\n"
    ".size test_aarch64_el0, . - test_aarch64_el0\n"
    ".popsection\n");

__attribute__((used))
static int test_aarch64_el0_on_high_stack(void)
{
    if ((uintptr_t)el0_blob_end - (uintptr_t)el0_blob > PAGE_4K_SIZE)
        return -1;
    for (unsigned round = 0; round < 8; round++) {
        struct space a = {0}, b = {0};
        bool ok = setup(&a, 0xa1 + round) && setup(&b, 0xb1 + round);
        if (ok)
            ok = exercise(&a, 0xa1 + round, 0) &&
                 exercise(&b, 0xb1 + round, 0) &&
                 exercise(&a, 0xa1 + round, 1) &&
                 exercise(&b, 0xb1 + round, (uintptr_t)&b);
        bool a_freed = teardown(&a);
        bool b_freed = teardown(&b);
        if (!ok || !a_freed || !b_freed) {
            log_err("M4.1: round=%u execution=%u reclaim=%u/%u\n",
                    round, ok, a_freed, b_freed);
            return -1;
        }
    }
    return 0;
}
#endif
