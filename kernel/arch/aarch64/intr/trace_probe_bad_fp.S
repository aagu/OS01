.section .text
.globl aarch64_trace_bad_fp_trigger
.globl aarch64_trace_bad_fp_pc

aarch64_trace_bad_fp_trigger:
    mov x29, #0x1000
    movk x29, #0xffff, lsl #48
aarch64_trace_bad_fp_pc:
    brk #0
    ret
