#ifndef _ARCH_X86_64_VMM_H
#define _ARCH_X86_64_VMM_H
/* Host shadow of the production x86 private VMM header for the IRQ
 * chain fixture: the fixture runs in ring 3, so the real CR3-reload
 * flush must never execute — flush_tlb() routes to the counted test
 * hook.  Uses the PRODUCTION include guard so the real header stays a
 * no-op when a TU includes both. */
#include <stdint.h>
#include <stddef.h>
typedef uint64_t *ptbl_t;
extern ptbl_t kernel_map;
void vmm_map_page(uint64_t *pgdir, uintptr_t physical_address, uintptr_t virtual_address, uint64_t flags);
void irq_test_flush_tlb(void);
#define flush_tlb() irq_test_flush_tlb()
#endif
