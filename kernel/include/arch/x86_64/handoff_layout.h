#ifndef _KERNEL_ARCH_X86_64_HANDOFF_LAYOUT_H
#define _KERNEL_ARCH_X86_64_HANDOFF_LAYOUT_H

/* Boot handoff window. Mirrors boot/uefi/arch/x86_64/boot.c
 * X86_HANDOFF_BASE = 0x60000 and X86_HANDOFF_PAGES = 4, giving
 * [0x60000, 0x64000). The kernel must subtract this from any
 * E820 RAM range before producing MEMORY_RANGE[] output. */
#define X86_64_HANDOFF_BASE  0x60000UL
#define X86_64_HANDOFF_END    0x64000UL

/* Linker symbols. Single-char style matches kernel/core/main.c:40-43
 * and kernel/include/sched/task.h:48,51. */
extern char _text;
extern char _edata;

#endif /* _KERNEL_ARCH_X86_64_HANDOFF_LAYOUT_H */