// kernel/selftest/test_slab_selftest.c —
// M2 Task 6: exercise every one of the 16 kmalloc caches
// (kmalloc_cache_size[0..15], 32 B .. 1 MiB) with an
// alloc -> memset pattern -> verify -> free round trip, and check the
// per-cache total_using counter deltas on the way.
//
// Prints one line per cache and the final parser-asserted marker:
//   [selftest] slab: 16/16 PASS
//
// Registered from selftest_run_all() (see selftest.c); compiled into
// both aarch64 and x86_64 kernel builds. Guarded by OS01_SELFTEST so
// (no OS01_SELFTEST guard: the symbol must exist for the unconditional
// registration in selftest.c on both arches; it only RUNS in selftest builds).

#include <stdint.h>
#include <core/printk.h>
#include <memory/slab.h>
#include <string.h>

#define SLAB_PATTERN 0x5A

int test_slab_16_caches(void)
{
    int pass = 0;

    for (int i = 0; i < 16; i++) {
        struct Slab_Cache *cache = &kmalloc_cache_size[i];
        uint64_t size = cache->size;
        uint64_t using_before = cache->total_using;
        uint8_t *p = kmalloc(size);

        if (!p) {
            serial_printk("[selftest] slab: cache %d size=%lu FAIL (kmalloc NULL)\n",
                   i, (unsigned long)size);
            return -1;
        }

        memset(p, SLAB_PATTERN, size);
        for (uint64_t j = 0; j < size; j++) {
            if (p[j] != SLAB_PATTERN) {
                serial_printk("[selftest] slab: cache %d size=%lu FAIL "
                       "(pattern corrupt at %lu)\n",
                       i, (unsigned long)size, (unsigned long)j);
                kfree(p);
                return -1;
            }
        }

        if (cache->total_using != using_before + 1) {
            serial_printk("[selftest] slab: cache %d size=%lu FAIL "
                   "(total_using %lu -> %lu, want +1)\n",
                   i, (unsigned long)size,
                   (unsigned long)using_before,
                   (unsigned long)cache->total_using);
            kfree(p);
            return -1;
        }

        kfree(p);

        if (cache->total_using != using_before) {
            serial_printk("[selftest] slab: cache %d size=%lu FAIL "
                   "(total_using %lu after kfree, want %lu)\n",
                   i, (unsigned long)size,
                   (unsigned long)cache->total_using,
                   (unsigned long)using_before);
            return -1;
        }

        pass++;
        serial_printk("[selftest] slab: cache %d size=%lu OK\n",
               i, (unsigned long)size);
    }

    if (pass == 16) {
        serial_printk("[selftest] slab: 16/16 PASS\n");
        return 0;
    }
    serial_printk("[selftest] slab: %d/16 PASS\n", pass);
    return -1;
}

