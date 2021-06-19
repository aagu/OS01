#include <kernel/panic.h>
#include <kernel/printk.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

char buf[4096] = {'[','k','e','r','n','e','l',' ','p','a','n','i','c',']',' ',0};

void kpanic(const char * msg,...)
{
    size_t offset;
    for (offset = 0; offset < Pos.XResolution * Pos.YResolution; offset++)
    {
        *(Pos.FB_addr + offset) = 0x006699ff;
    }
    Pos.XPosition = 12;
    Pos.YPosition = 20;

    va_list args;
    va_start(args,msg);
    vsprintf(buf+15, msg, args);
    va_end(args);
    color_printk(WHITE, 0x006699ff, buf);
}
