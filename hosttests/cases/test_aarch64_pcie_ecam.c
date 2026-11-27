/* @os01-test
 * id: test_aarch64_pcie_ecam
 * layer: host
 * subsystem: memory
 * timing: strict
 * tier: pr
 * link: aarch64-pcie-ecam
 * scenarios: [memory.aarch64.pcie_ecam]
 */
#include "test_framework.h"
#include <arch/pci.h>
#include <arch/aarch64/pcie.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/boot_direct_map.h>
#include <arch/aarch64/handoff_layout.h>
#include <arch/aarch64/boot_layout.h>
#include <arch/aarch64/uefi_memory_map.h>
#include <arch/aarch64/vmm_gate.h>
#include <arch/mmu.h>
#include <bus/pci/pci.h>
#include <core/bootinfo.h>
#include <log/log.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static uint8_t *pt_arena;
static size_t pt_arena_used;
static const size_t pt_arena_size = 2u * 1024u * 1024u;

static uint8_t *mmap_low;
static uint8_t *ecam_arena;
static uint64_t mock_root_pa;
static uint64_t *mock_root;
static unsigned ttbr1_read_count;

static struct aarch64_pcie_topology mock_topo;

int g_log_level = LOG_INFO;
void _log_info_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }

struct test_efi_desc {
    uint32_t type;
    uint32_t pad;
    uint64_t phys_start;
    uint64_t virt_start;
    uint64_t num_pages;
    uint64_t attribute;
};

uint64_t alloc_4k_page(void)
{
    if (!pt_arena || pt_arena_used + 0x1000u > pt_arena_size) {
        return 0;
    }
    uint64_t pa = pt_arena_used + 0x100000u;
    memset(pt_arena + pt_arena_used, 0, 0x1000u);
    pt_arena_used += 0x1000u;
    return pa;
}

void free_4k_page(uint64_t pa) { (void)pa; }

uint64_t aarch64_read_ttbr1(void)
{
    ttbr1_read_count++;
    return mock_root_pa;
}

void tlb_invalidate_local(uint64_t va) { (void)va; }
void tlb_shootdown_all(uint64_t va) { (void)va; }

const struct aarch64_pcie_topology *aarch64_pcie_topology_get(void)
{
    return &mock_topo;
}

static void setup_fixture(struct boot_context *ctx,
                          const struct test_efi_desc *descs,
                          uint32_t desc_count)
{
    pt_arena = mmap((void *)(uintptr_t)(ARCH_PAGE_OFFSET + 0x100000u),
                    pt_arena_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert_true(pt_arena != MAP_FAILED);
    pt_arena_used = 0;

    mmap_low = mmap((void *)(uintptr_t)0x20000000u, 0x10000u,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert_true(mmap_low != MAP_FAILED);
    if (descs && desc_count > 0) {
        memcpy(mmap_low, descs, (size_t)desc_count * sizeof(*descs));
    }

    aarch64_pt_init_locks();
    mock_root_pa = alloc_4k_page();
    mock_root = (uint64_t *)(uintptr_t)(mock_root_pa + ARCH_PAGE_OFFSET);
    ttbr1_read_count = 0;
    memset(&mock_topo, 0, sizeof(mock_topo));

    boot_context_init(ctx);
    ctx->flags = BOOT_CONTEXT_HAS_MEMORY_MAP;
    ctx->memory.entries = (uint64_t)(uintptr_t)mmap_low;
    ctx->memory.entry_size = sizeof(*descs);
    ctx->memory.entry_count = desc_count;
    ctx->memory.format = BOOT_MEMORY_FORMAT_UEFI_RAW;
    ctx->memory.descriptor_version = 1;
}

static void run_isolated(void (*fn)(void))
{
    pid_t pid = fork();
    assert_true(pid >= 0);
    if (pid == 0) {
        fn();
        _exit(__test_stats.failed ? 1 : 0);
    }
    int status = 0;
    assert_eq(pid, waitpid(pid, &status, 0));
    assert_true(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void case_zero_roots_graceful_fallback(void)
{
    struct boot_context ctx;
    struct test_efi_desc descs[1] = {
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x40000000u, .num_pages = 0x1000u }
    };
    setup_fixture(&ctx, descs, 1u);
    assert_eq(0, aarch64_pcie_prepare(&ctx));
    assert_eq(0u, ttbr1_read_count);
    assert_null(arch_pci_backend());
}

static void case_m0_memory_map_bounds(void)
{
    struct boot_context ctx;
    struct test_efi_desc descs[1] = {
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x40000000u, .num_pages = 0x1000u }
    };
    setup_fixture(&ctx, descs, 1u);
    mock_topo.root_count = 1;
    mock_topo.roots[0].valid = true;
    mock_topo.roots[0].kind = AARCH64_PCIE_CTRL_ECAM_GENERIC;
    mock_topo.roots[0].domain = 0;
    mock_topo.roots[0].bus_start = 0;
    mock_topo.roots[0].bus_end = 1;
    mock_topo.roots[0].reg_cpu_base = 0x10000000ULL;
    mock_topo.roots[0].reg_size = 0x200000ULL;

    /* 1. entries == 0 */
    ctx.memory.entries = 0;
    assert_eq(0, aarch64_pcie_prepare(&ctx));
    assert_null(arch_pci_backend());

    /* 2. entry_size < AARCH64_UEFI_MEM_DESC_MIN_SIZE */
    ctx.memory.entries = (uint64_t)(uintptr_t)mmap_low;
    ctx.memory.entry_size = AARCH64_UEFI_MEM_DESC_MIN_SIZE - 4u;
    assert_eq(0, aarch64_pcie_prepare(&ctx));
    assert_null(arch_pci_backend());

    /* 3. count * stride overflow */
    ctx.memory.entry_size = sizeof(*descs);
    ctx.memory.entry_count = UINT32_MAX;
    ctx.memory.entry_size = 0x10000000u;
    assert_eq(0, aarch64_pcie_prepare(&ctx));
    assert_null(arch_pci_backend());

    /* 4. entries + map_bytes overflow */
    ctx.memory.entries = UINT64_MAX - 16ULL;
    ctx.memory.entry_size = sizeof(*descs);
    ctx.memory.entry_count = 1u;
    assert_eq(0, aarch64_pcie_prepare(&ctx));
    assert_null(arch_pci_backend());

    /* 5. map_end > AARCH64_BOOT_MAP_END */
    ctx.memory.entries = (uint64_t)AARCH64_BOOT_MAP_END - 8ULL;
    ctx.memory.entry_size = sizeof(*descs);
    ctx.memory.entry_count = 1u;
    assert_eq(0, aarch64_pcie_prepare(&ctx));
    assert_null(arch_pci_backend());

    /* 6. Overlap with Boot Block B */
    ctx.memory.entries = (uint64_t)AARCH64_BOOT_BLOCK_BASE;
    ctx.memory.entry_size = sizeof(*descs);
    ctx.memory.entry_count = 1u;
    assert_eq(0, aarch64_pcie_prepare(&ctx));
    assert_null(arch_pci_backend());
}

static void case_ram_pa_limit_and_partial_conflict(void)
{
    struct boot_context ctx;
    struct test_efi_desc descs[2] = {
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x40000000u, .num_pages = 0x1000u },
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x12000000u, .num_pages = 1u },
    };
    setup_fixture(&ctx, descs, 2u);

    /* Root 0: overlaps RAM at 0x12000000 */
    mock_topo.roots[0].valid = true;
    mock_topo.roots[0].kind = AARCH64_PCIE_CTRL_ECAM_GENERIC;
    mock_topo.roots[0].domain = 0;
    mock_topo.roots[0].bus_start = 0;
    mock_topo.roots[0].bus_end = 0;
    mock_topo.roots[0].reg_cpu_base = 0x12000000ULL;
    mock_topo.roots[0].reg_size = 0x100000ULL;

    /* Root 1: >= AARCH64_PT_PA_LIMIT (1 TiB) */
    mock_topo.roots[1].valid = true;
    mock_topo.roots[1].kind = AARCH64_PCIE_CTRL_ECAM_GENERIC;
    mock_topo.roots[1].domain = 1;
    mock_topo.roots[1].bus_start = 0;
    mock_topo.roots[1].bus_end = 0;
    mock_topo.roots[1].reg_cpu_base = AARCH64_PT_PA_LIMIT;
    mock_topo.roots[1].reg_size = 0x100000ULL;

    /* Root 2: 4 MiB spanning 2 blocks (0x24000000 and 0x24200000), 2nd block conflicts with Normal RAM block */
    assert_eq(0, aarch64_pt_map_2m_block(mock_root, ARCH_PAGE_OFFSET + 0x24200000ULL,
                                         0x24200000ULL, AARCH64_PT_KERNEL_RW));
    mock_topo.roots[2].valid = true;
    mock_topo.roots[2].kind = AARCH64_PCIE_CTRL_ECAM_GENERIC;
    mock_topo.roots[2].domain = 2;
    mock_topo.roots[2].bus_start = 0;
    mock_topo.roots[2].bus_end = 3;
    mock_topo.roots[2].reg_cpu_base = 0x24000000ULL;
    mock_topo.roots[2].reg_size = 0x400000ULL;

    /* Root 3: clean 2 MiB root at 0x28000000 -> remains enabled */
    assert_true(mmap((void *)(uintptr_t)(ARCH_PAGE_OFFSET + 0x28000000ULL),
                     0x200000ULL, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED);
    mock_topo.roots[3].valid = true;
    mock_topo.roots[3].kind = AARCH64_PCIE_CTRL_ECAM_GENERIC;
    mock_topo.roots[3].domain = 3;
    mock_topo.roots[3].bus_start = 0;
    mock_topo.roots[3].bus_end = 1;
    mock_topo.roots[3].reg_cpu_base = 0x28000000ULL;
    mock_topo.roots[3].reg_size = 0x200000ULL;
    mock_topo.root_count = 4;

    assert_eq(0, aarch64_pcie_prepare(&ctx));

    const struct pci_backend *be = arch_pci_backend();
    assert_not_null(be);
    assert_eq(1u, be->root_count);
    assert_eq(3u, be->roots[0].domain);

    /* Root 2 first block mapped safely, second block untouched Normal RAM */
    uint64_t desc = 0;
    assert_eq(0, aarch64_pt_read_l2_desc(mock_root, ARCH_PAGE_OFFSET + 0x24000000ULL, &desc));
    assert_eq(aarch64_pt_encode_block_desc(0x24000000ULL, AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE, 0), desc);
    assert_eq(0, aarch64_pt_read_l2_desc(mock_root, ARCH_PAGE_OFFSET + 0x24200000ULL, &desc));
    assert_eq(aarch64_pt_encode_block_desc(0x24200000ULL, AARCH64_PT_KERNEL_RW, 0), desc);
}

static void case_ecam_access_ranges_and_errors(void)
{
    struct boot_context ctx;
    struct test_efi_desc descs[1] = {
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x40000000u, .num_pages = 0x1000u }
    };
    setup_fixture(&ctx, descs, 1u);

    /* Map fake ECAM backing at ARCH_PAGE_OFFSET + 0x10000000 (2 MiB, buses 2..3) */
    const uint64_t ecam_pa = 0x10000000ULL;
    ecam_arena = mmap((void *)(uintptr_t)(ARCH_PAGE_OFFSET + ecam_pa),
                      0x200000ULL, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert_true(ecam_arena != MAP_FAILED);

    mock_topo.root_count = 1;
    mock_topo.roots[0].valid = true;
    mock_topo.roots[0].kind = AARCH64_PCIE_CTRL_ECAM_GENERIC;
    mock_topo.roots[0].domain = 0;
    mock_topo.roots[0].bus_start = 2;
    mock_topo.roots[0].bus_end = 3;
    mock_topo.roots[0].reg_cpu_base = ecam_pa;
    mock_topo.roots[0].reg_size = 0x200000ULL;
    mock_topo.roots[0].range_count = 3;
    mock_topo.roots[0].ranges[0] = (struct aarch64_pcie_range){
        .space = PCI_SPACE_IO, .prefetchable = false,
        .pci_base = 0x0ULL, .cpu_base = 0x3eff0000ULL, .size = 0x10000ULL
    };
    mock_topo.roots[0].ranges[1] = (struct aarch64_pcie_range){
        .space = PCI_SPACE_MMIO32, .prefetchable = false,
        .pci_base = 0x10000000ULL, .cpu_base = 0x10000000ULL, .size = 0x2eff0000ULL
    };
    mock_topo.roots[0].ranges[2] = (struct aarch64_pcie_range){
        .space = PCI_SPACE_MMIO64, .prefetchable = true,
        .pci_base = 0x8000000000ULL, .cpu_base = 0x8000000000ULL, .size = 0x8000000000ULL
    };

    assert_eq(0, aarch64_pcie_prepare(&ctx));

    /* Verify Device 2 MiB descriptor installed before SMP */
    uint64_t desc = 0;
    assert_eq(0, aarch64_pt_read_l2_desc(mock_root, ARCH_PAGE_OFFSET + ecam_pa, &desc));
    assert_eq(aarch64_pt_encode_block_desc(ecam_pa, AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE, 0), desc);

    const struct pci_backend *be = arch_pci_backend();
    assert_not_null(be);
    assert_eq(4092u, be->max_config_offset);

    /* Verify ECAM offset encoding for bus=3 (bus - bus_start = 1), slot=4, fn=2, offset=0xFFC */
    uint64_t expected_off = (1ULL << 20) | (4ULL << 15) | (2ULL << 12) | 0xFFCU;
    *(volatile uint32_t *)(ecam_arena + expected_off) = 0x12345678U;

    enum pci_error_scope scope = PCI_ERROR_ROOT;
    uint32_t val = 0;
    assert_eq(0, be->read32(0, 3, 4, 2, 0xFFC, &val, &scope));
    assert_eq(0x12345678U, val);
    assert_eq(PCI_ERROR_FUNCTION, scope);

    assert_eq(0, be->write32(0, 3, 4, 2, 0x100, 0xCAFEBABEU, &scope));
    uint64_t off100 = (1ULL << 20) | (4ULL << 15) | (2ULL << 12) | 0x100ULL;
    assert_eq(0xCAFEBABEU, *(volatile uint32_t *)(ecam_arena + off100));

    /* Error scopes */
    assert_eq(-ENODEV, be->read32(1, 3, 4, 2, 0, &val, &scope));
    assert_eq(PCI_ERROR_ROOT, scope);
    assert_eq(-EINVAL, be->read32(0, 1, 0, 0, 0, &val, &scope)); /* bus < bus_start */
    assert_eq(PCI_ERROR_FUNCTION, scope);
    assert_eq(-EINVAL, be->read32(0, 4, 0, 0, 0, &val, &scope)); /* bus > bus_end */
    assert_eq(-EINVAL, be->read32(0, 2, 32, 0, 0, &val, &scope));
    assert_eq(-EINVAL, be->read32(0, 2, 0, 8, 0, &val, &scope));
    assert_eq(-EINVAL, be->read32(0, 2, 0, 0, 4096, &val, &scope));
    assert_eq(-EINVAL, be->read32(0, 2, 0, 0, 0x102, &val, &scope));

    /* route_gsi == -ENOTSUP */
    struct pci_device dummy_dev;
    memset(&dummy_dev, 0, sizeof(dummy_dev));
    assert_eq(-ENOTSUP, be->route_gsi(&dummy_dev, &val));

    /* bus_range */
    uint8_t bs = 0, be_num = 0;
    assert_eq(0, be->bus_range(0, &bs, &be_num));
    assert_eq(2, bs);
    assert_eq(3, be_num);
    assert_eq(-ENODEV, be->bus_range(1, &bs, &be_num));

    /* translate_resource for IO / MMIO32 / MMIO64 */
    uint64_t cpu_pa = 0;
    assert_eq(0, be->translate_resource(0, PCI_SPACE_IO, 0x1000ULL, 0, &cpu_pa));
    assert_eq(0x3eff1000ULL, cpu_pa);
    assert_eq(0, be->translate_resource(0, PCI_SPACE_MMIO32, 0x10004000ULL, 0x1000ULL, &cpu_pa));
    assert_eq(0x10004000ULL, cpu_pa);
    assert_eq(0, be->translate_resource(0, PCI_SPACE_MMIO64, 0x8000001000ULL, 0, &cpu_pa));
    assert_eq(0x8000001000ULL, cpu_pa);
    assert_eq(-ENOENT, be->translate_resource(0, PCI_SPACE_MMIO32, 0x70000000ULL, 0, &cpu_pa));
    assert_eq(-EOVERFLOW, be->translate_resource(0, PCI_SPACE_MMIO64, UINT64_MAX - 4ULL, 16ULL, &cpu_pa));
}

static void case_ecam_interrupt_map_routing(void)
{
    struct boot_context ctx;
    struct test_efi_desc descs[1] = {
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x40000000u, .num_pages = 0x1000u }
    };
    setup_fixture(&ctx, descs, 1u);

    const uint64_t ecam_pa = 0x10000000ULL;
    ecam_arena = mmap((void *)(uintptr_t)(ARCH_PAGE_OFFSET + ecam_pa),
                      0x200000ULL, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert_true(ecam_arena != MAP_FAILED);

    mock_topo.root_count = 1;
    mock_topo.roots[0].valid = true;
    mock_topo.roots[0].kind = AARCH64_PCIE_CTRL_ECAM_GENERIC;
    mock_topo.roots[0].domain = 0;
    mock_topo.roots[0].bus_start = 0;
    mock_topo.roots[0].bus_end = 1;
    mock_topo.roots[0].reg_cpu_base = ecam_pa;
    mock_topo.roots[0].reg_size = 0x200000ULL;
    mock_topo.roots[0].has_int_map = true;
    mock_topo.roots[0].int_map_mask[0] = 0x1800U;
    mock_topo.roots[0].int_map_mask[3] = 0x7U;
    mock_topo.roots[0].int_map_count = 2;
    mock_topo.roots[0].int_map[0] = (struct aarch64_pcie_intmap_entry){
        .addr_hi = 0x0800U, .int_pin = 1U, .gic_intid = 36U
    };
    mock_topo.roots[0].int_map[1] = (struct aarch64_pcie_intmap_entry){
        .addr_hi = 0x0800U, .int_pin = 2U, .gic_intid = 37U
    };

    assert_eq(0, aarch64_pcie_prepare(&ctx));
    const struct pci_backend *be = arch_pci_backend();
    assert_not_null(be);

    struct pci_device dev;
    memset(&dev, 0, sizeof(dev));
    dev.domain = 0;
    dev.bus = 0;
    dev.slot = 1;
    dev.fn = 0;

    uint32_t gsi = 0;
    /* pin == 0 -> -ENOTSUP */
    *(volatile uint32_t *)(ecam_arena + (1ULL << 15) + 0x3CU) = 0x00000000U;
    assert_eq(-ENOTSUP, be->route_gsi(&dev, &gsi));

    /* pin == 1 (INTA) -> INTID 36 */
    *(volatile uint32_t *)(ecam_arena + (1ULL << 15) + 0x3CU) = 0x00000100U;
    assert_eq(0, be->route_gsi(&dev, &gsi));
    assert_eq(36U, gsi);

    /* pin == 2 (INTB) -> INTID 37 */
    *(volatile uint32_t *)(ecam_arena + (1ULL << 15) + 0x3CU) = 0x00000200U;
    assert_eq(0, be->route_gsi(&dev, &gsi));
    assert_eq(37U, gsi);

    /* unmapped pin == 3 -> -ENOENT */
    *(volatile uint32_t *)(ecam_arena + (1ULL << 15) + 0x3CU) = 0x00000300U;
    assert_eq(-ENOENT, be->route_gsi(&dev, &gsi));

    /* unknown domain -> -ENODEV */
    dev.domain = 1;
    assert_eq(-ENODEV, be->route_gsi(&dev, &gsi));
}

static void case_ecam_mmio_window_bounds_and_isolation(void)
{
    struct boot_context ctx;
    /* RAM at 0x40000000..0x42000000 and 0x30000000..0x30200000 (conflict with root 0 range[2]) */
    struct test_efi_desc descs[2] = {
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x40000000u, .num_pages = 0x2000u },
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x30000000u, .num_pages = 0x200u },
    };
    setup_fixture(&ctx, descs, 2u);

    assert_true(mmap((void *)(uintptr_t)(ARCH_PAGE_OFFSET + 0x10000000ULL),
                     0x200000ULL, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED);
    assert_true(mmap((void *)(uintptr_t)(ARCH_PAGE_OFFSET + 0x18000000ULL),
                     0x200000ULL, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED);

    mock_topo.root_count = 2;

    /* Root 0: domain=0, ECAM at 0x10000000 (2 MiB)
     * - range[0]: unaligned MMIO32 [0x12000000, 0x12300000) (3 MiB, crosses 0x12200000 2 MiB boundary)
     * - range[1]: skipped huge MMIO32 [0x14000000, 0x54000001) (> 1 GiB)
     * - range[2]: RAM-conflicting MMIO32 [0x30000000, 0x30100000) (fails Stage-3 map)
     */
    mock_topo.roots[0].valid = true;
    mock_topo.roots[0].kind = AARCH64_PCIE_CTRL_ECAM_GENERIC;
    mock_topo.roots[0].domain = 0;
    mock_topo.roots[0].bus_start = 0;
    mock_topo.roots[0].bus_end = 1;
    mock_topo.roots[0].reg_cpu_base = 0x10000000ULL;
    mock_topo.roots[0].reg_size = 0x200000ULL;
    mock_topo.roots[0].range_count = 3;
    mock_topo.roots[0].ranges[0] = (struct aarch64_pcie_range){
        .space = PCI_SPACE_MMIO32, .pci_base = 0x12000000ULL, .cpu_base = 0x12000000ULL, .size = 0x00300000ULL
    };
    mock_topo.roots[0].ranges[1] = (struct aarch64_pcie_range){
        .space = PCI_SPACE_MMIO32, .pci_base = 0x14000000ULL, .cpu_base = 0x14000000ULL, .size = (1ULL << 30) + 1ULL
    };
    mock_topo.roots[0].ranges[2] = (struct aarch64_pcie_range){
        .space = PCI_SPACE_MMIO32, .pci_base = 0x30000000ULL, .cpu_base = 0x30000000ULL, .size = 0x00100000ULL
    };

    /* Root 1: domain=1, ECAM at 0x18000000 (2 MiB), MMIO32 [0x20000000, 0x20100000) */
    mock_topo.roots[1].valid = true;
    mock_topo.roots[1].kind = AARCH64_PCIE_CTRL_ECAM_GENERIC;
    mock_topo.roots[1].domain = 1;
    mock_topo.roots[1].bus_start = 0;
    mock_topo.roots[1].bus_end = 1;
    mock_topo.roots[1].reg_cpu_base = 0x18000000ULL;
    mock_topo.roots[1].reg_size = 0x200000ULL;
    mock_topo.roots[1].range_count = 1;
    mock_topo.roots[1].ranges[0] = (struct aarch64_pcie_range){
        .space = PCI_SPACE_MMIO32, .pci_base = 0x20000000ULL, .cpu_base = 0x20000000ULL, .size = 0x00100000ULL
    };

    assert_eq(0, aarch64_pcie_prepare(&ctx));
    const struct pci_backend *be = arch_pci_backend();
    assert_not_null(be);
    assert_not_null(be->map_mmio);

    void *va = NULL;
    /* 1) Crossing 2 MiB boundary inside range[0] ([0x12000000, 0x12300000)) -> succeeds */
    assert_eq(0, be->map_mmio(0, 0x121FF000ULL, 0x2000ULL, &va));
    assert_eq((uintptr_t)(ARCH_PAGE_OFFSET + 0x121FF000ULL), (uintptr_t)va);

    /* 2) Request ending exactly at raw window end (0x12300000, while 2 MiB PT covers 0x12400000) -> succeeds */
    assert_eq(0, be->map_mmio(0, 0x122FF000ULL, 0x1000ULL, &va));
    assert_eq((uintptr_t)(ARCH_PAGE_OFFSET + 0x122FF000ULL), (uintptr_t)va);

    /* 3) 1 byte past raw window end -> -ENOENT (2 MiB rounding must NOT expand allowed range) */
    va = (void *)0x1234;
    assert_eq(-ENOENT, be->map_mmio(0, 0x122FF000ULL, 0x1001ULL, &va));
    assert_null(va);

    /* 4) Root 1's window [0x20000000, 0x20100000) must NOT satisfy domain 0 */
    assert_eq(-ENOENT, be->map_mmio(0, 0x20000000ULL, 0x1000ULL, &va));
    /* But succeeds on domain 1 up to exact raw end */
    assert_eq(0, be->map_mmio(1, 0x200FF000ULL, 0x1000ULL, &va));
    assert_eq(-ENOENT, be->map_mmio(1, 0x200FF000ULL, 0x1001ULL, &va));

    /* 5) Skipped (> 1 GiB) and RAM-conflicting windows are NOT queryable */
    assert_eq(-ENOENT, be->map_mmio(0, 0x14000000ULL, 0x1000ULL, &va));
    assert_eq(-ENOENT, be->map_mmio(0, 0x30000000ULL, 0x1000ULL, &va));

    /* 6) Invalid length / domain */
    assert_eq(-EINVAL, be->map_mmio(0, 0x12000000ULL, 0ULL, &va));
    assert_eq(-ENODEV, be->map_mmio(9, 0x12000000ULL, 0x1000ULL, &va));
}

static void case_ecam_gicv2m_msix(void)
{
    struct boot_context ctx;
    struct test_efi_desc descs[1] = {
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x40000000u, .num_pages = 0x1000u }
    };
    setup_fixture(&ctx, descs, 1u);

    /* Map fake GICv2m frame at ARCH_PAGE_OFFSET + 0x08020000 (inside 0x08000000 2 MiB block) */
    assert_true(mmap((void *)(uintptr_t)(ARCH_PAGE_OFFSET + 0x10000000ULL),
                     0x200000ULL, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED);
    const uint64_t v2m_blk_pa = 0x08000000ULL;
    uint8_t *v2m_arena = mmap((void *)(uintptr_t)(ARCH_PAGE_OFFSET + v2m_blk_pa),
                              0x200000ULL, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert_true(v2m_arena != MAP_FAILED);

    /* MSI_TYPER @ 0x08020008: base_spi=80 (0x50 << 16), num_spis=64 (0x40) */
    *(volatile uint32_t *)(v2m_arena + 0x20008ULL) = (80U << 16) | 64U;

    mock_topo.root_count = 1;
    mock_topo.roots[0].valid = true;
    mock_topo.roots[0].kind = AARCH64_PCIE_CTRL_ECAM_GENERIC;
    mock_topo.roots[0].domain = 0;
    mock_topo.roots[0].bus_start = 0;
    mock_topo.roots[0].bus_end = 1;
    mock_topo.roots[0].msi_phandle = 0x8003U;
    mock_topo.roots[0].reg_cpu_base = 0x10000000ULL;
    mock_topo.roots[0].reg_size = 0x200000ULL;
    mock_topo.roots[0].range_count = 1;
    mock_topo.roots[0].ranges[0] = (struct aarch64_pcie_range){
        .space = PCI_SPACE_MMIO32, .pci_base = 0x12000000ULL, .cpu_base = 0x12000000ULL, .size = 0x00100000ULL
    };
    mock_topo.v2m.valid = true;
    mock_topo.v2m.phandle = 0x8003U;
    mock_topo.v2m.reg_cpu_base = 0x08020000ULL;
    mock_topo.v2m.reg_size = 0x1000ULL;

    assert_eq(0, aarch64_pcie_prepare(&ctx));

    struct pci_device dev;
    memset(&dev, 0, sizeof(dev));
    dev.domain = 0;

    assert_eq(0x08020040U, arch_pci_msi_address(&dev));

    void *tbl = NULL;
    assert_eq(0, arch_pci_msix_map(&dev, 0x12041000ULL, &tbl));
    assert_eq((uintptr_t)(ARCH_PAGE_OFFSET + 0x12041000ULL), (uintptr_t)tbl);
    arch_pci_msix_unmap(&dev, tbl);

    /* Unmapped MMIO range rejected */
    assert_eq(-ENOENT, arch_pci_msix_map(&dev, 0x13000000ULL, &tbl));
    assert_null(tbl);

    /* Unknown domain rejected */
    dev.domain = 1;
    assert_eq(0U, arch_pci_msi_address(&dev));
    assert_eq(-ENOTSUP, arch_pci_msix_map(&dev, 0x12041000ULL, &tbl));
}

/* 2026-11-27 regression guard: the PCIe Stage-3 pre-map installs Device
 * blocks over the WHOLE PCIe MMIO32 bus window ([0x10000000, 0x40000000) on
 * QEMU virt) at 2 MiB granularity.  The M3.5 shootdown probe maps its two
 * scratch pages at a kernel VA whose 2 MiB block must stay UNMAPPED — when
 * the pre-map claimed that block the probe failed with "scratch-non-empty"
 * and halted the kernel before the SMP scheduler selftest ever ran.  This
 * case pins the invariant: the probe's scratch PA stays outside every
 * device window the pre-map installs. */
static void case_scratch_probe_va_outside_pcie_device_windows(void)
{
    struct boot_context ctx;
    struct test_efi_desc descs[1] = {
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x40000000u, .num_pages = 0x1000u }
    };
    setup_fixture(&ctx, descs, 1u);

    /* QEMU virt DTB PCIe ranges (the same fixture the live DTB produces). */
    mock_topo.root_count = 1;
    mock_topo.roots[0].valid = true;
    mock_topo.roots[0].kind = AARCH64_PCIE_CTRL_ECAM_GENERIC;
    mock_topo.roots[0].domain = 0;
    mock_topo.roots[0].bus_start = 0;
    mock_topo.roots[0].bus_end = 255;
    mock_topo.roots[0].reg_cpu_base = 0x4010000000ULL;
    mock_topo.roots[0].reg_size = 0x10000000ULL;
    mock_topo.roots[0].range_count = 3;
    mock_topo.roots[0].ranges[0] = (struct aarch64_pcie_range){
        .space = PCI_SPACE_IO, .prefetchable = false,
        .pci_base = 0x0ULL, .cpu_base = 0x3eff0000ULL, .size = 0x10000ULL
    };
    mock_topo.roots[0].ranges[1] = (struct aarch64_pcie_range){
        .space = PCI_SPACE_MMIO32, .prefetchable = false,
        .pci_base = 0x10000000ULL, .cpu_base = 0x10000000ULL, .size = 0x2eff0000ULL
    };
    mock_topo.roots[0].ranges[2] = (struct aarch64_pcie_range){
        .space = PCI_SPACE_MMIO64, .prefetchable = true,
        .pci_base = 0x8000000000ULL, .cpu_base = 0x8000000000ULL, .size = 0x8000000000ULL
    };

    assert_eq(0, aarch64_pcie_prepare(&ctx));

    /* The scratch PA must be 2 MiB-aligned: map_4k_new cannot split an
     * occupied 2 MiB block, so the probe needs a whole free block. */
    assert_eq(0, AARCH64_SHOOTDOWN_PROBE_SCRATCH_PA & 0x1FFFFFu);

    /* Sanity: the PCIe MMIO32 window IS pre-mapped — proves this fixture
     * exercises the real pre-map path instead of a no-op. */
    uint64_t desc = 0;
    assert_eq(0, aarch64_pt_read_l2_desc(mock_root, ARCH_PAGE_OFFSET + 0x10000000ULL, &desc));
    assert_true(desc != 0);

    /* The invariant: the probe's scratch block stays unmapped. */
    desc = 0;
    int rc = aarch64_pt_read_l2_desc(mock_root,
                                      ARCH_PAGE_OFFSET + AARCH64_SHOOTDOWN_PROBE_SCRATCH_PA,
                                      &desc);
    assert_true(rc != AARCH64_PT_OK || desc == 0);
}

static void case_ecam_xhci_high_bar0_mapping(void)
{
    struct boot_context ctx;
    struct test_efi_desc descs[1] = {
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x40000000u, .num_pages = 0x1000u }
    };
    setup_fixture(&ctx, descs, 1u);

    ecam_arena = mmap((void *)(uintptr_t)(ARCH_PAGE_OFFSET + 0x10000000ULL),
                      0x200000ULL, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert_true(ecam_arena != MAP_FAILED);
    memset(ecam_arena, 0xFF, 0x200000ULL);

    /* Place qemu-xhci (1b36:000d, class 0c0330) at bus=0, slot=1, fn=0 with 64-bit BAR0 @ 0x8000000000 */
    uintptr_t cfg_off = (1ULL << 15);
    *(volatile uint32_t *)(ecam_arena + cfg_off + 0x00) = 0x000d1b36U;
    *(volatile uint32_t *)(ecam_arena + cfg_off + 0x08) = (0x0c0330U << 8);
    *(volatile uint32_t *)(ecam_arena + cfg_off + 0x10) = 0x00000004U;
    *(volatile uint32_t *)(ecam_arena + cfg_off + 0x14) = 0x00000080U;

    mock_topo.root_count = 1;
    mock_topo.roots[0].valid = true;
    mock_topo.roots[0].kind = AARCH64_PCIE_CTRL_ECAM_GENERIC;
    mock_topo.roots[0].domain = 0;
    mock_topo.roots[0].bus_start = 0;
    mock_topo.roots[0].bus_end = 1;
    mock_topo.roots[0].reg_cpu_base = 0x10000000ULL;
    mock_topo.roots[0].reg_size = 0x200000ULL;
    mock_topo.roots[0].range_count = 1;
    mock_topo.roots[0].ranges[0] = (struct aarch64_pcie_range){
        .space = PCI_SPACE_MMIO64,
        .pci_base = 0x8000000000ULL,
        .cpu_base = 0x8000000000ULL,
        .size = 0x10000000ULL,
    };

    assert_eq(0, aarch64_pcie_prepare(&ctx));

    struct pci_device pdev;
    memset(&pdev, 0, sizeof(pdev));
    pdev.domain = 0;
    pdev.bus = 0;
    pdev.slot = 1;
    pdev.fn = 0;
    pdev.vendor = 0x1b36;
    pdev.device = 0x000d;
    pdev.class_code = 0x0c0330U;
    pdev.bars[0].valid = true;
    pdev.bars[0].kind = PCI_BAR_MMIO64;
    pdev.bars[0].address = 0x8000000000ULL;

    /* Full MMIO64 window must NOT be marked mapped */
    const struct pci_backend *be = arch_pci_backend();
    assert_true(be != NULL && be->map_mmio != NULL);
    void *va = NULL;
    assert_eq(0, be->map_mmio(0, 0x8000000000ULL, 0x4000ULL, &va));
    assert_eq((uintptr_t)(ARCH_PAGE_OFFSET + 0x8000000000ULL), (uintptr_t)va);
    assert_eq(-ENOENT, be->map_mmio(0, 0x8000000000ULL, 0x4001ULL, &va));
    assert_eq(-ENOENT, be->map_mmio(0, 0x8000004000ULL, 0x1000ULL, &va));
    assert_true(aarch64_pcie_xhci_bar0_ready(&pdev));

    /* BAR relocation rejected */
    *(volatile uint32_t *)(ecam_arena + cfg_off + 0x10) = 0x00010004U;
    assert_true(!aarch64_pcie_xhci_bar0_ready(&pdev));

    /* Unmatched PCI ID rejected */
    *(volatile uint32_t *)(ecam_arena + cfg_off + 0x10) = 0x00000004U;
    *(volatile uint32_t *)(ecam_arena + cfg_off + 0x00) = 0x12345678U;
    assert_eq(0, aarch64_pcie_prepare(&ctx));
    assert_true(!aarch64_pcie_xhci_bar0_ready(&pdev));
    assert_eq(-ENOENT, be->map_mmio(0, 0x8000000000ULL, 0x4000ULL, &va));

    /* Insufficient MMIO64 range (< 16 KiB) rejected */
    *(volatile uint32_t *)(ecam_arena + cfg_off + 0x00) = 0x000d1b36U;
    mock_topo.roots[0].ranges[0].size = 0x2000ULL;
    assert_eq(0, aarch64_pcie_prepare(&ctx));
    assert_true(!aarch64_pcie_xhci_bar0_ready(&pdev));

    /* Overlap with RAM rejected */
    struct test_efi_desc ram_overlap[2] = {
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x40000000u, .num_pages = 0x1000u },
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x8000000000ULL, .num_pages = 0x10u },
    };
    setup_fixture(&ctx, ram_overlap, 2u);
    mock_topo.root_count = 1;
    mock_topo.roots[0].valid = true;
    mock_topo.roots[0].kind = AARCH64_PCIE_CTRL_ECAM_GENERIC;
    mock_topo.roots[0].domain = 0;
    mock_topo.roots[0].bus_start = 0;
    mock_topo.roots[0].bus_end = 1;
    mock_topo.roots[0].reg_cpu_base = 0x10000000ULL;
    mock_topo.roots[0].reg_size = 0x200000ULL;
    mock_topo.roots[0].range_count = 1;
    mock_topo.roots[0].ranges[0] = (struct aarch64_pcie_range){
        .space = PCI_SPACE_MMIO64,
        .pci_base = 0x8000000000ULL,
        .cpu_base = 0x8000000000ULL,
        .size = 0x10000000ULL,
    };
    assert_eq(0, aarch64_pcie_prepare(&ctx));
    assert_true(!aarch64_pcie_xhci_bar0_ready(&pdev));
>>>>>>> 8bfcfa31 (feat(aarch64): boot USB mass storage on QEMU PCI xHCI)
}

int main(void)
{
    TEST_SUITE("aarch64_pcie_ecam");
    run_isolated(case_zero_roots_graceful_fallback);
    run_isolated(case_m0_memory_map_bounds);
    run_isolated(case_ram_pa_limit_and_partial_conflict);
    run_isolated(case_ecam_access_ranges_and_errors);
    run_isolated(case_ecam_interrupt_map_routing);
    run_isolated(case_ecam_mmio_window_bounds_and_isolation);
    run_isolated(case_ecam_gicv2m_msix);
    run_isolated(case_scratch_probe_va_outside_pcie_device_windows);
    run_isolated(case_ecam_xhci_high_bar0_mapping);
    TEST_RESULTS();
    return __test_stats.failed ? 1 : 0;
}
