[bits 32]
GLOBAL  _start
GLOBAL io_hlt
GLOBAL io_cli
GLOBAL io_in8
GLOBAL io_in16
GLOBAL io_in32
GLOBAL io_out8
GLOBAL io_out16
GLOBAL io_out32
GLOBAL io_load_eflags
GLOBAL io_store_eflags
EXTERN  main

_start:
        mov  eax, main
        jmp  eax

io_hlt:
        hlt
        ret

io_cli:
        cli
        ret

io_in8:
        mov  edx, [esp + 4]
        mov  eax, 0
        in   al, dx
        ret

io_in16:
        mov  edx, [esp + 4]
        mov  eax, 0
        in   ax, dx
        ret

io_in32:
        mov edx, [esp + 4]
        in  eax, dx
        ret

io_out8:
        mov edx, [esp + 4]
        mov al, [esp + 8]
        out dx, al
        ret

io_out16:
        mov edx, [esp + 4]
        mov eax, [esp + 8]
        out dx, ax
        ret

io_out32:
        mov edx, [esp + 4]
        mov eax, [esp + 8]
        out dx, eax
        ret

io_load_eflags:
        pushfd
        pop  eax
        ret

io_store_eflags:
        mov eax, [esp + 4]
        push eax
        popfd
        ret
