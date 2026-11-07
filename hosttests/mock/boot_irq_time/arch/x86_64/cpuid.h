#pragma once
/* Test-controlled CPUID for the REAL time.c frequency cache test. */
#include <stdint.h>
extern int boot2_cpu_id15_answer;   /* 0 = invalid (ecx==0), 1 = valid */
static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx)
{
    (void)leaf;
    if (boot2_cpu_id15_answer) {
        *eax = 2; *ebx = 200; *ecx = 100; *edx = 0;   /* 100 * 200 / 2 Hz */
    } else {
        *eax = 0; *ebx = 0; *ecx = 0; *edx = 0;
    }
}
