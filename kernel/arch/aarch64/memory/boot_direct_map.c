#include <errno.h>
#include <string.h>
#include <arch/boot_memory.h>
#include <arch/aarch64/boot_direct_map.h>
#include <arch/aarch64/vmm_gate.h>
#include <arch/aarch64/early_arena.h>
#include <arch/aarch64/page_table_selftest.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/pmm_arch.h>
#include <log/log.h>

static enum { UNSTARTED, INITIALIZING, READY, FAILED } state;
static struct MEMORY_RANGE ram[MEMORY_RANGE_MAX];
static size_t ram_count;
static struct aarch64_runtime_tree tree;
static struct aarch64_runtime_tree_validate_buf vbuf;
static uint64_t pool_cursor, pool_end, installed_root;
static const struct aarch64_early_arena *arena;

static int alloc_table(void *ctx, uint64_t *pa, uint64_t **va)
{
    (void)ctx;
    if (pool_cursor >= pool_end)
        return -ENOMEM;
    *pa = pool_cursor;
    pool_cursor += 4096;
    *va = (uint64_t *)Phy_To_Virt(*pa);
    memset(*va, 0, 4096);
    return 0;
}
static uint64_t *resolve_table(void *ctx, uint64_t pa)
{
    (void)ctx;
    if ((pa & 4095) || pa < arena->table_base_pa || pa >= pool_cursor)
        return NULL;
    return (uint64_t *)Phy_To_Virt(pa);
}
static const struct aarch64_tree_ops ops = {.alloc = alloc_table, .resolve = resolve_table};
static int zones_match(void)
{
    if (PMMngr.zones_size != ram_count)
        return -EIO;
    for (size_t i = 0; i < ram_count; i++) {
        const struct Zone *z = &PMMngr.zones_struct[i];
        if (z->zone_start_address != ram[i].phys_start || z->zone_end_address != ram[i].phys_end ||
            z->pages_length != (ram[i].phys_end - ram[i].phys_start) / PAGE_2M_SIZE)
            return -EIO;
    }
    return 0;
}
int arch_boot_direct_map_init(void)
{
    if (state != UNSTARTED)
        return -EALREADY;
    state = INITIALIZING;
    int rc = -EINVAL;
    arena = aarch64_early_arena_get();
    if (!arena)
        goto fail;
    ram_count = pmm_arch_normalize(NULL, ram);
    if (!ram_count || ram_count > 16)
        goto fail;
    rc = zones_match();
    if (rc)
        goto fail;
    pool_cursor = arena->table_base_pa;
    pool_end = arena->table_end_pa;
#if AARCH64_M1_TABLE_EXHAUST
    /* Isolated failure variant limits the real allocator, not validator. */
    pool_end -= 4096;
#endif
    rc = aarch64_runtime_tree_build(ram, ram_count, arena, &ops, &tree);
    if (rc) {
#if AARCH64_M1_TABLE_EXHAUST
        log_err("M1 FATAL reason=table-exhaust\n");
#endif
        goto fail;
    }
    rc = aarch64_runtime_tree_validate(ram, ram_count, arena, &ops, &vbuf, &tree);
    if (rc)
        goto fail;
#if OS01_SELFTEST
    aarch64_page_table_prune_warm();
#endif
    /* M3 (Task 7): publish the M1 root BEFORE installing TTBR1 so later
     * shootdown backends' aarch64_pt_root_is_published() never sees the
     * in-use root as unpublished. Failure keeps the old root installed
     * (defensive -ENOSPC path: registry overflow is a violation, so this
     * branch is unreachable today). */
    if (!aarch64_pt_root_publish(tree.root_pa)) {
        rc = -ENOSPC;
        goto fail;
    }
    aarch64_install_ttbr1(tree.root_pa);
    installed_root = tree.root_pa;
    ZONE_NORMAL_INDEX = (uint32_t)(PMMngr.zones_size - 1);
    ZONE_UNMAPPED_INDEX = 0;
    if (aarch64_read_ttbr1() != installed_root) {
        rc = -EIO;
        goto fail;
    }
#if OS01_SELFTEST
    rc = aarch64_page_table_selftest(installed_root);
    if (rc)
        goto fail;
#endif
    rc = aarch64_runtime_tree_validate(ram, ram_count, arena, &ops, &vbuf, &tree);
    if (rc)
        goto fail;
    rc = zones_match();
    if (rc)
        goto fail;
    /* Freeze merged coverage before making readiness observable. */
    size_t merged = 0;
    for (size_t i = 0; i < ram_count; i++) {
        log_info("M1 ZONE id=%lu start=%lx end=%lx frames=%lu\n", (unsigned long)i,
                 (unsigned long)ram[i].phys_start, (unsigned long)ram[i].phys_end,
                 (unsigned long)((ram[i].phys_end - ram[i].phys_start) / PAGE_2M_SIZE));
        if (merged && ram[merged - 1].phys_end == ram[i].phys_start)
            ram[merged - 1].phys_end = ram[i].phys_end;
        else
            ram[merged++] = ram[i];
    }
    ram_count = merged;
    for (size_t i = 0; i < ram_count; i++)
        log_info("M1 COVERAGE id=%lu start=%lx end=%lx\n", (unsigned long)i,
                 (unsigned long)ram[i].phys_start, (unsigned long)ram[i].phys_end);
    *(uint64_t *)Phy_To_Virt(aarch64_runtime_root_address()) = installed_root;
    state = READY;
    log_info("M1 BSP PASS root=%lx ranges=%lu\n", (unsigned long)installed_root,
             (unsigned long)ram_count);
    return 0;
fail:
    state = FAILED;
    return rc;
}
bool arch_boot_direct_map_ready(void) { return state == READY; }
int arch_boot_direct_map_ranges(const struct MEMORY_RANGE **out, size_t *count)
{
    if (!out || !count)
        return -EINVAL;
    *out = NULL;
    *count = 0;
    if (state != READY)
        return -EAGAIN;
    *out = ram;
    *count = ram_count;
    return 0;
}
uint64_t aarch64_installed_root(void) { return installed_root; }
const struct aarch64_runtime_tree *aarch64_runtime_tree_get(void)
{
    return state == READY ? &tree : NULL;
}
#ifdef OS01_HOST_TEST
void arch_boot_direct_map__test_reset(void)
{
    state = UNSTARTED;
    installed_root = 0;
    ram_count = 0;
}
#endif
