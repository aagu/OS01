#ifndef _KERNEL_ARCH_X86_64_RANDOM_H
#define _KERNEL_ARCH_X86_64_RANDOM_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/arch/cpuid.h>

// RDRAND/RDSEED with bounded retry (Intel SDM recommends ≤ 10 attempts).
// Returns true and writes `*out` on success (CF=1); false if the instruction
// is not supported by the CPU (CPUID guard) or retries exhausted (instruction
// genuinely did not produce entropy).  The caller falls back to
// arch_cycle_counter() — matching the aarch64 stub contract.
//
// DEV NOTE (deviation from task brief): the brief's verbatim asm executed
// RDRAND/RDSEED unconditionally.  On QEMU TCG's default q35 CPU ("qemu64",
// which lacks both features) the instruction raises #UD and the kernel's
// #UD handler hangs — breaking boot.  The CPUID guard below uses the feature
// bits defined in regs.h (Step 1 of the brief) so unsupported CPUs take the
// false→cycle-counter fallback instead of #UD.

static inline bool rdrand64(uint64_t *out)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(ecx & CPUID_FEAT_ECX_RDRAND))
        return false;
    for (int i = 0; i < 10; i++) {
        uint64_t v;
        unsigned char cf;
        __asm__ __volatile__(
            "rdrand %0; setc %1"
            : "=r"(v), "=qm"(cf)
            :
            : "cc");
        if (cf) { *out = v; return true; }
    }
    return false;
}

static inline bool rdseed64(uint64_t *out)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
    if (!(ebx & CPUID_FEAT_EBX_RDSEED))
        return false;
    for (int i = 0; i < 10; i++) {
        uint64_t v;
        unsigned char cf;
        __asm__ __volatile__(
            "rdseed %0; setc %1"
            : "=r"(v), "=qm"(cf)
            :
            : "cc");
        if (cf) { *out = v; return true; }
    }
    return false;
}

#endif // _KERNEL_ARCH_X86_64_RANDOM_H
