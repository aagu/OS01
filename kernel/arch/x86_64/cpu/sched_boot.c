/* kernel/arch/x86_64/cpu/sched_boot.c — the x86 system startup that
 * used to live in kernel/sched/core.c's task_init()
 * (aarch64 EL1 scheduler plan Phase 2, Task 2 Step 3).
 *
 * The COMMON idle publication and scheduler start live in
 * kernel/sched/boot.c behind <sched/boot.h>; this file performs the
 * x86-only parts in their original order:
 *
 *   1. arch_task_init_platform() (TSS/IST programming)
 *   2. init_mm segment metadata
 *   3. sched_boot_publish_idle() (percpu_data[0].idle + cpu = 0 —
 *      exactly where the old task_init() set them, before the fd table
 *      and the user-init spawn)
 *   4. fd 0/1/2 on /dev/tty for the idle task
 *   5. spawn_user_task("/bin/init", NULL)
 *   5b. sched_boot_probe_create() (under OS01_SELFTEST) and net_lwip_init()
 *      run while the boot creation window is still open and BSP scheduler_ok == 0
 *   6. state = TASK_RUNNING, then sched_boot_start() (close the boot
 *      creation window, publish scheduler_ok — the window is cleared
 *      BEFORE scheduler_ok so no instant has both true)
 *   7. vfs_end_bootstrap()
 *   8. arch_thermal_start_worker()
 *   9. every existing x86 late selftest + the single selftest_end_run()
 *  10. the original hlt/schedule idle loop
 *
 * This TU is x86-only: it is not in the aarch64 source whitelist and the
 * aarch64 build must never link it.
 */

#include <sched/task.h>
#include <sched/internal.h>
#include <sched/boot.h>
#include <percpu/percpu.h>
#include <arch/spinlock.h>
#include <arch/irq.h>
#include <arch/thread.h>
#include <core/debug.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/slab.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <core/assert.h>
#include <core/printk.h>   // serial_printk
#include <selftest/result.h> // selftest_record_late / selftest_end_run
#include <kernel.h>
#include <fcntl.h>
#include <net/net.h>

void task_init(void)
{
    arch_task_init_platform();

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

    // init_task_union.task.list is pre-initialized as self-referencing
    // in INIT_TASK, so any tasks added before task_init() (e.g. AP idle tasks
    // from smp_boot_aps()) remain on the scheduler's list.  Do NOT call
    // list_init() here — it would orphan them.

    // BSP idle task pointer + cpu 0 publication (spec §6): the common
    // boot module publishes the scheduling-visible idle metadata.  It
    // must NOT reinitialize the task list or the CPU0 runqueue — boot
    // services are already queued on both.
    sched_boot_publish_idle();

    // ── Set up fd 0/1/2 on the idle task ─────────────────
    // These will be inherited by the first user task (init.elf).
    {
        files_t *files = files_alloc();
        if (files) {
            current->files = files;  // attach to idle task

            // All three fds go through /dev/tty:
            //   read  → keyboard (ASCII-translated scancodes)
            //   write → framebuffer (GTK window) + serial (terminal)
            vfs_node_t *tty = vfs_lookup("/dev/tty");
            if (tty) {
                file_t *f0 = file_alloc();
                if (f0) { f0->type = FD_DEV; f0->node = tty; f0->flags = O_RDWR;  fd_alloc(files, f0); }

                file_t *f1 = file_alloc();
                if (f1) { f1->type = FD_DEV; f1->node = tty; f1->flags = O_WRONLY; fd_alloc(files, f1); }

                file_t *f2 = file_alloc();
                if (f2) { f2->type = FD_DEV; f2->node = tty; f2->flags = O_WRONLY; fd_alloc(files, f2); }
            }
        }
    }

    // pid_counter starts at 1.  spawn_user_task("/bin/init", NULL) is the
    // first alloc_pid() consumer on the system (R1), so user init is
    // always PID 1 across all build variants.  user_init_task /
    // user_init_pid are also recorded on this first call.
    init_task_union.task.pgrp = 1;
    init_task_union.task.session = 1;
    int64_t init_pid = spawn_user_task("/bin/init", NULL);
    (void)init_pid;
    debug_task("init: spawned user-space init, pid=%d\n", (int)init_pid);

#ifdef OS01_SELFTEST
    // ── x86 UP boot-window probe (P5 Task 4 / spec §10.1 item 4) ──
    // Inside the legal pre-publication window (after spawn_user_task so
    // init gets PID 1, before sched_boot_start() closes the window and
    // publishes BSP scheduler_ok = 1).
    {
        extern void sched_boot_probe_create(void);
        sched_boot_probe_create();
    }
#endif

    // Bring up lwIP and create tcpip_thread after user init has claimed
    // PID 1, while the boot creation window is still open and BSP
    // scheduler_ok == 0 (R2: preserves UP CPU0 boot fallback and keeps
    // net_lwip_start()'s netif_add/dhcp_start loop non-preemptible on UP).
    net_lwip_init();

    // Activate the scheduler and enter the idle loop.
    // schedule() picks up the user init (PID 1) naturally.
    current->state = TASK_RUNNING;
    // sched_boot_start(): close the boot creation window FIRST, then
    // publish CPU0 scheduler_ok = 1, then return (Ruling P2-3).
    sched_boot_start();
    vfs_end_bootstrap();

    // Thermal plan Task 3: arch-neutral startup hook for any CPU-pinned
    // service worker (currently: BSP-pinned thermal core-DTS sampler).
    // Called exactly once per boot, after scheduler_ok=1 (kernel_thread
    // and schedule() must work) and before the idle loop drains the
    // scheduler for good.  The weak default returns 0 (no worker);
    // x86_64's strong backend returns -EIO/-ENOMEM on failure and the
    // hook can NEVER block boot.  We log rather than crash — the
    // /proc/thermal cache will surface READ_ERROR/WORKER_START instead.
    {
        extern int arch_thermal_start_worker(void);
        int wrc = arch_thermal_start_worker();
        if (wrc != 0) {
            log_warn("[sched] thermal worker start returned %d (cached "
                     "in thermal cache as READ_ERROR/WORKER_START)\n", wrc);
        }
    }

#ifdef OS01_SELFTEST
    // ── Kernel mutex selftest ────────────────────────────────
    // Runs before the idle loop so we can use kernel_thread +
    // schedule().  Two kernel threads increment a shared counter
    // under mutex_lock 1000 times each, verifying mutual exclusion.
    {
        extern int test_kernel_mutex(void);
        selftest_record_late("kernel_mutex", test_kernel_mutex());
    }
#endif

#ifdef OS01_SELFTEST
    // ── kthread self-reap selftest ───────────────────────────
    // Must run after scheduler_ok=1 so schedule() works.
    {
        extern int test_kthread_self_reap(void);
        selftest_record_late("kthread_self_reap", test_kthread_self_reap());
    }
#endif

#ifdef OS01_SELFTEST
    // ── boot-window probe selftest (P5 Task 4) ───────────────
    // The probe thread was created in task_init() (after spawn_user_task
    // and before sched_boot_start()) inside the legal pre-publication
    // window (CPU0 online, scheduler_ok == 0).  This case waits
    // (bounded, production schedule()) until its marker has ACTUALLY
    // executed under the scheduler, then verifies the runtime negative
    // placement calls are rejected now that the window is closed.
    // Execution order matches the registration order in selftest.c.
    {
        extern int test_sched_boot_probe(void);
        selftest_record_late("sched_boot_probe", test_sched_boot_probe());
    }
#endif

#ifdef OS01_SELFTEST
    // ── user init PID 1 invariant selftest ───────────────────
    // Verifies that /bin/init claimed PID 1 even with OS01_SELFTEST
    // boot-window probe and lwIP tcpip_thread enabled.
    {
        extern int test_init_pid(void);
        selftest_record_late("init_pid", test_init_pid());
    }
#endif

#ifdef OS01_SELFTEST
    // ── fd reference-protocol race test ─────────────────────
    // After scheduler_ok=1 (kernel_thread + schedule() work).
    // files_unpin is now a synchronous drop-to-zero → files_free.
    {
        extern int test_fd_refcount(void);
        selftest_record_late("fd_refcount", test_fd_refcount());
    }
#endif

#ifdef OS01_SELFTEST
    // ── pgrp signal selftest ─────────────────────────────────
    // After scheduler_ok=1 (kernel_thread + schedule() work).
    {
        extern int test_pgrp_signal(void);
        selftest_record_late("pgrp_signal", test_pgrp_signal());
    }
#endif

#ifdef OS01_SELFTEST
    // ── tty VINTR selftest ───────────────────────────────────
    // After scheduler_ok=1 (kernel_thread + schedule() work).
    {
        extern int test_tty_vintr(void);
        selftest_record_late("tty_vintr", test_tty_vintr());
    }
    // ── live x86 IRQ dispatch selftest ───────────────────────
    // Needs a second online CPU with ipi_ready (remote worker), the
    // running LAPIC timer on both CPUs and the installed IDT: runs
    // last so every earlier case has finished touching interrupts.
    {
        extern int test_x86_intr(void);
        selftest_record_late("x86_intr", test_x86_intr());
    }
    // All scheduled cases reported: emit the single END record.  A late
    // case that never ran is turned into a FAIL by selftest_end_run().
    selftest_end_run();
    serial_printk("[selftest] task tests done\n");
#endif

    // ── Idle loop ────────────────────────────────────────────
    // hlt pauses the CPU until the next interrupt (timer tick,
    // keyboard IRQ1, serial IRQ4).  The timer ISR sets
    // need_resched; ret_from_intr calls schedule() before iretq.
    // If a task was woken we switch to it; otherwise we loop
    // back to hlt.
    //
    // serial_poll() (IRQ fallback) moved to pit_handler — runs
    // at 100 Hz on every timer tick, so the serial input path
    // still has a fallback even if IOAPIC routing fails.
    while (1) {
        __asm__ __volatile__("hlt");
        if (this_cpu()->need_resched) {
            schedule();
            arch_local_irq_enable();
        }
    }
}
