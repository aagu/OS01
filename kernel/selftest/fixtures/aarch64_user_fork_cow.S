/* kernel/selftest/fixtures/aarch64_user_fork_cow.S
 *
 * Payloads of the M4 fork + COW runtime acceptance case.
 *
 * One source, four images selected by USER_FORK_COW_MODE:
 *
 *   MAIN     the full parent/child fork, COW, isolation, lazy page,
 *            stash, brk-tail, munmap-unique-upgrade and exec path
 *   CLEANUP  the inherited-catalog exec target: exits 0x33
 *   ILLEGAL  a read-only page write: the COW path must refuse it
 *   SHORT    a second short fork whose child exits immediately
 *
 * Fixed layout (aarch64_user_task.ld):
 *   0x400000  .text  RO/X
 *   0x401000  .data  RW/NX
 *   0x402000  heap base
 *
 * Runtime mmap window (the fixture maps 6 pages at 0x40000000):
 *   A +0x0000  parent writes after fork; the child must still read A1
 *   B +0x1000  stash round trip; the child's clock_gettime COWs it
 *   C +0x2000  untouched counted page, released at exec
 *   D +0x3000  parent munmaps; the child's write upgrades the same PA
 *   E +0x4000  left absent: the parent and child fill it independently
 *   F +0x5000  the parent's clock_gettime COWs it
 *
 * The kernel stage is queried with ``svc #0x4d43``; ``svc #0x4d40``
 * is a bounded yield.  Stage values are monotonic and are advanced by
 * real syscall, fault and reap observations in the kernel case.
 */

#include <uapi/syscall.h>

#ifndef USER_FORK_COW_MODE
#define USER_FORK_COW_MODE 0
#endif

#define FC_MODE_MAIN    0
#define FC_MODE_CLEANUP 1
#define FC_MODE_ILLEGAL 2
#define FC_MODE_SHORT   3

#define FC_MMAP_BASE  0x40000000
#define FC_HEAP_BASE  0x00402000

#define FC_PROT_NONE 0
#define FC_PROT_READ 1
#define FC_PROT_RW   3
#define FC_MAP_PRIVATE_ANON 0x22

#define FC_SVC_YIELD 0x4d40
#define FC_SVC_EXEC  0x4d41
#define FC_SVC_STAGE 0x4d43

.macro FC_LDR64 reg, val
    movz    \reg, #((\val) & 0xffff)
    movk    \reg, #(((\val) >> 16) & 0xffff), lsl #16
    movk    \reg, #(((\val) >> 32) & 0xffff), lsl #32
    movk    \reg, #(((\val) >> 48) & 0xffff), lsl #48
.endm

.macro FC_SYSCALL nr
    mov     x8, #\nr
    svc     #0
.endm

.macro FC_EXIT status
    mov     x0, #\status
    mov     x8, #SYS_exit
    svc     #0
.endm

/* Bounded wait: query the kernel stage, yield, retry.  x24 counts the
 * attempts; exceeding the bound exits with the failure status. */
.macro FC_WAIT target
    mov     x24, #0
1:
    svc     #FC_SVC_STAGE
    cmp     x0, #\target
    b.hs    2f
    svc     #FC_SVC_YIELD
    add     x24, x24, #1
    FC_LDR64 x25, 200000
    cmp     x24, x25
    b.lo    1b
    b       .Lfail
2:
.endm

.text
    .rept 4
    brk #0
    .endr
    .balign 16
.global _start
_start:

#if USER_FORK_COW_MODE == FC_MODE_CLEANUP
    FC_EXIT 0x33

#elif USER_FORK_COW_MODE == FC_MODE_ILLEGAL
    /* A private read-only page: the read demand-fills it, the write is
     * a permission fault and the COW policy must refuse it. */
    FC_LDR64 x0, FC_MMAP_BASE
    mov     x1, #0x1000
    mov     x2, #FC_PROT_READ
    mov     x3, #FC_MAP_PRIVATE_ANON
    mvn     x4, xzr
    mov     x5, #0
    FC_SYSCALL SYS_mmap
    FC_LDR64 x20, FC_MMAP_BASE
    cmp     x0, x20
    b.ne    .Lfail
    ldr     w1, [x20]
    cbnz    w1, .Lfail
    mov     w1, #0x1
    str     w1, [x20]            /* permission fault -> -EFAULT */
    b       .Lfail

#elif USER_FORK_COW_MODE == FC_MODE_SHORT
    FC_LDR64 x0, FC_MMAP_BASE
    mov     x1, #0x1000
    mov     x2, #FC_PROT_RW
    mov     x3, #FC_MAP_PRIVATE_ANON
    mvn     x4, xzr
    mov     x5, #0
    FC_SYSCALL SYS_mmap
    FC_LDR64 x20, FC_MMAP_BASE
    cmp     x0, x20
    b.ne    .Lfail
    mov     w1, #0x5a
    str     w1, [x20]

    FC_SYSCALL SYS_fork
    mov     x23, x0
    cbz     x23, .Lshort_child

    /* Parent: wait until the short child has been reaped, then prove
     * the shared page is still readable. */
    FC_WAIT 1
    ldr     w1, [x20]
    mov     w2, #0x5a
    cmp     w1, w2
    b.ne    .Lfail
    FC_EXIT 0x22

.Lshort_child:
    FC_EXIT 0x44

#else /* FC_MODE_MAIN */

    /* ── Six-page RW anonymous window ─────────────────────────── */
    FC_LDR64 x0, FC_MMAP_BASE
    mov     x1, #0x6000
    mov     x2, #FC_PROT_RW
    mov     x3, #FC_MAP_PRIVATE_ANON
    mvn     x4, xzr
    mov     x5, #0
    FC_SYSCALL SYS_mmap
    FC_LDR64 x20, FC_MMAP_BASE
    cmp     x0, x20
    b.ne    .Lfail

    mov     w1, #0xa1
    str     w1, [x20]                /* A */
    mov     w1, #0xc0
    str     w1, [x20, #0x1000]       /* B */
    mov     w1, #0xc1
    str     w1, [x20, #0x2000]       /* C */
    mov     w1, #0xd0
    str     w1, [x20, #0x3000]       /* D */
    mov     w1, #0xf0
    add     x3, x20, #0x5000
    str     w1, [x3]                 /* F */
    /* E (+0x4000) stays absent until both sides touch it. */

    /* ── Stash round trip on B ────────────────────────────────── */
    add     x0, x20, #0x1000
    mov     x1, #0x1000
    mov     x2, #FC_PROT_NONE
    FC_SYSCALL SYS_mprotect
    cbnz    x0, .Lfail
    add     x0, x20, #0x1000
    mov     x1, #0x1000
    mov     x2, #FC_PROT_RW
    FC_SYSCALL SYS_mprotect
    cbnz    x0, .Lfail
    ldr     w1, [x20, #0x1000]
    mov     w2, #0xc0
    cmp     w1, w2
    b.ne    .Lfail

    /* ── Heap page ────────────────────────────────────────────── */
    FC_LDR64 x22, FC_HEAP_BASE
    add     x0, x22, #0x1000
    FC_SYSCALL SYS_brk
    add     x1, x22, #0x1000
    cmp     x0, x1
    b.ne    .Lfail
    mov     w1, #0xa5a5
    str     w1, [x22]

    /* ── Fork: both sides resume at the next instruction ──────── */
    FC_SYSCALL SYS_fork
    mov     x23, x0                  /* child pid / 0 */
    FC_SYSCALL SYS_getpid            /* the child's first SVC */
    svc     #FC_SVC_STAGE
    cbz     x23, .Lchild

    /* ── Parent ───────────────────────────────────────────────── */
    /* Independent lazy page: the parent fills E first. */
    add     x3, x20, #0x4000
    ldr     w1, [x3]
    cbnz    w1, .Lfail
    mov     w2, #0xe1
    str     w2, [x3]

    /* Write A while the child still holds its reference: a real
     * multi-reference COW copy. */
    ldr     w1, [x20]
    mov     w2, #0xb1
    str     w2, [x20]

    FC_WAIT 2

    /* clock_gettime write-back into the shared F page. */
    mov     x0, #1
    add     x1, x20, #0x5000
    FC_SYSCALL SYS_clock_gettime
    cbnz    x0, .Lfail

    /* Shrink the heap: the retained shared tail must be privatized
     * before it is zeroed. */
    add     x0, x22, #0x800
    FC_SYSCALL SYS_brk
    add     x1, x22, #0x800
    cmp     x0, x1
    b.ne    .Lfail
    ldr     w1, [x22, #0x800]
    cbnz    w1, .Lfail
    ldr     w1, [x22]
    mov     w2, #0xa5a5
    cmp     w1, w2
    b.ne    .Lfail

    /* Release the parent's D reference: the child keeps the only one. */
    add     x0, x20, #0x3000
    mov     x1, #0x1000
    FC_SYSCALL SYS_munmap
    cbnz    x0, .Lfail

    FC_WAIT 5
    ldr     w1, [x20]                /* parent's own private copy */
    mov     w2, #0xb1
    cmp     w1, w2
    b.ne    .Lfail
    FC_EXIT 0x11

    /* ── Child ────────────────────────────────────────────────── */
.Lchild:
    cbnz    x23, .Lfail
    /* A must still hold the pre-fork value: the parent's write went
     * to its private copy. */
    ldr     w1, [x20]
    mov     w2, #0xa1
    cmp     w1, w2
    b.ne    .Lfail

    /* Independent lazy page: the child fills E here. */
    add     x3, x20, #0x4000
    ldr     w1, [x3]
    cbnz    w1, .Lfail
    mov     w2, #0xe2
    str     w2, [x3]

    /* clock_gettime write-back into the shared B page: the kernel's
     * copy-to-user preparation performs the COW. */
    mov     x0, #1
    add     x1, x20, #0x1000
    FC_SYSCALL SYS_clock_gettime
    cbnz    x0, .Lfail

    FC_WAIT 4

    /* The parent released its D reference: this write is a unique
     * reference, upgraded in place at the same PA. */
    ldr     w1, [x20, #0x3000]
    mov     w2, #0xd0
    cmp     w1, w2
    b.ne    .Lfail
    mov     w2, #0xd1
    str     w2, [x20, #0x3000]

    FC_WAIT 6

    /* The parent is gone: getppid must now name the BSP idle task. */
    FC_SYSCALL SYS_getppid

    /* The inherited catalog replaces this image with the cleanup one,
     * releasing the remaining shared pages (including C). */
    mov     x0, #0
    svc     #FC_SVC_EXEC
    b       .Lfail
#endif

.Lfail:
    FC_EXIT 0xf0

.data
.balign 8
fc_value:
    .quad 0
