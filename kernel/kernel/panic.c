#include <kernel/panic.h>
#include <kernel/printk.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

char buf[4096] = {'[','k','e','r','n','e','l',' ','p','a','n','i','c',']',' ',0};

void kpanic(const char * msg,...)
{
    va_list args;
    va_start(args,msg);
    vsprintf(buf+15, msg, args);
    va_end(args);
    serial_printk(buf);
}
