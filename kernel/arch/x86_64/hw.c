#include <hw.h>

inline uint8_t inb(uint16_t port)
{
    uint8_t rv;
    __asm__ __volatile__("in %1, %0":"=a"(rv):"dN"(port));
    return rv;
}

inline void outb(uint16_t port, uint8_t data)
{
    __asm__ __volatile__("out %1, %0"::"dN"(port),"a"(data));
}

inline uint16_t inw(uint16_t port)
{
    uint16_t rv;
    __asm__ __volatile__("in %1, %0":"=a"(rv):"dN"(port));
    return rv;
}

inline void outw(uint16_t port, uint16_t data)
{
    __asm__ __volatile__("out %1, %0"::"dN"(port),"a"(data));
}

inline uint32_t ind(uint16_t port)
{
    uint32_t rv;
    __asm__ __volatile__("in %1, %0":"=a"(rv):"dN"(port));
    return rv;
}

inline void outd(uint16_t port, uint32_t data)
{
    __asm__ __volatile__("out %1, %0"::"dN"(port),"a"(data));
}