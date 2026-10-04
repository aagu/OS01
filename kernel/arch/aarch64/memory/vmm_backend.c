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
        if (write) perm |= AARCH64_PT_KERNEL_RW;
        else       perm |= AARCH64_PT_KERNEL_RO;
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

/* ── arch_vmm_init ───────────────────────────────────────────────── */

/* aarch64 backend init: locate the M1-installed TTBR1 root, validate
 * it, pin kernel_map, re-publish the root (idempotent — boot_direct_map.c
 * already published at install time).  Returns 0 on success, -EINVAL
 * on a malformed TTBR1 read. */
int arch_vmm_init(void)
{
    vmm_gate_check();
    uint64_t raw = aarch64_read_ttbr1();
    uint64_t pa  = raw & AARCH64_TTBR_BASE_MASK;
    if (pa == 0)                       return -EINVAL;
    if ((pa & (PAGE_4K_SIZE - 1)) != 0) return -EINVAL;
    if (pa >= (UINT64_C(1) << 40))     return -EINVAL;
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
    return aarch64_pt_map_4k_ext(pgdir, virt, phys, perm, sw);
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
    if (rc != AARCH64_PT_OK) return rc;
    if (phys_out) *phys_out = pa;
    if (vm_out)   *vm_out   = perm_to_vm(perm, sw);
    return AARCH64_PT_OK;
}

int arch_vmm_unmap_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out,
                      uint32_t *old_vm_out)
{
    vmm_gate_check();
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
    if (rc != AARCH64_PT_OK) return rc;
    if (old_phys_out) *old_phys_out = old_pa;
    if (old_vm_out)   *old_vm_out   = perm_to_vm(old_perm, old_sw);
    return AARCH64_PT_OK;
}

/* ── 2 MiB block map / unmap / split ─────────────────────────────── */

/* Full block + split land in Task 18 (aarch64 M3.3 split follow-up)
 * with the published-root registry + pt_lock_for.  Stubs here so the
 * arch-neutral API is at least declared and an honest -ENOSYS /
 * -EINVAL is returned for callers that race to the Task 16 commit.
 * map_2m / unmap_2m return -ENOSYS (capability simply not present);
 * split_2m_to_4k returns -EPERM because spec §5.3 mandates that exact
 * code for a published root. */
int arch_vmm_map_2m(uint64_t *pgdir, uint64_t phys, uint64_t virt,
                    uint32_t vm_flags)
{
    vmm_gate_check();
    int rc = check_kernel_map();
    if (rc) return rc;
    if (pgdir != kernel_map) return -EINVAL;
    rc = check_vm_flags(vm_flags);
    if (rc) return rc;
    if ((phys & (PAGE_2M_SIZE - 1)) != 0) return -EINVAL;
    if ((virt & (PAGE_2M_SIZE - 1)) != 0) return -EINVAL;
    if (phys >= (UINT64_C(1) << 40))      return -EINVAL;
    (void)vm_flags;    /* Task 18 will consume */
    return -ENOSYS;
}

int arch_vmm_unmap_2m(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out)
{
    vmm_gate_check();
    int rc = check_kernel_map();
    if (rc) return rc;
    if (pgdir != kernel_map) return -EINVAL;
    if ((virt & (PAGE_2M_SIZE - 1)) != 0) return -EINVAL;
    if (phys_out) *phys_out = 0;
    return -ENOSYS;   /* Task 18 */
}

int arch_vmm_split_2m_to_4k(uint64_t *pgdir, uint64_t virt)
{
    vmm_gate_check();
    int rc = check_kernel_map();
    if (rc) return rc;
    if (pgdir != kernel_map) return -EINVAL;
    if ((virt & (PAGE_2M_SIZE - 1)) != 0) return -EINVAL;
    /* The M1 root IS published (boot_direct_map.c publishes it at
     * install time).  Spec §5.3: split on a published root → -EPERM
     * until F10 designs the BBM-level + cross-core invalidation. */
    return -EPERM;
}
