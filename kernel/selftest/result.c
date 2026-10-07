/*
 * kernel/selftest/result.c — protocol-v1 publisher for the in-kernel
 * selftest suite (Task 9).
 *
 * This file owns the case registry (early + late) and the single
 * START / SELECT / BEGIN / terminal / END publisher described in
 * kernel/include/selftest/result.h.  selftest.c keeps the test bodies
 * and the registration list (selftest_register_builtin_tests); this
 * file keeps the protocol state machine so the two concerns stay
 * separable and the publisher stays free of subsystem dependencies.
 */

#include <selftest/result.h>
#include <core/printk.h>   /* serial_printk */
#include <string.h>        /* strcmp */

/* The registry is intentionally a fixed array — no allocator is
 * available at every point the coordinator runs (AArch64 declares its
 * selection before the slab is fully wired).  A compile-time overflow
 * is surfaced as a protocol failure, never a dropped case. */
#define SELFTEST_MAX_EARLY 48
#define SELFTEST_MAX_LATE  16

typedef struct {
    const char *id;
    selftest_fn fn;
} selftest_early_t;

typedef struct {
    const char *id;
    int         recorded;
} selftest_late_t;

static selftest_early_t early_table[SELFTEST_MAX_EARLY];
static int early_count;

static selftest_late_t late_table[SELFTEST_MAX_LATE];
static int late_count;

static int run_error;     /* any contract violation seen this run */
static int run_started;   /* selftest_begin_run() has run */
static int end_emitted;   /* guards "exactly one END" */
static int n_pass, n_fail, n_skip;

static selftest_early_t *find_early(const char *id)
{
    for (int i = 0; i < early_count; i++)
        if (strcmp(early_table[i].id, id) == 0)
            return &early_table[i];
    return 0;
}

static selftest_late_t *find_late(const char *id)
{
    for (int i = 0; i < late_count; i++)
        if (strcmp(late_table[i].id, id) == 0)
            return &late_table[i];
    return 0;
}

void selftest_register(const char *id, selftest_fn fn)
{
    if (early_count >= SELFTEST_MAX_EARLY) {
        serial_printk("[selftest] ERROR: early registry overflow at '%s'\n", id);
        run_error = 1;
        return;
    }
    if (find_early(id) || find_late(id)) {
        serial_printk("[selftest] ERROR: duplicate case id '%s'\n", id);
        run_error = 1;
        return;
    }
    early_table[early_count].id = id;
    early_table[early_count].fn = fn;
    early_count++;
}

void selftest_register_late(const char *id)
{
    if (late_count >= SELFTEST_MAX_LATE) {
        serial_printk("[selftest] ERROR: late registry overflow at '%s'\n", id);
        run_error = 1;
        return;
    }
    if (find_late(id) || find_early(id)) {
        serial_printk("[selftest] ERROR: duplicate case id '%s'\n", id);
        run_error = 1;
        return;
    }
    late_table[late_count].id = id;
    late_table[late_count].recorded = 0;
    late_count++;
}

void selftest_begin_run(unsigned int expected_late)
{
    /* Re-registration on a second call must not double the selection. */
    early_count = 0;
    late_count = 0;
    run_error = 0;
    run_started = 1;
    end_emitted = 0;
    n_pass = n_fail = n_skip = 0;

    selftest_register_builtin_tests();

    if (late_count != (int)expected_late) {
        serial_printk("[selftest] ERROR: expected %u late case(s), "
                      "registered %d\n", expected_late, late_count);
        run_error = 1;
    }

    /* START declares the selection size; every SELECT (early and late)
     * precedes the first BEGIN, as parse_v1 requires.  When a contract
     * error was detected we append one phantom SELECT that is never
     * begun, so the emitted trace is rejected rather than silently
     * under-selected. */
    int total = early_count + late_count + (run_error ? 1 : 0);
    serial_printk("[TEST] START v=1 suite=" SELFTEST_SUITE_NAME
                  " expected=%d\n", total);
    for (int i = 0; i < early_count; i++)
        serial_printk("[TEST] SELECT %s required=1\n", early_table[i].id);
    for (int i = 0; i < late_count; i++)
        serial_printk("[TEST] SELECT %s required=1\n", late_table[i].id);
    if (run_error)
        serial_printk("[TEST] SELECT __selftest_contract_error__ required=1\n");
}

int selftest_run_all(void)
{
    if (!run_started) {
        serial_printk("[selftest] ERROR: selftest_run_all before "
                      "selftest_begin_run\n");
        return 1;
    }

    int failed = 0;
    for (int i = 0; i < early_count; i++) {
        const char *id = early_table[i].id;
        serial_printk("[TEST] BEGIN %s\n", id);
        serial_printk("[selftest] %s... ", id);
        int rc = early_table[i].fn ? early_table[i].fn() : -1;
        if (rc == 0) {
            serial_printk("PASS\n");
            serial_printk("[TEST] PASS %s\n", id);
            n_pass++;
        } else {
            serial_printk("FAIL (%d)\n", rc);
            serial_printk("[TEST] FAIL %s reason=rc_%d\n", id, rc);
            n_fail++;
            failed++;
        }
    }
    /* Legacy diagnostic summary — no longer a success gate. */
    serial_printk("[selftest] %d total: %d passed, %d failed\n",
                  early_count, early_count - failed, failed);
    return failed;
}

void selftest_record_late(const char *id, int rc)
{
    selftest_late_t *e = find_late(id);
    if (!e) {
        serial_printk("[selftest] ERROR: unknown late case id '%s'\n", id);
        run_error = 1;
        return;
    }
    if (e->recorded) {
        serial_printk("[selftest] ERROR: duplicate late record for '%s'\n", id);
        run_error = 1;
        return;
    }
    e->recorded = 1;
    serial_printk("[TEST] BEGIN %s\n", id);
    if (rc == 0) {
        serial_printk("[TEST] PASS %s\n", id);
        n_pass++;
    } else {
        serial_printk("[TEST] FAIL %s reason=rc_%d\n", id, rc);
        n_fail++;
    }
}

int selftest_end_run(void)
{
    if (!run_started) {
        serial_printk("[selftest] ERROR: selftest_end_run before "
                      "selftest_begin_run\n");
        return 1;
    }
    if (end_emitted) {
        serial_printk("[selftest] ERROR: selftest_end_run called twice\n");
        run_error = 1;
        return 1;
    }
    end_emitted = 1;

    /* A declared late case that never ran is a failure: force it to a
     * terminal so the run cannot pass with an unfinished scheduled case. */
    for (int i = 0; i < late_count; i++) {
        if (!late_table[i].recorded) {
            serial_printk("[selftest] ERROR: late case '%s' never ran\n",
                          late_table[i].id);
            serial_printk("[TEST] BEGIN %s\n", late_table[i].id);
            serial_printk("[TEST] FAIL %s reason=late_case_not_run\n",
                          late_table[i].id);
            n_fail++;
            run_error = 1;
        }
    }

    int total = n_pass + n_fail + n_skip;
    serial_printk("[TEST] END suite=" SELFTEST_SUITE_NAME
                  " total=%d passed=%d failed=%d skipped=%d\n",
                  total, n_pass, n_fail, n_skip);
    return run_error ? 1 : 0;
}
