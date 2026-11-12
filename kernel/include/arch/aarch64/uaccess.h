#ifndef OS01_AARCH64_UACCESS_H
#define OS01_AARCH64_UACCESS_H

#include <arch/aarch64/regs.h>
#include <arch/aarch64/sync_fault.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── The FORMAL user-copy window and its EL1 fixup ──────────────
 *
 * A formal copy (copy_{to,from}_user_ft_res) runs with local IRQs
 * masked, from the current formal user task's mm, over a half-open
 * user range.  It registers the window below before touching user
 * memory and clears it before restoring the caller's IRQ state, so an
 * EL1 same-EL abort on one of the two unprivileged instructions can
 * be recognised and recovered to -EFAULT.
 *
 * Everything here is a plain value: the matcher below reads no
 * global, no system register and no lock, so a host case can drive it
 * with crafted inputs and the fixup can run on the early-reachable
 * chain.
 */

/* One exception snapshot plus the two user-pointer registers the
 * copy loop keeps: x1 holds the LDTRB address, x0 the STTRB address.
 * Both are captured from the exception frame at fault time. */
struct aarch64_uaccess_abort {
    uint64_t esr;
    uint64_t spsr;
    uint64_t pc;
    uint64_t far;
    uint64_t read_ptr;
    uint64_t write_ptr;
};

/* The window the copy wrapper registered.  `task`/`mm` are opaque
 * identities compared by pointer; `root_pa` is mm->pgdir at open
 * time. */
struct aarch64_uaccess_window {
    bool     present;
    bool     write;      /* true: copy_to_user (STTRB), false: copy_from_user */
    uint64_t lo;         /* half-open user range */
    uint64_t hi;
    const void *task;
    const void *mm;
    uint64_t root_pa;
};

/* The live facts at fault time.  `root_pa` is mm->pgdir now;
 * `ttbr0_pa` is what TTBR0_EL1 actually holds. */
struct aarch64_uaccess_live {
    const void *task;
    const void *mm;
    uint64_t root_pa;
    uint64_t ttbr0_pa;
};

/* The PURE shape rule of a formal copy abort — INPUTS ONLY.
 *
 * Returns true when the exception snapshot describes a recoverable
 * copy abort and the registered window agrees on direction, PC and
 * range:
 *
 *   - ESR.EC is the same-EL data abort (0x25),
 *   - the FSC is a translation/access-flag/permission fault,
 *   - neither ISS.S1PTW nor ISS.CM is set and ISS.FnV is clear,
 *   - the complete SPSR M[4:0] is EL1h (extra PSTATE bits are fine),
 *   - a window is registered and its direction equals ISS.WnR,
 *   - the faulting PC is exactly that direction's user instruction,
 *   - FAR equals the user pointer that instruction still holds and
 *     lies inside the registered half-open range.
 *
 * This predicate reads no runtime state at all.  The fixup MUST call it
 * FIRST: only after a shape match may it read the current task, its mm
 * or TTBR0 (on the early boot stack those reads are not safe).  A
 * kernel-side ordinary load/store, a fault outside the window and a
 * missing window are NOT recoverable.
 */
static inline bool aarch64_uaccess_abort_shape_matches(
    const struct aarch64_uaccess_abort *ab,
    const struct aarch64_uaccess_window *win,
    uintptr_t load_pc, uintptr_t store_pc)
{
    if (!ab || !win)
        return false;

    /* 1. The syndrome must describe a recoverable same-EL data abort. */
    uint64_t ec = (ab->esr & AARCH64_ESR_EC_MASK) >> AARCH64_ESR_EC_SHIFT;
    uint64_t fsc = ab->esr & AARCH64_ESR_ISS_FSC_MASK;

    if (ec != AARCH64_EC_DATA_ABORT_SAME)
        return false;
    if (ab->esr & (AARCH64_ESR_ISS_S1PTW | AARCH64_ESR_ISS_CM |
                   AARCH64_ESR_FNV_BIT))
        return false;
    if (fsc < AARCH64_FSC_ACCESS_FIRST || fsc > AARCH64_FSC_ACCESS_LAST)
        return false;

    /* 2. The saved mode must be exactly EL1h.  M[4:0] is compared as a
     *    whole so EL1t (0x04), EL0t (0x00) and every other mode fail. */
    if ((ab->spsr & AARCH64_SPSR_MODE_MASK) != AARCH64_SPSR_MODE_EL1H)
        return false;

    /* 3. A window must exist and its direction must match ISS.WnR. */
    if (!win->present)
        return false;
    bool write = (ab->esr & AARCH64_ESR_ISS_WNR) != 0;
    if (write != win->write)
        return false;

    /* 4. The faulting PC must be the direction's user instruction. */
    if (ab->pc != (uint64_t)(write ? store_pc : load_pc))
        return false;

    /* 5. FAR must be the user pointer the instruction still holds and
     *    must lie inside the half-open registered range. */
    uint64_t user_ptr = write ? ab->write_ptr : ab->read_ptr;
    if (ab->far != user_ptr)
        return false;
    if (ab->far < win->lo || ab->far >= win->hi)
        return false;

    return true;
}

/* The window must still belong to the running task, its mm and its
 * root — and the hardware TTBR0 must still hold that root.  Called
 * only AFTER the shape predicate matched. */
static inline bool aarch64_uaccess_window_holds(
    const struct aarch64_uaccess_window *win,
    const struct aarch64_uaccess_live *live)
{
    if (!win || !live)
        return false;
    if (!win->present)
        return false;
    if (!win->task || !win->mm || !win->root_pa)
        return false;
    if (live->task != win->task || live->mm != win->mm)
        return false;
    if (live->root_pa != win->root_pa || live->ttbr0_pa != win->root_pa)
        return false;
    return true;
}

/* Convenience composition of the two rules; used by host cases that
 * supply every input explicitly. */
static inline bool aarch64_uaccess_abort_matches(
    const struct aarch64_uaccess_abort *ab,
    const struct aarch64_uaccess_window *win,
    const struct aarch64_uaccess_live *live,
    uintptr_t load_pc, uintptr_t store_pc)
{
    return aarch64_uaccess_abort_shape_matches(ab, win, load_pc, store_pc)
        && aarch64_uaccess_window_holds(win, live);
}

/* Pre-SMP M4 probe only, with its private TTBR0 installed. Return 0 on
 * success or -EFAULT. Failed preflight copies nothing; a hardware fault
 * can leave a copied prefix. Kernel buffers must remain valid in TTBR1. */
int aarch64_copy_from_user(void *dst, uint64_t src, size_t len);
int aarch64_copy_to_user(uint64_t dst, const void *src, size_t len);

/* Assembly internals: skip software preflight. Use the checked entry
 * points except when exercising a hardware abort in the probe. */
int aarch64_copy_from_user_raw(void *dst, uint64_t src, size_t len);
int aarch64_copy_to_user_raw(uint64_t dst, const void *src, size_t len);

bool aarch64_uaccess_fixup(pt_regs_t *regs, uint64_t esr, uint64_t far);

#endif
