// kernel/arch/x86_64/platform/boot.c — Boot-time x86 helpers extracted
// from kernel/core/main.c as part of the kernel-main refactor (Tasks 2
// and 4).  Owns:
//
//   • x86_64_boot_device_nodes()  — register keyboard/mouse/fb chrdevs
//                                    (Task 2).
//   • x86_64_boot_early()         — framebuffer Pos + arch_task_init_early
//                                    + IDT (sys_vector + irq_install) +
//                                    serial + EFER NXE (Task 4).
//   • x86_64_boot_subsystems()    — arch_boot_rsdp + arch_register_subsys
//                                    + subsys_init_all phases 3-6 (Task 4).
//
// See kernel/include/arch/x86_64/boot.h for the public contracts.
//
// x86_64_boot_early() lives here (under platform/) because its payload is
// tightly bound to x86-only infrastructure: the EFER.NXE MSR, the IDT
// install helpers, and the 8250-style serial port.  arch_task_init_early()
// is a no-op on x86 (per arch/thread.h) — it remains in the call list to
// keep symmetry with aarch64, which uses it to set up TPIDR_EL1.
//
// x86_64_boot_subsystems() must run BEFORE any device driver that reads
// RSDP (acpi, ahci), which is why arch_boot_rsdp is assigned here, ahead
// of arch_register_subsys() and the subsys_init_all() walk that drains
// SUBSYS_INITCALL phase 3-6.
//
// The mouse branch in x86_64_boot_device_nodes() must remain gated on
// subsys_status("mouse") == 1; the i8042 PS/2 mouse subsystem is only
// present on machines where the QEMU -device ps2-mouse or equivalent is
// configured.  Without the gate, mouse_register would race into a
// half-initialised controller.
//
// Call order in kernel_main:
//   x86_64_boot_early(bootctx);
//   x86_64_boot_memory(bootctx);
//   x86_64_boot_subsystems(bootctx);
//   random_init(bootctx);          // stays in main.c
//   fs_boot_prepare();
//   pty_init();
//   x86_64_boot_device_nodes();
//   fs_boot_mounts();
//   tty_boot_init();
//   fs_boot_probe_devfs();
//   // per-CPU/AP boot

#include <arch/x86_64/boot.h>

#include <core/bootinfo.h>         /* struct boot_context (graphics,
                                      firmware fields)                   */
#include <core/printk.h>           /* Pos, frame_buffer_*                */
#include <driver/serial.h>         /* init_serial, serial_printk,
                                      write_serial                       */
#include <arch/cpu.h>              /* arch_cpu_enable_nx, arch_cpu_halt  */
#include <arch/thread.h>           /* arch_task_init_early               */
#include <arch/gate.h>             /* sys_vector_install                 */
#include <intr/interrupt.h>        /* irq_install                        */
#include <arch/x86_64/spinlock.h>   /* spin_init                          */
#include <arch/subsys.h>           /* arch_boot_rsdp, arch_register_subsys*/
#include <subsys/subsys.h>         /* subsys_init_all                    */
#include <device/boot.h>
#include <core/panic.h>

#include <driver/keyboard.h>
#include <driver/mouse.h>
#include <driver/fb.h>
#include <driver/gfx.h>
#include <fs/devfs.h>
#include <log/log.h>

// ── Stage 1 of 3: early CPU + interrupt infrastructure ───────
// Mirrors the legacy lines 90-108 of kernel_main(): record the firmware
// framebuffer base into Pos so the early printk layer can address it,
// then bring up the IDT (syscall + IRQ entries), bring up the serial
// port (hardware only — IER=0), and finally enable EFER.NXE so user
// pages can be marked no-execute.  All log lines and call ordering are
// preserved verbatim.
void x86_64_boot_early(const struct boot_context *bootctx)
{
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
}

// ── Stage 3 of 3: subsystem framework (phases 3-6) ───────────
// Mirrors the legacy lines 125-135 of kernel_main().  arch_boot_rsdp
// MUST be set before arch_register_subsys() because the registered
// phase-3 acpi initcall reads it.  subsys_init_all() then drains
// SUBSYS_INITCALL entries for:
//   Phase 3: interrupt controllers (apic, pic)
//   Phase 4: timers (timer, pit, lapic-timer)
//   Phase 5: device IRQs (keyboard, serial)
//   Phase 6: storage (ahci)
// (See kernel/include/subsys/subsys.h for the phase definitions.)
void x86_64_boot_subsystems(const struct boot_context *bootctx)
{
    // ═══ RSDP: 传递给 arch 子系统 ═══
    arch_boot_rsdp = bootctx->firmware.acpi_rsdp;

    arch_register_subsys();
    subsys_init_all();

    int dev_res = device_boot_result();
    if (dev_res != 0) {
        kpanic("device boot failed: %d\n", dev_res);
    }
}


// ── Task-2 helper: chrdev registration through devfs ─────────
static const struct devfs_ops keyboard_ops = {
    .read = keyboard_devfs_read,
    .poll = keyboard_poll_dev,
};

// /dev/mouse — interrupt-driven PS/2 mouse event ring (gated on
// subsys_status because the i8042 controller may not have a mouse port)
static const struct devfs_ops mouse_ops = {
    .read = mouse_devfs_read,
    .poll = mouse_poll_dev,
};

void x86_64_boot_device_nodes(void)
{
    devfs_register_chrdev("keyboard", NULL, &keyboard_ops);

    if (subsys_status("mouse") == 1 &&
        devfs_register_chrdev("mouse", NULL, &mouse_ops) != 0)
        log_err("mouse: failed to register /dev/mouse\n");

    // fb_ops is defined in kernel/driver/fb.c; no header declares it
    // (it's only consumed from this boot path).
    extern const struct devfs_ops fb_ops;
    devfs_register_chrdev("fb", NULL, &fb_ops);

    // /dev/gfx0 — bounded 2D present device (gfx 2D API plan, Task 2).
    // gfx_init spins up the 16-entry view table; gfx_ops provides
    // the open/ioctl_file/release_file callbacks.  Registered LAST
    // so the gfx0 slot index never shifts the other devices'
    // indices (subsys-gated mouse, fb, keyboard).
    gfx_init();
    if (devfs_register_chrdev("gfx0", NULL, &gfx_ops) != 0)
        log_err("gfx0: failed to register /dev/gfx0\n");
}
