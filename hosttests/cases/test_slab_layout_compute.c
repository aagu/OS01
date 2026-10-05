/*
 * test_slab_layout_compute.c — hosttest for kernel/memory/slab.c slab_layout_compute
 *
 * Compiles real slab.c against host; only the spinlock/irq/arch_cpu inline
 * asm paths are stubbed by hosttests/mock/test_platform.h. PAGE_2M_SIZE
 * comes from <memory/pmm.h> via slab.h.
 */
#include "test_framework.h"
#include <memory/slab.h>
#include <memory/pmm.h>
#include <stdint.h>

TEST_FUNC(test_meta_bytes_matches_spec_formula) {
    /* spec §3.2:
     *   meta = Σ_{i=0..15} [ sizeof(struct Slab) + 10*sizeof(long)
     *                      + align8(PAGE_2M / size_i / 8) + 10*sizeof(long) ]
     */
    struct slab_layout l = slab_layout_compute();
    uint64_t expected = 0;
    for (int i = 0; i < 16; i++) {
        uint64_t entries = (uint64_t)PAGE_2M_SIZE / kmalloc_cache_size[i].size;
        /* align8: round (entries/8) up to multiple of 8 bytes */
        uint64_t bm = ((entries / 8 + 7) / 8) * 8;
        expected += sizeof(struct Slab) + 10 * sizeof(long)
                  + bm + 10 * sizeof(long);
    }
    assert_eq(l.meta_bytes, expected);
    assert_eq(l.reserved_2m_pages, 8);
}

TEST_FUNC(test_layout_is_pure) {
    /* slab_layout_compute is pure: no globals touched, two calls equal */
    struct slab_layout a = slab_layout_compute();
    struct slab_layout b = slab_layout_compute();
    assert_eq(a.meta_bytes, b.meta_bytes);
    assert_eq(a.reserved_2m_pages, b.reserved_2m_pages);
}

TEST_FUNC(test_layout_positive) {
    struct slab_layout l = slab_layout_compute();
    assert_true(l.meta_bytes > 0);
    assert_true(l.reserved_2m_pages > 0);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_meta_bytes_matches_spec_formula),
    TEST_ENTRY(test_layout_is_pure),
    TEST_ENTRY(test_layout_positive),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
