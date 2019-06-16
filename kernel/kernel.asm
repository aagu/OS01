[bits 32]
GLOBAL  _start
GLOBAL io_hlt, io_cli, io_sti, io_stihlt
GLOBAL io_in8, io_in16, io_in32
GLOBAL io_out8, io_out16, io_out32
GLOBAL io_load_eflags, io_store_eflags
GLOBAL load_gdtr, load_idtr
GLOBAL isr_common_stub, irq_common_stub
GLOBAL load_cr0, store_cr0, memtest_sub
GLOBAL load_tr, taskswitch7
GLOBAL systemFont
EXTERN  main
EXTERN	isr_handler, irq_handler

_start:
    mov  eax, main
    jmp  eax

io_hlt:
    hlt
    ret

io_cli:
    cli
    ret

io_sti:
    sti
    ret

io_stihlt:
    sti
    hlt
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

load_gdtr:
    mov  ax,[esp+4]
    lgdt  [eax]
    ret

load_idtr:
    mov  ax,[esp+4]
    lidt  [eax]
    ret

load_cr0:		; int load_cr0(void);
	mov		eax, cr0
	ret

store_cr0:		; void store_cr0(int cr0);
	mov		eax, [esp+4]
	mov		cr0, eax
	ret

memtest_sub:	; unsigned int memtest_sub(unsigned int start, unsigned int end)
	push	edi						; （由于还要使用ebx, esi, edi）
	push	esi
	push	ebx
	mov		esi,0xaa55aa55			; pat0 = 0xaa55aa55;
	mov		edi,0x55aa55aa			; pat1 = 0x55aa55aa;
	mov		eax,[esp+12+4]			; i = start;
mts_loop:
	mov		ebx,eax
	add		ebx,0xffc				; p = i + 0xffc;
	mov		edx,[ebx]				; old = *p;
	mov		[ebx],esi				; *p = pat0;
	xor		DWORD [ebx],0xffffffff	; *p ^= 0xffffffff;
	cmp		edi,[ebx]				; if (*p != pat1) goto fin;
	jne		mts_fin
	xor		DWORD [ebx],0xffffffff	; *p ^= 0xffffffff;
	cmp		esi,[ebx]				; if (*p != pat0) goto fin;
	jne		mts_fin
	mov		[ebx],edx				; *p = old;
	add		eax,0x1000				; i += 0x1000;
	cmp		eax,[esp+12+8]			; if (i <= end) goto mts_loop;
	jbe		mts_loop
	pop		ebx
	pop		esi
	pop		edi
	ret
mts_fin:
	mov		[ebx],edx				; *p = old;
	pop		ebx
	pop		esi
	pop		edi
	ret

load_tr:
	ltr     [esp + 4]
	ret

taskswitch7:
	jmp     7*8:0
	ret

; 定义两个构造中断处理函数的宏(有的中断有错误代码，有的没有)
; 用于没有错误代码的中断
%macro ISR_NOERRCODE 1
[GLOBAL isr%1]
isr%1:
	cli      ; 首先关闭中断
	push 0   ; push 无效的中断错误代码(起到占位作用，便于所有isr函数统一清栈)
	push %1  ; push 中断号
	jmp isr_common_stub
%endmacro

; 用于有错误代码的中断
%macro ISR_ERRCODE 1
[GLOBAL isr%1]
isr%1:
	cli                         ; 关闭中断
	push %1                     ; push 中断号
	jmp isr_common_stub
%endmacro

; 定义中断处理函数
ISR_NOERRCODE  0 	; 0 #DE 除 0 异常
ISR_NOERRCODE  1 	; 1 #DB 调试异常
ISR_NOERRCODE  2 	; 2 NMI
ISR_NOERRCODE  3 	; 3 BP 断点异常
ISR_NOERRCODE  4 	; 4 #OF 溢出
ISR_NOERRCODE  5 	; 5 #BR 对数组的引用超出边界
ISR_NOERRCODE  6 	; 6 #UD 无效或未定义的操作码
ISR_NOERRCODE  7 	; 7 #NM 设备不可用(无数学协处理器)
ISR_ERRCODE    8 	; 8 #DF 双重故障(有错误代码)
ISR_NOERRCODE  9 	; 9 协处理器跨段操作
ISR_ERRCODE   10 	; 10 #TS 无效TSS(有错误代码)
ISR_ERRCODE   11 	; 11 #NP 段不存在(有错误代码)
ISR_ERRCODE   12 	; 12 #SS 栈错误(有错误代码)
ISR_ERRCODE   13 	; 13 #GP 常规保护(有错误代码)
ISR_ERRCODE   14 	; 14 #PF 页故障(有错误代码)
ISR_NOERRCODE 15 	; 15 CPU 保留
ISR_NOERRCODE 16 	; 16 #MF 浮点处理单元错误
ISR_ERRCODE   17 	; 17 #AC 对齐检查
ISR_NOERRCODE 18 	; 18 #MC 机器检查
ISR_NOERRCODE 19 	; 19 #XM SIMD(单指令多数据)浮点异常

; 20~31 Intel 保留
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31
; 32～255 用户自定义
ISR_NOERRCODE 255


; 中断服务程序
isr_common_stub:
	pusha            ; Pushes edi, esi, ebp, esp, ebx, edx, ecx, eax
	mov ax, ds
	push eax         ; 保存数据段描述符
	
	mov ax, 0x10     ; 加载内核数据段描述符表
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax
	
	push esp	   ; 此时的 esp 寄存器的值等价于 pt_regs 结构体的指针
	call isr_handler   ; 在 C 语言代码里
	add esp, 4 	   ; 清除压入的参数
	
	pop ebx            ; 恢复原来的数据段描述符
	mov ds, bx
	mov es, bx
	mov fs, bx
	mov gs, bx
	mov ss, bx
	
	popa               ; Pops edi, esi, ebp, esp, ebx, edx, ecx, eax
	add esp, 8         ; 清理栈里的 error code 和 ISR
	iret
.end:

; 构造中断请求的宏
%macro IRQ 2
[GLOBAL irq%1]
irq%1:
	cli
	push 0
	push %2
	jmp irq_common_stub
%endmacro

IRQ   0,    32 	; 电脑系统计时器
IRQ   1,    33 	; 键盘
IRQ   2,    34 	; 与 IRQ9 相接，MPU-401 MD 使用
IRQ   3,    35 	; 串口设备
IRQ   4,    36 	; 串口设备
IRQ   5,    37 	; 建议声卡使用
IRQ   6,    38 	; 软驱传输控制使用
IRQ   7,    39 	; 打印机传输控制使用
IRQ   8,    40 	; 即时时钟
IRQ   9,    41 	; 与 IRQ2 相接，可设定给其他硬件
IRQ  10,    42 	; 建议网卡使用
IRQ  11,    43 	; 建议 AGP 显卡使用
IRQ  12,    44 	; 接 PS/2 鼠标，也可设定给其他硬件
IRQ  13,    45 	; 协处理器使用
IRQ  14,    46 	; IDE0 传输控制使用
IRQ  15,    47 	; IDE1 传输控制使用

irq_common_stub:
	pusha                    ; pushes edi, esi, ebp, esp, ebx, edx, ecx, eax
	
	mov ax, ds
	push eax                 ; 保存数据段描述符
	
	mov ax, 0x10  		 ; 加载内核数据段描述符
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax
	
	push esp
	call irq_handler
	add esp, 4
	
	pop ebx                   ; 恢复原来的数据段描述符
	mov ds, bx
	mov es, bx
	mov fs, bx
	mov gs, bx
	mov ss, bx
	
	popa                     ; Pops edi,esi,ebp...
	add esp, 8     		 ; 清理压栈的 错误代码 和 ISR 编号
	iret          		 ; 出栈 CS, EIP, EFLAGS, SS, ESP
.end:

;系统字体
systemFont:
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,070H ,088H ,04H ,054H ,054H ,04H ,04H ,054H ,024H ,088H ,070H ,00H ,00H ,00H
db 00H ,00H ,070H ,0f8H ,0fcH ,0acH ,0acH ,0fcH ,0fcH ,0acH ,0dcH ,0f8H ,070H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,0d8H ,0fcH ,0fcH ,0fcH ,0f8H ,070H ,020H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,020H ,070H ,0f8H ,0fcH ,0f8H ,070H ,020H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,020H ,070H ,0a8H ,0fcH ,0a8H ,020H ,070H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,020H ,070H ,0f8H ,0fcH ,0acH ,020H ,070H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,030H ,078H ,078H ,030H ,00H ,00H ,00H ,00H ,00H ,00H
db 0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0ceH ,086H ,086H ,0ceH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH
db 00H ,00H ,00H ,00H ,00H ,078H ,0ccH ,084H ,084H ,0ccH ,078H ,00H ,00H ,00H ,00H ,00H
db 0feH ,0feH ,0feH ,0feH ,0feH ,086H ,032H ,07aH ,07aH ,032H ,086H ,0feH ,0feH ,0feH ,0feH ,0feH
db 00H ,020H ,070H ,0a8H ,024H ,020H ,020H ,070H ,088H ,04H ,04H ,04H ,088H ,070H ,00H ,00H
db 00H ,070H ,088H ,04H ,04H ,04H ,088H ,070H ,020H ,020H ,0fcH ,020H ,020H ,020H ,00H ,00H
db 00H ,00H ,018H ,01cH ,016H ,016H ,014H ,010H ,010H ,030H ,0f0H ,0f0H ,0e0H ,00H ,00H ,00H
db 00H ,00H ,03eH ,03eH ,022H ,022H ,022H ,022H ,022H ,022H ,0eeH ,0feH ,0ccH ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,020H ,0a8H ,070H ,050H ,070H ,0a8H ,020H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,080H ,0c0H ,0e0H ,0f0H ,0f8H ,0fcH ,0f8H ,0f0H ,0e0H ,0c0H ,080H ,00H ,00H ,00H
db 00H ,04H ,0cH ,01cH ,03cH ,07cH ,0fcH ,0fcH ,0fcH ,07cH ,03cH ,01cH ,0cH ,04H ,00H ,00H
db 00H ,00H ,020H ,070H ,0a8H ,024H ,020H ,020H ,020H ,024H ,0a8H ,070H ,020H ,00H ,00H ,00H
db 00H ,00H ,088H ,088H ,088H ,088H ,088H ,088H ,088H ,088H ,00H ,00H ,088H ,088H ,00H ,00H
db 00H ,07cH ,094H ,014H ,014H ,014H ,014H ,094H ,074H ,014H ,014H ,014H ,014H ,014H ,00H ,00H
db 0f8H ,04H ,080H ,040H ,070H ,088H ,04H ,04H ,04H ,088H ,070H ,010H ,08H ,04H ,0f8H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,0fcH ,0fcH ,0fcH ,00H ,00H
db 00H ,00H ,020H ,070H ,0a8H ,024H ,020H ,020H ,020H ,024H ,0a8H ,070H ,020H ,0f8H ,00H ,00H
db 00H ,020H ,070H ,0a8H ,024H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,00H ,00H
db 00H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,024H ,0a8H ,070H ,020H ,00H ,00H
db 00H ,00H ,00H ,00H ,020H ,010H ,08H ,0fcH ,08H ,010H ,020H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,020H ,040H ,080H ,0fcH ,080H ,040H ,020H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,0fcH ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,050H ,088H ,0fcH ,088H ,050H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,020H ,020H ,070H ,070H ,0f8H ,0f8H ,0fcH ,0fcH ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,0fcH ,0fcH ,0f8H ,0f8H ,070H ,070H ,020H ,020H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,00H ,00H ,020H ,020H ,00H ,00H
db 050H ,050H ,050H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,088H ,088H ,088H ,0fcH ,088H ,088H ,088H ,088H ,088H ,0fcH ,088H ,088H ,088H ,00H ,00H
db 020H ,074H ,0acH ,024H ,024H ,020H ,0a0H ,070H ,028H ,024H ,024H ,024H ,0a8H ,070H ,020H ,020H
db 0c4H ,024H ,028H ,028H ,0d0H ,010H ,020H ,020H ,040H ,058H ,0a4H ,0a4H ,024H ,018H ,00H ,00H
db 00H ,0e0H ,010H ,010H ,010H ,020H ,0c0H ,08eH ,044H ,024H ,014H ,08H ,08cH ,072H ,00H ,00H
db 08H ,010H ,020H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 04H ,08H ,010H ,010H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,010H ,010H ,08H ,04H ,00H
db 00H ,080H ,040H ,040H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,040H ,040H ,080H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,020H ,024H ,0a8H ,070H ,0a8H ,024H ,020H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,020H ,020H ,020H ,0fcH ,020H ,020H ,020H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,030H ,030H ,010H ,010H ,020H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,0fcH ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,030H ,030H ,00H ,00H
db 04H ,04H ,08H ,08H ,010H ,010H ,010H ,020H ,020H ,040H ,040H ,080H ,080H ,080H ,00H ,00H
db 00H ,030H ,048H ,048H ,084H ,084H ,084H ,084H ,084H ,084H ,084H ,048H ,048H ,030H ,00H ,00H
db 00H ,010H ,030H ,050H ,010H ,010H ,010H ,010H ,010H ,010H ,010H ,010H ,010H ,07cH ,00H ,00H
db 00H ,030H ,048H ,084H ,084H ,04H ,08H ,010H ,020H ,040H ,040H ,080H ,080H ,0fcH ,00H ,00H
db 00H ,030H ,048H ,084H ,04H ,04H ,08H ,030H ,08H ,04H ,04H ,084H ,048H ,030H ,00H ,00H
db 00H ,018H ,018H ,018H ,028H ,028H ,028H ,048H ,048H ,088H ,0fcH ,08H ,08H ,03cH ,00H ,00H
db 00H ,0f8H ,080H ,080H ,080H ,0b0H ,0c8H ,04H ,04H ,04H ,04H ,084H ,048H ,030H ,00H ,00H
db 00H ,030H ,048H ,084H ,080H ,0b0H ,0c8H ,084H ,084H ,084H ,084H ,084H ,048H ,030H ,00H ,00H
db 00H ,0fcH ,084H ,084H ,08H ,08H ,010H ,010H ,010H ,020H ,020H ,020H ,020H ,070H ,00H ,00H
db 00H ,030H ,048H ,084H ,084H ,084H ,048H ,030H ,048H ,084H ,084H ,084H ,048H ,030H ,00H ,00H
db 00H ,030H ,048H ,084H ,084H ,084H ,084H ,084H ,04cH ,034H ,04H ,084H ,048H ,030H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,030H ,030H ,00H ,00H ,00H ,00H ,00H ,030H ,030H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,030H ,030H ,00H ,00H ,00H ,00H ,030H ,030H ,010H ,010H ,020H
db 00H ,04H ,08H ,010H ,020H ,040H ,080H ,00H ,00H ,080H ,040H ,020H ,010H ,08H ,04H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,0fcH ,00H ,00H ,0fcH ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,080H ,040H ,020H ,010H ,08H ,04H ,04H ,08H ,010H ,020H ,040H ,080H ,00H ,00H
db 00H ,070H ,088H ,04H ,04H ,04H ,08H ,010H ,020H ,020H ,00H ,00H ,030H ,030H ,00H ,00H
db 00H ,070H ,088H ,04H ,034H ,054H ,054H ,054H ,054H ,054H ,038H ,00H ,08cH ,070H ,00H ,00H
db 00H ,030H ,030H ,030H ,030H ,048H ,048H ,048H ,048H ,0fcH ,084H ,084H ,084H ,0ceH ,00H ,00H
db 00H ,0e0H ,090H ,088H ,088H ,088H ,090H ,0f0H ,088H ,084H ,084H ,084H ,088H ,0f0H ,00H ,00H
db 00H ,074H ,08cH ,084H ,04H ,00H ,00H ,00H ,00H ,00H ,04H ,084H ,088H ,070H ,00H ,00H
db 00H ,0f0H ,088H ,088H ,084H ,084H ,084H ,084H ,084H ,084H ,084H ,088H ,088H ,0f0H ,00H ,00H
db 00H ,0fcH ,084H ,084H ,080H ,080H ,088H ,0f8H ,088H ,080H ,080H ,084H ,084H ,0fcH ,00H ,00H
db 00H ,0fcH ,084H ,084H ,080H ,080H ,088H ,0f8H ,088H ,088H ,080H ,080H ,080H ,0e0H ,00H ,00H
db 00H ,074H ,08cH ,084H ,04H ,00H ,00H ,03cH ,04H ,04H ,04H ,084H ,08cH ,070H ,00H ,00H
db 00H ,0ceH ,084H ,084H ,084H ,084H ,084H ,0fcH ,084H ,084H ,084H ,084H ,084H ,0ceH ,00H ,00H
db 00H ,0f8H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,0f8H ,00H ,00H
db 00H ,03eH ,08H ,08H ,08H ,08H ,08H ,08H ,08H ,08H ,08H ,08H ,08H ,090H ,060H ,00H
db 00H ,0ceH ,084H ,088H ,090H ,0a0H ,0a0H ,0c0H ,0a0H ,0a0H ,090H ,088H ,084H ,0ceH ,00H ,00H
db 00H ,0e0H ,080H ,080H ,080H ,080H ,080H ,080H ,080H ,080H ,080H ,084H ,084H ,0fcH ,00H ,00H
db 00H ,086H ,084H ,0ccH ,0ccH ,0ccH ,0b4H ,0b4H ,0b4H ,084H ,084H ,084H ,084H ,0ceH ,00H ,00H
db 00H ,08eH ,084H ,0c4H ,0c4H ,0a4H ,0a4H ,0a4H ,094H ,094H ,094H ,08cH ,08cH ,0c4H ,00H ,00H
db 00H ,070H ,088H ,04H ,04H ,04H ,04H ,04H ,04H ,04H ,04H ,04H ,088H ,070H ,00H ,00H
db 00H ,0f0H ,088H ,084H ,084H ,084H ,088H ,0f0H ,080H ,080H ,080H ,080H ,080H ,0e0H ,00H ,00H
db 00H ,070H ,088H ,04H ,04H ,04H ,04H ,04H ,04H ,04H ,024H ,014H ,088H ,074H ,00H ,00H
db 00H ,0f8H ,084H ,084H ,084H ,084H ,0f8H ,088H ,084H ,084H ,084H ,084H ,084H ,0ceH ,00H ,00H
db 00H ,074H ,08cH ,04H ,04H ,00H ,080H ,070H ,08H ,04H ,04H ,04H ,088H ,070H ,00H ,00H
db 00H ,0fcH ,024H ,024H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,0f8H ,00H ,00H
db 00H ,0ceH ,084H ,084H ,084H ,084H ,084H ,084H ,084H ,084H ,084H ,084H ,048H ,078H ,00H ,00H
db 00H ,0ceH ,084H ,084H ,084H ,084H ,048H ,048H ,048H ,048H ,030H ,030H ,030H ,030H ,00H ,00H
db 00H ,0ceH ,084H ,084H ,084H ,0b4H ,0b4H ,0b4H ,0b4H ,048H ,048H ,048H ,048H ,048H ,00H ,00H
db 00H ,0ceH ,084H ,084H ,048H ,048H ,048H ,030H ,048H ,048H ,048H ,084H ,084H ,0ceH ,00H ,00H
db 00H ,0dcH ,088H ,088H ,088H ,050H ,050H ,050H ,020H ,020H ,020H ,020H ,020H ,0f8H ,00H ,00H
db 00H ,0fcH ,08H ,08H ,010H ,010H ,020H ,020H ,040H ,040H ,080H ,084H ,04H ,0fcH ,00H ,00H
db 00H ,07cH ,040H ,040H ,040H ,040H ,040H ,040H ,040H ,040H ,040H ,040H ,040H ,040H ,07cH ,00H
db 00H ,00H ,080H ,080H ,040H ,040H ,040H ,020H ,020H ,010H ,010H ,08H ,08H ,08H ,04H ,04H
db 00H ,0f8H ,08H ,08H ,08H ,08H ,08H ,08H ,08H ,08H ,08H ,08H ,08H ,08H ,0f8H ,00H
db 00H ,020H ,050H ,088H ,04H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,0fcH ,00H
db 020H ,010H ,08H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,0e0H ,010H ,08H ,078H ,088H ,08H ,08H ,018H ,0ecH ,00H ,00H
db 080H ,080H ,080H ,080H ,080H ,0b0H ,0c8H ,084H ,084H ,084H ,084H ,084H ,0c8H ,0b0H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,060H ,098H ,08H ,08H ,00H ,00H ,04H ,088H ,070H ,00H ,00H
db 018H ,08H ,08H ,08H ,08H ,068H ,098H ,08H ,08H ,08H ,08H ,08H ,098H ,06cH ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,070H ,088H ,04H ,04H ,0f8H ,00H ,04H ,084H ,078H ,00H ,00H
db 01cH ,020H ,020H ,020H ,020H ,0f8H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,0f8H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,06cH ,098H ,08H ,08H ,08H ,08H ,098H ,068H ,08H ,08H ,0f0H
db 080H ,080H ,080H ,080H ,080H ,0b0H ,0c8H ,084H ,084H ,084H ,084H ,084H ,084H ,0c6H ,00H ,00H
db 00H ,020H ,020H ,00H ,00H ,060H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,070H ,00H ,00H
db 00H ,08H ,08H ,00H ,00H ,018H ,08H ,08H ,08H ,08H ,08H ,08H ,08H ,010H ,010H ,060H
db 080H ,080H ,080H ,080H ,080H ,09cH ,088H ,090H ,0a0H ,0c0H ,0a0H ,090H ,088H ,0ccH ,00H ,00H
db 060H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,070H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,0ecH ,092H ,092H ,092H ,092H ,092H ,092H ,092H ,0b6H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,0b0H ,0c8H ,084H ,084H ,084H ,084H ,084H ,084H ,0c6H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,070H ,088H ,04H ,04H ,04H ,04H ,04H ,088H ,070H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,0b0H ,0c8H ,084H ,084H ,084H ,084H ,084H ,0c8H ,0b0H ,080H ,0c0H
db 00H ,00H ,00H ,00H ,00H ,068H ,098H ,08H ,08H ,08H ,08H ,08H ,098H ,068H ,08H ,01cH
db 00H ,00H ,00H ,00H ,00H ,0b8H ,0c4H ,084H ,080H ,080H ,080H ,080H ,080H ,0c0H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,0f4H ,0cH ,04H ,080H ,070H ,0cH ,04H ,084H ,078H ,00H ,00H
db 00H ,00H ,020H ,020H ,020H ,0f8H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,01cH ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,08cH ,084H ,084H ,084H ,084H ,084H ,084H ,08cH ,076H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,0ceH ,084H ,084H ,084H ,048H ,048H ,048H ,030H ,030H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,0ceH ,084H ,084H ,0b4H ,0b4H ,0b4H ,048H ,048H ,048H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,08cH ,088H ,050H ,050H ,020H ,050H ,050H ,088H ,08cH ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,0ceH ,084H ,084H ,048H ,048H ,048H ,030H ,030H ,020H ,020H ,0c0H
db 00H ,00H ,00H ,00H ,00H ,0fcH ,04H ,08H ,010H ,020H ,040H ,084H ,04H ,0fcH ,00H ,00H
db 00H ,0cH ,010H ,020H ,020H ,020H ,020H ,0c0H ,020H ,020H ,020H ,020H ,010H ,0cH ,00H ,00H
db 020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H
db 00H ,0c0H ,020H ,010H ,010H ,010H ,010H ,0cH ,010H ,010H ,010H ,010H ,020H ,0c0H ,00H ,00H
db 00H ,0e4H ,018H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,020H ,050H ,088H ,04H ,0fcH ,04H ,0fcH ,00H ,00H ,00H ,00H ,00H
db 00H ,070H ,088H ,04H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,04H ,088H ,070H ,020H ,040H
db 00H ,00H ,048H ,048H ,00H ,04H ,04H ,04H ,04H ,04H ,04H ,04H ,084H ,07cH ,00H ,00H
db 018H ,010H ,020H ,00H ,00H ,070H ,088H ,04H ,04H ,0fcH ,00H ,04H ,088H ,070H ,00H ,00H
db 00H ,020H ,050H ,088H ,00H ,0f0H ,08H ,08H ,078H ,088H ,08H ,08H ,088H ,07cH ,00H ,00H
db 00H ,00H ,048H ,048H ,00H ,0f0H ,08H ,08H ,078H ,088H ,08H ,08H ,088H ,07cH ,00H ,00H
db 020H ,010H ,08H ,00H ,00H ,0f0H ,08H ,08H ,078H ,088H ,08H ,08H ,088H ,07cH ,00H ,00H
db 00H ,030H ,048H ,030H ,00H ,0f0H ,08H ,08H ,078H ,088H ,08H ,08H ,088H ,07cH ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,078H ,084H ,00H ,00H ,00H ,00H ,00H ,084H ,078H ,010H ,020H
db 00H ,020H ,050H ,088H ,00H ,070H ,088H ,04H ,04H ,0fcH ,00H ,04H ,088H ,070H ,00H ,00H
db 00H ,00H ,048H ,048H ,00H ,070H ,088H ,04H ,04H ,0fcH ,00H ,04H ,088H ,070H ,00H ,00H
db 020H ,010H ,08H ,00H ,00H ,070H ,088H ,04H ,04H ,0fcH ,00H ,04H ,088H ,070H ,00H ,00H
db 00H ,00H ,048H ,048H ,00H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,00H ,00H
db 00H ,020H ,050H ,088H ,00H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,00H ,00H
db 020H ,010H ,08H ,00H ,00H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,00H ,00H
db 048H ,048H ,00H ,070H ,088H ,04H ,04H ,04H ,04H ,0fcH ,04H ,04H ,04H ,04H ,00H ,00H
db 00H ,070H ,088H ,070H ,088H ,04H ,04H ,04H ,04H ,0fcH ,04H ,04H ,04H ,04H ,00H ,00H
db 018H ,010H ,020H ,0fcH ,00H ,00H ,00H ,00H ,0f0H ,00H ,00H ,00H ,00H ,0fcH ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,0c0H ,038H ,024H ,0e4H ,03cH ,020H ,020H ,024H ,0d8H ,00H ,00H
db 018H ,020H ,040H ,050H ,050H ,050H ,0fcH ,050H ,050H ,050H ,050H ,050H ,050H ,050H ,00H ,00H
db 00H ,020H ,050H ,088H ,00H ,070H ,088H ,04H ,04H ,04H ,04H ,04H ,088H ,070H ,00H ,00H
db 00H ,00H ,048H ,048H ,00H ,070H ,088H ,04H ,04H ,04H ,04H ,04H ,088H ,070H ,00H ,00H
db 020H ,010H ,08H ,00H ,00H ,070H ,088H ,04H ,04H ,04H ,04H ,04H ,088H ,070H ,00H ,00H
db 00H ,020H ,050H ,088H ,00H ,04H ,04H ,04H ,04H ,04H ,04H ,04H ,084H ,07cH ,00H ,00H
db 020H ,010H ,08H ,00H ,00H ,04H ,04H ,04H ,04H ,04H ,04H ,04H ,084H ,07cH ,00H ,00H
db 00H ,00H ,048H ,048H ,00H ,04H ,04H ,088H ,088H ,050H ,050H ,020H ,020H ,040H ,040H ,080H
db 048H ,048H ,00H ,070H ,088H ,04H ,04H ,04H ,04H ,04H ,04H ,04H ,088H ,070H ,00H ,00H
db 048H ,048H ,00H ,04H ,04H ,04H ,04H ,04H ,04H ,04H ,04H ,04H ,088H ,070H ,00H ,00H
db 00H ,050H ,050H ,050H ,078H ,0d4H ,050H ,050H ,050H ,050H ,050H ,0d4H ,078H ,050H ,050H ,050H
db 00H ,018H ,024H ,040H ,040H ,040H ,0f8H ,040H ,040H ,040H ,0c0H ,040H ,064H ,098H ,00H ,00H
db 00H ,04H ,04H ,088H ,050H ,020H ,0fcH ,020H ,020H ,0fcH ,020H ,020H ,020H ,020H ,00H ,00H
db 00H ,0c0H ,020H ,010H ,010H ,010H ,028H ,0c8H ,03eH ,08H ,08H ,08H ,08H ,08H ,00H ,00H
db 00H ,018H ,024H ,020H ,020H ,020H ,0fcH ,020H ,020H ,020H ,020H ,020H ,020H ,0c0H ,00H ,00H
db 018H ,010H ,020H ,00H ,00H ,0f0H ,08H ,08H ,078H ,088H ,08H ,08H ,088H ,07cH ,00H ,00H
db 018H ,010H ,020H ,00H ,00H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,00H ,00H
db 018H ,010H ,020H ,00H ,00H ,070H ,088H ,04H ,04H ,04H ,04H ,04H ,088H ,070H ,00H ,00H
db 018H ,010H ,020H ,00H ,00H ,04H ,04H ,04H ,04H ,04H ,04H ,04H ,084H ,07cH ,00H ,00H
db 00H ,024H ,054H ,048H ,00H ,0f0H ,08H ,04H ,04H ,04H ,04H ,04H ,04H ,04H ,00H ,00H
db 024H ,054H ,048H ,00H ,04H ,084H ,084H ,044H ,024H ,024H ,014H ,0cH ,0cH ,04H ,00H ,00H
db 00H ,00H ,00H ,0f0H ,08H ,08H ,078H ,088H ,08H ,08H ,088H ,07cH ,00H ,0fcH ,00H ,00H
db 00H ,00H ,00H ,070H ,088H ,04H ,04H ,04H ,04H ,04H ,088H ,070H ,00H ,0fcH ,00H ,00H
db 00H ,020H ,020H ,00H ,00H ,020H ,020H ,040H ,088H ,04H ,04H ,04H ,088H ,070H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,0fcH ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,0fcH ,04H ,04H ,04H ,00H ,00H
db 00H ,020H ,060H ,020H ,020H ,020H ,00H ,0fcH ,00H ,0f0H ,08H ,070H ,080H ,0f8H ,00H ,00H
db 00H ,020H ,060H ,020H ,020H ,020H ,00H ,0fcH ,00H ,030H ,050H ,090H ,0f8H ,010H ,00H ,00H
db 00H ,020H ,020H ,00H ,00H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,00H ,00H
db 00H ,00H ,00H ,00H ,024H ,048H ,090H ,020H ,020H ,090H ,048H ,024H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,020H ,090H ,048H ,024H ,024H ,048H ,090H ,020H ,00H ,00H ,00H ,00H
db 022H ,088H ,022H ,088H ,022H ,088H ,022H ,088H ,022H ,088H ,022H ,088H ,022H ,088H ,022H ,088H
db 0aaH ,054H ,0aaH ,054H ,0aaH ,054H ,0aaH ,054H ,0aaH ,054H ,0aaH ,054H ,0aaH ,054H ,0aaH ,054H
db 0eeH ,0baH ,0eeH ,0baH ,0eeH ,0baH ,0eeH ,0baH ,0eeH ,0baH ,0eeH ,0baH ,0eeH ,0baH ,0eeH ,0baH
db 020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H
db 020H ,020H ,020H ,020H ,020H ,020H ,020H ,0e0H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H
db 020H ,020H ,020H ,020H ,020H ,020H ,020H ,0e0H ,020H ,0e0H ,020H ,020H ,020H ,020H ,020H ,020H
db 028H ,028H ,028H ,028H ,028H ,028H ,028H ,0e8H ,028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,0f8H ,028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,0e0H ,020H ,0e0H ,020H ,020H ,020H ,020H ,020H ,020H
db 028H ,028H ,028H ,028H ,028H ,028H ,028H ,0e8H ,08H ,0e8H ,028H ,028H ,028H ,028H ,028H ,028H
db 028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,0f8H ,08H ,0e8H ,028H ,028H ,028H ,028H ,028H ,028H
db 028H ,028H ,028H ,028H ,028H ,028H ,028H ,0e8H ,08H ,0f8H ,00H ,00H ,00H ,00H ,00H ,00H
db 028H ,028H ,028H ,028H ,028H ,028H ,028H ,0f8H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 020H ,020H ,020H ,020H ,020H ,020H ,020H ,0e0H ,020H ,0e0H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,0e0H ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H
db 020H ,020H ,020H ,020H ,020H ,020H ,020H ,03eH ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 020H ,020H ,020H ,020H ,020H ,020H ,020H ,0feH ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,0feH ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H
db 020H ,020H ,020H ,020H ,020H ,020H ,020H ,03eH ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,0feH ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 020H ,020H ,020H ,020H ,020H ,020H ,020H ,0feH ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H
db 020H ,020H ,020H ,020H ,020H ,020H ,020H ,03eH ,020H ,03eH ,020H ,020H ,020H ,020H ,020H ,020H
db 028H ,028H ,028H ,028H ,028H ,028H ,028H ,02eH ,028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H
db 028H ,028H ,028H ,028H ,028H ,028H ,028H ,02eH ,020H ,03eH ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,03eH ,020H ,02eH ,028H ,028H ,028H ,028H ,028H ,028H
db 028H ,028H ,028H ,028H ,028H ,028H ,028H ,0eeH ,00H ,0feH ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,0feH ,00H ,0eeH ,028H ,028H ,028H ,028H ,028H ,028H
db 028H ,028H ,028H ,028H ,028H ,028H ,028H ,02eH ,020H ,02eH ,028H ,028H ,028H ,028H ,028H ,028H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,0feH ,00H ,0feH ,00H ,00H ,00H ,00H ,00H ,00H
db 028H ,028H ,028H ,028H ,028H ,028H ,028H ,0eeH ,00H ,0eeH ,028H ,028H ,028H ,028H ,028H ,028H
db 020H ,020H ,020H ,020H ,020H ,020H ,020H ,0feH ,00H ,0feH ,00H ,00H ,00H ,00H ,00H ,00H
db 028H ,028H ,028H ,028H ,028H ,028H ,028H ,0feH ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,0feH ,00H ,0feH ,020H ,020H ,020H ,020H ,020H ,020H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,0feH ,028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H
db 028H ,028H ,028H ,028H ,028H ,028H ,028H ,03eH ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 020H ,020H ,020H ,020H ,020H ,020H ,020H ,03eH ,020H ,03eH ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,03eH ,020H ,03eH ,020H ,020H ,020H ,020H ,020H ,020H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,03eH ,028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H
db 028H ,028H ,028H ,028H ,028H ,028H ,028H ,0eeH ,028H ,028H ,028H ,028H ,028H ,028H ,028H ,028H
db 020H ,020H ,020H ,020H ,020H ,020H ,020H ,0feH ,020H ,0feH ,020H ,020H ,020H ,020H ,020H ,020H
db 020H ,020H ,020H ,020H ,020H ,020H ,020H ,0e0H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,03eH ,020H ,020H ,020H ,020H ,020H ,020H ,020H ,020H
db 0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH
db 0e0H ,0e0H ,0e0H ,0e0H ,0e0H ,0e0H ,0e0H ,0e0H ,0e0H ,0e0H ,0e0H ,0e0H ,0e0H ,0e0H ,0e0H ,0e0H
db 01eH ,01eH ,01eH ,01eH ,01eH ,01eH ,01eH ,01eH ,01eH ,01eH ,01eH ,01eH ,01eH ,01eH ,01eH ,01eH
db 0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,0feH ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
db 00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H ,00H
