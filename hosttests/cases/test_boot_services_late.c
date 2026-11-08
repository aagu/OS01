/* hosttests/cases/test_boot_services_late.c — BOOT-5 Task 3 late selftests test.
 *
 * Verifies kernel/selftest/boot_late.c:
 *   - selftest_boot_creation_probe() calls sched_boot_probe_create()
 *   - selftest_boot_run_late() runs the 9 late cases in exact sequence:
 *     kernel_mutex, kthread_self_reap, sched_boot_probe, init_pid,
 *     fd_refcount, bcache_smp_coherence, pgrp_signal, tty_vintr, x86_intr.
 *   - selftest_end_run() called exactly once at end.
 */

#include "test_framework.h"
#include <selftest/boot_late.h>
#include <string.h>

#define MAX_LATE_TRACE 32
static char late_trace[MAX_LATE_TRACE][32];
static int late_trace_count = 0;

static void record_late_trace(const char *name)
{
    if (late_trace_count < MAX_LATE_TRACE) {
        snprintf(late_trace[late_trace_count++], 32, "%s", name);
    }
}

/* Stubs for late cases */
static int probe_create_calls = 0;
void sched_boot_probe_create(void)
{
    probe_create_calls++;
}

int test_kernel_mutex(void) { record_late_trace("kernel_mutex"); return 0; }
int test_kthread_self_reap(void) { record_late_trace("kthread_self_reap"); return 0; }
int test_sched_boot_probe(void) { record_late_trace("sched_boot_probe"); return 0; }
int test_init_pid(void) { record_late_trace("init_pid"); return 0; }
int test_fd_refcount(void) { record_late_trace("fd_refcount"); return 0; }
int test_bcache_smp_coherence(void) { record_late_trace("bcache_smp_coherence"); return 0; }
int test_pgrp_signal(void) { record_late_trace("pgrp_signal"); return 0; }
int test_tty_vintr(void) { record_late_trace("tty_vintr"); return 0; }
int test_x86_intr(void) { record_late_trace("x86_intr"); return 0; }

static int end_run_calls = 0;
void selftest_record_late(const char *name, int result)
{
    (void)name; (void)result;
}
void selftest_end_run(void)
{
    end_run_calls++;
    record_late_trace("END");
}
void serial_printk(const char *fmt, ...) { (void)fmt; }

static void test_creation_probe(void)
{
    probe_create_calls = 0;
    selftest_boot_creation_probe();
    assert_eq(1, probe_create_calls);
}

static void test_run_late_sequence(void)
{
    late_trace_count = 0;
    end_run_calls = 0;

    selftest_boot_run_late();

    assert_eq(1, end_run_calls);
    assert_eq(10, late_trace_count);
    assert_str_eq("kernel_mutex", late_trace[0]);
    assert_str_eq("kthread_self_reap", late_trace[1]);
    assert_str_eq("sched_boot_probe", late_trace[2]);
    assert_str_eq("init_pid", late_trace[3]);
    assert_str_eq("fd_refcount", late_trace[4]);
    assert_str_eq("bcache_smp_coherence", late_trace[5]);
    assert_str_eq("pgrp_signal", late_trace[6]);
    assert_str_eq("tty_vintr", late_trace[7]);
    assert_str_eq("x86_intr", late_trace[8]);
    assert_str_eq("END", late_trace[9]);
}

int main(void)
{
    TEST_SUITE("BOOT-5 test_boot_services_late");

    test_creation_probe();
    test_run_late_sequence();

    TEST_RESULTS();
    return 0;
}
