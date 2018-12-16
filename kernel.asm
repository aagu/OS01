%include "pm.inc"

LEDS   equ  0x0ff1
VMODE  equ  0x0ff2     ;关于颜色数目的信息。颜色的位数
SCRNX  equ  0x0ff4     ;分辨率X
SCRNY  equ  0x0ff6     ;分辨率Y
VRAM   equ  0x0ff8     ;图像缓冲区起始地址

ORG  0xc200

[SECTION .gdt]
 ;                                  段基址          段界限                属性
LABEL_GDT:          Descriptor        0,            0,                   0  
LABEL_DESC_CODE32:  Descriptor        0,      SegCode32Len - 1,       DA_C + DA_32
LABEL_DESC_VIDEO:   Descriptor     0B8000h,         0ffffh,            DA_DRW

GdtLen     equ    $ - LABEL_GDT
GdtPtr     dw     GdtLen - 1
           dd     0

SelectorCode32    equ   LABEL_DESC_CODE32 -  LABEL_GDT
SelectorVideo     equ   LABEL_DESC_VIDEO  -  LABEL_GDT

[SECTION  .s16]
[BITS  16]
LABEL_BEGIN:
    mov   ax, cs
    mov   ds, ax
    mov   es, ax
    mov   ss, ax
    mov   sp, 0100h

    xor   eax, eax
    mov   ax,  cs
    shl   eax, 4
    add   eax, LABEL_SEG_CODE32
    mov   word [LABEL_DESC_CODE32 + 2], ax
    shr   eax, 16
    mov   byte [LABEL_DESC_CODE32 + 4], al
    mov   byte [LABEL_DESC_CODE32 + 7], ah

    xor   eax, eax
    mov   ax, ds
    shl   eax, 4
    add   eax,  LABEL_GDT
    mov   dword  [GdtPtr + 2], eax

    lgdt  [GdtPtr]

    cli   ;关中断

    in    al,  92h
    or    al,  00000010b
    out   92h, al

    mov   eax, cr0
    or    eax , 1
    mov   cr0, eax

    jmp   dword  SelectorCode32: 0

[SECTION .s32]
[BITS  32]
LABEL_SEG_CODE32:
    mov   ax, SelectorVideo
    mov   gs, ax
    mov   si, msg
    mov   ebx, 10
    mov   ecx, 2
showChar:
    mov   edi, (80*5)
    add   edi, ebx
    mov   eax, edi
    mul   ecx
    mov   edi, eax
    mov   ah, 0bh
    mov   al, [si]
    cmp   al, 0
    je    fin
    add   ebx,1
    add   si, 1
    mov   [gs:edi], ax
    jmp   showChar
fin:
    hlt
    jmp  fin
msg:
    DB     "Protect Mode", 0

SegCode32Len   equ  $ - LABEL_SEG_CODE32