#include <core/printk.h>      // serial_printk (still printed from kernel_main)
#include <core/bootinfo.h>    // struct boot_context (used by signature)
#include <core/boot_failure.h>
#include <core/panic.h>
#include <log/log.h>
#include <memory/memory.h>
#include <arch/cpu.h>         // arch_cycle_counter, arch_cpu_halt
#include <arch/boot.h>        // arch_* boot facade hooks and OS01_HAS_* feature gates
#include <random/random.h>    // random_init

#if OS01_HAS_VFS
#include <fs/boot.h>          // fs_boot_prepare / _mounts / _probe_devfs
#include <bus/pci/pci_devfs.h> // pci_devfs_register_all
#include <driver/usb_devfs.h> // usb_devfs_register_all
#include <tty/boot.h>         // tty_boot_init
#include <tty/pty.h>          // pty_init
#endif

#if OS01_HAS_USERSPACE
#include <sched/task.h>       // task_init
#include <sched/boot.h>       // sched_boot_creation_begin (Phase 2, Ruling P2-2)
#include <sync/futex.h>       // futex_init
#include <tty/console.h>      // console_init
#endif

#if defined(__x86_64__) && (defined(X86_INTR_FAULT_TEST_BP) || defined(X86_INTR_FAULT_TEST_NMI))
#include <arch/x86_64/intr_fault_probe.h>   // x86_intr_fault_probe_run
#endif

// ── Stack canary (single source: kernel/core/stack_chk.c) ──
// kernel_main() replaces __stack_chk_guard with arch_cycle_counter() as
// its first statement (canary fail-closed if CSPRNG is not yet seeded).
// __stack_chk_fail itself lives in stack_chk.c per AAGU-4 spec §2.1.
extern unsigned long __stack_chk_guard;

#if defined(__x86_64__) && defined(OS01_CANARY_SELFTEST)
__attribute__((noinline))
static void kernel_canary_selftest_trip(void)
{
    volatile char buffer[16];
    __asm__ __volatile__("" :: "r"(buffer) : "memory");
    __asm__ __volatile__("movq $0, -8(%%rbp)" ::: "memory");
}
#endif

// ═══════════════════════════════════════════════════════════════
//  Kernel init — called from head.S after bootloader handoff
// ═══════════════════════════════════════════════════════════════
//
//  Init phases (ordered by dependency):
//    1. CPU + interrupt infrastructure
//    2. Memory subsystem (PMM, VMM)
//    3. Interrupt controllers
//    4. Timers & per-CPU local structures
//    5. Device IRQ registration & subsystems
//    6. Storage + filesystem (AHCI, VFS, FAT, devfs) [if OS01_HAS_VFS]
//    7. Console TTY [if OS01_HAS_VFS]
//    8. Per-CPU + SMP bringup
//    9. Scheduler + user-space init (/init.elf) [if OS01_HAS_USERSPACE]
//
__attribute__((no_stack_protector))
int kernel_main(const struct boot_context *bootctx)
{
    // ═══ 0. Stack canary — MUST be the first statement ════════
    __stack_chk_guard = arch_cycle_counter() ^ 0xDEADBEEFCAFEBABE;

#if defined(__x86_64__) && defined(OS01_CANARY_SELFTEST)
    kernel_canary_selftest_trip();
    __builtin_unreachable();
#endif

    // ═══ 0.5. Handoff sanity ══════════════════════════════════
    if (!boot_context_valid(bootctx)) {
        arch_early_panic("corrupt UEFI handoff");
    }

    // ═══ 1. CPU + interrupt infrastructure ═══════════════════
    arch_early_init(bootctx);

    // ═══ 2. Memory subsystem ═════════════════════════════════
    arch_boot_memory(bootctx);

    // ═══ 2.5. Pre-subsystem selftest / fault probe ════════════
    arch_pre_subsys_selftest(bootctx);

    // ═══ 3. Subsystem framework / interrupt controller ═══════
    arch_boot_subsystems(bootctx);

    // ═══ 4. BSP per-CPU registration ══════════════════════════
    arch_boot_percpu();

    // ═══ 5. Seed the CSPRNG pool ═════════════════════════════
    random_init(bootctx);

#if defined(__x86_64__) && (defined(X86_INTR_FAULT_TEST_BP) || defined(X86_INTR_FAULT_TEST_NMI))
    x86_intr_fault_probe_run();
    for (;;) arch_cpu_halt();
#endif

#if OS01_HAS_VFS
    // ═══ 6. FS bring-up + device node registration ═══════════
    int fs_prep = fs_boot_prepare();
    if (fs_prep != 0)
        boot_failure_stop("fs", "devfs", fs_prep, -1, -1);

    pci_devfs_register_all();
    usb_devfs_register_all();

#if defined(CAPACITY_TEST) && CAPACITY_TEST
    void arch10_capacity_fixture_init(void);
    arch10_capacity_fixture_init();
#endif

    int pty_rc = pty_init();
    if (pty_rc != 0)
        boot_failure_stop("pty", "ptmx", pty_rc, -1, -1);

    if (arch_boot_device_nodes() != 0)
        kpanic("arch_boot_device_nodes failed\n");

    fs_boot_mounts();

    // ═══ 7. Console TTY + /dev smoke probe ════════════════════
    int tty_rc = tty_boot_init();
    if (tty_rc != 0)
        boot_failure_stop("tty", "console", tty_rc, -1, -1);
    fs_boot_probe_devfs();
#endif /* OS01_HAS_VFS */

    // ═══ 8. Per-CPU + SMP ════════════════════════════════════
    arch_boot_aps();

    // ═══ 8.5. Post-SMP selftest & probes ══════════════════════
    arch_post_smp_selftest();

#if OS01_HAS_USERSPACE
    // ═══ 9. Scheduler + user-space init ══════════════════════
    sched_boot_creation_begin();
    futex_init();
    console_init();
    task_init();
#else
    // ═══ Non-userspace architectures: enter kernel idle loop ══
    arch_start_kernel_idle();
#endif

    while (1) arch_cpu_halt();
    return 0;
}
