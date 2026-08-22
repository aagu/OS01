#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <limits.h>
#include "stdio_internal.h"

int sprintf(char *buf, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	size_t r = vformatter(buf, 4096, fmt, args, 1);
	va_end(args);
	if (r == SIZE_MAX || r > INT_MAX)
		return -1;
	return (int)r;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	size_t r = vformatter(buf, size, fmt, args, 1);
	va_end(args);

	/* vformatter already NUL-terminates when cap > 0; re-assert the
	 * truncation termination explicitly for safety. */
	if (size > 0 && r >= size)
		buf[size - 1] = '\0';

	if (r == SIZE_MAX || r > INT_MAX)
		return -1;
	return (int)r;
}
