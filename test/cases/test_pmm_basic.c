/*
 * test/cases/test_pmm_basic.c — Physical Memory Manager unit tests
 */
#include "test_framework.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define PAGE_2M_SHIFT  21
#define PAGE_2M_SIZE (1UL << PAGE_2M_SHIFT)
#define TOTAL_PAGES 128
#define ZONE_NORMAL (1 << 1)

typedef struct Page {
    uint64_t phy_address;
    uint64_t attribute;
    uint64_t reference_count;
    struct Zone *zone;
    uint64_t total_pages_link;
} Page_t;

typedef struct Zone {
    uint64_t page_free_count;
    uint64_t page_using_count;
    uint64_t zone_start_address;
    uint64_t zone_end_address;
} Zone_t;

static Page_t pages[TOTAL_PAGES];
static uint64_t bits[(TOTAL_PAGES + 63) / 64];
static Zone_t zone;

static void setup(void) {
    memset(pages, 0, sizeof(pages));
    memset(bits, 0x00, sizeof(bits)); /* all pages free */
    memset(&zone, 0, sizeof(zone));
    zone.zone_start_address = 0;
    zone.zone_end_address = TOTAL_PAGES * PAGE_2M_SIZE;
    zone.page_free_count = TOTAL_PAGES;
    for (int i = 0; i < TOTAL_PAGES; i++) {
        pages[i].phy_address = (uint64_t)i * PAGE_2M_SIZE;
        pages[i].zone = &zone;
    }
}

static Page_t *alloc_one(void) {
    for (int i = 0; i < TOTAL_PAGES; i++) {
        uint64_t word_idx = (unsigned)i / 64;
        uint64_t bit_idx = (unsigned)i % 64;
        if (!(bits[word_idx] & (1UL << bit_idx))) {
            bits[word_idx] |= (1UL << bit_idx);
            zone.page_using_count++;
            zone.page_free_count--;
            pages[i].attribute = 1;
            return &pages[i];
        }
    }
    return NULL;
}

TEST_FUNC(test_pmm_alloc_one) {
    setup();
    assert_eq(zone.page_free_count, TOTAL_PAGES);
    Page_t *p = alloc_one();
    assert_not_null(p);
    assert_eq(zone.page_using_count, 1);
    assert_eq(zone.page_free_count, TOTAL_PAGES - 1);
}

TEST_FUNC(test_pmm_alloc_distinct) {
    setup();
    Page_t *p1 = alloc_one();
    Page_t *p2 = alloc_one();
    assert_not_null(p1);
    assert_not_null(p2);
    assert_true(p1 != p2);
}

TEST_FUNC(test_pmm_alloc_all) {
    setup();
    int count = 0;
    while (alloc_one()) count++;
    assert_eq(count, TOTAL_PAGES);
    assert_eq(zone.page_free_count, 0);
    assert_eq(alloc_one(), NULL); /* no more */
}

TEST_FUNC(test_pmm_bits_map) {
    setup();
    assert_eq(bits[0], 0UL);  /* all free initially */
    Page_t *p = alloc_one();   /* alloc page 0 */
    assert_not_null(p);
    assert_true(bits[0] & 1);  /* bit 0 set */
    assert_eq(bits[0], 1UL);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_pmm_alloc_one),
    TEST_ENTRY(test_pmm_alloc_distinct),
    TEST_ENTRY(test_pmm_alloc_all),
    TEST_ENTRY(test_pmm_bits_map),
TEST_LIST_END

int main() {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
