#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char buf[4096]={0};

int printf(const char* restrict format, ...) {
	int len = 0;
	int count = 0;
	va_list args;

	va_start(args, format);
	len = vsprintf(buf,format, args);
	va_end(args);

	for(count = 0;count < len;count++)
	{
		if (!putchar((unsigned char)*(buf + count)))
            return -1;
	}

	return count;
}