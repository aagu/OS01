[bits 32]
GLOBAL  _start
EXTERN  main

_start:
        mov  eax, main
        jmp  eax

stop:
        hlt
        jmp     stop


