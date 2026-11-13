#ifndef OS01_AARCH64_USER_IMAGE_H
#define OS01_AARCH64_USER_IMAGE_H

/* AArch64 M4 user image — a private, mm-owned address space built from
 * a memory-resident ET_EXEC file.
 *
 * The image owns:
 *   - one mm_t with its own user root (the mm's pgdir field holds the
 *     root PA, per the M4 representation contract),
 *   - nothing else: every ELF and stack leaf is owned by the mm's
 *     resident chain (user_vm_map_image_page), and the teardown
 *     releases each page after verifying its PA.
 *
 * The construction and the teardown both run from an EL1 thread
 * context with local interrupts enabled: the M3 arch_vmm_* write
 * entries require that, and the teardown must be able to make progress
 * while the pages are released.
 */

#include <stddef.h>
#include <stdint.h>

#include <arch/regs.h>     /* pt_regs_t */

struct mm_struct;
typedef struct mm_struct mm_t;

struct aarch64_user_image;

/* User stack: exactly one RW/NX page at [0x20400000, 0x20401000).
 * The page below it (0x203ff000) stays unmapped and acts as the guard
 * page — the ELF layout validator caps every segment end at
 * 0x203ff000, so nothing can legitimately live there. */
#define AARCH64_USER_STACK_BASE  UINT64_C(0x20400000)
#define AARCH64_USER_STACK_TOP   UINT64_C(0x20401000)
#define AARCH64_USER_STACK_GUARD UINT64_C(0x203ff000)

/* Initial EL0 PSTATE: EL0t (M = 0) with D/A/F masked and I enabled.
 * User code runs with interrupts on; the FP/SIMD trap stays masked
 * because the kernel explicitly disables FPEN. */
#define AARCH64_USER_TASK_PSTATE  UINT64_C(0x340)

/* Build an image from `bytes`/`size`.
 *
 * On success *out receives an owned image and the function returns 0.
 * On failure *out is NULL and nothing is owned; the error is -EINVAL
 * (bad argument), -ENOEXEC (rejected format or layout), -ENOMEM (no
 * memory for the image, its pages or its root) or the runtime-gate
 * errno (-EAGAIN / -EOPNOTSUPP) propagated from the mm runtime. */
int aarch64_user_image_load(const void *bytes, size_t size,
                             struct aarch64_user_image **out);

/* Build the empty fork shell: a new image record that copies the
 * parent's entry and owns a fresh, unpublished, empty mm (no VMA, no
 * resident record, no leaf).  user_vm_clone_fork() fills the mm.  On
 * failure *out is NULL and nothing is owned. */
int aarch64_user_image_fork_shell(const struct aarch64_user_image *parent,
                                  struct aarch64_user_image **out);

/* Tear an image down: the leaves are unmapped in reverse order, each
 * returned PA is verified against the manifest before the page is
 * released, and only then is the empty mm destroyed.  A root that is
 * still published or still installed, or a leaf whose PA does not match
 * the manifest, is a fatal invariant violation — the kernel stops
 * instead of releasing pages that may still be reachable. */
void aarch64_user_image_destroy(struct aarch64_user_image *image);

/* The mm that owns the image's user root. */
mm_t *aarch64_user_image_mm(const struct aarch64_user_image *image);

/* The physical address of the image's user root (what TTBR0_EL1 takes
 * and what the M3 publish registry records). */
uint64_t aarch64_user_image_root_pa(const struct aarch64_user_image *image);

/* Fill an exception frame with the image's initial state: a zeroed
 * register file, the ELF entry as the return address, the user stack
 * top as SP_EL0 and AARCH64_USER_TASK_PSTATE as the saved PSTATE. */
void aarch64_user_image_init_regs(const struct aarch64_user_image *image,
                                  pt_regs_t *regs);

#endif /* OS01_AARCH64_USER_IMAGE_H */
