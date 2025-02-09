#ifndef _KERNEL_HW_H
#define _KERNEL_HW_H

#include <stdint.h>

inline uint8_t __attribute__((always_inline)) inb(uint16_t port)
{
    uint8_t rv;
    __asm__ __volatile__("inb %%dx, %0\n\t"
                        "mfence \n\t"
                        :"=a"(rv)
                        :"d"(port)
                        :"memory"
                        );
    return rv;
}

inline void __attribute__((always_inline)) outb(uint16_t port, uint8_t data)
{
   __asm__ __volatile__("outb %0, %%dx \n\t"
                        "mfence \n\t"
                        :
                        :"a"(data),"d"(port)
                        :"memory"
                        ); 
}

inline uint16_t __attribute__((always_inline)) inw(uint16_t port)
{
    uint16_t rv;
    __asm__ __volatile__("inw %%dx, %0\n\t"
                        "mfence \n\t"
                        :"=a"(rv)
                        :"d"(port)
                        :"memory"
                        );
    return rv;
}

inline void __attribute__((always_inline)) outw(uint16_t port, uint16_t data)
{
   __asm__ __volatile__("outw %0, %%dx \n\t"
                        "mfence \n\t"
                        :
                        :"a"(data),"d"(port)
                        :"memory"
                        ); 
}

inline uint32_t __attribute__((always_inline)) ind(uint16_t port)
{
    uint32_t rv;
    __asm__ __volatile__("inl %%dx, %0\n\t"
                        "mfence \n\t"
                        :"=a"(rv)
                        :"d"(port)
                        :"memory"
                        );
    return rv;
}
inline void __attribute__((always_inline)) outd(uint16_t port, uint32_t data)
{
   __asm__ __volatile__("outl %0, %%dx \n\t"
                        "mfence \n\t"
                        :
                        :"a"(data),"d"(port)
                        :"memory"
                        ); 
}

#endif