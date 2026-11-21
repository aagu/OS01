.section .text
.globl x86_unwind_load_pair
.globl x86_unwind_load_next_fp
.globl x86_unwind_load_ret_pc
.globl x86_unwind_load_fail

/* bool x86_unwind_load_pair(uintptr_t addr, struct unwind_frame *out);
 *   %rdi = addr (preserved across both loads for #PF CR2 matching)
 *   %rsi = out
 */
x86_unwind_load_pair:
x86_unwind_load_next_fp:
    movq (%rdi), %rax
x86_unwind_load_ret_pc:
    movq 8(%rdi), %rdx
    movq %rax, (%rsi)
    movq %rdx, 8(%rsi)
    movl $1, %eax
    ret

x86_unwind_load_fail:
    xorl %eax, %eax
    ret
