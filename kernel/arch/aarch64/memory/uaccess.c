#if AARCH64_EL0_TEST

#include <arch/aarch64/el0.h>
#include <arch/aarch64/sync_fault.h>
#include <arch/aarch64/uaccess.h>
#include <arch/mmu.h>
#include <errno.h>

int aarch64_copy_from_user(void *dst, uint64_t src, size_t len)
{
    uint64_t *root = aarch64_el0_active_root();
    if (!root || !arch_user_range_accessible(root, src, len, false))
        return -EFAULT;
    return aarch64_copy_from_user_raw(dst, src, len);
}

int aarch64_copy_to_user(uint64_t dst, const void *src, size_t len)
{
    uint64_t *root = aarch64_el0_active_root();
    if (!root || !arch_user_range_accessible(root, dst, len, true))
        return -EFAULT;
    return aarch64_copy_to_user_raw(dst, src, len);
}

extern const char aarch64_uaccess_load[], aarch64_uaccess_store[];
extern const char aarch64_uaccess_fault[];

bool aarch64_uaccess_fixup(pt_regs_t *regs, uint64_t esr, uint64_t far)
{
    /* The syndrome layout comes from the shared sync_fault.h truth
     * source: the copy fixup and the normalized event decoder must
     * agree bit for bit. */
    uint64_t ec = (esr & AARCH64_ESR_EC_MASK) >> AARCH64_ESR_EC_SHIFT;
    uint64_t fsc = esr & AARCH64_ESR_ISS_FSC_MASK;

    /* A kernel-side load/store (or another kernel fault) must never be
     * swallowed. Only normal translation/access/permission aborts on
     * the two unprivileged instructions are recoverable. */
    if ((regs->spsr_el1 & AARCH64_SPSR_MODE_MASK) != AARCH64_SPSR_MODE_EL1H ||
        ec != AARCH64_EC_DATA_ABORT_SAME ||
        (esr & (AARCH64_ESR_FNV_BIT | AARCH64_ESR_ISS_CM |
                AARCH64_ESR_ISS_S1PTW)) ||
        fsc < AARCH64_FSC_ACCESS_FIRST || fsc > AARCH64_FSC_ACCESS_LAST ||
        far < AARCH64_USER_MIN || far >= AARCH64_USER_LIMIT ||
        !aarch64_el0_active_root())
        return false;

    bool write = (esr & AARCH64_ESR_ISS_WNR) != 0;
    uintptr_t pc = write ? (uintptr_t)aarch64_uaccess_store
                         : (uintptr_t)aarch64_uaccess_load;
    if (regs->elr_el1 != pc || far != (write ? regs->x0 : regs->x1))
        return false;

    regs->elr_el1 = (uintptr_t)aarch64_uaccess_fault;
    return true;
}
#endif
