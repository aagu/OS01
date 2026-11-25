/* @os01-test
 * id: test_aarch64_virtio_mmio_map
 * layer: host
 * subsystem: memory
 * timing: strict
 * tier: nightly
 * link: aarch64-virtio-mmio-map
 * scenarios: [memory.aarch64.virtio_mmio_map]
 */
#include "test_framework.h"
#include <arch/virtio_mmio.h>
#include <arch/aarch64/virtio_mmio.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/boot_direct_map.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/handoff_layout.h>
#include <arch/aarch64/uefi_memory_map.h>
#include <arch/aarch64/vmm_gate.h>
#include <arch/mmu.h>
#include <core/bootinfo.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* Backing arena mapped at ARCH_PAGE_OFFSET (0x100000000) for page tables,
 * and a separate low backing below 2 GiB for raw UEFI memory map. */
static uint8_t *pt_arena;
static size_t pt_arena_used;
static const size_t pt_arena_size = 2u * 1024u * 1024u;

static uint8_t *mmap_low;
static uint64_t mock_root_pa;
static uint64_t *mock_root;
static unsigned ttbr1_read_count;

static struct aarch64_virtio_mmio_slot mock_slots[AARCH64_VIRTIO_MMIO_MAX];
static uint32_t mock_slot_count;

struct test_efi_desc {
    uint32_t type;
    uint32_t pad;
    uint64_t phys_start;
    uint64_t virt_start;
    uint64_t num_pages;
    uint64_t attribute;
};

/* Production stubs required by page_table.c / virtio_mmio.c */
uint64_t alloc_4k_page(void)
{
    if (!pt_arena || pt_arena_used + 0x1000u > pt_arena_size)
        return 0;
    uint64_t pa = pt_arena_used + 0x100000u; /* arbitrary PA in arena */
    memset(pt_arena + pt_arena_used, 0, 0x1000u);
    pt_arena_used += 0x1000u;
    return pa;
}

void free_4k_page(uint64_t pa)
{
    (void)pa;
}

uint64_t aarch64_read_ttbr1(void)
{
    ttbr1_read_count++;
    return mock_root_pa;
}

void tlb_invalidate_local(uint64_t va) { (void)va; }
void tlb_shootdown_all(uint64_t va) { (void)va; }

uint32_t dtb_virtio_mmio_count(void) { return mock_slot_count; }
const struct aarch64_virtio_mmio_slot *dtb_virtio_mmio_get(uint32_t i)
{
    if (i >= mock_slot_count)
        return NULL;
    return &mock_slots[i];
}

void log_err(const char *fmt, ...) { (void)fmt; }

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
    memcpy(mmap_low, descs, (size_t)desc_count * sizeof(*descs));

    aarch64_pt_init_locks();
    mock_root_pa = alloc_4k_page();
    mock_root = (uint64_t *)(uintptr_t)(mock_root_pa + ARCH_PAGE_OFFSET);
    ttbr1_read_count = 0;
    mock_slot_count = 0;

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

static void case_zero_slots_no_ttbr1(void)
{
    struct boot_context ctx;
    struct test_efi_desc descs[1] = {
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x40000000u, .num_pages = 0x1000u }
    };
    setup_fixture(&ctx, descs, 1u);
    assert_eq(0, aarch64_virtio_mmio_prepare(&ctx));
    assert_eq(0u, ttbr1_read_count);
}

static void case_merge_and_cross_block_and_smp_gate(void)
{
    struct boot_context ctx;
    struct test_efi_desc descs[1] = {
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x40000000u, .num_pages = 0x1000u }
    };
    uint64_t desc = 0;

    setup_fixture(&ctx, descs, 1u);
    mock_slot_count = 3u;
    mock_slots[0].base = UINT64_C(0x0a000000);
    mock_slots[1].base = UINT64_C(0x0a000200);
    mock_slots[2].base = UINT64_C(0x0a1fff00); /* crosses 0x0a200000 */

    assert_eq(0, aarch64_virtio_mmio_prepare(&ctx));
    assert_eq(0, aarch64_pt_read_l2_desc(mock_root, ARCH_PAGE_OFFSET + UINT64_C(0x0a000000), &desc));
    assert_eq(aarch64_pt_encode_block_desc(UINT64_C(0x0a000000), AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE, 0), desc);
    assert_eq(0, aarch64_pt_read_l2_desc(mock_root, ARCH_PAGE_OFFSET + UINT64_C(0x0a200000), &desc));
    assert_eq(aarch64_pt_encode_block_desc(UINT64_C(0x0a200000), AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE, 0), desc);

    /* After prepare, entering SMP gate and calling arch_virtio_mmio_map_early()
     * must return 0 without reading TTBR1 or touching page tables. */
    unsigned reads_before = ttbr1_read_count;
    smp_starting_enter();
    assert_eq(0, arch_virtio_mmio_map_early());
    assert_eq(reads_before, ttbr1_read_count);
}

static void case_idempotent_existing_device_block(void)
{
    struct boot_context ctx;
    struct test_efi_desc descs[1] = {
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x40000000u, .num_pages = 0x1000u }
    };
    setup_fixture(&ctx, descs, 1u);
    assert_eq(0, aarch64_pt_map_2m_block(mock_root, ARCH_PAGE_OFFSET + UINT64_C(0x0a000000),
                                         UINT64_C(0x0a000000),
                                         AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE));
    mock_slot_count = 1u;
    mock_slots[0].base = UINT64_C(0x0a000000);
    assert_eq(0, aarch64_virtio_mmio_prepare(&ctx));
}

static void case_conflicts_leave_descriptor_untouched(void)
{
    struct boot_context ctx;
    struct test_efi_desc descs[2] = {
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x40000000u, .num_pages = 0x1000u },
        { .type = AARCH64_UEFI_MEM_CONVENTIONAL, .phys_start = 0x0c000000u, .num_pages = 1u }, /* < 2 MiB RAM */
    };
    uint64_t desc = 0;

    /* 1. Normal block conflict */
    setup_fixture(&ctx, descs, 2u);
    assert_eq(0, aarch64_pt_map_2m_block(mock_root, ARCH_PAGE_OFFSET + UINT64_C(0x0a000000),
                                         UINT64_C(0x0a000000), AARCH64_PT_KERNEL_RW));
    uint64_t normal_desc = 0;
    assert_eq(0, aarch64_pt_read_l2_desc(mock_root, ARCH_PAGE_OFFSET + UINT64_C(0x0a000000), &normal_desc));
    assert_eq(AARCH64_PT_ECONFLICT, aarch64_pt_ensure_device_2m(mock_root, ARCH_PAGE_OFFSET + UINT64_C(0x0a000000), UINT64_C(0x0a000000)));
    assert_eq(0, aarch64_pt_read_l2_desc(mock_root, ARCH_PAGE_OFFSET + UINT64_C(0x0a000000), &desc));
    assert_eq(normal_desc, desc);

    /* 2. Wrong PA conflict */
    assert_eq(0, aarch64_pt_map_2m_block(mock_root, ARCH_PAGE_OFFSET + UINT64_C(0x0a200000),
                                         UINT64_C(0x0a400000), AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE));
    assert_eq(AARCH64_PT_ECONFLICT, aarch64_pt_ensure_device_2m(mock_root, ARCH_PAGE_OFFSET + UINT64_C(0x0a200000), UINT64_C(0x0a200000)));

    /* 3. L3 table conflict */
    assert_eq(0, aarch64_pt_map_4k(mock_root, ARCH_PAGE_OFFSET + UINT64_C(0x0a400000),
                                   UINT64_C(0x0a400000), AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE));
    assert_eq(AARCH64_PT_ECONFLICT, aarch64_pt_ensure_device_2m(mock_root, ARCH_PAGE_OFFSET + UINT64_C(0x0a400000), UINT64_C(0x0a400000)));

    /* 4. Non-empty software descriptor conflict (PROTNONE) */
    assert_eq(0, aarch64_pt_map_4k_new(mock_root, ARCH_PAGE_OFFSET + UINT64_C(0x0a600000),
                                       UINT64_C(0x0a600000), AARCH64_PT_KERNEL_RW, AARCH64_PT_SOFTWARE_PROTNONE));
    assert_eq(AARCH64_PT_ECONFLICT, aarch64_pt_ensure_device_2m(mock_root, ARCH_PAGE_OFFSET + UINT64_C(0x0a600000), UINT64_C(0x0a600000)));

    /* 5. Small (< 2 MiB) RAM overlap rejected */
    mock_slot_count = 1u;
    mock_slots[0].base = UINT64_C(0x0c000000);
    assert_true(aarch64_virtio_mmio_prepare(&ctx) < 0);
}

int main(void)
{
    TEST_SUITE("aarch64_virtio_mmio_map");
    run_isolated(case_zero_slots_no_ttbr1);
    run_isolated(case_merge_and_cross_block_and_smp_gate);
    run_isolated(case_idempotent_existing_device_block);
    run_isolated(case_conflicts_leave_descriptor_untouched);
    TEST_RESULTS();
    return __test_stats.failed ? 1 : 0;
}
