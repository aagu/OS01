/* kernel/selftest/fixtures/aarch64_user_vm.S
 *
 * Payload of the M4 anonymous-VM acceptance case.
 *
 * One source, six images.  The compile-time USER_VM_MODE selects what
 * the payload does; every image shares the fixed VA layout:
 *
 *   0x400000  .text      RO / X
 *   0x401000  .data      RW / NX   (8 bytes)
 *   0x402000  heap base  (brk window, unmapped until first touch)
 *
 * The payload drives the formal native AArch64 syscall ABI
 * (number in x8, arguments in x0..x5, result in x0) and the internal
 * exec SVC #0x4d41 used by the existing user-task case.
 *
 * Modes:
 *   NORMAL    full lazy/mprotect/brk/clock/stash path, exits 0x11
 *   RW_ONLY   write to a present read-only leaf -> permission fault
 *   NONE_READ read a PROT_NONE stash -> translation fault
 *   UNMAPPED  read after munmap -> translation fault
 *   REPLACE   present + stash pages around an injected exec failure
 *   LEAVE     replacement image: exits 0x33
 */

#include <uapi/syscall.h>

#ifndef USER_VM_MODE
#define USER_VM_MODE 0
#endif

#define VM_MODE_NORMAL     0
#define VM_MODE_RW_ONLY    1
#define VM_MODE_NONE_READ  2
#define VM_MODE_UNMAPPED   3
#define VM_MODE_REPLACE    4
#define VM_MODE_LEAVE      5

#define VM_ERR_EFAULT 14
#define VM_ERR_ENOMEM 12

#define VM_FAIL_STATUS 0xf0

#define PROT_NONE_ 0
#define PROT_READ_ 1
#define PROT_RW_   3

#define MAP_PRIVATE_ANON 0x22

/* Fixed layout constants (linker script aarch64_user_task.ld). */
#define VM_MMAP_BASE     0x0040000000
#define VM_CLOCK_REGION  0x0040010000
#define VM_CLOCK_TARGET  0x0040010ff8   /* last 8 bytes of the page */
#define VM_HEAP_BASE     0x00402000
#define VM_CLOCK_SENTINEL 0xdeadbeefdeadbeef

#define SVC_EXEC 0x4d41

/* Load a full 64-bit constant into a general register. */
.macro VM_LDR64 reg, val
    movz    \reg, #((\val) & 0xffff)
    movk    \reg, #(((\val) >> 16) & 0xffff), lsl #16
    movk    \reg, #(((\val) >> 32) & 0xffff), lsl #32
    movk    \reg, #(((\val) >> 48) & 0xffff), lsl #48
.endm

.macro VM_SYSCALL nr
    mov     x8, #\nr
    svc     #0
.endm

.macro VM_EXIT status
    mov     x0, #\status
    mov     x8, #SYS_exit
    svc     #0
.endm

.text
    .rept 4
    brk #0                       /* the entry is not the segment base */
    .endr
    .balign 16
.global _start
_start:

#if USER_VM_MODE == VM_MODE_LEAVE
    VM_EXIT 0x33

#elif USER_VM_MODE == VM_MODE_RW_ONLY
    /* Read-only mapping: the read fills the leaf, the write is a
     * permission fault (not a demand fault) and must terminate. */
    VM_LDR64 x0, VM_MMAP_BASE
    mov     x1, #0x1000
    mov     x2, #PROT_READ_
    mov     x3, #MAP_PRIVATE_ANON
    mvn     x4, xzr
    mov     x5, #0
    VM_SYSCALL SYS_mmap
    VM_LDR64 x20, VM_MMAP_BASE
    cmp     x0, x20
    b.ne    .Lfail
    ldr     w1, [x20]
    cbnz    w1, .Lfail
    mov     w1, #0x1
    str     w1, [x20]            /* permission fault */
    b       .Lfail

#elif USER_VM_MODE == VM_MODE_NONE_READ
    /* PROT_NONE keeps the resident page stashed; reading it is a
     * translation fault that the demand path must refuse. */
    VM_LDR64 x0, VM_MMAP_BASE
    mov     x1, #0x1000
    mov     x2, #PROT_RW_
    mov     x3, #MAP_PRIVATE_ANON
    mvn     x4, xzr
    mov     x5, #0
    VM_SYSCALL SYS_mmap
    VM_LDR64 x20, VM_MMAP_BASE
    cmp     x0, x20
    b.ne    .Lfail
    ldr     w1, [x20]
    cbnz    w1, .Lfail
    mov     x0, x20
    mov     x1, #0x1000
    mov     x2, #PROT_NONE_
    VM_SYSCALL SYS_mprotect
    cbnz    x0, .Lfail
    ldr     w1, [x20]            /* fault */
    b       .Lfail

#elif USER_VM_MODE == VM_MODE_UNMAPPED
    /* munmap removes the VMA and its page; the following read has no
     * authorized range and must terminate. */
    VM_LDR64 x0, VM_MMAP_BASE
    mov     x1, #0x1000
    mov     x2, #PROT_RW_
    mov     x3, #MAP_PRIVATE_ANON
    mvn     x4, xzr
    mov     x5, #0
    VM_SYSCALL SYS_mmap
    VM_LDR64 x20, VM_MMAP_BASE
    cmp     x0, x20
    b.ne    .Lfail
    ldr     w1, [x20]
    cbnz    w1, .Lfail
    mov     x0, x20
    mov     x1, #0x1000
    VM_SYSCALL SYS_munmap
    cbnz    x0, .Lfail
    ldr     w1, [x20]            /* fault */
    b       .Lfail

#elif USER_VM_MODE == VM_MODE_REPLACE
    /* Establish one present and one stashed page, then let the kernel
     * inject an -ENOMEM into the first replacement.  The old image
     * (root, VMA and data) must survive; the stash is restored,
     * checked, re-stashed, and a second replacement succeeds. */
    VM_LDR64 x0, VM_MMAP_BASE
    mov     x1, #0x2000
    mov     x2, #PROT_RW_
    mov     x3, #MAP_PRIVATE_ANON
    mvn     x4, xzr
    mov     x5, #0
    VM_SYSCALL SYS_mmap
    VM_LDR64 x20, VM_MMAP_BASE
    cmp     x0, x20
    b.ne    .Lfail

    ldr     w1, [x20]
    cbnz    w1, .Lfail
    mov     w1, #0x1234
    str     w1, [x20]

    ldr     w1, [x20, #0x1000]
    cbnz    w1, .Lfail
    mov     w1, #0x5678
    str     w1, [x20, #0x1000]

    add     x0, x20, #0x1000
    mov     x1, #0x1000
    mov     x2, #PROT_NONE_
    VM_SYSCALL SYS_mprotect
    cbnz    x0, .Lfail

    /* First replacement: the observer arms the one-shot image failure,
     * so this returns -ENOMEM and the old image keeps running. */
    mov     x0, #0
    svc     #SVC_EXEC
    cmn     x0, #VM_ERR_ENOMEM
    b.ne    .Lfail

    ldr     w1, [x20]
    movz    w2, #0x1234
    cmp     w1, w2
    b.ne    .Lfail

    add     x0, x20, #0x1000
    mov     x1, #0x1000
    mov     x2, #PROT_RW_
    VM_SYSCALL SYS_mprotect
    cbnz    x0, .Lfail

    ldr     w1, [x20, #0x1000]
    movz    w2, #0x5678
    cmp     w1, w2
    b.ne    .Lfail

    /* Re-stash and replace for real; the new image owns the exit. */
    add     x0, x20, #0x1000
    mov     x1, #0x1000
    mov     x2, #PROT_NONE_
    VM_SYSCALL SYS_mprotect
    cbnz    x0, .Lfail

    mov     x0, #0
    svc     #SVC_EXEC
    b       .Lfail

#else /* VM_MODE_NORMAL */
    /* ── Lazy zero page + sentinel ────────────────────────────── */
    VM_LDR64 x0, VM_MMAP_BASE
    mov     x1, #0x3000
    mov     x2, #PROT_RW_
    mov     x3, #MAP_PRIVATE_ANON
    mvn     x4, xzr
    mov     x5, #0
    VM_SYSCALL SYS_mmap
    VM_LDR64 x20, VM_MMAP_BASE
    cmp     x0, x20
    b.ne    .Lfail

    ldr     w1, [x20]            /* demand-fill zero */
    cbnz    w1, .Lfail
    mov     w1, #0x1111
    str     w1, [x20]

    ldr     w1, [x20, #0x1000]   /* second lazy page */
    cbnz    w1, .Lfail

    /* ── PROT_NONE stash round trip ───────────────────────────── */
    mov     x0, x20
    mov     x1, #0x2000
    mov     x2, #PROT_NONE_
    VM_SYSCALL SYS_mprotect
    cbnz    x0, .Lfail
    mov     x0, x20
    mov     x1, #0x2000
    mov     x2, #PROT_RW_
    VM_SYSCALL SYS_mprotect
    cbnz    x0, .Lfail
    ldr     w1, [x20]
    movz    w2, #0x1111
    cmp     w1, w2
    b.ne    .Lfail

    /* ── Split boundaries from an inner mprotect ──────────────── */
    add     x0, x20, #0x1000
    mov     x1, #0x1000
    mov     x2, #PROT_READ_
    VM_SYSCALL SYS_mprotect
    cbnz    x0, .Lfail
    add     x0, x20, #0x1000
    mov     x1, #0x1000
    mov     x2, #PROT_RW_
    VM_SYSCALL SYS_mprotect
    cbnz    x0, .Lfail

    /* ── Stashed page released by munmap ──────────────────────── */
    mov     x0, x20
    mov     x1, #0x1000
    mov     x2, #PROT_NONE_
    VM_SYSCALL SYS_mprotect
    cbnz    x0, .Lfail
    mov     x0, x20
    mov     x1, #0x1000
    VM_SYSCALL SYS_munmap
    cbnz    x0, .Lfail

    /* ── clock_gettime across a hole ──────────────────────────── */
    VM_LDR64 x0, VM_CLOCK_REGION
    mov     x1, #0x1000
    mov     x2, #PROT_RW_
    mov     x3, #MAP_PRIVATE_ANON
    mvn     x4, xzr
    mov     x5, #0
    VM_SYSCALL SYS_mmap
    VM_LDR64 x21, VM_CLOCK_REGION
    cmp     x0, x21
    b.ne    .Lfail

    /* First: the target page stays absent — the whole range is
     * validated before a single leaf is created. */
    mov     x19, x21
    VM_LDR64 x1, VM_CLOCK_TARGET
    mov     x0, #1               /* CLOCK_MONOTONIC */
    VM_SYSCALL SYS_clock_gettime
    cmn     x0, #VM_ERR_EFAULT
    b.ne    .Lfail

    /* Second: the same target on a resident page with a sentinel in
     * its last 8 bytes.  The -EFAULT must not copy a prefix. */
    ldr     w1, [x21]
    VM_LDR64 x2, VM_CLOCK_SENTINEL
    str     x2, [x21, #0xff8]
    mov     x19, x21
    VM_LDR64 x1, VM_CLOCK_TARGET
    mov     x0, #1
    VM_SYSCALL SYS_clock_gettime
    cmn     x0, #VM_ERR_EFAULT
    b.ne    .Lfail
    ldr     x2, [x21, #0xff8]
    VM_LDR64 x3, VM_CLOCK_SENTINEL
    cmp     x2, x3
    b.ne    .Lfail

    /* ── brk: grow lazily, shrink zeroes the retained tail ────── */
    VM_LDR64 x22, VM_HEAP_BASE
    add     x0, x22, #0x1000
    VM_SYSCALL SYS_brk
    add     x1, x22, #0x1000
    cmp     x0, x1
    b.ne    .Lfail

    ldr     w1, [x22]
    cbnz    w1, .Lfail
    mov     w2, #0xa5a5
    str     w2, [x22]

    add     x0, x22, #0x100
    VM_SYSCALL SYS_brk
    add     x1, x22, #0x100
    cmp     x0, x1
    b.ne    .Lfail

    add     x0, x22, #0x1000
    VM_SYSCALL SYS_brk
    add     x1, x22, #0x1000
    cmp     x0, x1
    b.ne    .Lfail

    ldr     x1, [x22, #0x100]    /* tail zeroed */
    cbnz    x1, .Lfail
    ldr     w1, [x22]            /* prefix intact */
    movz    w2, #0xa5a5
    cmp     w1, w2
    b.ne    .Lfail

    /* ── Leave a stashed page for the exit cleanup ────────────── */
    add     x0, x20, #0x1000
    mov     x1, #0x1000
    mov     x2, #PROT_NONE_
    VM_SYSCALL SYS_mprotect
    cbnz    x0, .Lfail

    VM_EXIT 0x11
#endif

.Lfail:
    VM_EXIT VM_FAIL_STATUS

.data
.balign 8
user_value:
    .quad 0
