[bits 32]
GLOBAL  _start
GLOBAL io_hlt
GLOBAL io_cli_hlt
EXTERN  main

_start:
        mov  eax, main
        jmp  eax

stop:
        hlt
        jmp     stop

io_hlt:
        hlt
        ret

io_cli_hlt:
        cli
        hlt
        ret