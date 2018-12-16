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

    mov  al, 0x101
    mov  ah,0x00
    int  0x10

    jmp  KERNEL_ADDR

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

    ;填充空间，使得扇区末尾为aa55，方能被识别为MBR引导
    times	510-($-$$) DB 0
	DW	0xaa55