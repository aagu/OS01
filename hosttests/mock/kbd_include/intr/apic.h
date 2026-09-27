/* Host shadow of <intr/apic.h> for compiling kernel/driver/keyboard.c
 * (PS/2 mouse driver Task 5).  keyboard.c no longer includes it; the
 * shadow exists so stale includes cannot drag the APIC header tree
 * (IOAPIC/MADT structures) into the host build. */
#ifndef _KERNEL_APIC_H
#define _KERNEL_APIC_H

#include <stdint.h>

#endif /* _KERNEL_APIC_H */
