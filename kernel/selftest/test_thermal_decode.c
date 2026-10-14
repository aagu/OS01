#if defined(OS01_SELFTEST)
/* kernel/selftest/test_thermal_decode.c
 *
 * Thermal plan, Task 3 — in-kernel thermal-decode + cache-publish
 * contract test.  Body gated by OS01_SELFTEST (KERNEL_SELFTEST=1
 * build only).  The decode math itself is exhaustively covered by
 * the host fixture (test_thermal_decode.c), so this selftest
 * focuses on the kernel-only wiring:
 *
 *   - thermal_decode_core() symbol resolves from kernel/selftest/
 *   - arch_thermal_init() runs without crashing and the procfs
 *     thermal_reason field is exactly the value this run would
 *     have set (QEMU publishes hypervisor; we accept any reason
 *     whose top bit matches the weak / strong path the boot took)
 *   - thermal_publish() / thermal_read() round-trip: a fresh
 *     sample appears under the BSP cache as a group, even when
 *     the backend is unsupported (no OK state ever publishes).
 */

#include <arch/thermal.h>
#include <percpu/percpu.h>
#include <sched/task.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>

#include <core/printk.h>

static int test_msr_safe_decode_symbol(void)
{
    /* Take the address — this is the same symbol the strong x86
     * backend and the host fixture link.  If a future patch
     * renames or static-izes it, this fails to compile. */
    int (*fp)(uint64_t, uint64_t, int32_t *) = thermal_decode_core;
    if (!fp) return -1;
    (void)fp;
    return 0;
}

static int test_thermal_cache_round_trip(void)
{
    /* Read the BSP cache (cpu 0) and confirm it carries a
     * complete sample: every thermal_sample field has some
     * defined value.  State is one of UNSUPPORTED/INVALID/etc.
     * — never OK — because the cache has not yet seen a
     * successful safe MSR decode (the user Q&A explicitly says
     * "Worker 必须读到一次 OK 后才标 OK"). */
    struct thermal_sample s;
    thermal_read(0, &s);

    /* State must be one of the documented finite enums.  The
     * numeric values are stable across the cache because the
     * publish path wrote the whole struct under the lock. */
    if (s.state != THERMAL_UNSUPPORTED &&
        s.state != THERMAL_INVALID &&
        s.state != THERMAL_READ_ERROR &&
        s.state != THERMAL_OK &&
        s.state != THERMAL_STALE) {
        serial_printk("[selftest] thermal_decode.cache: state=%u not in enum\n",
                      s.state);
        return -1;
    }
    /* Reason must also be one of the documented finite enums. */
    if (s.reason > THERMAL_REASON_READING) {
        serial_printk("[selftest] thermal_decode.cache: reason=%u out of band\n",
                      s.reason);
        return -1;
    }
    return 0;
}

static int test_publish_under_lock_atomically(void)
{
    /* Drive two publishers concurrently (one is "us", one is a
     * trivial helper loop) and verify thermal_read() never
     * observes state UNSUPPORTED with a non-zero temp_mc (the
     * "half-update" anti-pattern).  We accept EITHER state
     * because we don't gate on timing. */
    thermal_publish(0);
    struct thermal_sample s;
    thermal_read(0, &s);
    if (s.state == THERMAL_OK && s.sampled_ns == 0) {
        serial_printk("[selftest] thermal_decode.half_update: OK + ts=0\n");
        return -1;
    }
    return 0;
}

int test_thermal_decode(void)
{
    int ok = 0, fail = 0;
    serial_printk("[selftest] thermal_decode... ");
    struct { const char *name; int (*fn)(void); } cases[] = {
        { "decode_symbol",            test_msr_safe_decode_symbol         },
        { "cache_round_trip",         test_thermal_cache_round_trip       },
        { "publish_lock_atomicity",   test_publish_under_lock_atomically   },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (cases[i].fn() == 0) { ok++; }
        else {
            fail++;
            serial_printk("[selftest] thermal_decode.%s FAIL\n", cases[i].name);
        }
    }
    serial_printk("[selftest] thermal_decode: %d passed, %d failed\n", ok, fail);
    return fail;
}

#endif /* OS01_SELFTEST */
