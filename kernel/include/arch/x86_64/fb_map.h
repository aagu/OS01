#ifndef _ARCH_X86_64_FB_MAP_H
#define _ARCH_X86_64_FB_MAP_H

#include <stdint.h>
#include <driver/fb_cache.h>   /* fb_cache_policy_t */

/*
 * fb_x86_map_checked:
 * Maps physical framebuffer memory into VIRT_FRAMEBUFFER_OFFSET using checked
 * 2MiB pages. Verifies alignment, checks range overflow, verifies each PMD
 * entry physical address and cache flags, flushes TLB, and leaves no partial
 * mapping on error.
 *
 * `policy` is the CANDIDATE framebuffer cache policy for this mapping (see
 * <driver/fb_cache.h>).  The mapping and the policy are published as one
 * transaction: the global policy is committed only after every leaf entry is
 * installed and verified.  On any failure the page table is restored AND the
 * global policy is left untouched, so the /dev/fb alias can never end up with
 * a memory type that contradicts the kernel mapping.
 */
int fb_x86_map_checked(uint64_t phys, uint64_t size, fb_cache_policy_t policy,
                       uint32_t **out_addr);

/*
 * fb_x86_revoke_early_alias:
 * Removes the boot-time early framebuffer alias.  frame_buffer_early_init()
 * maps the framebuffer physical pages a second time at VIRT_FRAMEBUFFER_EARLY
 * inside the boot page table -- and that boot page table IS the kernel page
 * table (vmm_init() adopts the boot PGD), so the alias stays reachable after
 * the permanent mapping is installed.  Leaving it in place would let one
 * physical page be accessed under two different cache attributes.
 *
 * pmd_table is the PMD table that holds the early alias entries (the boot
 * PUD[0] PMD table); va_base/size describe the aliased range.  Returns the
 * number of entries that were cleared, or -EINVAL on a bad argument.
 */
int fb_x86_revoke_early_alias(uint64_t *pmd_table, uint64_t va_base, uint64_t size);

/*
 * fb_x86_publish_and_revoke_early_alias:
 * Switches the framebuffer from the early boot alias to the permanent
 * mapping IN THAT ORDER, and is the single place where the order is
 * defined (the kernel path and the host regression call the same
 * function, so the regression covers the production order).
 *
 *   1. publish the permanent mapping in the coordinator -- the
 *      coordinator hands that address to every writer lease, so after
 *      this step no lease can hold the early address any more;
 *   2. revoke the early alias, but only while no writer lease is
 *      outstanding (a lease holds the address it was granted, and that
 *      address must not become unmapped underneath it).
 *
 * The order matters: revoking first would leave a window in which the
 * coordinator still hands out the early address -- a console/logger
 * writer that takes a lease in that window would write into the
 * revoked range (#PF).  Publishing Pos.FB_addr alone does NOT close
 * that window, because Pos.FB_addr and the coordinator's published
 * address are different variables.
 *
 * Returns the number of revoked entries (0 when the revocation was
 * skipped because a writer was active), or -EINVAL on a bad argument.
 */
int fb_x86_publish_and_revoke_early_alias(uint32_t *perm_addr, uint64_t perm_size,
                                           uint64_t *early_pmd, uint64_t early_va,
                                           uint64_t early_size);

#endif /* _ARCH_X86_64_FB_MAP_H */
