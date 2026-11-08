// kernel/tty/boot.c — Boot-time console TTY wiring, extracted from
// kernel/core/main.c as part of the kernel-main refactor (Task 3).
//
// Owns tty_boot_init(); see kernel/include/tty/boot.h for the contract.
// Implementation mirrors the original 28 lines (kernel/core/main.c:159-174
// in the f500458 baseline) verbatim in call order, edge-case handling,
// and log strings.  The `if (console)` scope MUST be preserved verbatim
// — even if tty_alloc() returns NULL, the code still registers /dev/tty
// and /dev/tty0, with keyboard_get_tty() returning NULL as private_data
// (matching the pre-refactor behavior).
//
// Any intentional change belongs in a follow-up task with its own
// test/RED-first cycle; silent behavior changes are out of scope here.

#include <tty/boot.h>

#include <tty/tty.h>
#include <tty/console.h>
#include <driver/serial.h>
#include <driver/keyboard.h>
#include <fs/devfs.h>
#include <log/log.h>          // log_info for "tty: console TTY created"
#include <errno.h>

int tty_boot_register_devfs(tty_t *console)
{
    if (!console) {
        return -EINVAL;
    }

    // Register /dev/tty (magic → controlling terminal) and /dev/tty0
    // (direct physical console) with explicit console instance.
    int ret = devfs_register_chrdev("tty", console, &tty_magic_ops);
    if (ret != 0) {
        log_err("tty: failed to register /dev/tty (%d)\n", ret);
        return ret;
    }

    ret = devfs_register_chrdev("tty0", console, &tty_phys_ops);
    if (ret != 0) {
        log_err("tty: failed to register /dev/tty0 (%d)\n", ret);
        return ret;
    }

    return 0;
}

int tty_boot_init(void)
{
    // console_putchar as output — routes all user-space writes
    // through the VT100 CSI terminal emulator.
    tty_t *console = tty_alloc(console_putchar, NULL);
    if (!console) {
        return -ENOMEM;
    }

    int ret = tty_boot_register_devfs(console);
    if (ret != 0) {
        return ret;
    }

    serial_set_tty(console);         // serial IRQ → TTY
    keyboard_set_tty(console);       // keyboard IRQ → TTY
    tty_set_dev_tty(console);        // /dev/tty read/write → TTY
    log_info("tty: console TTY created\n");

    return 0;
}
