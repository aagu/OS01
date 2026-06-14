#include <kernel/arch/x86_64/gate.h>
#include <kernel/printk.h>
#include <stddef.h>

// ── Per-vector C handler table ──────────────────────────────
// Indexed by interrupt vector number (0-255).
// REGISTER_INTR_HANDLER fills these tables and sets the IDT gate.

intr_handler_fn intr_handler_table[256];
void *          intr_handler_param[256];

// ── Generic dispatch ────────────────────────────────────────
// Called from auto-generated assembly stubs with:
//   rdi = pt_regs*  (pointer to saved register frame)
//   rsi = vector    (interrupt vector number)
//
// Looks up the handler in intr_handler_table[vector] and calls
// it.  Returns → ret_from_intr → RESTORE_ALL → iretq.

void generic_intr_dispatch(pt_regs_t *regs, uint64_t vector)
{
    if (vector < 256 && intr_handler_table[vector]) {
        intr_handler_table[vector](vector,
                                   (uint64_t)intr_handler_param[vector],
                                   regs);
    } else {
        serial_printk("intr: spurious vector %u (no handler)\n",
                      (unsigned)vector);
    }
}
