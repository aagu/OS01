#ifndef _ARCH_GATE_H
#define _ARCH_GATE_H

// Install per-architecture IDT entries (syscall, exceptions, IRQs).
// Defined in arch/<arch>/trap.c.
void sys_vector_install(void);

#endif /* _ARCH_GATE_H */
