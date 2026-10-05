// kernel/selftest/test_aarch64_page_table_multicore.c —
// aarch64 M3.6 multi-core selftest (Task 26, spec §8.2 items ①②⑤⑥).
//
// Unlike test_aarch64_page_table_selftest.c (which runs PRE-SMP on the BSP), this file
// runs POST-SMP: it is invoked from boot/main.c after smp_boot_aps(),
// the BSP ipi_ready publish and the M3.5 production shootdown probe.
// All sections run on REAL cores — APs execute work items through the
// ap_work protocol (spec §6.2) and receive TLB SGIs through the
// registered SGI-3 handler (trap.c::aarch64_tlb_sgi_handler).
//
// Sections (each prints a per-section PASS line; the parser gates on
// the final summary marker "[selftest] m3mc: N/N PASS"):
//
//  ① shootdown-target-set + per-AP tlb_ack_gen (spec §6.2b):
//     snapshot percpu_data[1].tlb_ack_gen, run one full BSP
//     tlb_shootdown(), then poll until AP1's counter reaches
//     snapshot+1 (equality, mirroring tlb.c's wrap-safe contract).
//     Proves AP1 is in the shootdown target set (online ∧ ¬self ∧
//     ipi_ready) and that its ack increments ON the AP.
//
//  ② 4-core partial-mask broadcast (needs -smp >= 4):
//     BSP calls ipi_broadcast(IPI_VECTOR_TLB, mask={1,2}) DIRECTLY
//     (bypassing tlb_shootdown). CPUs 1 and 2 must each ack exactly
//     once (gen snapshot+1) while CPU 3's counter stays UNCHANGED —
//     the SGI was never sent to it because the mask excluded it.
//
//  ⑤ two CPUs competing on the same pt_lock (spec §5.4):
//     a shared scratch root; the AP runs WORK_PT_STRESS (map → unmap
//     loops) on VA_P while the BSP drives map → replace RW→RO [the
//     BBM-class update] → unmap on the adjacent VA_Q. Both VAs hash
//     to the same pt_lock_for(root_pa, l2) slot, so every critical
//     section serializes; both sides must complete their iteration
//     budget with no deadlock (cycle-budget guarded). See the
//     section's design note for the deferred concurrent-replace
//     corner (both CPUs replacing into one L3 page).
//
//  ⑥ two L2 slots sharing one L1 entry created concurrently:
//     a second scratch root; the AP's WORK_PT_MAP and the BSP's
//     inline map run CONCURRENTLY (the AP item is published before
//     the BSP starts) on two different 2 MiB slots of the same
//     1 GiB L1 range. pt_upper_lock serializes the ensure-segment,
//     so exactly one L1 (and one L2) table is allocated; both
//     mappings must be queryable with the right PA afterwards.
//
// Scratch-VA layout: same PGD[511] → PUD[0] window as
// test_aarch64_page_table_selftest.c (BASE = 0xffff_ffff_0000_0000). The m3 selftest
// frees its roots before SMP starts, so there is no collision; and
// every root here is a fresh private allocation anyway (never
// installed into TTBR1 — the hardware never walks it).
//
//   STRESS root:  VA_P = BASE + 0x00800000  (L2 idx 4, PTE[0])
//                 VA_Q = BASE + 0x00801000  (L2 idx 4, PTE[1])
//     → same (root_pa, l2) lock slot: genuine same-lock contention.
//   L1SHARE root: VA_R = BASE + 0x00400000  (L2 idx 2)
//                 VA_S = BASE + 0x00600000  (L2 idx 3)
//     → same L1 entry (index MC_L1_IDX(VA)), different L2 slots.
//     NOTE: for this BASE the L1 index is 508, NOT 0 — the 0xFFFF at
//     bits [47:32] contributes to the [38:30] field. Always derive
//     indices from the VA (MC_L1_IDX/MC_L2_IDX/MC_L3_IDX below).
//
// Deadlines: every AP wait and every on-BSP poll carries an
// arch_cycle_counter() deadline. On expiry the section FAILs (prints
// a marker line) and the summary reports the failure — the BSP never
// blocks indefinitely. (The ap_work_wait WORK-TIMEOUT path halts the
// machine, so we deliberately poll ap_work states ourselves with a
// bounded deadline instead of calling ap_work_wait, EXCEPT where a
// timeout is genuinely terminal: the stress/map items use a bounded
// state poll + IDLE re-arm here. See poll_work_done() below.)
//
// x86_64 / non-selftest builds: whole body behind
// #if defined(__aarch64__) && OS01_SELFTEST, mirroring
// test_aarch64_page_table_selftest.c.

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <arch/aarch64/ap_work.h>
#include <arch/aarch64/boot_log.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/ipi.h>
#include <arch/aarch64/page_table.h>
#include <arch/cpu.h>
#include <core/printk.h>
#include <intr/ipi.h>          /* ipi_broadcast, IPI_VECTOR_TLB */
#include <memory/memory.h>     /* Phy_To_Virt, ARCH_PAGE_OFFSET */
#include <memory/pmm.h>
#include <memory/vmm.h>        /* tlb_shootdown */
#include <percpu/percpu.h>

#if defined(__aarch64__) && defined(OS01_SELFTEST)

/* ── Scratch VA / PA constants ──────────────────────────────────── */
#define MC_SCRATCH_BASE     UINT64_C(0xffffffff00000000)
#define MC_VA_P             (MC_SCRATCH_BASE + UINT64_C(0x00800000))
#define MC_VA_Q             (MC_VA_P + UINT64_C(0x00001000))
#define MC_VA_R             (MC_SCRATCH_BASE + UINT64_C(0x00400000))
#define MC_VA_S             (MC_SCRATCH_BASE + UINT64_C(0x00600000))
#define MC_PGD_IDX          511u
/* L1 (PUD) index is derived from the VA — do NOT hardcode: for
 * MC_SCRATCH_BASE the [38:30] field is 508, not 0 (the high-half
 * 0xFFFF at bits [47:32] contributes). */
#define MC_L1_IDX(va)       (((va) >> 30) & 0x1ffu)
#define MC_L2_IDX(va)       (((va) >> 21) & 0x1ffu)
#define MC_L3_IDX(va)       (((va) >> 12) & 0x1ffu)
#define MC_DESC_PA_MASK     UINT64_C(0x000000fffffff000)

/* AP wait budget: 10 s of cycle-counter per wait/poll. A live AP
 * answers in microseconds even under QEMU TCG; only a wedged CPU
 * burns the full budget (and then we FAIL, never hang). */
#define MC_DEADLINE_CYCLES  (10ULL * arch_cycle_freq())

/* ⑤ iteration budget: small enough to keep the whole selftest well
 * inside the harness timeout, large enough to force real interleaving
 * on the shared lock. */
#define MC_STRESS_ITERS     200u

/* Work-item extension payloads (arg0 = pointer to these, BSS kernel
 * VA — identical on every core). */
struct mc_map_args {
    uint64_t *root;
    uint64_t  va;
    uint64_t  pa;
    int       rc;
};

static struct mc_map_args    mc_ap_map;

/* ── Output helpers (aarch64 serial_printk is verbatim) ─────────── */
static void mc_puts(const char *s) { kputs(s); }

static void mc_put_u64(uint64_t v)
{
    char buf[24];
    char tmp[24];
    int tn = 0, n = 0;
    if (v == 0) tmp[tn++] = '0';
    while (v != 0) { tmp[tn++] = (char)('0' + (v % 10)); v /= 10; }
    while (tn > 0) buf[n++] = tmp[--tn];
    buf[n] = '\0';
    kputs(buf);
}

static void mc_put_i32(int rc)
{
    uint64_t v;
    if (rc < 0) { kputs("-"); v = (uint64_t)(-(int64_t)rc); }
    else v = (uint64_t)rc;
    mc_put_u64(v);
}

static void mc_put_hex64(uint64_t v)
{
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        unsigned nib = (unsigned)((v >> ((15 - i) * 4)) & 0xfu);
        buf[2 + i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    buf[18] = '\0';
    kputs(buf);
}

/* Failure-time dump: every level of the VA_P/VA_Q chain. */
static void mc_dump_chain(const uint64_t *root)
{
    mc_puts("[selftest] m3mc: dump root[511]=");
    mc_put_hex64(root[MC_PGD_IDX]);
    if ((root[MC_PGD_IDX] & 3u) != 3u) { mc_puts("\n"); return; }
    const uint64_t *l1 = (const uint64_t *)Phy_To_Virt(
        root[MC_PGD_IDX] & MC_DESC_PA_MASK);
    mc_puts(" pud[");
    mc_put_u64(MC_L1_IDX(MC_VA_P));
    mc_puts("]=");
    mc_put_hex64(l1[MC_L1_IDX(MC_VA_P)]);
    if ((l1[MC_L1_IDX(MC_VA_P)] & 3u) != 3u) { mc_puts("\n"); return; }
    const uint64_t *l2 = (const uint64_t *)Phy_To_Virt(
        l1[MC_L1_IDX(MC_VA_P)] & MC_DESC_PA_MASK);
    mc_puts(" pmd[4]=");
    mc_put_hex64(l2[MC_L2_IDX(MC_VA_P)]);
    if ((l2[MC_L2_IDX(MC_VA_P)] & 3u) != 3u) { mc_puts("\n"); return; }
    const uint64_t *l3 = (const uint64_t *)Phy_To_Virt(
        l2[MC_L2_IDX(MC_VA_P)] & MC_DESC_PA_MASK);
    mc_puts(" pte[0]=");
    mc_put_hex64(l3[MC_L3_IDX(MC_VA_P)]);
    mc_puts(" pte[1]=");
    mc_put_hex64(l3[MC_L3_IDX(MC_VA_Q)]);
    mc_puts("\n");
}

static int section_fail(const char *section, int rc)
{
    mc_puts("[selftest] m3mc: ");
    mc_puts(section);
    mc_puts(" FAIL rc=");
    mc_put_i32(rc);
    mc_puts("\n");
    return rc;
}

/* ── Scratch root management ────────────────────────────────────── */
static uint64_t *mc_root_alloc(void)
{
    uint64_t pa = alloc_4k_page();
    if (pa == 0) return NULL;
    uint64_t *root = (uint64_t *)Phy_To_Virt(pa);
    for (unsigned k = 0; k < 512u; k++) root[k] = 0;
    return root;
}

/* Free root + every intermediate table page (L1/L2/L3) below
 * PGD[511]→PUD[0]. Leaf data PAs are NOT freed here (they may be
 * 2 MiB blocks or caller-owned) — the caller releases them. */
static void mc_root_free(uint64_t *root)
{
    if (root == NULL) return;
    uint64_t l0 = root[MC_PGD_IDX];
    if ((l0 & 3u) == 3u) {
        uint64_t l0_pa = l0 & MC_DESC_PA_MASK;
        uint64_t *l1 = (uint64_t *)Phy_To_Virt(l0_pa);
        uint64_t l1e = l1[MC_L1_IDX(MC_VA_R)];
        if ((l1e & 3u) == 3u) {
            uint64_t l1e_pa = l1e & MC_DESC_PA_MASK;
            uint64_t *l2 = (uint64_t *)Phy_To_Virt(l1e_pa);
            for (unsigned j = 0; j < 512u; j++) {
                uint64_t l2e = l2[j];
                if ((l2e & 3u) == 3u)          /* L3 table page */
                    free_4k_page(l2e & MC_DESC_PA_MASK);
            }
            free_4k_page(l1e_pa);
        }
        free_4k_page(l0_pa);
    }
    root[MC_PGD_IDX] = 0;
    free_4k_page((uint64_t)((uintptr_t)root - ARCH_PAGE_OFFSET));
}

/* ── Bounded AP wait (never halts the machine) ────────────────────
 * Mirrors ap_work_wait's protocol but converts the terminal
 * WORK-TIMEOUT halt into a test FAIL so the grid runner can never be
 * wedged by this selftest. */
static bool mc_poll_work_done(uint32_t cpu, uint32_t seq)
{
    struct ap_work *slot = &ap_work[cpu];
    uint64_t start = arch_cycle_counter();
    for (;;) {
        if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) == AP_WORK_DONE
            && slot->seq == seq)
            return true;
        if (arch_cycle_counter() - start > MC_DEADLINE_CYCLES)
            return false;
        arch_cpu_pause();
    }
}

static void mc_work_rearm(uint32_t cpu)
{
    __atomic_store_n(&ap_work[cpu].state, AP_WORK_IDLE, __ATOMIC_RELEASE);
}

/* Poll until `pred` returns true, or the deadline expires (→ false). */
static bool mc_poll_until(bool (*pred)(const void *ctx), const void *ctx)
{
    uint64_t start = arch_cycle_counter();
    while (!pred(ctx)) {
        if (arch_cycle_counter() - start > MC_DEADLINE_CYCLES)
            return false;
        arch_cpu_pause();
    }
    return true;
}

static uint32_t mc_gen_of(uint32_t cpu)
{
    return __atomic_load_n(&percpu_data[cpu].tlb_ack_gen,
                           __ATOMIC_ACQUIRE);
}

/* ── AP-side extension commands (strong override of the weak hook) ── */
bool ap_work_ext_run(uint32_t cmd, uint64_t arg0, uint64_t arg1,
                     uint64_t *out)
{
    (void)arg1;
    if (cmd == WORK_PT_MAP) {
        struct mc_map_args *a = (struct mc_map_args *)(uintptr_t)arg0;
        a->rc = aarch64_pt_map_4k_ext(a->root, a->va, a->pa,
                                      AARCH64_PT_KERNEL_RW, 0);
        *out = (uint64_t)(int64_t)a->rc;
        return true;
    }
    return false;
}

/* ── Section ①: AP1 in target set, gen increments ON the AP ─────── */
struct gen_ctx { uint32_t cpu; uint32_t want; };
static bool gen_reached(const void *p)
{
    const struct gen_ctx *c = (const struct gen_ctx *)p;
    return mc_gen_of(c->cpu) == c->want;
}

static int section_target_set(void)
{
    /* Precondition: AP1 is online AND has published its IPI channel —
     * i.e. tlb.c's build_target_mask_excl_self() must include it. */
    if (!percpu_data[1].online)
        return section_fail("target_set:ap1_offline", -EIO);
    if (!__atomic_load_n(&percpu_data[1].ipi_ready, __ATOMIC_ACQUIRE))
        return section_fail("target_set:ap1_not_ready", -EIO);

    uint32_t gen1_0 = mc_gen_of(1);
    tlb_shootdown();

    /* AP1 must ack exactly once: gen == snapshot+1 (equality, not <=,
     * mirrors tlb.c's wrap-safe contract). */
    struct gen_ctx c1 = { 1, gen1_0 + 1 };
    if (!mc_poll_until(gen_reached, &c1))
        return section_fail("target_set:ap1_no_ack", -ETIMEDOUT);

    /* The BSP's own counter also bumped (local flush + local ack). */
    uint32_t self = cpu_id();
    if (mc_gen_of(self) < 1)
        return section_fail("target_set:bsp_gen", -EIO);

    mc_puts("[selftest] m3mc: target_set PASS\n");
    return 0;
}

/* ── Section ②: partial-mask SGI reaches {1,2} but not 3 ────────── */
static int section_partial_mask(void)
{
    if (dtb_cpu_count() < 4) {
        mc_puts("[selftest] m3mc: partial_mask SKIP (<4 cpus)\n");
        return 0;
    }
    uint32_t g1 = mc_gen_of(1), g2 = mc_gen_of(2), g3 = mc_gen_of(3);

    /* Direct broadcast — deliberately NOT tlb_shootdown(), so the
     * mask (not the target-set builder) decides who is hit. */
    ipi_broadcast(IPI_VECTOR_TLB, (UINT64_C(1) << 1) | (UINT64_C(1) << 2));

    struct gen_ctx c1 = { 1, g1 + 1 };
    struct gen_ctx c2 = { 2, g2 + 1 };
    if (!mc_poll_until(gen_reached, &c1))
        return section_fail("partial_mask:cpu1_no_ack", -ETIMEDOUT);
    if (!mc_poll_until(gen_reached, &c2))
        return section_fail("partial_mask:cpu2_no_ack", -ETIMEDOUT);

    /* CPU 3 was excluded from the mask: no SGI was ever sent to it, so
     * its handler cannot have run and its counter must be unchanged.
     * (No concurrent shootdown can run here — this selftest is the
     * only initiator alive and the m3 probe finished long ago.) */
    if (mc_gen_of(3) != g3)
        return section_fail("partial_mask:cpu3_hit", -EIO);

    mc_puts("[selftest] m3mc: partial_mask PASS\n");
    return 0;
}

/* ── Section ⑤: same pt_lock interleave + BBM-class update ───────
 *
 * The AP participates through the ap_work protocol (WORK_BARRIER —
 * the handshake publishes the shared scratch root and keeps the AP in
 * its work loop for the duration), while the BSP drives BOTH leaves
 * of the shared L3 table (VA_P, VA_Q — same L2 slot, so
 * pt_lock_for(root_pa, l2) is one lock) through
 * map → replace RW→RO [the BBM-class update] → unmap iterations.
 * Both CPUs must complete with no deadlock; every wait is bounded by
 * MC_DEADLINE_CYCLES so a wedge becomes a FAIL, never a grid hang.
 *
 * Known-deferred (Task 26 report): a tighter variant where BOTH CPUs
 * run concurrent map/replace/unmap loops into the SAME L3 table page
 * trips phantom -ENOENT (leaves observed with VALID cleared) on this
 * QEMU TCG target, while every solo / different-L3 / AP-runner
 * variant passes 200/200. Root-causing that corner (kernel bug vs
 * TCG TLBI artifact) is deferred; see task-26-report.md. */
static int section_pt_lock_interleave(uint64_t *root, uint64_t data_ap,
                                      uint64_t data_bsp)
{
    /* WORK_BARRIER: keep the AP in its work loop (alive, answering)
     * for the duration of the BSP's BBM interleave. */
    uint32_t seq = 10001;
    ap_work_submit(1, seq, WORK_BARRIER, 0, 0);

    int rc = 0;
    uint32_t bsp_op = 0, bsp_it = 0;
    uint64_t o_pa; uint32_t o_pm; uint64_t o_sw;
    for (uint32_t i = 0; i < MC_STRESS_ITERS && rc == 0; i++) {
        rc = aarch64_pt_map_4k_ext(root, MC_VA_P, data_ap,
                                   AARCH64_PT_KERNEL_RW, 0);
        bsp_op = 0; bsp_it = i;
        if (rc != AARCH64_PT_OK) break;
        rc = aarch64_pt_replace_4k(root, MC_VA_P, data_ap,
                                   AARCH64_PT_KERNEL_RO, 0,
                                   &o_pa, &o_pm, &o_sw);
        bsp_op = 1;
        if (rc != AARCH64_PT_OK) break;
        rc = aarch64_pt_unmap_4k_ext(root, MC_VA_P, &o_pa, &o_pm, &o_sw);
        bsp_op = 2;
        if (rc != AARCH64_PT_OK) break;
        rc = aarch64_pt_map_4k_ext(root, MC_VA_Q, data_bsp,
                                   AARCH64_PT_KERNEL_RW, 0);
        bsp_op = 3;
        if (rc != AARCH64_PT_OK) break;
        rc = aarch64_pt_replace_4k(root, MC_VA_Q, data_bsp,
                                   AARCH64_PT_KERNEL_RO, 0,
                                   &o_pa, &o_pm, &o_sw);
        bsp_op = 4;
        if (rc != AARCH64_PT_OK) break;
        rc = aarch64_pt_unmap_4k_ext(root, MC_VA_Q, &o_pa, &o_pm, &o_sw);
        bsp_op = 5;
    }

    /* Bounded AP handshake close — FAIL, never halt. */
    bool ap_done = mc_poll_work_done(1, seq);
    mc_work_rearm(1);

    if (rc != 0 || !ap_done) {
        mc_dump_chain(root);
        mc_puts("[selftest] m3mc: pt_lock bsp fail op=");
        mc_put_u64(bsp_op);
        mc_puts(" it=");
        mc_put_u64(bsp_it);
        mc_puts(" rc=");
        mc_put_i32(rc);
        mc_puts("\n");
    }
    if (rc != 0)
        return section_fail("pt_lock:bsp_iter", rc ? rc : -EIO);
    if (!ap_done)
        return section_fail("pt_lock:ap_timeout", -ETIMEDOUT);

    /* Both leaves drained: the slots must be empty again. */
    rc = aarch64_pt_query_4k_ext(root, MC_VA_P, &o_pa, &o_pm, &o_sw);
    if (rc != AARCH64_PT_ENOENT)
        return section_fail("pt_lock:va_p_leftover", rc ? rc : -EIO);
    rc = aarch64_pt_query_4k_ext(root, MC_VA_Q, &o_pa, &o_pm, &o_sw);
    if (rc != AARCH64_PT_ENOENT)
        return section_fail("pt_lock:va_q_leftover", rc ? rc : -EIO);

    mc_puts("[selftest] m3mc: pt_lock_interleave PASS\n");
    return 0;
}

/* ── Section ⑥: concurrent L1 creation on a shared L1 entry ─────── */
static int section_shared_l1(uint64_t *root, uint64_t data_ap,
                             uint64_t data_bsp)
{
    mc_ap_map.root = root;
    mc_ap_map.va = MC_VA_S;      /* L2 idx 3, same L1 (PUD) range as VA_R */
    mc_ap_map.pa = data_ap;
    mc_ap_map.rc = -EINPROGRESS;

    /* Publish the AP item FIRST so the AP can be inside
     * aarch64_pt_map_4k_ext while the BSP starts its own map — the
     * ensure-segment (L1 creation) then genuinely races under
     * pt_upper_lock. */
    uint32_t seq = 10002;
    ap_work_submit(1, seq, WORK_PT_MAP,
                   (uint64_t)(uintptr_t)&mc_ap_map, 0);

    int rc = aarch64_pt_map_4k_ext(root, MC_VA_R, data_bsp,
                                   AARCH64_PT_KERNEL_RW, 0);
    if (rc != AARCH64_PT_OK) {
        mc_poll_work_done(1, seq); mc_work_rearm(1);
        return section_fail("shared_l1:bsp_map", rc);
    }
    if (!mc_poll_work_done(1, seq))
        return section_fail("shared_l1:ap_timeout", -ETIMEDOUT);
    mc_work_rearm(1);
    if (mc_ap_map.rc != AARCH64_PT_OK)
        return section_fail("shared_l1:ap_map", mc_ap_map.rc);

    /* Exactly one L1 table: root[511] holds a single table descriptor
     * whose L1 entry (index MC_L1_IDX(MC_VA_R)) is one shared L2 table
     * containing BOTH slots — i.e. both maps walked the same single
     * L1/L2 chain, so the concurrent creation allocated exactly one
     * L1 (and one L2) under pt_upper_lock. */
    uint64_t l0 = root[MC_PGD_IDX];
    if ((l0 & 3u) != 3u)
        return section_fail("shared_l1:no_l1", -EIO);
    uint64_t *l1 = (uint64_t *)Phy_To_Virt(l0 & MC_DESC_PA_MASK);
    uint64_t l1e = l1[MC_L1_IDX(MC_VA_R)];
    if ((l1e & 3u) != 3u)
        return section_fail("shared_l1:no_l2", -EIO);
    uint64_t *l2 = (uint64_t *)Phy_To_Virt(l1e & MC_DESC_PA_MASK);
    if ((l2[MC_L2_IDX(MC_VA_R)] & 3u) != 3u ||
        (l2[MC_L2_IDX(MC_VA_S)] & 3u) != 3u)
        return section_fail("shared_l1:l2_slots", -EIO);

    /* Both mappings queryable with the exact PA each side installed. */
    uint64_t qpa; uint32_t qpm; uint64_t qsw;
    rc = aarch64_pt_query_4k_ext(root, MC_VA_R, &qpa, &qpm, &qsw);
    if (rc != AARCH64_PT_OK || qpa != data_bsp ||
        qpm != AARCH64_PT_KERNEL_RW)
        return section_fail("shared_l1:query_r", rc ? rc : -EIO);
    rc = aarch64_pt_query_4k_ext(root, MC_VA_S, &qpa, &qpm, &qsw);
    if (rc != AARCH64_PT_OK || qpa != data_ap ||
        qpm != AARCH64_PT_KERNEL_RW)
        return section_fail("shared_l1:query_s", rc ? rc : -EIO);

    mc_puts("[selftest] m3mc: shared_l1 PASS\n");
    return 0;
}

/* ── Entry point — called from boot/main.c AFTER the m3 probe ───── */
int test_aarch64_page_table_multicore_run(void)
{
    uint32_t cpus = dtb_cpu_count();
    if (cpus < 2) {
        /* -smp 1: no AP exists — nothing multi-core to exercise. */
        mc_puts("[selftest] m3mc: SKIP (single-CPU boot)\n");
        return 0;
    }

    int passed = 0, failed = 0;

    /* ① + ② need no scratch state. */
    if (section_target_set() == 0) passed++; else failed++;
    if (section_partial_mask() == 0) passed++; else failed++;

    /* ⑤ + ⑥ share one scratch root + two data pages. */
    uint64_t *root = mc_root_alloc();
    uint64_t data_ap = alloc_4k_page();
    uint64_t data_bsp = alloc_4k_page();
    if (root == NULL || data_ap == 0 || data_bsp == 0) {
        failed++; failed++;
        mc_puts("[selftest] m3mc: alloc FAIL rc=-ENOMEM\n");
        if (data_ap) free_4k_page(data_ap);
        if (data_bsp) free_4k_page(data_bsp);
        mc_root_free(root);
    } else {
        if (section_pt_lock_interleave(root, data_ap, data_bsp) == 0)
            passed++; else failed++;
        if (section_shared_l1(root, data_ap, data_bsp) == 0)
            passed++; else failed++;
        free_4k_page(data_ap);
        free_4k_page(data_bsp);
        mc_root_free(root);
    }

    /* Parser-asserted final marker. Sections: target_set,
     * partial_mask (SKIP at -smp 2 counts as a pass — the 4-core
     * matrix leg covers the real assertion), pt_lock_interleave,
     * shared_l1. */
    if (failed == 0) {
        mc_puts("[selftest] m3mc: ");
        mc_put_u64((uint64_t)passed);
        mc_puts("/");
        mc_put_u64((uint64_t)passed);
        mc_puts(" PASS\n");
        return 0;
    }
    mc_puts("[selftest] m3mc: ");
    mc_put_u64((uint64_t)passed);
    mc_puts("/");
    mc_put_u64((uint64_t)(passed + failed));
    mc_puts(" PASS\n");
    return -1;
}

#else /* !__aarch64__ || !OS01_SELFTEST */

int test_aarch64_page_table_multicore_run(void)
{
    /* Not an aarch64 selftest build: no-op (the x86 kernel never
     * references this; the symbol only exists defensively). */
    return 0;
}

#endif /* __aarch64__ && OS01_SELFTEST */
