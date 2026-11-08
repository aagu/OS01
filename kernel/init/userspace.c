/* kernel/init/userspace.c — public userspace startup orchestration.
 *
 * Part of BOOT-5 scheduler and services decoupling.
 */

#include <init/userspace.h>
#include <init/services.h>
#include <selftest/boot_late.h>
#include <selftest/boot_services_fixture.h>
#include <sched/task.h>
#include <sched/resource.h>
#include <sched/internal.h>
#include <sched/boot.h>
#include <sched/boot_stdio.h>
#include <arch/boot.h>
#include <arch/cpu.h>
#include <sync/futex.h>
#include <tty/console.h>
#include <core/boot_failure.h>
#include <core/printk.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <fs/vfs.h>
#include <selftest/boot_failure_fixture.h>

static void init_mm_setup(void)
{
    init_mm.start_code = PMMngr.start_code;
    init_mm.end_code = PMMngr.end_code;
    init_mm.start_data = (uint64_t)&_data;
    init_mm.end_data = PMMngr.end_data;
    init_mm.start_rodata = (uint64_t)&_rodata;
    init_mm.end_rodata = PMMngr.end_rodata;
    init_mm.start_brk = 0;
    init_mm.end_brk = PMMngr.start_brk;
    init_mm.start_stack = _stack_start;

    list_init(&init_mm.vma_list);
    init_mm.mmap_base = 0;
}

void kernel_start_userspace(void)
{
    // 1. Creation window begin, futex, console
    sched_boot_creation_begin();
    futex_init();
    console_init();

    // 2. Arch prepare, init_mm setup, publish idle
    arch_sched_boot_prepare();
    init_mm_setup();
    sched_boot_publish_idle();

    // 3. Stdio setup
    const char *failed_comp = NULL;
    int stdio_rc = sched_boot_prepare_stdio(&init_task_union.task, &failed_comp);
    if (stdio_rc != 0) {
        boot_failure_stop("stdio", failed_comp, stdio_rc, -1, -1);
    }
    boot3_test_sentinel("stdio-ready");

    // 4. End VFS bootstrap & emit marker
    vfs_end_bootstrap();
    log_info("[boot-5] vfs-runtime\n");

    // 5. Spawn user task init
    init_task_union.task.pgrp = 1;
    init_task_union.task.session = 1;
    boot3_test_spawn_enter();
    boot5_test_arm();
    int64_t init_pid = spawn_user_task(boot3_test_init_path(), NULL);
    boot3_test_spawn_leave();
    if (init_pid < 0) {
        boot_failure_stop("init", "/bin/init", init_pid, -1, -1);
    }
    if (init_pid != 1) {
        boot_failure_stop("init", "/bin/init", init_pid, 1, init_pid);
    }
    log_info("init: spawned user-space init, pid=%d\n", (int)init_pid);
    log_info("[boot-5] init-created pid=1\n");
    boot5_test_after_init();

    // 6. Selftest probe & boot services prepare (net)
    selftest_boot_creation_probe();
    boot_services_prepare();

    // 7. Sched boot start & runtime ready
    current->state = TASK_RUNNING;
    sched_boot_start();
    log_info("[boot-5] runtime-ready window=closed\n");

    // 8. Boot services start (thermal), late selftests, idle
    boot_services_start();
    selftest_boot_run_late();
    arch_sched_boot_idle();
}
