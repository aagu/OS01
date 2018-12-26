[bits 16]
CYLS         equ  10d

ORG  0x7c00

jmp  entry
DB   0x90
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

VBEMODE	EQU		0x103			;800 x  600 x 8bit 彩色

; BOOT_INFO 相关
VMODE	EQU		0x0ff2			; 关于颜色的信息
SCRNX	EQU		0x0ff4			; 分辨率X
SCRNY	EQU		0x0ff6			; 分辨率Y
VRAM	EQU		0x0ff8			; 图像缓冲区的起始地址

entry:
    mov  ax, 0
    mov  ss, ax
    mov  sp, 0x7c00
    mov  ds, ax
    mov  es, ax

readfloppy:
    mov  ax, 0x7e0
    mov  es, ax   ;将数据读到内存0x07e00之后，以免覆盖当前内容
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
    mov  ax, es
    add  ax, 0x020
    mov  es, ax        ;地址后移512byte
    add  cl, 1
    cmp  cl ,18
    jbe  readloop
    mov  cl, 1
    add  dh, 1
    cmp  dh, 2
    jb   readloop
    mov  dh, 0
    add  ch, 1
    cmp  ch, CYLS
    jb   readloop
    mov  [0x0ff0],ch	;保存读取的扇区数

goto_PM:
;确认VBE是否存在
    mov  ax, 0x9000
    mov  es, ax
    mov  di, 0
    mov  ax, 0x4f00
    int  0x10
    cmp  ax, 0x004f
    jne  scrn320
;检测VBE版本
    mov  ax, [es:di+4]
    cmp  ax, 0x0200
    jb  scrn320
;取得画面模式信息
    mov  cx, VBEMODE  ;800 x  600 x 8bit 彩色
    mov  ax, 0x4f01
    int  0x10
    cmp  ax, 0x004f
    jne  scrn320
; 画面模式信息的确认
	cmp  BYTE [es:di+0x19],8		;颜色数必须为8
	jne  scrn320
	cmp  BYTE [es:di+0x1b],4		;颜色的指定方法必须为4(4是调色板模式)
	jne  scrn320
	mov  ax,[es:di+0x00]				;模式属性bit7不是1就不能加上0x4000
	and  ax,0x0080
	jz  scrn320					; 模式属性的bit7是0，所以放弃

;	画面设置

	mov  BX,VBEMODE+0x4000
	mov  ax,0x4f02
	int  0x10
	mov  BYTE [VMODE],8	; 屏幕的模式（参考C语言的引用）
	mov  ax,[es:di+0x12]
	mov  [SCRNX],ax
	mov  ax,[es:di+0x14]
	mov  [SCRNY],ax
	mov  eax,[es:di+0x28] ;VRAM的地址
	mov  [VRAM],eax
	jmp  keystatus

scrn320:
	mov  AL,0x13						; VGA图、320x200x8bit彩色
	mov  AH,0x00
	int  0x10
	mov  BYTE [VMODE],8		; 记下画面模式（参考C语言）
	mov  WORD [SCRNX],320
	mov  WORD [SCRNY],200
	mov  DWORD [VRAM],0x000a0000

keystatus:
    mov  al, 0xff
    out 0x21, al
    nop
    out 0xa1, al

    cli

    ;通过键盘控制器打开A20
    call  waitkbd_8042
    mov  al, 0xd1
    out  0x64, al
    call  waitkbd_8042
    mov  al, 0xdf
    out  0x60, al
    call  waitkbd_8042

    cli
    xor  ax, ax
    mov  ds, ax
    lgdt  [gdt_desc]
    ;对于较新的机器，也可以通过此方法开启A20
    in  al, 92h
    or  al, 0x02
    out  92h, al
    mov  eax, cr0
    or  eax, 1
    mov  cr0, eax
    jmp  08h:PM_MODE

[bits 32]
PM_MODE:
	mov  ax, 10h             ; Save data segment identifyer
    mov  ds, ax              ; Move a valid data segment into the data segment register
    mov  ss, ax              ; Move a valid data segment into the stack segment register
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  esp, 090000h        ; Move the stack pointer to 090000h
    jmp  08h:7e00h

waitkbd_8042:
	IN	AL,0x64
	AND	AL,0x02    ;输入缓冲区是否满了？
	JNZ	waitkbd_8042 ;Yes---跳转
	RET

ALIGN 16
gdt:                    ; Address for the GDT
gdt_null:               ; Null Segment
        DD 0
        DD 0
gdt_code:               ; Code segment, read/execute, nonconforming
        DW 0FFFFh
        DW 0
        DB 0
        DB 10011010b
        DB 11001111b
        DB 0
gdt_data:               ; Data segment, read/write, expand down
        DW 0FFFFh
        DW 0
        DB 0
        DB 10010010b
        DB 11001111b
        DB 0
gdt_end:                ; Used to calculate the size of the GDT

gdt_desc:                       ; The GDT descriptor
        DW gdt_end - gdt - 1    ; Limit (size)
        DD gdt                  ; Address of the GDT
ALIGN 16

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