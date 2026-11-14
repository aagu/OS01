#ifndef _ARCH_X86_64_USER_LAYOUT_H
#define _ARCH_X86_64_USER_LAYOUT_H

/*
 * x86_64 runtime user-VA layout constants.
 *
 * Single source of truth for the user address window every x86 VM
 * consumer shares (exec, trap, uaccess, futex, devfs, sys_proc, brk,
 * mmap).  The common ELF validator (kernel/fs/elf_layout.c) keeps its
 * own load window on purpose: it validates an image before any mm
 * exists, with independently pinned boundary numbers in its tests.
 *
 *   [X86_USER_MIN_ADDR, X86_USER_HEAP_LIMIT)   user envelope (ELF +
 *                                              heap, heap capped one
 *                                              page below the stack)
 *   [X86_USER_STACK_BASE, X86_USER_STACK_END)  the 2 MiB user stack
 */

#define X86_USER_MIN_ADDR     0x400000UL
#define X86_USER_STACK_BASE   0x20400000UL
#define X86_USER_STACK_SIZE   0x200000UL
#define X86_USER_STACK_END    (X86_USER_STACK_BASE + X86_USER_STACK_SIZE)
#define X86_USER_STACK_TOP    (X86_USER_STACK_END - 16)
#define X86_USER_HEAP_LIMIT   (X86_USER_STACK_BASE - 0x1000UL)

#endif /* _ARCH_X86_64_USER_LAYOUT_H */
