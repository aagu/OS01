#include <unistd.h>
#include <stdio.h>

int getchar(void)
{
    unsigned char c;
    int64_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0)
        return EOF;
    return (int)c;
}

int getchar_unlocked(void)
{
    return getchar();
}
