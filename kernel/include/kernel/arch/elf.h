#ifndef _ARCH_ELF_H
#define _ARCH_ELF_H

#ifdef __x86_64__
#define ARCH_ELF_MACHINE  0x3E    // EM_X86_64
#elif defined(__aarch64__)
#define ARCH_ELF_MACHINE  0xB7    // EM_AARCH64
#else
#error "Unsupported architecture"
#endif

#endif
