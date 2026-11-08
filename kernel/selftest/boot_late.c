/* kernel/selftest/boot_late.c — late boot selftests for x86_64.
 *
 * Part of BOOT-5 scheduler and services decoupling.
 */

#include <selftest/boot_late.h>

#ifdef OS01_SELFTEST
#include <core/printk.h>
#include <selftest/result.h>

extern void sched_boot_probe_create(void);
extern int test_kernel_mutex(void);
extern int test_kthread_self_reap(void);
extern int test_sched_boot_probe(void);
extern int test_init_pid(void);
extern int test_fd_refcount(void);
extern int test_bcache_smp_coherence(void);
extern int test_pgrp_signal(void);
extern int test_tty_vintr(void);
extern int test_x86_intr(void);

void selftest_boot_creation_probe(void)
{
    sched_boot_probe_create();
}

void selftest_boot_run_late(void)
{
    selftest_record_late("kernel_mutex", test_kernel_mutex());
    selftest_record_late("kthread_self_reap", test_kthread_self_reap());
    selftest_record_late("sched_boot_probe", test_sched_boot_probe());
    selftest_record_late("init_pid", test_init_pid());
    selftest_record_late("fd_refcount", test_fd_refcount());
    selftest_record_late("bcache_smp_coherence", test_bcache_smp_coherence());
    selftest_record_late("pgrp_signal", test_pgrp_signal());
    selftest_record_late("tty_vintr", test_tty_vintr());
    selftest_record_late("x86_intr", test_x86_intr());

    selftest_end_run();
    serial_printk("[selftest] task tests done\n");
}
#endif /* OS01_SELFTEST */
