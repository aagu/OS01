[bits 16]
KERNEL_ADDR  equ  0xc200

ORG  0x7c00

jmp  entry
db   0x90
DB   "OSKERNEL"
DW   512
DB   1
DW   1
DB   2
DW   224
DW   2880
DB   0xf0
DW   9
DW   18
DW   2
DD   0
DD   2880
DB   0,0,0x29
DD   0xFFFFFFFF
DB   "MYFIRSTOS  "
DB   "FAT12   "
times  18  DB 0

entry:
    mov  ax, 0
    mov  ss, ax
    mov  sp, 0x7c00
    mov  ds, ax
    mov  es, ax

readfloppy:
    mov  ax, 0x0820   ;将数据读到内存0x0820之后，以免覆盖当前内容
    mov  bx, KERNEL_ADDR
    mov  ch, 0        ;CH 用来存储柱面号
    mov  dh, 0        ;DH 用来存储磁头号
    mov  cl, 2        ;CL 用来存储扇区号

readloop:
    mov  si, 0

retry:
    mov  ah, 0x02      ; AH = 02 表示要做的是读盘操作
    mov  al, 1         ; AL 表示要连续读取几个扇区
    mov  dl, 0x00      ;驱动器编号，A盘
    int  0x13          ;调用BIOS中断实现磁盘读取功能
    jnc  next
    add  si, 1
    cmp  si, 5         ;重试5次
    jae  error
    mov  ah, 0x00
    mov  dl, 0x00
    int  0x13          ;重置驱动器
    jmp  retry

next:
    mov  ax, bx
    add  ax, 0x0200
    mov  bx, ax        ;地址后移512byte
    add  cl, 1
    cmp  cl ,18

goto_PM:
    mov  al, 0x03
    mov  ah, 0x00
    int  0x10

    mov  al, 0xff
    out 0x21, al
    nop
    out 0xa1, al

    cli

    call  waitkbd_8042
    mov  al, 0xd1
    out  0x64, al
    call  waitkbd_8042
    mov  al, 0xdf
    out  0x60, al
    call  waitkbd_8042

    mov  ah, 0x0e
    mov  al, 'O'
    int  0x10

    cli
    lgdt  [GDTR0]
    in  al, 92h
    or  al, 0x02
    out  92h, al
    mov  eax, cr0
    or  al, 1
    mov  cr0, eax
    jmp  dword 0x08:PM_MODE

[bits 32]
PM_MODE:
	MOV	EAX,0x00000010
	MOV	DS,AX
	MOV	ES,AX
	MOV	FS,AX
	MOV	GS,AX
	MOV	SS,AX
       
    MOV     EAX,0x8080
    JMP     EAx ;dword 0x08:0x8200
;
;	显示需要的相关字符串
;

waitkbd_8042:
	IN	AL,0x64
	AND	AL,0x02    ;输入缓冲区是否满了？
	JNZ	waitkbd_8042 ;Yes---跳转
	RET

;
;进入保护模式后，不再按照CS*16+IP取指令执行，需要按照向全局描述符
;	具体可参考《linux内核设计的艺术》
;

GDT0:
	DW      0x0000,0x0000,0x0000,0x0000
        ;---代码段基地址 0x0047取00，0x9a28取28，0x0000取全部===0x00280000
	DW	0xffff,0x0000,0x9a00,0x00cf
        ;---数据段基地址 0x00cf取00，0x9200取00，0x0000取全部===0x00000000
	DW	0xffff,0x0000,0x9200,0x00cf
        DW      0xffff,0x8000,0xf20b,0x000f
        ;为tss准备的
	DW      0x0000,0x0000,0x0000,0x0000
        ;为idt准备的
	DW      0x0000,0x0000,0x0000,0x0000
        ;DW      0xffff,0x8000,0xf20b,0x000f
GDT0_LEN EQU $-GDT0
GDTR0:
	DW	GDT0_LEN-1
	DD	GDT0

error:
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
    DB "load error"
    DB  0x0a

    gdt_size         dw 0
    gdt_base         dd 0x00007e00     ;GDT的物理地址 

    ;填充空间，使得扇区末尾为aa55，方能被识别为MBR引导
    times	510-($-$$) DB 0
	DW	0xaa55