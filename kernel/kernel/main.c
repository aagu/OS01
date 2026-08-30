#include <string.h>
#include <kernel/printk.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>
#include <kernel/arch/gate.h>
#include <kernel/arch/spinlock.h>
#include <kernel/arch/cpu.h>
#include <kernel/arch/irq.h>
#include <kernel/interrupt.h>
#include <kernel/task.h>
#include <kernel/percpu.h>
#include <kernel/smp.h>
#include <kernel/tty.h>
#include <kernel/apic.h>
#include <driver/serial.h>
#include <driver/keyboard.h>
#include <block/blockdev.h>
#include <fs/vfs.h>
#include <fs/fat.h>
#include <fs/gpt.h>
#include <fs/ext2.h>
#include <fs/devfs.h>
#include <fs/procfs.h>
#include <fs/tmpfs.h>
#include <kernel/selftest.h>
#include <kernel/futex.h>
#include <stdlib.h>
#include <kernel/subsys.h>
#include <kernel/arch/subsys.h>
#include <kernel/console.h>
#include <kernel/logo.h>
#include <kernel/fb.h>
#include <kernel/pty.h>
#include <kernel/clockevent.h>
#include <net/net.h>
#include <kernel/random.h>

// ── Stack canary ─────────────────────────────────────────────

extern char _text;
extern char _etext;
extern char _edata;
extern char _erodata;
extern char _end;

// ── Stack canary ─────────────────────────────────────────────
// Initial value is non-zero (defense-in-depth).  kernel_main()
// replaces it with arch_cycle_counter() as its first statement.
// ── Safe raw hex output (for __stack_chk_fail — only write_serial) ──
static void write_hex(uint64_t val)
{
    char b[17];
    int i;
    for (i = 15; i >= 0; i--) {
        int d = (int)(val & 0xf);
        b[i] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        val >>= 4;
    }
    b[16] = '\0';
    for (i = 0; i < 16; i++) write_serial(b[i]);
}

uint64_t __stack_chk_guard = 0xDEADBEEFCAFEBABE;

// ── Stack smashing handler (safety net, rarely called) ────
__attribute__((noreturn, no_stack_protector, cold))
void __stack_chk_fail(void)
{
    arch_local_irq_disable();
    {
        const char *p = "\n*** Kernel stack smashing detected ***\n";
        for (; *p; p++) write_serial(*p);
    }
    // Raw stack walk via RBP chain
    write_serial('>'), write_serial('>'), write_serial('>'), write_serial(' ');
    {
        uint64_t rbp_val;
        __asm__ __volatile__("movq %%rbp, %0" : "=r"(rbp_val));
        for (int fi = 0; fi < 10; fi++) {
            if (rbp_val < 0xffff800000000000ULL || rbp_val > 0xfffffffffffff000ULL)
                break;
            uint64_t ret_addr = *(volatile uint64_t *)(rbp_val + 8);
            write_hex(ret_addr); write_serial(' ');
            if (fi == 4) { write_serial('\n'); }
            rbp_val = *(volatile uint64_t *)rbp_val;
        }
        write_serial('\n');
    }
    task_t *t = get_current_task();
    if (t && (uint64_t)t >= 0xffff800000000000ULL) {
        const char *a = "pid=";
        for (; *a; a++) write_serial(*a);
        int64_t pid = t->pid;
        char b[21]; int i = 0;
        if (pid < 0) { write_serial('-'); pid = -pid; }
        if (pid == 0) { b[i++] = '0'; }
        while (pid > 0) { b[i++] = '0' + (char)(pid % 10); pid /= 10; }
        while (i > 0) write_serial(b[--i]);
        write_serial('\n');
    }
    while (1) arch_cpu_halt();
    __builtin_unreachable();
}

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

    pmm_init(&bootctx->memory);             // physical page allocator
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

    random_init();                      // seed the CSPRNG pool (BSP, once)

    vfs_init();                         // init mount table BEFORE any mount calls

    devfs_init();                   // mount /dev + register chrdev
                                    // ★ MUST be before any devfs_register_* call

    pty_init();                     // init PTY table + register /dev/ptmx

    static const struct devfs_ops keyboard_ops = {
        .read = keyboard_devfs_read,
        .poll = keyboard_poll_dev,
    };
    extern const struct devfs_ops fb_ops;
    devfs_register_chrdev("keyboard", NULL, &keyboard_ops);
    devfs_register_chrdev("fb", NULL, &fb_ops);

    // Register physical disks in /dev
    for (int i = 0; i < block_device_count(); i++) {
        block_device_t *dev = block_device_get(i);
        devfs_register_blkdev(dev->name, dev);
    }

    // Try GPT partition table scan
    gpt_info_t *gpt = (block_device_count() > 0)
                      ? gpt_scan(block_device_get(0)) : NULL;

    if (!gpt) {
        // Fallback: old single-FAT32 layout
        if (block_device_count() > 0) {
            block_device_t *dev = block_device_get(0);
            fat32_fs_t *fs = NULL;
            if (0 == fat32_init(dev, &fs))
                vfs_mount("/", dev, &fat_vfs_ops, fs);
        }
    } else {
        // Dual-partition layout:
        //   gpt->partitions[0] = hda1 (FAT32 ESP) → /boot
        //   gpt->partitions[1] = hda2 (ext2)      → /
        if (gpt->count >= 2) {
            ext2_fs_t *ext2_fs = NULL;
            fat32_fs_t *fat_fs = NULL;

            if (0 == ext2_init(gpt->partitions[1].dev, &ext2_fs))
                vfs_mount("/", gpt->partitions[1].dev, &ext2_vfs_ops, ext2_fs);
            else
                serial_printk("EXT2: mount failed — / not available\n");

            if (0 == fat32_init(gpt->partitions[0].dev, &fat_fs))
                vfs_mount("/boot", gpt->partitions[0].dev, &fat_vfs_ops, fat_fs);
            else
                serial_printk("FAT32: /boot mount failed\n");
        }
    }

    // /tmp → tmpfs (independent of disk)
    tmpfs_init();

    procfs_init();                  // /proc

    // ═══ 7. Console TTY ═════════════════════════════════════
    // console_putchar as output — routes all user-space writes
    // through the VT100 CSI terminal emulator.
    tty_t *console = tty_alloc(console_putchar, NULL);
    if (console) {
        serial_set_tty(console);         // serial IRQ → TTY
        keyboard_set_tty(console);       // keyboard IRQ → TTY
        tty_set_dev_tty(console);        // /dev/tty read/write → TTY
        serial_printk("tty: console TTY created\n");
    }

    // Register /dev/tty (magic → controlling terminal) and /dev/tty0
    // (direct physical console) AFTER keyboard_set_tty so that
    // keyboard_get_tty() returns the correct pointer for private_data.
    devfs_register_chrdev("tty",  keyboard_get_tty(), &tty_magic_ops);
    devfs_register_chrdev("tty0", keyboard_get_tty(), &tty_phys_ops);

    vfs_debug_list("/dev");

    // Quick smoke test: /dev/null
    vfs_node_t *nul = vfs_lookup("/dev/null");
    if (nul) {
        char c;
        int r = vfs_read(nul, 0, 1, &c);
        int w = vfs_write(nul, 0, 4, "test");
        serial_printk("devfs: /dev/null read=%d write=%d\n", r, w);
        vfs_node_put(nul);
    }

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
