#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <arch/cpu.h>
#include <arch/early_print.h>
#include <arch/aarch64/mmu.h>
#include <arch/aarch64/boot_direct_map.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/unwind.h>
#include <memory/pmm.h>
#include <memory/memory.h>
#include <sched/task.h>

#if defined(AARCH64_EXCEPTION_TRACE_READ)

static _Noreturn void aarch64_trace_read_fail(const char *reason)
{
    arch_early_puts("[exception-trace] FAIL arch=aarch64 case=read reason=");
    arch_early_puts(reason);
    arch_early_putc('\n');
    for (;;) {
        arch_cpu_halt();
    }
}

void aarch64_exception_trace_read_run(void)
{
    arch_early_puts("[exception-trace] armed arch=aarch64 case=read variant=exception-trace-read\n");

    uint64_t ttbr_raw = aarch64_read_ttbr1();
    if ((ttbr_raw & ~AARCH64_TTBR_ALLOWED_MASK) != 0) {
        aarch64_trace_read_fail("ttbr_invalid");
    }
    uint64_t ttbr_pa = ttbr_raw & AARCH64_TTBR_BASE_MASK;
    uint64_t *root = (uint64_t *)(uintptr_t)(ttbr_pa + ARCH_PAGE_OFFSET);

    const uint64_t va = AARCH64_PT_SELFTEST_VA;
    uint64_t qpa = 0;
    uint32_t qperm = 0;
    if (aarch64_pt_query_4k(root, va, &qpa, &qperm) != AARCH64_PT_ENOENT) {
        aarch64_trace_read_fail("page1_not_empty");
    }
    if (aarch64_pt_query_4k(root, va + PAGE_4K_SIZE, &qpa, &qperm) != AARCH64_PT_ENOENT) {
        aarch64_trace_read_fail("page2_not_empty");
    }

    struct Page *page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!page) {
        aarch64_trace_read_fail("alloc_page");
    }
    uint64_t pa = page->phy_address;
    uint8_t *kva = (uint8_t *)Phy_To_Virt(pa);
    memset(kva, 0, PAGE_4K_SIZE);

    const uint64_t word0 = UINT64_C(0x1122334455667788);
    const uint64_t word1 = UINT64_C(0x99aabbccddeeff00);
    volatile uint64_t *tail = (volatile uint64_t *)(kva + PAGE_4K_SIZE - 16);
    tail[0] = word0;
    tail[1] = word1;

    if (aarch64_pt_map_4k(root, va, pa, AARCH64_PT_KERNEL_RW) != AARCH64_PT_OK) {
        aarch64_trace_read_fail("map_page");
    }
    __asm__ __volatile__("dsb ishst\n\ttlbi vmalle1\n\tdsb ish\n\tisb" ::: "memory");

    if (aarch64_pt_query_4k(root, va + PAGE_4K_SIZE, &qpa, &qperm) != AARCH64_PT_ENOENT) {
        aarch64_trace_read_fail("page2_mapped");
    }

    struct unwind_frame frame;
    frame.next_fp = 0;
    frame.return_pc = 0;
    if (!aarch64_unwind_load_pair((uintptr_t)(va + PAGE_4K_SIZE - 16), &frame)) {
        aarch64_trace_read_fail("same_page_failed");
    }
    if (frame.next_fp != word0 || frame.return_pc != word1) {
        aarch64_trace_read_fail("same_page_mismatch");
    }

    const uintptr_t s0 = UINT64_C(0xdeadbeefcafebabe);
    const uintptr_t s1 = UINT64_C(0xfeedface12345678);
    frame.next_fp = s0;
    frame.return_pc = s1;
    bool cross_ok = aarch64_unwind_load_pair((uintptr_t)(va + PAGE_4K_SIZE - 8), &frame);
    if (cross_ok) {
        aarch64_trace_read_fail("cross_page_unexpected_ok");
    }
    if (frame.next_fp != s0 || frame.return_pc != s1) {
        aarch64_trace_read_fail("cross_page_clobbered_sentinel");
    }

    arch_early_puts("[exception-trace] PASS arch=aarch64 case=read\n");
    for (;;) {
        arch_cpu_halt();
    }
}

#endif /* AARCH64_EXCEPTION_TRACE_READ */

#if defined(AARCH64_EXCEPTION_TRACE_BAD_FP)

extern void aarch64_trace_bad_fp_trigger(void);

void aarch64_exception_trace_bad_fp_run(void)
{
    arch_early_puts("[exception-trace] armed arch=aarch64 case=bad-fp variant=exception-trace-bad-fp\n");
    aarch64_trace_bad_fp_trigger();
    arch_early_puts("[exception-trace] FAIL arch=aarch64 case=bad-fp reason=returned\n");
    for (;;) {
        arch_cpu_halt();
    }
}

#endif /* AARCH64_EXCEPTION_TRACE_BAD_FP */

#if defined(AARCH64_EXCEPTION_TRACE_TASK)

__attribute__((noinline))
static void aarch64_trace_task_barrier(void)
{
    __asm__ __volatile__("" ::: "memory");
}

__attribute__((noinline))
static void aarch64_trace_task_inner(void)
{
    aarch64_trace_task_barrier();
    __asm__ __volatile__(
        ".globl aarch64_trace_task_pc\n\t"
        "aarch64_trace_task_pc:\n\t"
        "brk #0\n\t"
        ::: "memory", "x30");
    __asm__ __volatile__("" ::: "memory");
}

__attribute__((noinline))
static void aarch64_trace_task_middle(void)
{
    aarch64_trace_task_inner();
    __asm__ __volatile__("" ::: "memory");
}

__attribute__((noinline))
static void aarch64_trace_task_outer(void)
{
    aarch64_trace_task_middle();
    __asm__ __volatile__("" ::: "memory");
}

static uint64_t aarch64_trace_task_thread(uint64_t arg)
{
    (void)arg;
    task_t *cur = current;
    uintptr_t sp_now = 0;
    __asm__ __volatile__("mov %0, sp" : "=r"(sp_now));

    if (!cur || cur == &init_task_union.task || !cur->thread ||
        sp_now < cur->thread->usable_low || sp_now >= cur->thread->stack_top) {
        arch_early_puts("[exception-trace] FAIL arch=aarch64 case=task reason=invalid_task_sp\n");
        for (;;) {
            arch_cpu_halt();
        }
    }

    arch_early_puts("[exception-trace] armed arch=aarch64 case=task variant=exception-trace-task\n");
    aarch64_trace_task_outer();
    arch_early_puts("[exception-trace] FAIL arch=aarch64 case=task reason=returned\n");
    for (;;) {
        arch_cpu_halt();
    }
}

void aarch64_exception_trace_task_start(void)
{
    int pid = kernel_thread_on_cpu(aarch64_trace_task_thread, 0, 0);
    if (pid < 0) {
        arch_early_puts("[exception-trace] FAIL arch=aarch64 case=task reason=spawn_failed\n");
        for (;;) {
            arch_cpu_halt();
        }
    }
}

#endif /* AARCH64_EXCEPTION_TRACE_TASK */
