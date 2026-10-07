#include <core/printk.h>      // serial_printk (still printed from kernel_main)
#include <core/bootinfo.h>    // struct boot_context (used by signature)
#include <memory/memory.h>
#include <arch/cpu.h>         // arch_cycle_counter, arch_cpu_halt
#include <sched/task.h>       // task_init
#include <driver/serial.h>    // write_serial
#include <fs/boot.h>          // fs_boot_prepare / _mounts / _probe_devfs
#include <tty/boot.h>         // tty_boot_init
#include <arch/x86_64/boot.h> // x86_64_boot_early/_memory/_subsystems/_device_nodes
#include <arch/x86_64/smp_boot.h> // x86_64_boot_percpu / _aps
#include <core/selftest.h>    // selftest_run_all
#include <selftest/result.h>  // protocol-v1 coordinator (begin/run/end)
#include <sync/futex.h>       // futex_init
#include <tty/console.h>      // console_init
#include <tty/pty.h>          // pty_init
#include <net/net.h>          // net_lwip_init
#include <random/random.h>    // random_init

// ── Kernel symbols ─────────────────────────────────────────

extern char _text;
extern char _etext;
extern char _edata;
extern char _erodata;
extern char _end;

// ── Stack canary (single source: kernel/core/stack_chk.c) ──
// kernel_main() replaces __stack_chk_guard with arch_cycle_counter() as
// its first statement (canary fail-closed if CSPRNG is not yet seeded).
// __stack_chk_fail itself lives in stack_chk.c per AAGU-4 spec §2.1.
extern unsigned long __stack_chk_guard;

#ifdef OS01_CANARY_SELFTEST
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
//    3. Interrupt controllers (APIC → IOAPIC → PIC)
//    4. Timers (PIT 100 Hz, LAPIC timer calibrated)
//    5. Device IRQ registration (keyboard, serial)
//    6. Storage + filesystem (AHCI, VFS, FAT, devfs)
//    7. Console TTY (connects IRQ input to shell stdin)
//    8. Per-CPU + SMP bringup
//    9. Scheduler + user-space init (/init.elf)
//
__attribute__((no_stack_protector))
int kernel_main(const struct boot_context *bootctx)
{
    // ═══ 0. Stack canary — MUST be the first statement ════════
    __stack_chk_guard = arch_cycle_counter() ^ 0xDEADBEEFCAFEBABE;

#ifdef OS01_CANARY_SELFTEST
    kernel_canary_selftest_trip();
    __builtin_unreachable();
#endif

    // ═══ 0.5. Handoff sanity — symmetric with aarch64_main ═══
    // init_serial() has not run yet, so report via write_serial directly.
    if (!boot_context_valid(bootctx)) {
        const char *p = "\n*** Corrupt UEFI handoff ***\n";
        for (; *p; p++) write_serial(*p);
        while (1) arch_cpu_halt();
    }

    // ═══ 1. CPU + interrupt infrastructure ═══════════════════
    x86_64_boot_early(bootctx);

    // ═══ 2. Memory subsystem ═════════════════════════════════
    x86_64_boot_memory(bootctx);

    // ═══ 3-6. Subsystem framework ══════════════════════════════════
    // arch_register_subsys() + subsys_init_all() dispatches:
    //   Phase 3: interrupt controllers (apic, pic)
    //   Phase 4: timers (timer, pit, lapic-timer)
    //   Phase 5: device IRQs (keyboard, serial)
    //   Phase 6: storage (ahci)
    x86_64_boot_subsystems(bootctx);

    // ═══ 8a. BSP per-CPU registration (Task 6) ═════════════════
    // x86_64_boot_percpu() owns MADT traversal, percpu_init(), BSP
    // TSS/GS/online, and num_cpus publication; it runs BEFORE the
    // filesystem phase so device/FS code sees a valid this_cpu().
    // Audit summary: num_cpus has exactly one writer (boot.c), GS is
    // installed before online=1, tlb_shootdown/ipi_broadcast only
    // target online CPUs (APs are offline until x86_64_boot_aps()),
    // and the slab lock is statically initialized.  See the Task 6
    // report in .superpowers/sdd/ for the full audit.
    x86_64_boot_percpu();

    random_init(bootctx);               // seed the CSPRNG pool (BSP, once)

    // ── FS bring-up + x86 device node registration (Task 2 split) ──
    // Order matters: devfs must exist before any devfs_register_*() call;
    // PTY must exist before keyboard_set_tty() (later in tty_boot_init())
    // can route input through the master fd; the x86 device nodes
    // (keyboard/mouse/fb) are registered after PTY so pty_init has a
    // clean view of devfs; the partition-mount + tmpfs + procfs come
    // last so user-space can see /boot, /, /tmp, /proc by the time
    // init.elf spawns.  TTY wiring and the /dev smoke probe are now
    // owned by tty_boot_init() and fs_boot_probe_devfs() (Task 3).
    fs_boot_prepare();

    pty_init();                     // init PTY table + register /dev/ptmx

    x86_64_boot_device_nodes();     // register keyboard/mouse/fb chrdevs

    fs_boot_mounts();               // block-device devfs + GPT/FAT32/ext2 mounts + tmpfs/procfs

    // ═══ 7. Console TTY + /dev smoke probe (Task 3 split) ═════
    // tty_boot_init() allocates the console TTY, wires serial/keyboard/
    // dev_tty to it (preserving the if(console) scope verbatim), then
    // registers /dev/tty and /dev/tty0.  fs_boot_probe_devfs() lists
    // /dev and runs the /dev/null read/write smoke test.  The split
    // mirrors the Task-2 partition between FS mount work and probe
    // work; the brief forbids calling fs_boot_probe_devfs() from
    // fs_boot_mounts().
    tty_boot_init();
    fs_boot_probe_devfs();

    // ═══ 8. Per-CPU + SMP ═══════════════════════════════════
    // x86_64_boot_percpu() owns MADT traversal, percpu_init(), BSP
    // TSS/GS/online, and num_cpus publication.  x86_64_boot_aps()
    // owns tick_start() → smp_boot_aps() → per-CPU subsystem
    // dispatch.  Both helpers live in kernel/arch/x86_64/smp/boot.c;
    // see <arch/x86_64/smp_boot.h> for the interface.
    //
    // Task 6 moved x86_64_boot_percpu() to just after
    // x86_64_boot_subsystems() (before random_init) — see the audit
    // note there.  x86_64_boot_aps() stays in place here.
    x86_64_boot_aps();

#ifdef OS01_SELFTEST
    serial_printk("[selftest] running built-in tests...\n");
    /* Declare the complete protocol-v1 selection before START: the
     * registered boot-time (early) cases plus the five scheduled cases
     * task_init() runs later.  selftest_run_all() executes the early
     * subset; task_init() records the late cases and emits the single
     * END via selftest_end_run(). */
    selftest_begin_run(5);
    selftest_run_all();
    serial_printk("[selftest] done\n");
#endif

    // ═══ Network stack init (post-SMP, pre-scheduler) ═══
    // lwIP creates kernel threads (tcpip_thread) — must happen
    // after SMP is up and before the scheduler starts.  The task
    // list is no longer reset by task_init() (INIT_TASK pre-initializes
    // .list as self-referencing), so tcpip_thread stays schedulable.
    net_lwip_init();

    futex_init();                        // init futex hash buckets

    // Initialize the software terminal cursor — called at the very end
    // of kernel init so Pos.YPosition won't change after this point.
    console_init();

    // ═══ 9. Scheduler + user-space init ═════════════════════
    task_init();                         // spawns /init.elf, enters idle loop

    // unreachable
    while (1) arch_cpu_halt();
    return 0;
}
