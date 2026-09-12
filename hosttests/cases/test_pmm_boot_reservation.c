/* Exercise real pmm_init + slab_init + alloc/free with a nonzero RAM base.
 * Only the firmware map and direct-map address translation are synthetic.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory/pmm.h>
#include <memory/memory.h>
#include <memory/slab.h>

uintptr_t test_direct_map_offset;
static uint64_t ram_base;
#define RAM_SIZE (64UL << 20)
int g_log_level = 3;
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
int color_printk(unsigned int fg, unsigned int bg, const char *fmt, ...)
{ (void)fg; (void)bg; (void)fmt; return 0; }
size_t pmm_arch_normalize(const struct boot_context *ctx, struct MEMORY_RANGE *out)
{
    (void)ctx;
    out[0] = (struct MEMORY_RANGE){ram_base, ram_base + RAM_SIZE, MEMORY_TYPE_RAM};
    return 1;
}
uint64_t pmm_arch_zone_split(void) { return UINT64_MAX; }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    ram_base = strtoull(argv[1], NULL, 0);
    uint64_t image_offset = strtoull(argv[2], NULL, 0);
    unsigned metadata_frames = strtoul(argv[3], NULL, 0);
    void *ram = aligned_alloc(PAGE_2M_SIZE, RAM_SIZE);
    CHECK(ram != NULL);
    memset(ram, 0, RAM_SIZE);
    test_direct_map_offset = (uintptr_t)ram - ram_base;
    PMMngr.start_brk = (uintptr_t)ram + image_offset;
    struct boot_context ctx = {
        .magic = BOOT_CONTEXT_MAGIC, .version = BOOT_CONTEXT_VERSION,
        .size = sizeof(ctx), .flags = BOOT_CONTEXT_HAS_MEMORY_MAP,
    };
    pmm_init(&ctx);
    /* Kernel image/metadata occupies the first RAM frame, including slot 0. */
    CHECK(PMMngr.bits_map[0] & 1);
    CHECK(PMMngr.pages_struct[0].reference_count == 1);
    CHECK(PMMngr.pages_struct[0].zone_struct->total_pages_link == metadata_frames + 8);
    if (metadata_frames == 2) CHECK(PMMngr.bits_map[0] & 2);
    for (size_t i = 0; i < 8; ++i) {
        struct Slab *slab = kmalloc_cache_size[i].cache_pool;
        uint64_t phys = Virt_To_Phy(slab->address);
        CHECK(slab->page->phy_address == phys);
        CHECK(Phy_to_2M_Page(phys) == slab->page);
        CHECK(Virt_To_2M_Page(slab->address) == slab->page);
        uint64_t index = (phys - ram_base) >> PAGE_2M_SHIFT;
        CHECK(PMMngr.bits_map[index >> 6] & (1UL << (index % 64)));
    }
    unsigned count = 0;
    struct Page *allocated[32];
    struct Page *page;
    while ((page = alloc_pages(ZONE_NORMAL, 1, 0))) {
        CHECK(count < 32);
        CHECK(page->phy_address >= ram_base + metadata_frames * PAGE_2M_SIZE);
        for (size_t i = 0; i < 8; ++i)
            CHECK(page->phy_address != Virt_To_Phy(kmalloc_cache_size[i].cache_pool->address));
        for (unsigned i = 0; i < count; ++i) CHECK(allocated[i] != page);
        allocated[count++] = page;
    }
    CHECK(count == 32 - metadata_frames - 8);
    for (unsigned i = 0; i < count; ++i) {
        /* ELF/VMM teardown uses the address-to-Page conversion before freeing. */
        CHECK(Phy_to_2M_Page(allocated[i]->phy_address) == allocated[i]);
        free_pages(Phy_to_2M_Page(allocated[i]->phy_address), 1);
    }
    CHECK(PMMngr.zones_struct[0].page_free_count == 32 - metadata_frames - 8);
    free(ram);
    printf("PMM boot/slab reservation passed, RAM base=%#lx, metadata frames=%u\n", ram_base, metadata_frames);
    return 0;
}
