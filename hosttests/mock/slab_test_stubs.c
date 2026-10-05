/* test/mock/slab_test_stubs.c — host definition for slab hosttest.
 *
 * The kmalloc_cache_size[] array is declared extern in
 * kernel/include/memory/slab.h. The production definition lives in
 * kernel/memory/slab.c (which the hosttest does not compile). Provide
 * a 16-slot stub here so hosttests/cases/test_slab_layout_compute can
 * link without pulling in the full slab.c.
 *
 * Sizes match production: 32, 64, 128, 256, 512, 1024, 2048, 4096,
 *                         8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576.
 */
#include <memory/slab.h>

struct Slab_Cache kmalloc_cache_size[16] = {
    { .size = 32 },     { .size = 64 },     { .size = 128 },    { .size = 256 },
    { .size = 512 },    { .size = 1024 },   { .size = 2048 },   { .size = 4096 },
    { .size = 8192 },   { .size = 16384 },  { .size = 32768 },  { .size = 65536 },
    { .size = 131072 }, { .size = 262144 }, { .size = 524288 }, { .size = 1048576 },
};
