#ifndef OS01_AARCH64_EXEC_H
#define OS01_AARCH64_EXEC_H

/* AArch64 VFS-backed user program launch (M4-remaining product
 * userspace).  The arch-neutral declaration lives in
 * <sched/task.h> (spawn_user_task); this header pins the aarch64
 * implementation contract and the size bound for the transient ELF
 * buffer. */

#include <stdint.h>
#include <stddef.h>

/* A product user ELF is a static libc program; cap the transient read
 * buffer at 4 MiB — generous for init/sh, small enough that a corrupt
 * size field cannot exhaust the kernel heap. */
#define AARCH64_SPAWN_ELF_MAX_BYTES UINT64_C(0x400000)

/* Spawn a real EL0 user task from an ELF file in the VFS.
 *
 * Returns the new PID, or a negative errno (-ENOENT for a missing
 * path, -ENOEXEC for a rejected or oversized image, -ENOMEM /
 * -EAGAIN for resource or runtime-gate failures).  `argv` is the
 * kernel-side argv vector (argv[0] conventionally names the program
 * path); envp is NULL for the boot spawn. */
int64_t spawn_user_task(const char *path, const char *const *argv);

struct vfs_node;

/* Read the whole file behind `node` into a kmalloc'd buffer for image
 * construction (shared by spawn_user_task and the product
 * arch_syscall_exec path).  On success *out is a caller-owned buffer
 * of *out_size bytes; the node reference stays with the caller. */
int aarch64_exec_read_elf(struct vfs_node *node, void **out,
                          size_t *out_size);

#endif /* OS01_AARCH64_EXEC_H */
