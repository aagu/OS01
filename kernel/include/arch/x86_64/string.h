#ifndef __KERNEL_ARCH_STRING_H__
#define __KERNEL_ARCH_STRING_H__

#include "linkage.h"

// inline void * __attribute__((always_inline)) memset(void * dst, unsigned char c, long len)
void * memset(void * dst, unsigned char c, long len)
{
   int d0, d1;
   unsigned long tmp = c * 0x0101010101010101UL;
   __asm__ __volatile__ (
       "cld \n\t"
       "rep \n\t"
       "stosq \n\t"
       "testb $4, %b3 \n\t"
       "je 1f \n\t"
       "stosl \n\t"
       "1:\ttestb $2, %b3 \n\t"
       "je 2f \n\t"
       "stosw \n\t"
       "2:\ttestb $1, %b3 \n\t"
       "je 3f \n\t"
       "stosb \n\t"
       "3: \n\t"
       : "=&c"(d0), "=&D"(d1)
       : "a"(tmp), "q"(len), "0"(len/8),"1"(dst)
       : "memory"
   );

   return dst;
}

#endif
