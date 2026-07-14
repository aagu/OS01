#ifndef _ARCH_GATE_H
#define _ARCH_GATE_H

#ifdef __x86_64__
#include <kernel/arch/x86_64/gate.h>
#elif defined(__aarch64__)
// aarch64 has no TSS — all TSS operations are no-ops
#define set_tss64(rsp0, rsp1, rsp2, ist1, ist2, ist3, ist4, ist5, ist6, ist7) \
    do { (void)(rsp0); (void)(rsp1); (void)(rsp2); \
         (void)(ist1); (void)(ist2); (void)(ist3); \
         (void)(ist4); (void)(ist5); (void)(ist6); \
         (void)(ist7); } while (0)
#else
#error "Unsupported architecture"
#endif

#endif /* _ARCH_GATE_H */
