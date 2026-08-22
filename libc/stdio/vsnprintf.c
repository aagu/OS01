#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "stdio_internal.h"

int vsnprintf(char *b, unsigned long s, const char *fmt, va_list ap)
{
	size_t r = vformatter(b, s, fmt, ap, 1);
	if (r == SIZE_MAX || r > INT_MAX)
		return -1;
	return (int)r;
}
