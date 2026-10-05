/* kernel/arch/aarch64/memory/vmm_backend.c
 *
 * aarch64 arch_vmm_* backend (M3.3 Task 16) — implements the
 * arch-neutral semantic API declared in <memory/vmm.h> on top of the
 * aarch64_pt_* primitives in page_table.c.  See design §4.3 / §4.4
 * for the full contract.
 *
 * Two distinct responsibilities live here:
 *   1. arch_vmm_init: locate the installed M1 root via TTBR1, publish
 *      it in the vmm_gate registry, and pin it as kernel_map.  The
 *      M1 boot path already published the root (boot_direct_map.c),
 *      so this is the idempotent idempotent re-publish.
 *   2. arch_vmm_{map,update,unmap,query}_{4k,2m} + split: thin VM_*
 *      → aarch64_pt_* translation with VM_NOCACHE-requires-VM_NO_EXEC
 *      rejection (4 rejected combos per spec §4.2 / §4.4.4) and
 *      VM_PROTNONE / VM_COW → AARCH64_PT_SOFTWARE_* stash.
 *
 * All entry points call vmm_gate_check() first (covered by the
 * underlying aarch64_pt_* primitives, which also gate-check on their
 * own — the duplicate is cheap and documents the contract).
 *
 * kernel_map is the single address-space root for aarch64 (M1 root).
 * The x86 backend owns its own kernel_map in kernel/memory/vmm.c; the
 * aarch64 build never compiles that file (kernel/Makefile whitelist),
 * so no symbol clash.
 *
 * Block ops (map_2m / unmap_2m / split_2m_to_4k) are stubs in this
 * commit (M3.3 keep-step; full block + split live in Task 18 with the
 * published-root registry).  They validate input and return -EPERM /
 * -EINVAL as appropriate so callers get an honest failure mode rather
 * than a silent miscompile.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* No <arch/irq.h> — the DEBUG invariant below reads DAIF directly
 * via a local `mrs` asm; arch_local_irq_save/restore are not used. */
#include <arch/mmu.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/vmm_backend.h>
#include <arch/aarch64/vmm_gate.h>
#include <arch/aarch64/boot_direct_map.h>
#include <memory/pmm.h>
#include <memory/vmm.h>

/* kernel_map — sole definer for the aarch64 build.  The x86 backend
 * owns its own definition in kernel/memory/vmm.c.  arch_vmm_init()
 * pins it on first call; before that it is NULL and any
 * arch_vmm_* entry returns -EINVAL (a check_kernel_map helper). */
mmap kernel_map = NULL;

/* ── Small helpers ──────────────────────────────────────────────── */

/* VM_* → AARCH64_PT_KERNEL/USER_* + EXEC translation.  Returns the
 * public perm word suitable for aarch64_pt_* primitives, or 0 when
 * the input is illegal (caller turns that into -EINVAL).
 *
 * The brief's 12-legal-combinations map cleanly:
 *   VM_KERNEL_RO    → AARCH64_PT_KERNEL_RO
 *   VM_KERNEL_RW    → AARCH64_PT_KERNEL_RW
 *   VM_USER_RO      → AARCH64_PT_USER_RO
 *   VM_USER_RW      → AARCH64_PT_USER_RW
 *   VM_DEVICE       → AARCH64_PT_DEVICE (= KERNEL_RW + DEVICE flag)
 *
 * VM_NO_EXEC is implicit in DEVICE; for the 4 non-DEVICE combos the
 * EXEC flag is set (i.e. neither XN bit asserted) when VM_NO_EXEC is
 * absent.  VM_NOCACHE forces PXN | UXN on aarch64 — the encode_perm
 * path for AARCH64_PT_DEVICE already does this.  VM_HUGE signals a
 * block path; the 4 KiB backend returns 0 here and the caller routes
 * to arch_vmm_map_2m. */
static uint32_t vm_to_perm(uint32_t vm)
{
    /* Reject VM_NOCACHE without VM_NO_EXEC — 4 illegal combos
     * (RO/RW × USER/KERNEL), spec §4.2.  aarch64 forces PXN|UXN on
     * NOCACHE mappings so VM_NO_EXEC is implicit; rejecting the
     * mixed-flag case keeps the contract symmetric with the rest of
     * the backend family. */
    if ((vm & VM_NOCACHE) && !(vm & VM_NO_EXEC)) return 0;

    bool user   = (vm & VM_USER) != 0;
    bool write  = (vm & VM_WRITE) != 0;
    bool device = (vm & VM_NOCACHE) != 0;

    /* VM_PRESENT must be set for a valid mapping.  An absent
     * PROTNONE stash carries VM_PRESENT=0; the 4 KiB backend passes
     * software_bits to encode PROT_NONE.  We return the perm here
     * for the descriptor bits — the present bit is encoded in the
     * software_bits path, not the perm word. */
    if (!(vm & VM_PRESENT) && !(vm & VM_PROTNONE)) return 0;

    uint32_t perm;
    if (device) {
        perm = AARCH64_PT_DEVICE;  /* forces PXN | UXN in encode_perm */
        if (write) perm |= user ? AARCH64_PT_USER_RW : AARCH64_PT_KERNEL_RW;
        else       perm |= user ? AARCH64_PT_USER_RO : AARCH64_PT_KERNEL_RO;
    } else {
        perm = user
            ? (write ? AARCH64_PT_USER_RW : AARCH64_PT_USER_RO)
            : (write ? AARCH64_PT_KERNEL_RW : AARCH64_PT_KERNEL_RO);
        if (!(vm & VM_NO_EXEC)) perm |= AARCH64_PT_EXEC;
    }
    return perm;
}

static uint64_t vm_to_sw(uint32_t vm)
{
    /* Both bits set is an illegal stash — caller rejects via -EINVAL.
     * PROTNONE without PRESENT is the only legal "invalid" combo;
     * PROTNONE with PRESENT is contradictory and rejected. */
    if ((vm & VM_PROTNONE) && (vm & VM_PRESENT)) return 0;
    uint64_t sw = 0;
    if (vm & VM_PROTNONE) sw |= AARCH64_PT_SOFTWARE_PROTNONE;
    if (vm & VM_COW)      sw |= AARCH64_PT_SOFTWARE_COW;
    return sw;
}

static int check_vm_flags(uint32_t vm)
{
    /* Validate the bit envelope (reject unknown bits) AND the
     * 12-legal-combination table.  Unknown bits beyond the
     * documented VM_* mask would silently alias descriptor
     * software bits. */
    const uint32_t known =
        VM_PRESENT | VM_WRITE | VM_USER | VM_NO_EXEC |
        VM_HUGE    | VM_NOCACHE | VM_PROTNONE | VM_COW;
    if ((vm & ~known) != 0) return -EINVAL;
    /* PROTNONE | PRESENT is contradictory (already handled in
     * vm_to_sw) — surface as EINVAL here too. */
    if ((vm & VM_PROTNONE) && (vm & VM_PRESENT)) return -EINVAL;
    /* NOCACHE without NO_EXEC — the 4 rejected combos. */
    if ((vm & VM_NOCACHE) && !(vm & VM_NO_EXEC))  return -EINVAL;
    /* COW without WRITE is meaningless (COW is a write-fault marker
     * on an otherwise RO mapping — but x86 semantics requires the
     * mapping to be eligible for privatization, which means write-
     * intent.  Refuse the meaningless combination.) */
    if ((vm & VM_COW) && !(vm & VM_WRITE))        return -EINVAL;
    return 0;
}

/* Inverse of vm_to_perm.  Used by query_4k to translate the
 * descriptor's decoded perm back to a VM_* mask.  The descriptor's
 * perm word does not preserve the VM_HUGE distinction (block vs page
 * is queried separately); for the 4 KiB path we always set VM_HUGE=0. */
static uint32_t perm_to_vm(uint32_t perm, uint64_t sw)
{
    uint32_t vm = VM_PRESENT;
    /* PROTNONE is the "invalid but holds PA" state — a stashed
     * descriptor with VALID cleared.  For the VM_* representation
     * VM_PRESENT and VM_PROTNONE are mutually exclusive (the
     * PRESENT bit describes "is the translation valid", and a
     * PROTNONE stash is invalid by definition).  arch_vmm_query_4k
     * hardcodes *vm_out = VM_PROTNONE (no PRESENT) for this state,
     * so the inverse direction here must clear PRESENT too —
     * otherwise callers reconstructing VMA-prot save/restore see
     * both bits set, which is semantically contradictory.  Task 19
     * Fix round 1. */
    if (sw & AARCH64_PT_SOFTWARE_PROTNONE) vm &= ~VM_PRESENT;
    if (perm & AARCH64_PT_USER_RO)   vm |= VM_USER;
    if (perm & AARCH64_PT_USER_RW)   vm |= VM_USER;
    if ((perm & AARCH64_PT_KERNEL_RW) || (perm & AARCH64_PT_USER_RW))
        vm |= VM_WRITE;
    if ((perm & AARCH64_PT_EXEC) == 0) vm |= VM_NO_EXEC;
    if (perm & AARCH64_PT_DEVICE) {
        vm |= VM_NOCACHE;
        vm |= VM_NO_EXEC;   /* DEVICE forces NX */
    }
    if (sw & AARCH64_PT_SOFTWARE_PROTNONE) vm |= VM_PROTNONE;
    if (sw & AARCH64_PT_SOFTWARE_COW)      vm |= VM_COW;
    return vm;
}

/* Refuse any backend call before arch_vmm_init has pinned kernel_map.
 * Returns 0 when ready, -EINVAL otherwise. */
static int check_kernel_map(void)
{
    if (kernel_map == NULL) return -EINVAL;
    return 0;
}

/* ── DEBUG-build invariants (spec §5.4 I1 / I2) ──────────────────────
 *
 * I1: the three page-table / shootdown locks (pt_locks, pt_upper_lock,
 *     tlb_sd_lock) are never taken from interrupt context — only the
 *     BSP/AP vmm-change API entries acquire them, and the TLB IPI
 *     handler takes none.  Enforced by never calling them from
 *     trap.c.
 *
 * I2: every vmm-change API entry runs with local IRQs ENABLED, so
 *     a waiter spinning on the (plain) spin_lock can still answer
 *     a TLB IPI.  We assert this in DEBUG builds via
 *     `DEBUG_ASSERT(irqs_enabled())` at each map/unmap/update/split
 *     entry point.  M1 selftest smoke paths are intentionally exempt
 *     (they run before SMP bring-up and don't need the assertion). */

#ifdef __aarch64__
/* `irqs_disabled` is the converse of the entry-time invariant.  Reads
 * DAIF and returns true when the I bit (bit 1) is set.  Wrapped in
 * __aarch64__ because the host-test harness (x86 clang) doesn't have
 * DAIF; a separate DEBUG-only mock is unnecessary since the assertion
 * is a no-op in host tests anyway. */
static inline int vmm_irqs_disabled(void)
{
    uint64_t daif;
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
    return (daif & (1UL << 1)) != 0;
}

static inline void vmm_debug_assert_irqs_enabled(const char *where)
{
    if (vmm_irqs_disabled()) {
        /* DEBUG build: spin forever with a kputs note.  Production
         * release builds (NDEBUG) skip this entirely. */
        __asm__ __volatile__("" ::: "memory");
        for (;;) { __asm__ __volatile__("wfi"); }
    }
    (void)where;
}
#else
/* Host build: no DAIF — assertion is a no-op. */
static inline void vmm_debug_assert_irqs_enabled(const char *where)
{
    (void)where;
}
#endif

/* ── arch_vmm_init ───────────────────────────────────────────────── */

/* aarch64 backend init: locate the M1-installed TTBR1 root, validate
 * it, pin kernel_map, re-publish the root (idempotent — boot_direct_map.c
 * already published at install time).  Also re-initialises the page-
 * table spinlocks (Task 17 §5.4 double insurance in case some future
 * change accidentally drops the static initializers).  Returns 0 on
 * success, -EINVAL on a malformed TTBR1 read. */
int arch_vmm_init(void)
{
    vmm_gate_check();
    uint64_t raw = aarch64_read_ttbr1();
    uint64_t pa  = raw & AARCH64_TTBR_BASE_MASK;
    if (pa == 0)                       return -EINVAL;
    if ((pa & (PAGE_4K_SIZE - 1)) != 0) return -EINVAL;
    if (pa >= (UINT64_C(1) << 40))     return -EINVAL;
    /* Task 17 §5.4: double insurance on the static-initialised
     * pt_locks / pt_upper_lock.  spin_init() writes 1UL to lock->lock,
     * which is idempotent. */
    (void)aarch64_pt_init_locks();
    /* Re-publish idempotently: boot_direct_map.c publishes the same PA
     * at TTBR1 install time.  The second call hits the "already in
     * registry" fast path and returns true.  If the registry is full
     * aarch64_pt_root_publish calls vmm_gate_violation (default spin)
     * — we don't need to gate against that here. */
    (void)aarch64_pt_root_publish(pa);
    kernel_map = (mmap)(uintptr_t)(pa + ARCH_PAGE_OFFSET);
    return 0;
}

/* ── 4 KiB map / query / unmap / update ───────────────────────────── */

int arch_vmm_map_4k_new(uint64_t *pgdir, uint64_t phys, uint64_t virt,
                        uint32_t vm_flags)
{
    vmm_gate_check();
    vmm_debug_assert_irqs_enabled("arch_vmm_map_4k_new");
    int rc = check_kernel_map();
    if (rc) return rc;
    if (pgdir != kernel_map) return -EINVAL;
    if ((vm_flags & VM_HUGE) != 0) return -EINVAL;   /* use map_2m */
    rc = check_vm_flags(vm_flags);
    if (rc) return rc;
    if ((phys & (PAGE_4K_SIZE - 1)) != 0) return -EINVAL;
    if ((virt & (PAGE_4K_SIZE - 1)) != 0) return -EINVAL;
    if (phys >= (UINT64_C(1) << 40)) return -EINVAL;
    uint32_t perm = vm_to_perm(vm_flags);
    uint64_t sw   = vm_to_sw(vm_flags);
    if (perm == 0 && sw == 0) return -EINVAL;
    rc = aarch64_pt_map_4k_ext(pgdir, virt, phys, perm, sw);
    /* Normalize the page-table layer's internal sentinel codes to
     * Linux errno (spec §4.2/§4.3).  The AARCH64_PT_ enum uses
     * sequential negative numbers that don't match Linux errno
     * values (AARCH64_PT_EEXIST = -2 = -ENOENT in Linux).  Without
     * this, callers cannot tell "slot occupied" from "slot absent"
     * — and the user-facing arch_vmm_* contract mandates -EEXIST /
     * -ENOENT specifically.  Task 19 Fix round 1. */
    if (rc == AARCH64_PT_EEXIST) return -EEXIST;
    if (rc == AARCH64_PT_ENOENT) return -ENOENT;
    return rc;
}

int arch_vmm_query_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out,
                      uint32_t *vm_out)
{
    vmm_gate_check();
    int rc = check_kernel_map();
    if (rc) return rc;
    if (pgdir != kernel_map) return -EINVAL;

    uint64_t pa = 0;
    uint32_t perm = 0;
    uint64_t sw = 0;
    rc = aarch64_pt_query_4k_ext(pgdir, virt, &pa, &perm, &sw);
    if (rc == AARCH64_PT_EPROT_NONE) {
        if (phys_out) *phys_out = pa;
        if (vm_out)   *vm_out   = VM_PROTNONE;
        return AARCH64_PT_EPROT_NONE;
    }
    /* Normalize to Linux errno per spec §4.3 — the page-table layer
     * uses AARCH64_PT_ENOENT = -3, but the backend contract mandates
     * -ENOENT (-2).  Task 19 Fix round 1. */
    if (rc == AARCH64_PT_ENOENT) return -ENOENT;
    if (rc != AARCH64_PT_OK) return rc;
    if (phys_out) *phys_out = pa;
    if (vm_out)   *vm_out   = perm_to_vm(perm, sw);
    return AARCH64_PT_OK;
}

int arch_vmm_unmap_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out,
                      uint32_t *old_vm_out)
{
    vmm_gate_check();
    vmm_debug_assert_irqs_enabled("arch_vmm_unmap_4k");
    int rc = check_kernel_map();
    if (rc) return rc;
    if (pgdir != kernel_map) return -EINVAL;

    uint64_t pa = 0;
    uint32_t perm = 0;
    uint64_t sw = 0;
    rc = aarch64_pt_unmap_4k_ext(pgdir, virt, &pa, &perm, &sw);
    if (rc == AARCH64_PT_EPROT_NONE) {
        if (phys_out)   *phys_out   = pa;
        if (old_vm_out) *old_vm_out = VM_PROTNONE;
        return AARCH64_PT_EPROT_NONE;
    }
    /* Normalize AARCH64_PT_ENOENT → -ENOENT (spec §4.3). */
    if (rc == AARCH64_PT_ENOENT) return -ENOENT;
    if (rc != AARCH64_PT_OK) return rc;
    if (phys_out)   *phys_out   = pa;
    if (old_vm_out) *old_vm_out = perm_to_vm(perm, sw);
    return AARCH64_PT_OK;
}

int arch_vmm_update_4k(uint64_t *pgdir, uint64_t phys, uint64_t virt,
                       uint32_t vm_flags, uint64_t *old_phys_out,
                       uint32_t *old_vm_out)
{
    vmm_gate_check();
    vmm_debug_assert_irqs_enabled("arch_vmm_update_4k");
    int rc = check_kernel_map();
    if (rc) return rc;
    if (pgdir != kernel_map) return -EINVAL;
    if ((vm_flags & VM_HUGE) != 0) return -EINVAL;
    rc = check_vm_flags(vm_flags);
    if (rc) return rc;
    if ((virt & (PAGE_4K_SIZE - 1)) != 0) return -EINVAL;
    if (phys >= (UINT64_C(1) << 40)) return -EINVAL;
    uint32_t perm = vm_to_perm(vm_flags);
    uint64_t sw   = vm_to_sw(vm_flags);
    if (perm == 0 && sw == 0) return -EINVAL;

    /* aarch64_pt_replace_4k hands back the prior decoded perm + sw
     * (taken BEFORE the descriptor is rewritten), so the caller can
     * reconstruct the prior VM_* state precisely. */
    uint64_t old_pa = 0;
    uint32_t old_perm = 0;
    uint64_t old_sw = 0;
    rc = aarch64_pt_replace_4k(pgdir, virt, phys, perm, sw,
                               &old_pa, &old_perm, &old_sw);
    /* Normalize: aarch64_pt_replace_4k can return -ENOENT when no
     * leaf (valid OR PROTNONE-stashed) exists at VA. */
    if (rc == AARCH64_PT_ENOENT) return -ENOENT;
    if (rc != AARCH64_PT_OK) return rc;
    if (old_phys_out) *old_phys_out = old_pa;
    if (old_vm_out)   *old_vm_out   = perm_to_vm(old_perm, old_sw);
    return AARCH64_PT_OK;
}

/* ── 2 MiB block map / unmap / split ─────────────────────────────── */

/* Full block + split land in Task 18 (aarch64 M3.3 split follow-up)
 * with the published-root registry + pt_lock_for.  map_2m / unmap_2m
 * now route through the new aarch64_pt_* block primitives; split
 * remains -EPERM (Task 21 implements the un-published-root split
 * with the published-root registry). */
int arch_vmm_map_2m(uint64_t *pgdir, uint64_t phys, uint64_t virt,
                    uint32_t vm_flags)
{
    vmm_gate_check();
    vmm_debug_assert_irqs_enabled("arch_vmm_map_2m");
    int rc = check_kernel_map();
    if (rc) return rc;
    if (pgdir != kernel_map) return -EINVAL;
    rc = check_vm_flags(vm_flags);
    if (rc) return rc;
    /* Block API requires VM_HUGE; leaf calls (no HUGE) belong in
     * arch_vmm_map_4k_new. */
    if (!(vm_flags & VM_HUGE)) return -EINVAL;
    if ((phys & (PAGE_2M_SIZE - 1)) != 0) return -EINVAL;
    if ((virt & (PAGE_2M_SIZE - 1)) != 0) return -EINVAL;
    if (phys >= (UINT64_C(1) << 40))      return -EINVAL;
    uint32_t perm = vm_to_perm(vm_flags);
    if (perm == 0) return -EINVAL;
    rc = aarch64_pt_map_2m_block(pgdir, virt, phys, perm);
    /* Normalize AARCH64_PT_EEXIST → -EEXIST (spec §4.2). */
    if (rc == AARCH64_PT_EEXIST) return -EEXIST;
    return rc;
}

int arch_vmm_unmap_2m(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out)
{
    vmm_gate_check();
    vmm_debug_assert_irqs_enabled("arch_vmm_unmap_2m");
    int rc = check_kernel_map();
    if (rc) return rc;
    if (pgdir != kernel_map) return -EINVAL;
    if ((virt & (PAGE_2M_SIZE - 1)) != 0) return -EINVAL;
    uint64_t pa = 0;
    rc = aarch64_pt_unmap_2m_block(pgdir, virt, &pa);
    if (phys_out) *phys_out = pa;
    /* Normalize AARCH64_PT_ENOENT → -ENOENT (spec §4.3). */
    if (rc == AARCH64_PT_ENOENT) return -ENOENT;
    return rc;
}

int arch_vmm_split_2m_to_4k(uint64_t *pgdir, uint64_t virt)
{
    vmm_gate_check();
    vmm_debug_assert_irqs_enabled("arch_vmm_split_2m_to_4k");
    int rc = check_kernel_map();
    if (rc) return rc;
    if (pgdir != kernel_map) return -EINVAL;
    if ((virt & (PAGE_2M_SIZE - 1)) != 0) return -EINVAL;
    /* The M1 root IS published (boot_direct_map.c publishes it at
     * install time).  Spec §5.3: split on a published root → -EPERM
     * until F10 designs the BBM-level + cross-core invalidation.
     * Route through the primitive so the stub stays consistent with
     * the page-table layer. */
    return aarch64_pt_split_block_2m(pgdir, virt);
}
