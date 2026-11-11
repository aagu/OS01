#if AARCH64_EL0_TEST

#include <arch/aarch64/el0.h>
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
    /* PL011 still relies on the boot TTBR0. Report only after restore. */
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
                            handle_sync, &run);
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

__attribute__((used))
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
#endif
