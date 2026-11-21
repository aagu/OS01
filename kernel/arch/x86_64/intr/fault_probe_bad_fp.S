.section .text
.globl x86_trace_bad_fp_trigger
.globl x86_intr_fault_bad_fp_resume

x86_trace_bad_fp_trigger:
    movq $0xffff800000001000, %rbp
    int3
x86_intr_fault_bad_fp_resume:
    ret
