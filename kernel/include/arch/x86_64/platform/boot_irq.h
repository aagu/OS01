#ifndef _ARCH_X86_64_PLATFORM_BOOT_IRQ_H
#define _ARCH_X86_64_PLATFORM_BOOT_IRQ_H

/* BOOT-2: x86-private boot IRQ/time coordination.
 *
 * One-shot state machine:  cold → context ⇄ calibration → runtime
 *
 *   cold        before x86_boot_irq_context_prepare(): no GS reads, no
 *               calibration windows, no runtime IRQs.
 *   context     BSP percpu/GS/stack/softirq ready; the controlled RTC
 *               calibration window may open (begin/end pairs).
 *   runtime     regular IRQs enabled (PIT is the periodic source).
 *
 * Illegal transitions fail closed (fatal) BEFORE any sti/GS read.  The
 * transitions are driven by the x86 boot orchestrator only; there is
 * no re-initialization and no shutdown.
 */

#include <stdbool.h>

/* phase 3, after the controllers: verify + publish the context state.
 * Verifies entry IF=0, BOOT-1 BSP percpu (online/self), the BSP stack
 * and the softirq base; logs "[boot-2] context-ready". */
void x86_boot_irq_context_prepare(void);

/* May a calibration window open now?  Pure state/IF check, no hardware. */
bool x86_boot_irq_calibration_allowed(void);

/* Open the controlled RTC calibration window (state → calibration). */
void x86_boot_irq_calibration_begin(void);

/* Close the controlled window (state → context). */
void x86_boot_irq_calibration_end(void);

/* phase 4 complete: verify timer/PIT/LAPIC-stub success, PIT start,
 * publish runtime-open and enable regular IRQs.  Logs
 * "[boot-2] runtime-open source=pit". */
void x86_boot_irq_runtime_open(void);

bool x86_boot_irq_runtime_opened(void);

#endif /* _ARCH_X86_64_PLATFORM_BOOT_IRQ_H */
