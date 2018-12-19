ORG  0x08200
[bits 32]
;GLOBAL  _start
;GLOBAL	myprintf
;EXTERN  kernel_start

;BOTPAK	EQU		0x00280000
;DSKCAC	EQU		0x00100000
;DSKCAC0	EQU		0x00008000

;BOOT_INFO信息
CYLS		EQU	0x0ff0
LEDS		EQU	0x0ff1
LCDMODE		EQU	0x0ff2  ;
SCREENX		EQU	0x0ff4  ;	x
SCREENY		EQU	0x0ff6  ;	y
LCDRAM		EQU	0x0ff8  ; 图像缓冲区的开始地址

_start:
        mov byte [ds:0B8000h], 'P'   
        mov byte [ds:0B8001h], 1ch
        mov byte [ds:0B8002h], 'r'
        mov byte [ds:0B8003h], 1ch
        mov byte [ds:0B8004h], 'o'
        mov byte [ds:0B8005h], 1ch
        mov byte [ds:0B8006h], 't'
        mov byte [ds:0B8007h], 1ch
        mov byte [ds:0B8008h], 'e'      
        mov byte [ds:0B8009h], 1ch     
        mov byte [ds:0B800ah], 'c'     
        mov byte [ds:0B800bh], 1ch    
        mov byte [ds:0B800ch], 't'     
        mov byte [ds:0B800dh], 1ch     
        mov byte [ds:0B800eh], ' '     
        mov byte [ds:0B800fh], 1ch      
        mov byte [ds:0B8010h], 'm'     
        mov byte [ds:0B8011h], 1ch     
        mov byte [ds:0B8012h], 'o'     
        mov byte [ds:0B8013h], 1ch     
        mov byte [ds:0B8014h], 'd'     
        mov byte [ds:0B8015h], 1ch   
        mov byte [ds:0B8016h], 'e'     
        mov byte [ds:0B8017h], 1ch 
        jmp  stop

stop:
        hlt
        jmp     stop




