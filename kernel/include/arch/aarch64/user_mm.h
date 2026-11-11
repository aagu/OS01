#ifndef OS01_AARCH64_USER_MM_H
#define OS01_AARCH64_USER_MM_H

/* AArch64 M4 user address-space lifecycle (user-mm design §mm 生命周期
 * 和所有权).  A private user root owned by one mm_t: the kernel creates
 * the root at runtime, drives it through the M3 arch_vmm_* 4 KiB
 * entries (which recognise registered user roots via owns_root), and
 * destroys it only after every leaf is gone and the root is detached
 * from TTBR0 and unpublished.
 *
 * v1 scope: single-core BSP, ASID 0, normal-RAM 4 KiB pages.  The
 * holder serialises map/install/destroy from one BSP kernel context;
 * the ownership lock below only covers registration. */

#include <stdint.h>
#include <stdbool.h>

struct mm_struct;
typedef struct mm_struct mm_t;

/* Allocate an mm_t plus its private zeroed user root, register the
 * ownership record and set mm->pgdir to the root PA (physical address
 * stored in the pointer-typed field, per the M4 representation
 * contract).  *out is zeroed on entry and only written on success.
 * Returns 0, -EINVAL (out == NULL), -EAGAIN/-EOPNOTSUPP (runtime gate,
 * see aarch64_user_mm_runtime_check) or -ENOMEM (every allocation is
 * rolled back in reverse order). */
int aarch64_user_mm_create(mm_t **out);

/* Destroy an mm-owned user root.  Callers must first detach the root,
 * flush TLB and drop their leaf data pages.  Refuses NULL, init_mm and
 * unowned mms with -EINVAL.  If the root is installed or still
 * published, or a read-only scan finds any leaf (including unexpected
 * block mappings or PA-stashing invalid leaves), returns -EBUSY with
 * the tree, registration and mm fully intact.  On success the
 * registration is revoked, then every intermediate table and the root
 * page are freed, then the mm — returns 0.  Empty tables left behind
 * by failed table builds are reclaimed by the same second pass. */
int aarch64_user_mm_destroy(mm_t *mm);

/* True iff `root` — the direct-map root pointer exactly as passed as
 * pgdir to the arch_vmm_* entries — belongs to a live mm-owned user
 * root.  The ownership lock is released before returning; callers then
 * run their page-table operations unlocked. */
bool aarch64_user_mm_owns_root(const uint64_t *root);

/* True iff the owned root must not be written: it is still published
 * in the M3 install registry OR equals the live TTBR0_EL1 root.  The
 * -EBUSY reject path is legal with DAIF.I masked (SVC callback
 * context) — it never enters a page-table primitive. */
bool aarch64_user_mm_root_busy(const uint64_t *root);

#endif /* OS01_AARCH64_USER_MM_H */
