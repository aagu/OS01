#include <stdio.h>
#include <string.h>
#include <stdio/file.h>

/* puts writes the string and a newline to the standard output STREAM.
 * The write goes through the stream state, so a failed write is reported
 * as EOF (and sets stdout's error indicator) instead of being reported
 * as a successful write of N characters. */
int puts(const char * string)
{
    if (!string) return EOF;
    size_t len = strlen(string);

    long w = __stdio_write(stdout, string, len);
    if (w < 0) return EOF;
    if (__stdio_write_byte(stdout, '\n') == EOF) return EOF;
    return (int)(len + 1);
}
