#include <unistd.h>
#include <stdio.h>

#define STDIN_BUF_SIZE 4096

int getchar(void)
{
    static unsigned char buf[STDIN_BUF_SIZE];
    static int pos;
    static int len;

    if (pos >= len) {
        int64_t n = read(STDIN_FILENO, buf, sizeof(buf));
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
