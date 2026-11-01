/* P2 (spec §5.3): the idle wait helper.  Entry IRQs MASKED (DAIF.I=1),
 * no lock held.  The WFI wakes on any pending enabled interrupt (an
 * SGI stays pending while PSTATE.I masks delivery); the IRQ state is
 * deliberately NOT changed here — the C caller re-enables IRQs + ISB
 * after the return, closing the check-to-sleep lost-wakeup window
 * (opening IRQs BEFORE the WFI would let the SGI be consumed and the
 * CPU sleep with work pending).
 *
 * ELF audit: the harness (P2 Task 4) requires this function to be
 * EXACTLY dsb sy / wfi / ret with no DAIF-setting instruction. */
#include <arch/aarch64/context.h>

.text
.align 2
.global aarch64_idle_wait_irq
.type aarch64_idle_wait_irq, %function
aarch64_idle_wait_irq:
    dsb sy
    wfi
    ret
.size aarch64_idle_wait_irq, .-aarch64_idle_wait_irq
