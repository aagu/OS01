#if AARCH64_EL0_TEST

#include <arch/aarch64/el0.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/timer.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/vmm_gate.h>
#include <arch/mmu.h>
#include <stddef.h>
#include <percpu/percpu.h>

extern void aarch64_el0_enter(uint64_t pc, uint64_t sp, uint64_t x0, bool irq);

struct el0_peer {
    uint64_t *root;
    pt_regs_t regs;
};

/* Only live on the BSP before scheduler handoff. */
static aarch64_el0_handler active_handler;
static aarch64_el0_timer_handler active_timer;
static void *active_arg;
static uint64_t *active_root;
static struct el0_peer *parked;

uint64_t *aarch64_el0_active_root(void)
{
    return active_root;
}

static void switch_ttbr0(uint64_t raw)
{
    /* ASID 0 throughout this probe. Invalidate all local EL1&0 entries
     * (including global ones) before using the replacement root. */
    __asm__ volatile("dsb ishst\n\t"
                     "msr ttbr0_el1, %0\n\tisb\n\t"
                     "tlbi vmalle1\n\tdsb ish\n\tisb"
                     :: "r"(raw) : "memory");
}

static bool probe_stage_valid(bool irq)
{
    if (!irq)
        return aarch64_vmm_pre_smp();
    return !aarch64_vmm_pre_smp() && dtb_cpu_count() == 1 &&
           ipi_ready_count_get() == 1 && cpu_id() == 0 &&
           !__atomic_load_n(&percpu_data[0].scheduler_ok, __ATOMIC_ACQUIRE) &&
           aarch64_timer_local_running();
}

static int run(const struct aarch64_el0_start *start, struct el0_peer *peer,
               bool irq, aarch64_el0_handler handler,
               aarch64_el0_timer_handler timer, void *arg)
{
    uintptr_t kernel_sp;
    __asm__ volatile("mov %0, sp" : "=r"(kernel_sp));
    if (!start->root || !handler || active_handler || !probe_stage_valid(irq) ||
        kernel_sp < ARCH_PAGE_OFFSET)
        return -1;
    uint64_t root_pa = (uintptr_t)start->root - ARCH_PAGE_OFFSET;
    if (aarch64_pt_root_is_published(&root_pa))
        return -1;
    uint64_t peer_pa = 0;
    if (peer) {
        if (!peer->root || peer->root == start->root)
            return -1;
        peer_pa = (uintptr_t)peer->root - ARCH_PAGE_OFFSET;
        if (!peer_pa || aarch64_pt_root_is_published(&peer_pa))
            return -1;
    }
    uint64_t boot_ttbr0, saved_sp, daif;
    __asm__ volatile("mrs %0, ttbr0_el1\n\t"
                     "mrs %1, sp_el0\n\t"
                     "mrs %2, daif\n\tmsr daifset, #0xf"
                     : "=r"(boot_ttbr0), "=r"(saved_sp), "=r"(daif) :: "memory");
    if (!aarch64_pt_root_publish(root_pa))
        goto restore_daif;
    if (peer_pa && !aarch64_pt_root_publish(peer_pa)) {
        aarch64_pt_root_unpublish_local(root_pa);
        goto restore_daif;
    }

    active_arg = arg;
    active_handler = handler;
    active_timer = timer;
    parked = peer;
    switch_ttbr0(root_pa);
    active_root = start->root;
    aarch64_el0_enter(start->pc, start->sp, start->x0, irq);
    active_root = NULL;
    parked = NULL;
    switch_ttbr0(boot_ttbr0);
    if (peer_pa)
        aarch64_pt_root_unpublish_local(peer_pa);
    aarch64_pt_root_unpublish_local(root_pa);
    active_handler = NULL;
    active_timer = NULL;
    active_arg = NULL;
    __asm__ volatile("msr sp_el0, %0\n\tmsr daif, %1"
                     :: "r"(saved_sp), "r"(daif) : "memory");
    return 0;

restore_daif:
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
    return -1;
}

int aarch64_el0_run(uint64_t *root, uint64_t pc, uint64_t sp, uint64_t x0,
                   bool irq, aarch64_el0_handler handler, void *arg)
{
    const struct aarch64_el0_start start = { root, pc, sp, x0 };
    return run(&start, NULL, irq, handler, NULL, arg);
}

int aarch64_el0_run_pair(const struct aarch64_el0_start starts[2],
                        aarch64_el0_handler handler,
                        aarch64_el0_timer_handler timer, void *arg)
{
    struct el0_peer peer = {
        .root = starts[1].root,
        .regs = {
            .elr_el1 = starts[1].pc,
            .sp_el0 = starts[1].sp,
            .x0 = starts[1].x0,
            .spsr_el1 = AARCH64_EL0_PROBE_PSTATE,
        },
    };
    if (timer)
        peer.regs.spsr_el1 &= ~AARCH64_EL0_PROBE_IRQ_MASK;
    return run(&starts[0], &peer, timer != NULL, handler, timer, arg);
}

bool aarch64_el0_swap(pt_regs_t *regs)
{
    if (!parked)
        return false;
    uint64_t *next_root = parked->root;
    pt_regs_t next = parked->regs;
    parked->root = active_root;
    parked->regs = *regs;

    switch_ttbr0((uintptr_t)next_root - ARCH_PAGE_OFFSET);
    active_root = next_root;
    *regs = next;
    return true;
}

void aarch64_el0_timer_return(pt_regs_t *regs)
{
    if (active_timer)
        active_timer(regs, active_arg);
}

/* Called only by the lower-AArch64 synchronous vector. */
bool aarch64_el0_sync(pt_regs_t *regs, uint64_t esr, uint64_t far)
{
    if (!active_handler || (regs->spsr_el1 & 0xf) != 0) {
        vmm_gate_violation("unexpected EL0 sync");
        return false;
    }
    return active_handler(regs, esr, far, active_arg);
}
#endif
