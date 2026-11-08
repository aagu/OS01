/* hosttests/cases/test_boot_contract_sequence.c — BOOT-4 Task 3.
 *
 * Unit tests verifying the unified kernel_main boot orchestration sequence:
 *   - spec §3 total order of boot stages and checkpoints;
 *   - failure at ARCH_BOOT_CHECK_DEVICES halts without entering FS or SMP;
 *   - failure at fs_boot_prepare halts without entering SMP;
 *   - failure at pty_init halts without entering SMP;
 *   - failure at ARCH_BOOT_CHECK_PRE_SMP halts without calling arch_boot_aps;
 *   - failure at ARCH_BOOT_CHECK_RUNTIME halts without calling kernel_start_userspace;
 *   - normal completion reaches user space or kernel idle.
 */

#include "test_framework.h"
#include <core/bootinfo.h>
#include <arch/boot.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifndef OS01_KERNEL_SRC
#define OS01_KERNEL_SRC "."
#endif

/* Tracing execution */
#define MAX_TRACE 64
static char trace_events[MAX_TRACE][64];
static int trace_count = 0;

static void trace_record(const char *name)
{
    if (trace_count < MAX_TRACE) {
        snprintf(trace_events[trace_count], sizeof(trace_events[trace_count]), "%s", name);
        trace_count++;
    }
}

static void trace_reset(void)
{
    trace_count = 0;
    memset(trace_events, 0, sizeof(trace_events));
}

static int trace_find(const char *name)
{
    for (int i = 0; i < trace_count; i++) {
        if (strcmp(trace_events[i], name) == 0)
            return i;
    }
    return -1;
}

/* Control flags for fault injection */
static int fail_checkpoint_devices = 0;
static int fail_checkpoint_pre_smp = 0;
static int fail_checkpoint_runtime = 0;
static int fail_fs_prep = 0;
static int fail_pty_init = 0;

/* Mock boot_failure_stop & jump */
static jmp_buf g_test_exit_jmp;
static char last_stop_stage[32];
static char last_stop_comp[64];
static int64_t last_stop_rc = 0;

void boot_failure_stop(const char *stage, const char *component,
                       int64_t rc, int phase, int cpu)
{
    (void)phase; (void)cpu;
    snprintf(last_stop_stage, sizeof(last_stop_stage), "%s", stage ? stage : "");
    snprintf(last_stop_comp, sizeof(last_stop_comp), "%s", component ? component : "");
    last_stop_rc = rc;
    trace_record("boot_failure_stop");
    longjmp(g_test_exit_jmp, 1);
}

void boot_failure_warn(const char *stage, const char *component,
                       int64_t rc, int phase, int cpu)
{
    (void)stage; (void)component; (void)rc; (void)phase; (void)cpu;
}

/* Mock logger */
int g_log_level = 6;
void _log_info_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }

void kpanic(const char *fmt, ...)
{
    (void)fmt;
    trace_record("kpanic");
    longjmp(g_test_exit_jmp, 5);
}

/* Mock hooks */
void arch_early_panic(const char *msg)
{
    (void)msg;
    trace_record("arch_early_panic");
    longjmp(g_test_exit_jmp, 2);
}

void arch_early_init(const struct boot_context *bootctx)
{
    (void)bootctx;
    trace_record("arch_early_init");
}

void arch_boot_memory(const struct boot_context *bootctx)
{
    (void)bootctx;
    trace_record("arch_boot_memory");
}

void arch_pre_subsys_selftest(const struct boot_context *bootctx)
{
    (void)bootctx;
    trace_record("arch_pre_subsys_selftest");
}

void arch_boot_controllers(const struct boot_context *bootctx)
{
    (void)bootctx;
    trace_record("arch_boot_controllers");
}

void arch_boot_percpu(void)
{
    trace_record("arch_boot_percpu");
}

void arch_boot_devices(const struct boot_context *bootctx)
{
    (void)bootctx;
    trace_record("arch_boot_devices");
}

void random_init(const struct boot_context *bootctx)
{
    (void)bootctx;
    trace_record("random_init");
}

int fs_boot_prepare(void)
{
    trace_record("fs_boot_prepare");
    return fail_fs_prep ? -EIO : 0;
}

int pci_devfs_register_all(void)
{
    trace_record("pci_devfs_register_all");
    return 0;
}

int usb_devfs_register_all(void)
{
    trace_record("usb_devfs_register_all");
    return 0;
}

int pty_init(void)
{
    trace_record("pty_init");
    return fail_pty_init ? -ENOMEM : 0;
}

int arch_boot_device_nodes(void)
{
    trace_record("arch_boot_device_nodes");
    return 0;
}

void fs_boot_mounts(void)
{
    trace_record("fs_boot_mounts");
}

int tty_boot_init(void)
{
    trace_record("tty_boot_init");
    return 0;
}

void fs_boot_probe_devfs(void)
{
    trace_record("fs_boot_probe_devfs");
}

void arch_boot_smp_prepare(void)
{
    trace_record("arch_boot_smp_prepare");
}

void arch_boot_aps(void)
{
    trace_record("arch_boot_aps");
}

void arch_boot_runtime(void)
{
    trace_record("arch_boot_runtime");
}

void arch_post_smp_selftest(void)
{
    trace_record("arch_post_smp_selftest");
}

void sched_boot_creation_begin(void)
{
    trace_record("sched_boot_creation_begin");
}

void futex_init(void)
{
    trace_record("futex_init");
}

void console_init(void)
{
    trace_record("console_init");
}

void kernel_start_userspace(void)
{
    trace_record("kernel_start_userspace");
    longjmp(g_test_exit_jmp, 10);
}

void arch_start_kernel_idle(void)
{
    trace_record("arch_start_kernel_idle");
    longjmp(g_test_exit_jmp, 3);
}

void arch_cpu_halt(void)
{
    trace_record("arch_cpu_halt");
    longjmp(g_test_exit_jmp, 4);
}

uint64_t arch_cycle_counter(void)
{
    return 0xCAFEBABE12345678ULL;
}

int arch_boot_validate(enum arch_boot_checkpoint point, const char **component)
{
    switch (point) {
    case ARCH_BOOT_CHECK_DEVICES:
        trace_record("validate_devices");
        if (fail_checkpoint_devices) {
            if (component) *component = "devices_fault";
            return -ENODEV;
        }
        return 0;
    case ARCH_BOOT_CHECK_PRE_SMP:
        trace_record("validate_pre_smp");
        if (fail_checkpoint_pre_smp) {
            if (component) *component = "pre_smp_fault";
            return -EINVAL;
        }
        return 0;
    case ARCH_BOOT_CHECK_RUNTIME:
        trace_record("validate_runtime");
        if (fail_checkpoint_runtime) {
            if (component) *component = "runtime_fault";
            return -EIO;
        }
        return 0;
    default:
        if (component) *component = "checkpoint";
        return -EINVAL;
    }
}

unsigned long __stack_chk_guard = 0;

/* Include production kernel/core/main.c */
#define kernel_main production_kernel_main
#include "../../../kernel/core/main.c"
#undef kernel_main

static struct boot_context make_valid_bootctx(void)
{
    struct boot_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.magic = BOOT_CONTEXT_MAGIC;
    ctx.version = BOOT_CONTEXT_VERSION;
    ctx.size = sizeof(ctx);
    return ctx;
}

/* ── Test cases ────────────────────────────────────────────── */

/* 1. Normal full sequence order */
static void test_normal_sequence_order(void)
{
    trace_reset();
    fail_checkpoint_devices = 0;
    fail_checkpoint_pre_smp = 0;
    fail_checkpoint_runtime = 0;
    fail_fs_prep = 0;
    fail_pty_init = 0;

    struct boot_context ctx = make_valid_bootctx();
    if (setjmp(g_test_exit_jmp) == 0) {
        production_kernel_main(&ctx);
    }

    /* Verify complete order */
    int idx_early = trace_find("arch_early_init");
    int idx_mem = trace_find("arch_boot_memory");
    int idx_preselftest = trace_find("arch_pre_subsys_selftest");
    int idx_ctrl = trace_find("arch_boot_controllers");
    int idx_percpu = trace_find("arch_boot_percpu");
    int idx_dev = trace_find("arch_boot_devices");
    int idx_val_dev = trace_find("validate_devices");
    int idx_rand = trace_find("random_init");
    int idx_fs = trace_find("fs_boot_prepare");
    int idx_smp_prep = trace_find("arch_boot_smp_prepare");
    int idx_val_smp = trace_find("validate_pre_smp");
    int idx_aps = trace_find("arch_boot_aps");
    int idx_run = trace_find("arch_boot_runtime");
    int idx_val_run = trace_find("validate_runtime");
    int idx_postselftest = trace_find("arch_post_smp_selftest");
    int idx_task = trace_find("kernel_start_userspace");

    assert_true(idx_early >= 0);
    assert_true(idx_mem > idx_early);
    assert_true(idx_preselftest > idx_mem);
    assert_true(idx_ctrl > idx_preselftest);
    assert_true(idx_percpu > idx_ctrl);
    assert_true(idx_dev > idx_percpu);
    assert_true(idx_val_dev > idx_dev);
    assert_true(idx_rand > idx_val_dev);
    assert_true(idx_fs > idx_rand);
    assert_true(idx_smp_prep > idx_fs);
    assert_true(idx_val_smp > idx_smp_prep);
    assert_true(idx_aps > idx_val_smp);
    assert_true(idx_run > idx_aps);
    assert_true(idx_val_run > idx_run);
    assert_true(idx_postselftest > idx_val_run);
    assert_true(idx_task > idx_postselftest);
}

/* 2. Failure at devices checkpoint halts before FS/SMP */
static void test_failure_at_devices_checkpoint(void)
{
    trace_reset();
    fail_checkpoint_devices = 1;
    fail_checkpoint_pre_smp = 0;
    fail_checkpoint_runtime = 0;

    struct boot_context ctx = make_valid_bootctx();
    if (setjmp(g_test_exit_jmp) == 0) {
        production_kernel_main(&ctx);
    }

    assert_str_eq("subsys", last_stop_stage);
    assert_str_eq("devices_fault", last_stop_comp);
    assert_eq(-ENODEV, last_stop_rc);

    assert_true(trace_find("validate_devices") >= 0);
    assert_true(trace_find("fs_boot_prepare") < 0);
    assert_true(trace_find("arch_boot_smp_prepare") < 0);
    assert_true(trace_find("arch_boot_aps") < 0);
}

/* 3. Failure at fs_boot_prepare halts before SMP */
static void test_failure_at_fs_prep(void)
{
    trace_reset();
    fail_checkpoint_devices = 0;
    fail_fs_prep = 1;

    struct boot_context ctx = make_valid_bootctx();
    if (setjmp(g_test_exit_jmp) == 0) {
        production_kernel_main(&ctx);
    }

    assert_str_eq("fs", last_stop_stage);
    assert_str_eq("devfs", last_stop_comp);
    assert_eq(-EIO, last_stop_rc);

    assert_true(trace_find("fs_boot_prepare") >= 0);
    assert_true(trace_find("arch_boot_smp_prepare") < 0);
    assert_true(trace_find("arch_boot_aps") < 0);
}

/* 4. Failure at pty_init halts before SMP */
static void test_failure_at_pty_init(void)
{
    trace_reset();
    fail_checkpoint_devices = 0;
    fail_fs_prep = 0;
    fail_pty_init = 1;

    struct boot_context ctx = make_valid_bootctx();
    if (setjmp(g_test_exit_jmp) == 0) {
        production_kernel_main(&ctx);
    }

    assert_str_eq("pty", last_stop_stage);
    assert_str_eq("ptmx", last_stop_comp);
    assert_eq(-ENOMEM, last_stop_rc);

    assert_true(trace_find("pty_init") >= 0);
    assert_true(trace_find("arch_boot_smp_prepare") < 0);
    assert_true(trace_find("arch_boot_aps") < 0);
}

/* 5. Failure at pre-smp checkpoint halts before arch_boot_aps */
static void test_failure_at_pre_smp_checkpoint(void)
{
    trace_reset();
    fail_checkpoint_devices = 0;
    fail_fs_prep = 0;
    fail_pty_init = 0;
    fail_checkpoint_pre_smp = 1;

    struct boot_context ctx = make_valid_bootctx();
    if (setjmp(g_test_exit_jmp) == 0) {
        production_kernel_main(&ctx);
    }

    assert_str_eq("subsys", last_stop_stage);
    assert_str_eq("pre_smp_fault", last_stop_comp);
    assert_eq(-EINVAL, last_stop_rc);

    assert_true(trace_find("validate_pre_smp") >= 0);
    assert_true(trace_find("arch_boot_aps") < 0);
}

/* 6. Failure at runtime checkpoint halts before kernel_start_userspace */
static void test_failure_at_runtime_checkpoint(void)
{
    trace_reset();
    fail_checkpoint_devices = 0;
    fail_checkpoint_pre_smp = 0;
    fail_checkpoint_runtime = 1;

    struct boot_context ctx = make_valid_bootctx();
    if (setjmp(g_test_exit_jmp) == 0) {
        production_kernel_main(&ctx);
    }

    assert_str_eq("subsys", last_stop_stage);
    assert_str_eq("runtime_fault", last_stop_comp);
    assert_eq(-EIO, last_stop_rc);

    assert_true(trace_find("validate_runtime") >= 0);
    assert_true(trace_find("kernel_start_userspace") < 0);
}

int main(void)
{
    TEST_SUITE("BOOT-4 test_boot_contract_sequence");

    test_normal_sequence_order();
    test_failure_at_devices_checkpoint();
    test_failure_at_fs_prep();
    test_failure_at_pty_init();
    test_failure_at_pre_smp_checkpoint();
    test_failure_at_runtime_checkpoint();

    TEST_RESULTS();
    return 0;
}
