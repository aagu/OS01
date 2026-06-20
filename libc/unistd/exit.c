void _exit(int status)
{
    __asm__ volatile (
        "int $0x80"
        :
        : "a" ((unsigned long)2),
          "D" ((unsigned long)status)
        : "memory");
    __builtin_unreachable();
}
