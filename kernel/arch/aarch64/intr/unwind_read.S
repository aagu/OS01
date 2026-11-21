.section .text
.globl aarch64_unwind_load_pair
.globl aarch64_unwind_load_next_fp
.globl aarch64_unwind_load_ret_pc
.globl aarch64_unwind_load_fail

/* bool aarch64_unwind_load_pair(uintptr_t addr, struct unwind_frame *out);
 *   x0 = addr (preserved across both loads for FAR matching)
 *   x1 = out
 */
aarch64_unwind_load_pair:
aarch64_unwind_load_next_fp:
    ldr x2, [x0]
aarch64_unwind_load_ret_pc:
    ldr x3, [x0, #8]
    str x2, [x1]
    str x3, [x1, #8]
    mov w0, #1
    ret

aarch64_unwind_load_fail:
    mov w0, #0
    ret
