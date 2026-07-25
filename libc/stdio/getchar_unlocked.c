#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#define STDIN_BUF_SIZE 4096

int getchar(void)
{
    static unsigned char *buf;
    static int pos;
    static int len;
    static int init;

    if (!init) {
        buf = (unsigned char *)malloc(STDIN_BUF_SIZE);
        if (!buf)
            return EOF;
        init = 1;
    }

    if (pos >= len) {
        int64_t n = read(STDIN_FILENO, buf, STDIN_BUF_SIZE);
        if (n <= 0)
            return EOF;
        len = (int)n;
        pos = 0;
    }
    return (int)buf[pos++];
}

int getchar_unlocked(void)
{
    return getchar();
}
