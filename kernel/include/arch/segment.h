#ifndef _ARCH_SEGMENT_H
#define _ARCH_SEGMENT_H

#ifdef __x86_64__
#define ARCH_KERNEL_CS  0x08
#define ARCH_KERNEL_DS  0x10
#define ARCH_USER_CS    0x2B
#define ARCH_USER_DS    0x33
#elif defined(__aarch64__)

// aarch64 has no segmentation — all selectors are 0
#define ARCH_KERNEL_CS 0
#define ARCH_KERNEL_DS 0
#define ARCH_USER_CS   0
#define ARCH_USER_DS   0
#else
#error "Unknown architecture"
#endif

// Legacy aliases for existing code that uses bare names
#ifndef KERNEL_CS
#define KERNEL_CS  ARCH_KERNEL_CS
#endif
#ifndef KERNEL_DS
#define KERNEL_DS  ARCH_KERNEL_DS
#endif
#ifndef USER_CS
#define USER_CS    ARCH_USER_CS
#endif
#ifndef USER_DS
#define USER_DS    ARCH_USER_DS
#endif

#endif
