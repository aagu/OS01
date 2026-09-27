// kernel/arch/x86_64/platform/boot.c — Boot-time x86 device-node
// registration, extracted from kernel/core/main.c as part of the
// kernel-main refactor (Task 2).
//
// Owns x86_64_boot_device_nodes(); see kernel/include/arch/x86_64/boot.h
// for the contract.  This file lives under arch/x86_64/platform/ because
// the chrdevs registered here (keyboard, mouse, fb) are tied to x86-only
// drivers — see kernel/arch/x86_64/make.config for ARCH_PLATFORM_C_SOURCES.
//
// The mouse branch must remain gated on subsys_status("mouse") == 1;
// the i8042 PS/2 mouse subsystem is only present on machines where the
// QEMU -device ps2-mouse or equivalent is configured.  Without the
// gate, mouse_register would race into a half-initialised controller.
//
// Call order in kernel_main:  fs_boot_prepare(); pty_init();
//                             x86_64_boot_device_nodes(); fs_boot_mounts();
// TTY creation, /dev/tty and /dev/tty0 chrdev registration, /dev probe,
// and the devfs smoke test remain in kernel/core/main.c (Task 3 moves
// them into fs_boot_probe_devfs()).

#include <arch/x86_64/boot.h>

#include <driver/keyboard.h>
#include <driver/mouse.h>
#include <driver/fb.h>
#include <fs/devfs.h>
#include <log/log.h>
#include <subsys/subsys.h>

// /dev/keyboard — interrupt-driven PS/2 keyboard input stream
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
}
