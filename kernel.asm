ORG  0xc200

    mov  al, 0x13
    mov  ah, 0x00
    int  0x10
    mov  si, msg

putloop:               ;显示消息
    mov  al, [si]
    add  si, 1
    cmp  al, 0
    je   fin
    mov  ah, 0x0e
    mov  bx, 15
    int  0x10
    jmp  putloop

fin:
    hlt
    jmp  fin

msg:
    DB  0x0a, 0x0a
    DB "GVA mode, print from kernel"
    DB  0x0a