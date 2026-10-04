#ifndef _KERNEL_ARCH_X86_64_TSS_H
#define _KERNEL_ARCH_X86_64_TSS_H

#include <stdint.h>
#include <arch/cpu.h>

struct tss_struct
{
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint32_t reserved2;
    uint16_t reserved3;
    uint16_t iomapbaseaddr;
} __attribute__((packed));

#define INIT_TSS \
{ \
    .reserved0 = 0, \
    .rsp0 = 0xffff800000007c00, \
    .rsp1 = 0xffff800000007c00, \
    .rsp2 = 0xffff800000007c00, \
    .reserved1 = 0, \
    .ist1 = 0xffff800000007c00, /* exception stack (4KB from 0x6c00) */ \
    .ist2 = 0xffff800000006c00, /* IRQ stack (4KB from 0x5c00) */ \
    .ist3 = 0xffff800000005c00, /* double fault stack (4KB from 0x4c00) */ \
    .ist4 = 0, \
    .ist5 = 0, \
    .ist6 = 0, \
    .ist7 = 0, \
    .reserved2 = 0, \
    .reserved3 = 0, \
    .iomapbaseaddr = 0 \
}

extern struct tss_struct init_tss[NR_CPUS];

#endif /* _KERNEL_ARCH_X86_64_TSS_H */
