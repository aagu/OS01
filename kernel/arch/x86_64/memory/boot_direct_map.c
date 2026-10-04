#include <errno.h>
#include <arch/boot_memory.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)

enum { UNSTARTED, INITIALIZING, READY, FAILED };
static int state;
static struct MEMORY_RANGE coverage[MEMORY_RANGE_MAX];
static size_t coverage_count;

static int freeze_coverage(void)
{
    coverage_count = 0;
    for (size_t i = 0; i < PMMngr.zones_size; i++) {
        if (ZONE_UNMAPPED_INDEX && i == ZONE_UNMAPPED_INDEX)
            break;
        const struct Zone *z = &PMMngr.zones_struct[i];
        uint64_t s = z->zone_start_address, e = z->zone_end_address;
        if (s >= e || ((s | e) & (PAGE_2M_SIZE - 1)) || e > (UINT64_C(1) << 47) ||
            (coverage_count && s < coverage[coverage_count - 1].phys_end))
            return -EINVAL;
        if (coverage_count && s == coverage[coverage_count - 1].phys_end) {
            coverage[coverage_count - 1].phys_end = e;
        } else {
            if (coverage_count == MEMORY_RANGE_MAX)
                return -ENOSPC;
            coverage[coverage_count++] =
                (struct MEMORY_RANGE){.phys_start = s, .phys_end = e, .type = MEMORY_TYPE_RAM};
        }
    }
    return coverage_count ? 0 : -EINVAL;
}

static bool mapped(void)
{
    if (!kernel_map)
        return false;
    for (size_t i = 0; i < coverage_count; i++) {
        for (uint64_t pa = coverage[i].phys_start; pa < coverage[i].phys_end; pa += PAGE_2M_SIZE) {
            uint64_t va = pa + ARCH_PAGE_OFFSET;
            uint64_t d = kernel_map[(va >> 39) & 511];
            if (!(d & PAGE_VALID) || (d & PAGE_HUGE))
                return false;
            uint64_t *pud = (uint64_t *)Phy_To_Virt(d & PAGE_4K_MASK);
            d = pud[(va >> 30) & 511];
            if (!(d & PAGE_VALID) || (d & PAGE_HUGE))
                return false;
            uint64_t *pmd = (uint64_t *)Phy_To_Virt(d & PAGE_4K_MASK);
            d = pmd[(va >> 21) & 511];
            if ((d & (PAGE_VALID | PAGE_HUGE | PAGE_WRITE | PAGE_USER)) !=
                    (PAGE_VALID | PAGE_HUGE | PAGE_WRITE) ||
                (d & PAGE_2M_MASK) != pa)
                return false;
        }
    }
    return true;
}

int arch_boot_direct_map_init(void)
{
    if (state != UNSTARTED)
        return -EALREADY;
    state = INITIALIZING;
    int rc = freeze_coverage();
    if (!rc)
        rc = vmm_init();
    if (!rc && !mapped())
        rc = -EIO;
    state = rc ? FAILED : READY;
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
    *out = coverage;
    *count = coverage_count;
    return 0;
}
#ifdef OS01_HOST_TEST
void arch_boot_direct_map__test_reset(void)
{
    state = UNSTARTED;
    coverage_count = 0;
}
#endif
