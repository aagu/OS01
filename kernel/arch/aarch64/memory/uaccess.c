#if AARCH64_EL0_TEST

#include <arch/aarch64/el0.h>
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
    enum {
        EC_MASK = 0x3f, EC_SHIFT = 26, DATA_ABORT_EL1 = 0x25,
        DFSC_MASK = 0x3f, WNR = 1 << 6, S1PTW = 1 << 7,
        CM = 1 << 8, FNV = 1 << 10,
    };

    /* A kernel-side load/store (or another kernel fault) must never be
     * swallowed. Only normal translation/access/permission aborts on
     * the two unprivileged instructions are recoverable. */
    if ((regs->spsr_el1 & 0xf) != 5 ||
        ((esr >> EC_SHIFT) & EC_MASK) != DATA_ABORT_EL1 ||
        (esr & (FNV | CM | S1PTW)) ||
        (esr & DFSC_MASK) < 4 || (esr & DFSC_MASK) > 15 ||
        far < AARCH64_USER_MIN || far >= AARCH64_USER_LIMIT ||
        !aarch64_el0_active_root())
        return false;

    bool write = (esr & WNR) != 0;
    uintptr_t pc = write ? (uintptr_t)aarch64_uaccess_store
                         : (uintptr_t)aarch64_uaccess_load;
    if (regs->elr_el1 != pc || far != (write ? regs->x0 : regs->x1))
        return false;

    regs->elr_el1 = (uintptr_t)aarch64_uaccess_fault;
    return true;
}
#endif
