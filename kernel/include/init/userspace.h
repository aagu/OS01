#ifndef KERNEL_INIT_USERSPACE_H
#define KERNEL_INIT_USERSPACE_H

/* Public userspace initialization contract (spec §3 / §4).
 * Single entry point for userspace policy after boot runtime verification.
 * Only compiled and called when OS01_HAS_USERSPACE is set. */
void kernel_start_userspace(void) __attribute__((noreturn));

#endif /* KERNEL_INIT_USERSPACE_H */
