#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint64_t *ptbl_t;
extern ptbl_t kernel_map;
void x86_vmm_map_2m(uint64_t *pgdir, uintptr_t physical_address, uintptr_t virtual_address, uint64_t flags);
void irq_test_flush_tlb(void);
#define flush_tlb() irq_test_flush_tlb()
