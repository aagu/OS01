#include <kernel/bootinfo.h>
#include <string.h>
#include <kernel/printk.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>
#include <kernel/arch/x86_64/trap.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/asm.h>
#include <kernel/arch/x86_64/spinlock.h>
#include <kernel/interrupt.h>
#include <kernel/task.h>
#include <kernel/percpu.h>
#include <kernel/arch/x86_64/trampoline.h>
#include <kernel/tty.h>
#include <device/pic.h>
#include <kernel/apic.h>
#include <driver/pit.h>
#include <driver/serial.h>
#include <driver/keyboard.h>
#include <driver/ahci.h>
#include <block/blockdev.h>
#include <fs/vfs.h>
#include <fs/fat.h>
#include <fs/devfs.h>
#include <device/timer.h>
#include <stdlib.h>

extern char _text;
extern char _etext;
extern char _edata;
extern char _erodata;
extern char _end;

// ── /dev/fb write handler ──────────────────────────────────
// Writes characters one-by-one to the framebuffer via color_printk.
static int fb_dev_write(struct vfs_node *node, uint64_t offset,
                        uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;
    for (uint64_t i = 0; i < size; i++)
        color_printk(WHITE, BLACK, "%c", ((char *)buffer)[i]);
    return (int)size;
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
int kernel_main(struct BOOT_INFO *bootinfo)
{
    // ═══ 1. CPU + interrupt infrastructure ═══════════════════
    Pos.Phy_addr = (uint32_t *)bootinfo->Graphics_Info.FrameBufferBase;
    Pos.FB_length = bootinfo->Graphics_Info.FrameBufferSize;
    Pos.XResolution = bootinfo->Graphics_Info.HorizontalResolution;
    Pos.YResolution = bootinfo->Graphics_Info.VerticalResolution;
    spin_init(&Pos.lock);

    load_TR(8);
    set_tss64(0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7800, 0x7400,
              0, 0, 0, 0);

    sys_vector_install();      // syscall + exception IDT entries
    irq_install();             // IRQ 0x20–0x37 IDT entries

    // Serial: hardware init only (IER=0, no IRQ yet).
    init_serial();             // baud/line/FIFO — for serial_printk
    serial_printk("serial port init succeed\n");

    // EFER NXE — enable No-eXecute for user-space page tables
    uint32_t eax, edx;
    asm volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(0xC0000080));
    if (!(eax & (1 << 11))) {
        eax |= (1 << 11);
        asm volatile("wrmsr" :: "a"(eax), "d"(edx), "c"(0xC0000080));
        serial_printk("EFER: NXE enabled\n");
    }

    // ═══ 2. Memory subsystem ═════════════════════════════════
    PMMngr.start_code  = (uint64_t)&_text;
    PMMngr.end_code    = (uint64_t)&_etext;
    PMMngr.end_data    = (uint64_t)&_edata;
    PMMngr.end_rodata  = (uint64_t)&_erodata;
    PMMngr.start_brk   = (uint64_t)&_end;

    frame_buffer_early_init();
    color_printk(RED, BLACK, "Hello, World!\n");

    pmm_init(bootinfo->E820_Info);      // physical page allocator
    vmm_init();                          // virtual memory (page tables)
    frame_buffer_init();                 // remap FB at VIRT_FRAMEBUFFER_OFFSET
    color_printk(GREEN, BLACK, "frame buffer remap succeed\n");

    // ═══ 3. Interrupt controllers ════════════════════════════
    // APIC mode: LAPIC for local delivery, IOAPIC for external IRQs.
    // All IOAPIC redirection entries start masked.
    apic_init(bootinfo->RSDP);

    // Legacy PIC (i8259) — all IRQs masked; used only when !apic_available.
    pic_init();

    // ═══ 4. Timers ═══════════════════════════════════════════
    timer_init();                        // softirq timer list
    pit_init();                          // PIT IRQ0 at 100 Hz
    lapic_timer_init();                  // calibrate LAPIC timer vs PIT

    // ═══ 5. Device IRQs ═════════════════════════════════════
    keyboard_init();                     // PS/2: init + register IRQ1
    init_serial_irq();                   // COM1: register IRQ4 + IER=0x01

    // ═══ 6. Storage + filesystem ════════════════════════════
    ahci_init();

    vfs_init();
    if (block_device_count() > 0) {
        block_device_t *dev = block_device_get(0);
        fat32_fs_t *fs = fat32_mount(dev);
        if (fs)
            vfs_mount("/", dev, &fat_vfs_ops, fs);
    }
    devfs_init();                        // /dev: null, zero, serial, tty
    devfs_register_chrdev("keyboard", NULL, keyboard_devfs_read, NULL);
    devfs_register_chrdev("fb", NULL, NULL, fb_dev_write);

    // ═══ 7. Console TTY ═════════════════════════════════════
    // tty_alloc(NULL, NULL) uses default output → fb + serial dual-write.
    tty_t *console = tty_alloc(NULL, NULL);
    if (console) {
        serial_set_tty(console);         // serial IRQ → TTY
        keyboard_set_tty(console);       // keyboard IRQ → TTY
        devfs_set_tty(console);          // /dev/tty read/write → TTY
        serial_printk("tty: console TTY created\n");
    }

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

    smp_boot_aps();
    lapic_timer_start(100);              // per-CPU scheduling tick

    // ═══ 9. Scheduler + user-space init ═════════════════════
    task_init();                         // spawns /init.elf, enters idle loop

    // unreachable
    while (1) hlt();
    return 0;
}