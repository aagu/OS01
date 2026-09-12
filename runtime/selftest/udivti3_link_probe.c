extern unsigned __int128 __udivti3(unsigned __int128, unsigned __int128);

void _start(void)
{
    volatile unsigned __int128 result = __udivti3(7, 3);
    (void)result;
    for (;;) { }
}
