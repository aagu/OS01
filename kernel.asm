LEDS   equ  0x0ff1
VMODE  equ  0x0ff2     ;关于颜色数目的信息。颜色的位数
SCRNX  equ  0x0ff4     ;分辨率X
SCRNY  equ  0x0ff6     ;分辨率Y
VRAM   equ  0x0ff8     ;图像缓冲区起始地址

ORG  0xc200

;进入画面模式
    mov  al, 0x13
    mov  ah, 0x00
    int  0x10
    mov  byte [VMODE], 8
    mov  word [SCRNX], 320
    mov  word [SCRNY], 200
    mov  dword [VRAM], 0x000a0000

;获取键盘灯状态
    mov  ah, 0x02
    int  0x16
    mov  [LEDS], al

;显示消息
    mov  si, msg

showchar:               ;显示消息
    mov  al, [si]
    add  si, 1
    cmp  al, 0
    je   fin
    mov  ah, 0x0e
    mov  bx, 15
    int  0x10
    jmp  showchar

fin:
    hlt
    jmp  fin

msg:
    DB  0x0a, 0x0a
    DB "GVA mode, print from kernel"
    DB  0x0a