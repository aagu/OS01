#ifndef OS01_AARCH64_USER_ASID_H
#define OS01_AARCH64_USER_ASID_H

/* AArch64 8-bit user ASID pool (TCR_EL1.A1=0, AS=0, TTBR0_EL1[55:48]).
 *
 * Tags 1..255 are dynamically allocated to formal user mm instances;
 * tag 0 is reserved for the shared empty TTBR0 root and as the
 * exhaustion fallback.  Release completes ASID TLB invalidation
 * before returning the tag to the pool, without holding the pool lock
 * across TLBI.
 */

#include <stdint.h>

/* Allocate a non-zero 8-bit ASID (1..255), or return 0 if exhausted. */
uint8_t aarch64_asid_alloc(void);

/* Complete ASID TLB invalidation for a previously uninstalled non-zero
 * ASID and return the tag to the pool.  Tag 0 is a no-op. */
void aarch64_asid_release(uint8_t asid);

/* Return a non-zero ASID tag to the pool after the caller has already
 * completed aarch64_asid_tlbi(asid), unregistered, and reclaimed tables. */
void aarch64_asid_release_quiesced(uint8_t asid);

/* Perform inner-shareable ASID TLB invalidation for `asid` (or full
 * VMALL invalidation if `asid == 0`).  Weak symbol so host tests can
 * observe the pre-reclaim window. */
void aarch64_asid_tlbi(uint8_t asid);

#endif /* OS01_AARCH64_USER_ASID_H */
