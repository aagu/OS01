#include <errno.h>
#include <arch/boot_memory.h>
#include <arch/aarch64/boot_direct_map.h>
#include <arch/aarch64/early_arena.h>
#include <arch/aarch64/m1_selftest.h>
#include <arch/aarch64/page_table.h>
#include <arch/spinlock.h>
#include <arch/cpu.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/pmm_arch.h>
#include <log/log.h>

#define PA_MASK UINT64_C(0x000000fffffff000)
#define NX_FLAGS UINT64_C(0x60000000000705)
#define SENTINEL UINT64_C(0x4d3150524f424531)
static struct Page *probe_frame;
static spinlock_T marker_lock;
static bool probe_finished;
static volatile uint64_t *scalar(uint64_t address)
{
    return (volatile uint64_t *)Phy_To_Virt(address);
}

#if OS01_SELFTEST

void aarch64_m1_prune_warm(void)
{
    if (aarch64_m1_translation(ARCH_PAGE_OFFSET + UINT64_C(0x00200000)) & 1) {
        log_err("M1 FATAL reason=prune-precondition\n");
        for (;;)
            arch_cpu_halt();
    }
}
static int frame_rw(struct Page *p)
{
    uint64_t pa = p->phy_address;
    for (unsigned end = 0; end < 2; end++) {
        volatile uint64_t *v =
            (volatile uint64_t *)Phy_To_Virt(pa + (end ? PAGE_2M_SIZE - PAGE_4K_SIZE : 0));
        for (size_t i = 0; i < 512; i++)
            v[i] = SENTINEL ^ pa ^ i ^ end;
        for (size_t i = 0; i < 512; i++)
            if (v[i] != (SENTINEL ^ pa ^ i ^ end))
                return -EIO;
    }
    return 0;
}
static bool frame_mapped(uint64_t root_pa, uint64_t pa)
{
    uint64_t *t = (uint64_t *)Phy_To_Virt(root_pa);
    uint64_t va = ARCH_PAGE_OFFSET + pa;
    for (unsigned shift = 39; shift >= 30; shift -= 9) {
        uint64_t d = t[(va >> shift) & 511];
        if ((d & 3) != 3)
            return false;
        t = (uint64_t *)Phy_To_Virt(d & PA_MASK);
    }
    return t[(va >> 21) & 511] == (pa | NX_FLAGS);
}
static int smoke(uint64_t root_pa)
{
    uint64_t *root = (uint64_t *)Phy_To_Virt(root_pa);
    if (root[256])
        return -EIO;
    uint64_t data = alloc_4k_page();
    int rc = data ? 0 : -ENOMEM;
    if (!data) {
        log_err("M1 allocator normal=%u zones=%lu bits=%lx\n", ZONE_NORMAL_INDEX,
                (unsigned long)PMMngr.zones_size, (unsigned long)PMMngr.bits_map[0]);
        for (size_t i = 0; i < PMMngr.zones_size; i++)
            log_err("M1 allocator zone=%lu free=%lu using=%lu\n", (unsigned long)i,
                    (unsigned long)PMMngr.zones_struct[i].page_free_count,
                    (unsigned long)PMMngr.zones_struct[i].page_using_count);
    }
    if (data) {
        rc = aarch64_pt_map_4k(root, AARCH64_PT_SELFTEST_VA, data, AARCH64_PT_KERNEL_RW);
        if (!rc) {
            volatile uint64_t *alias = (volatile uint64_t *)(uintptr_t)AARCH64_PT_SELFTEST_VA;
            volatile uint64_t *direct = (volatile uint64_t *)Phy_To_Virt(data);
            *alias = SENTINEL;
            if (*direct != SENTINEL || *alias != SENTINEL)
                rc = -EIO;
            if (!rc && aarch64_pt_map_4k(root, AARCH64_PT_SELFTEST_VA, data,
                                         AARCH64_PT_KERNEL_RW) != AARCH64_PT_EEXIST)
                rc = -EIO;
            uint64_t qpa = 0;
            uint32_t perm = 0;
            if (!rc && (aarch64_pt_query_4k(root, AARCH64_PT_SELFTEST_VA, &qpa, &perm) ||
                        qpa != data || perm != AARCH64_PT_KERNEL_RW))
                rc = -EIO;
            if (!rc && aarch64_pt_unmap_4k(root, AARCH64_PT_SELFTEST_VA, &qpa, &perm))
                rc = -EIO;
        }
    }
    /* Even partial map allocation owns a partial chain under slot256. */
    int cleanup = aarch64_m1_smoke_cleanup(root, data);
    if (cleanup) {
        log_err("M1 smoke cleanup error=%lu\n", (unsigned long)(-cleanup));
        return cleanup;
    }
    if (root[256])
        return -EIO;
    if (rc) {
        log_err("M1 smoke error=%lu\n", (unsigned long)(-rc));
        return rc;
    }
    log_info("UEFI-A64: pt map smoke OK\n");
    log_info("M1 CLEANUP PASS slot=256\n");
    return 0;
}
int aarch64_m1_selftest(uint64_t root_pa)
{
    if (!(aarch64_m1_translation(ARCH_PAGE_OFFSET + UINT64_C(0x00200000)) & 1))
        return -EIO;
    if (aarch64_m1_translation(ARCH_PAGE_OFFSET + UINT64_C(0x08000000)) & 1)
        return -EIO;
    log_info("M1 PRUNE PASS pa=0x00200000 before=valid after=fault\n");
    int rc = smoke(root_pa);
    if (rc)
        return rc;
    uint64_t total = 0;
    for (size_t i = 0; i < PMMngr.zones_size; i++) {
        const struct Zone *z = &PMMngr.zones_struct[i];
        for (uint64_t pa = z->zone_start_address; pa < z->zone_end_address; pa += PAGE_2M_SIZE) {
            if (!frame_mapped(root_pa, pa))
                return -EIO;
            total++;
        }
        struct Page *first =
            pmm_claim_free_frame(z->zone_start_address, z->zone_end_address, false);
        if (!first)
            return -ENOMEM;
        rc = frame_rw(first);
        uint64_t first_pa = first->phy_address;
        /* Keep first claimed while selecting last; single-free frame is tested once. */
        struct Page *last = pmm_claim_free_frame(z->zone_start_address, z->zone_end_address, true);
        uint64_t last_pa = last ? last->phy_address : first_pa;
        if (!rc && last)
            rc = frame_rw(last);
        free_pages(first, 1);
        if (last)
            free_pages(last, 1);
        if (rc)
            return rc;
        log_info("M1 EDGE PASS zone=%lu first=%lx last=%lx\n", (unsigned long)i,
                 (unsigned long)first_pa, (unsigned long)last_pa);
    }
    log_info("M1 WALK PASS frames=%lu\n", (unsigned long)total);
#if AARCH64_M1_SPARSE
    uint64_t *t = (uint64_t *)Phy_To_Virt(root_pa);
    t = (uint64_t *)Phy_To_Virt(t[0] & PA_MASK);
    t = (uint64_t *)Phy_To_Virt(t[1] & PA_MASK);
    if (t[(UINT64_C(0x50000000) >> 21) & 511])
        return -EIO;
    struct Page *left = pmm_claim_free_frame(0x4fe00000, 0x50000000, false);
    struct Page *right = pmm_claim_free_frame(0x50200000, 0x50400000, false);
    if (!left || !right) {
        if (left)
            free_pages(left, 1);
        if (right)
            free_pages(right, 1);
        return -ENOMEM;
    }
    rc = frame_rw(left);
    if (!rc)
        rc = frame_rw(right);
    free_pages(left, 1);
    free_pages(right, 1);
    if (rc)
        return rc;
    log_info("M1 SPARSE PASS left=0x4fe00000 right=0x50200000 hole=absent\n");
#endif
    log_info("M1 SELFTEST PASS\n");
    return 0;
}
#else
void aarch64_m1_prune_warm(void) {}
int aarch64_m1_selftest(uint64_t root_pa)
{
    (void)root_pa;
    return 0;
}
#endif

int aarch64_m1_probe_prepare(void)
{
    spin_init(&marker_lock);
#if OS01_SELFTEST
    bool outside = false;
    for (size_t i = 0; i < PMMngr.zones_size; i++)
        if (PMMngr.zones_struct[i].zone_end_address > 0x80000000)
            outside = true;
    probe_frame = pmm_claim_free_frame(outside ? 0x80000000 : 0, UINT64_C(1) << 40, false);
    if (!probe_frame)
        return -ENOMEM;
    uint64_t pa = probe_frame->phy_address;
    *scalar(aarch64_probe_pa_address()) = pa;
    *scalar(aarch64_probe_expected_address()) = SENTINEL;
    volatile uint64_t *v = (volatile uint64_t *)Phy_To_Virt(pa);
    for (size_t i = 0; i < 512; i++)
        v[i] = SENTINEL;
    for (size_t i = 0; i < 512; i++)
        if (v[i] != SENTINEL)
            return -EIO;
    log_info("M1 PROBE PASS pa=%lx\n", (unsigned long)pa);
#endif
#if AARCH64_M1_AP_BAD_ROOT
    *scalar(aarch64_runtime_root_address()) = 0;
#endif
    return 0;
}
int aarch64_m1_ap_verify(unsigned int cpu)
{
    uint64_t root = *scalar(aarch64_runtime_root_address());
    bool ok = root && aarch64_read_ttbr1() == root;
#if OS01_SELFTEST
    uint64_t pa = *scalar(aarch64_probe_pa_address());
    uint64_t expected = *scalar(aarch64_probe_expected_address());
    ok = ok && pa && *(volatile uint64_t *)Phy_To_Virt(pa) == expected;
#endif
    uint64_t flags = spin_lock_irqsave(&marker_lock);
    if (ok) {
#if OS01_SELFTEST
        log_info("M1 AP PASS cpu=%u root=%lx probe=%lx\n", cpu, (unsigned long)root,
                 (unsigned long)pa);
#else
        log_info("M1 AP PASS cpu=%u root=%lx probe=none\n", cpu, (unsigned long)root);
#endif
    } else
        log_err("M1 AP FAIL cpu=%u\n", cpu);
    spin_unlock_irqrestore(&marker_lock, flags);
    return ok ? 0 : -EIO;
}
void aarch64_m1_probe_finish(bool all_requested_acked)
{
    if (!probe_finished && all_requested_acked) {
        probe_finished = true;
        if (probe_frame) {
            free_pages(probe_frame, 1);
            probe_frame = NULL;
        }
    }
}
void aarch64_m1_publish_ranges(void (*clean)(uint64_t, uint64_t, uint64_t), uint64_t line_size)
{
    const struct aarch64_runtime_tree *tree = aarch64_m1_tree_get();
    if (!tree) {
        log_err("M1 FATAL reason=publish-before-ready\n");
        for (;;)
            arch_cpu_halt();
    }
    clean((uint64_t)(uintptr_t)Phy_To_Virt(tree->table_base_pa),
          (uint64_t)(uintptr_t)Phy_To_Virt(tree->table_used_end_pa), line_size);
    uint64_t addr = aarch64_runtime_root_address();
    clean(addr, addr + 8, line_size);
#if OS01_SELFTEST
    addr = aarch64_probe_pa_address();
    clean(addr, addr + 8, line_size);
    addr = aarch64_probe_expected_address();
    clean(addr, addr + 8, line_size);
    if (probe_frame) {
        addr = (uint64_t)(uintptr_t)Phy_To_Virt(probe_frame->phy_address);
        clean(addr, addr + 4096, line_size);
    }
#endif
}
