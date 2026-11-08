#ifndef _MOCK_GATE_H
#define _MOCK_GATE_H

#include_next <arch/x86_64/gate.h>

void mock_set_tss64(void *Table, unsigned long rsp0, unsigned long rsp1, unsigned long rsp2,
                    unsigned long ist1, unsigned long ist2, unsigned long ist3,
                    unsigned long ist4, unsigned long ist5, unsigned long ist6, unsigned long ist7);

#undef set_tss64
#define set_tss64 mock_set_tss64

#endif
