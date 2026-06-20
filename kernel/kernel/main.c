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

timer_t * timer;

void test_timer(void * data __attribute__((unused)))
{
    color_printk(GREEN, BLACK, "test_timer\n");
}

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

int kernel_main(struct BOOT_INFO *bootinfo)
{
    Pos.Phy_addr = (uint32_t *)bootinfo->Graphics_Info.FrameBufferBase;
    Pos.FB_length = bootinfo->Graphics_Info.FrameBufferSize;
    Pos.XResolution = bootinfo->Graphics_Info.HorizontalResolution;
    Pos.YResolution = bootinfo->Graphics_Info.VerticalResolution;

    spin_init(&Pos.lock);

    load_TR(8);
    set_tss64(0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7800, 0x7400, 0, 0, 0, 0);
    sys_vector_install();
    irq_install();
    init_serial();
    serial_printk("serial port init succedd\n");
    // Enable NX (No-eXecute) for user-space page table entries
    uint32_t eax, edx;
    asm volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(0xC0000080));
    if (!(eax & (1 << 11))) {
        eax |= (1 << 11);  // NXE bit
        asm volatile("wrmsr" :: "a"(eax), "d"(edx), "c"(0xC0000080));
        serial_printk("EFER: NXE enabled\n");
    } else {
        serial_printk("EFER: NXE already set\n");
    }

    serial_printk("PMMgr: 0x%p\n", &PMMngr);
    unsigned long cr3 = (unsigned long)get_cr3() & (~ 0xffffUL);
    serial_printk("cr3 address: %#08x\n", cr3);

    frame_buffer_early_init();
    
    color_printk(RED, BLACK, "Hello, World!\n");

    PMMngr.start_code = (uint64_t)&_text;
    serial_printk("_text: 0x%p\n", &_text);
    PMMngr.end_code = (uint64_t)&_etext;
    serial_printk("_etext: 0x%p\n", &_etext);
    PMMngr.end_data = (uint64_t)&_edata;
    serial_printk("_edata: 0x%p\n", &_edata);
    PMMngr.end_rodata = (uint64_t)&_erodata;
    serial_printk("_erodata: 0x%p\n", &_erodata);
    PMMngr.start_brk = (uint64_t)&_end;
    serial_printk("_end: 0x%p\n", &_end);

    pmm_init(bootinfo->E820_Info);
    serial_printk("PMMgr.end_of_struct: 0x%p\n", PMMngr.end_of_struct);

    vmm_init();

    frame_buffer_init();
    color_printk(GREEN, BLACK, "frame buffer remap succeed\n");

    apic_init(bootinfo->RSDP);

    pic_init();
    timer_init();
    pit_init();
    lapic_timer_init();
    keyboard_init();

    ahci_init();

    // ── Initialize VFS and mount filesystem ──────────
    vfs_init();
    if (block_device_count() > 0) {
        block_device_t *dev = block_device_get(0);
        fat32_fs_t *fs = fat32_mount(dev);
        if (fs) {
            vfs_mount("/", dev, &fat_vfs_ops, fs);
        }
    }

    // ── Mount devfs at /dev and register device nodes ──
    devfs_init();

    // Register keyboard on devfs — raw scancode reads
    devfs_register_chrdev("keyboard", NULL,
                          keyboard_devfs_read, NULL);

    // Register /dev/fb — writes characters to the framebuffer
    devfs_register_chrdev("fb", NULL, NULL, fb_dev_write);

    vfs_debug_list("/dev");

    // Quick verification: /dev/null should be findable and work
    vfs_node_t *nul = vfs_lookup("/dev/null");
    if (nul) {
        serial_printk("devfs: /dev/null found (type=%u)\n", nul->type);
        char c;
        int r = vfs_read(nul, 0, 1, &c);
        int w = vfs_write(nul, 0, 4, "test");
        serial_printk("devfs: /dev/null read=%d write=%d\n", r, w);
        vfs_node_put(nul);
    }

    // timer = create_timer(test_timer, NULL, 100);;
    // add_timer(timer);

    // ── Initialize per-CPU subsystem ─────────────────
    // Walk the MADT-discovered LAPIC list and set up percpu_data[]
    // for every enabled processor.  Must happen before task_init() —
    // schedule() and __switch_to expect this_cpu() / cpu->tss to work.
    {
        uint32_t cpu_idx = 0;

        for (uint32_t i = 0; i < apic_info.lapic_count; i++) {
            if (!(apic_info.lapics[i].flags & 1))
                continue;  // not enabled

            if (cpu_idx >= NR_CPUS) {
                serial_printk("percpu: APIC id=%u DROPPED (NR_CPUS=%u full)\n",
                              apic_info.lapics[i].apic_id, (unsigned)NR_CPUS);
                continue;
            }

            percpu_init(cpu_idx, apic_info.lapics[i].apic_id);

            if (cpu_idx == 0) {
                // BSP — install GS base now so the scheduler works
                percpu_data[0].tss = &init_tss[0];
                percpu_install_gs(0);
                percpu_data[0].online = 1;
                // GS base set — BSP per-CPU data now accessible
                serial_printk("percpu: BSP  (cpu=%u, apic_id=%u) online\n",
                              cpu_idx, apic_info.lapics[i].apic_id);
            } else {
                // AP — will be brought online by smp_boot_aps() (Phase 2)
                serial_printk("percpu: AP   (cpu=%u, apic_id=%u) registered\n",
                              cpu_idx, apic_info.lapics[i].apic_id);
            }
            cpu_idx++;
        }

        serial_printk("percpu: %u CPU(s) registered (%u enabled in MADT)\n",
                      cpu_idx, apic_info.lapic_count);

        num_cpus = cpu_idx;
    }

    // ── Boot APs ───────────────────────────────────
    // Must happen before task_init() — APs enter idle loop
    // with scheduler_ok=0 and wait for BSP to set up scheduling.
    // After task_init(), APs set scheduler_ok=1 via
    // their own initialization path.
    smp_boot_aps();

    // Start per-CPU LAPIC timer on BSP after all APs are up.
    lapic_timer_start(100);

    task_init();

    serial_printk("kernel_main: after task_init, entering idle loop\n");
    while(1)
    {
        hlt();
    }
    return 0;
}