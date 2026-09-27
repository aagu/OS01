#ifndef _KERNEL_TTY_BOOT_H
#define _KERNEL_TTY_BOOT_H

// ── Boot-time console TTY wiring ─────────────────────────────
// Pulled out of kernel/core/main.c as part of the kernel-main refactor
// (Task 3).  Owns the early console TTY bring-up sequence:
//
//   1. Allocate a single console TTY with console_putchar as output.
//   2. Wire the three input sources (serial IRQ, keyboard IRQ, dev_tty
//      routing) to that TTY.
//   3. Register /dev/tty (controlling-terminal magic chrdev) and
//      /dev/tty0 (direct physical console chrdev).
//
// The `if (console) { ... }` scope is preserved verbatim — even if
// tty_alloc() returns NULL, the code still registers /dev/tty and
// /dev/tty0 (they will bind to NULL private_data, matching pre-refactor
// behavior).  The `tty: console TTY created` log line is emitted inside
// the same scope.
//
// Ordering contract from the brief:
//   fs_boot_mounts();    // (Task 2) block-device devfs + GPT/FAT32/ext2 mounts
//   tty_boot_init();     // (this file)
//   fs_boot_probe_devfs();  // (Task 3) /dev list + /dev/null smoke probe
//
// Must run after fs_boot_mounts() because /dev/tty0's registration depends
// on devfs existing.  Must run before fs_boot_probe_devfs() because the
// /dev debug-list and /dev/null smoke test run after the TTY node
// registration is in place.
void tty_boot_init(void);

#endif // _KERNEL_TTY_BOOT_H
