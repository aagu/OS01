#pragma once
void irq_test_flush_tlb(void);
#define flush_tlb() irq_test_flush_tlb()
