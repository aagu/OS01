#include <unistd.h>
#include <stdio.h>
#include <stdio/file.h>

/* getchar reads one byte from the standard input STREAM.
 *
 * The old implementation kept a private 4096-byte read-ahead buffer.
 * That buffer made the stream's state a lie: the bytes it had already
 * pulled from the kernel were invisible to the stream, so an EOF
 * indicator could not be reported at the right moment and a byte pushed
 * back with ungetc() would be read AFTER the buffered bytes instead of
 * immediately.  The stream state now owns the read, so the standard
 * input behaves exactly like any other stream.
 *
 * This round keeps the libc unbuffered (setvbuf is out of scope), so
 * every getchar() is one read() of one byte. */
int getchar(void)
{
    return __stdio_read_byte(stdin);
}

int getchar_unlocked(void)
{
    return getchar();
}
