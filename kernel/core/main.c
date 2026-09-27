#include <string.h>
#include <core/printk.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <arch/gate.h>
#include <arch/spinlock.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <intr/interrupt.h>
#include <sched/task.h>
#include <percpu/percpu.h>
#include <core/smp.h>
#include <intr/apic.h>
#include <driver/serial.h>
#include <fs/boot.h>
#include <tty/boot.h>
#include <arch/x86_64/boot.h>
#include <core/selftest.h>
#include <sync/futex.h>
#include <stdlib.h>
#include <subsys/subsys.h>
#include <arch/subsys.h>
#include <tty/console.h>
#include <driver/logo.h>
#include <driver/fb.h>
#include <tty/pty.h>
#include <time/clockevent.h>
#include <net/net.h>
#include <random/random.h>

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
    Pos.Phy_addr = (uint32_t *)bootctx->graphics.FrameBufferBase;
    Pos.FB_length = bootctx->graphics.FrameBufferSize;
    Pos.XResolution = bootctx->graphics.HorizontalResolution;
    Pos.YResolution = bootctx->graphics.VerticalResolution;
    spin_init(&Pos.lock);

    arch_task_init_early();

    sys_vector_install();      // syscall + exception IDT entries
    irq_install();             // IRQ 0x20–0x37 IDT entries

    // Serial: hardware init only (IER=0, no IRQ yet).
    init_serial();             // baud/line/FIFO — for serial_printk
    serial_printk("serial port init succeed\n");

    // EFER NXE — enable No-eXecute for user-space page tables
    arch_cpu_enable_nx();
    serial_printk("EFER: NXE enabled\n");

    // ═══ 2. Memory subsystem ═════════════════════════════════
    PMMngr.start_code  = (uint64_t)&_text;
    PMMngr.end_code    = (uint64_t)&_etext;
    PMMngr.end_data    = (uint64_t)&_edata;
    PMMngr.end_rodata  = (uint64_t)&_erodata;
    PMMngr.start_brk   = (uint64_t)&_end;

    frame_buffer_early_init();
    boot_logo_show();                 // OS01 boot logo

    pmm_init(bootctx);                       // physical page allocator
    vmm_init();                          // virtual memory (page tables)
    frame_buffer_init();                 // remap FB at VIRT_FRAMEBUFFER_OFFSET
    color_printk(GREEN, BLACK, "frame buffer remap succeed\n");

    // ═══ RSDP: 传递给 arch 子系统 ═══
    arch_boot_rsdp = bootctx->firmware.acpi_rsdp;

    // ═══ 3-6. Subsystem framework ══════════════════════════════════
    // arch_register_subsys() + subsys_init_all() dispatches:
    //   Phase 3: interrupt controllers (apic, pic)
    //   Phase 4: timers (timer, pit, lapic-timer)
    //   Phase 5: device IRQs (keyboard, serial)
    //   Phase 6: storage (ahci)
    arch_register_subsys();
    subsys_init_all();

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
    {
        uint32_t cpu_idx = 0;
        for (uint32_t i = 0; i < apic_info.lapic_count; i++) {
            if (!(apic_info.lapics[i].flags & 1))
                continue;

            if (cpu_idx >= NR_CPUS) {
                serial_printk("percpu: APIC id=%u DROPPED (NR_CPUS=%u)\n",
                              apic_info.lapics[i].apic_id, (unsigned)NR_CPUS);
                continue;
            }

            percpu_init(cpu_idx, apic_info.lapics[i].apic_id);

            if (cpu_idx == 0) {
                percpu_data[0].tss = &init_tss[0];
                percpu_data[0].tss_hw = arch_task_boot_state();
                percpu_install_gs(0);
                percpu_data[0].online = 1;
                serial_printk("percpu: BSP  (cpu=%u, apic_id=%u) online\n",
                              cpu_idx, apic_info.lapics[i].apic_id);
            } else {
                serial_printk("percpu: AP   (cpu=%u, apic_id=%u) registered\n",
                              cpu_idx, apic_info.lapics[i].apic_id);
            }
            cpu_idx++;
        }
        serial_printk("percpu: %u CPU(s) registered (%u in MADT)\n",
                      cpu_idx, apic_info.lapic_count);
        num_cpus = cpu_idx;
    }

    // 显式启动 tick 源：GS base 已装（main.c percpu_install_gs(0)），
    // this_cpu() 可用。tick_start 先掩 PIT 再启 LAPIC，失败回退 PIT。
    tick_start();

    smp_boot_aps();

    // per-CPU 子系统二次 init
    arch_register_subsys_percpu();
    subsys_init_percpu();

#ifdef OS01_SELFTEST
    serial_printk("[selftest] running built-in tests...\n");
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
