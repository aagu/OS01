#ifndef OS01_AARCH64_USER_TASK_H
#define OS01_AARCH64_USER_TASK_H

/* AArch64 M4 real user tasks.
 *
 * A user task is a PF_PROCESS scheduler task with its own private mm
 * (the image's user root), its own kernel stack and its own exception
 * frame.  It enters EL0 through the shared restore path and comes back
 * through the same path, so the kernel and the user share one stack
 * discipline: the live EL0 frame lives at the top of the task's kernel
 * stack and every kernel-side frame lives below it.
 *
 * Scope of this increment: single-core BSP, ASID 0, static in-memory
 * ELF images, cooperative scheduling, no TLS, no FP/SIMD.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <arch/regs.h>

struct task_struct;
struct aarch64_user_image;

/* One entry of the kernel-provided, immutable image catalog.  The bytes
 * and the length stay valid for the whole life of the task; the
 * catalog is referenced, never copied. */
struct aarch64_user_elf_source {
    const void *bytes;
    size_t size;
};

/* The per-task owner record.  It holds the image, the catalog
 * reference and the private state of one real EL0 user task; the task's
 * thread context keeps a pointer to it. */
struct aarch64_user_task {
    struct aarch64_user_image *image;
    const struct aarch64_user_elf_source *catalog;
    size_t catalog_count;
    size_t catalog_index;
};

/* Spawn a real EL0 user task from the in-memory catalog.
 *
 * `index` selects the catalog entry; the whole range is checked before
 * anything is constructed.  Returns the new PID, or a negative errno
 * (-EINVAL for a bad argument, -ENOEXEC for a rejected image,
 * -ENOMEM for a resource shortage, or the runtime-gate errno). */
int aarch64_user_task_spawn(const struct aarch64_user_elf_source *catalog,
                            size_t count, size_t index, const char *name);

/* Runtime initialisation: remember the kernel TTBR0 root and disable
 * the FP/SIMD trap so user code cannot touch FP state.  Called from the
 * BSP handoff before the scheduler publishes its readiness. */
void aarch64_user_runtime_init(void);

/* Install the address space of the task that is about to run.  Called
 * from the switch path with local interrupts masked, before the next
 * task's switch tail.  A kernel task or the idle task gets the saved
 * kernel root back. */
void aarch64_user_switch_mm(const struct task_struct *next);

/* True iff `tsk` is a real EL0 user task owned by this runtime. */
bool aarch64_user_task_is_user(const struct task_struct *tsk);

/* Admission check for a lower-EL IRQ frame that claims to come from a
 * real user task: the CPU must be the BSP, the scheduler must be ready,
 * the saved mode must be EL0t, the current task must be a real user
 * task, and the root actually installed in TTBR0_EL1 must be that
 * task's root. */
bool aarch64_user_frame_is_valid(const struct pt_regs *regs);

/* The address of the live EL0 exception frame of a user task: the top
 * of its kernel stack minus one pt_regs frame. */
uint64_t aarch64_user_task_frame(const struct task_struct *tsk);

/* Clear the two EL0 thread-pointer registers.  The first increment has
 * no TLS, so every user entry starts from a clean slate. */
void aarch64_user_clear_tls(void);

/* Hand a prepared pt_regs frame to the shared restore path.  The
 * frame address goes in x0; the helper moves the stack pointer onto
 * the frame and ERETs into EL0 with the frame's saved PSTATE.  Never
 * returns. */
__attribute__((noreturn)) void aarch64_el0_restore(uint64_t frame_addr);

/* The M4 internal SVC selector.  The immediate travels in
 * ESR_EL1.ISS[15:0] for an SVC taken from AArch64 EL0. */
#define AARCH64_USER_SVC_YIELD 0x4d40
#define AARCH64_USER_SVC_EXEC  0x4d41
#define AARCH64_USER_SVC_EXIT  0x4d42

/* ESR_EL1.EC of an SVC taken from AArch64 EL0. */
#define A64_EC_SVC_LOWER 0x15

/* ── Low-EL synchronous routing ─────────────────────────────────
 * The lower-EL sync vector routes every frame through
 * aarch64_el0_sync_route().
 *
 *   true  — the frame may be restored: the caller resumes EL0 with
 *           the saved frame.
 *   false — the legacy M4 probe ended its run: the caller unwinds the
 *           probe's stack frame instead of resuming EL0.
 *
 * A frame that matches neither the probe nor a real user task is an
 * unrecognised entry and stops the kernel — it must never borrow the
 * probe's continuation. */
bool aarch64_el0_sync_route(pt_regs_t *regs, uint64_t esr, uint64_t far);

/* The user-task SVC/fault handler.  A normal return hands the frame
 * back to the shared restore path; the exit branch never returns. */
void aarch64_user_task_sync(pt_regs_t *regs, uint64_t esr, uint64_t far);

/* Publish, install and track a user root.  The previous user root is
 * revoked only after the new one is installed. */
void aarch64_user_install_root(uint64_t root_pa);

/* Install the kernel root again and revoke the live user root. */
void aarch64_user_restore_kernel_root(void);

#endif /* OS01_AARCH64_USER_TASK_H */
