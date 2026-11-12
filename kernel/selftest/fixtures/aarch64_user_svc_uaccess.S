/* kernel/selftest/fixtures/aarch64_user_svc_uaccess.S
 *
 * Payload of the formal SVC + uaccess acceptance case.
 *
 * Layout (the loader maps exactly these pages):
 *
 *   0x400000  .text          RO / X      one page
 *   0x401000  .data          RW / NX      page 1 of the RW segment
 *   0x402000  .bss           RW / NX      page 2 of the RW segment
 *   0x403000  (unmapped)                 the page right after the segment
 *   0x20400000 user stack    RW / NX      supplied by the loader
 *
 * The payload drives the native AArch64 syscall ABI:
 *
 *   - getpid / getppid are saved in x19 / x20; the kernel observation
 *     point reads them back from the saved exception frame at the first
 *     clock_gettime;
 *   - a 16-byte timespec is written across the 0x401ff8 boundary, which
 *     straddles the two mapped RW pages;
 *   - clock_gettime into the read-only code page and across the
 *     unmapped page at the end of the segment must both fail with
 *     -EFAULT, and the latter must leave the prefix sentinel intact;
 *   - an unknown SVC immediate, a defined-but-unserved number and an
 *     x8 with a non-zero high 32 bits all report -ENOSYS;
 *   - the task leaves through the native exit, so the kernel can check
 *     the encoded exit status at reap time.
 */

#include <uapi/syscall.h>

#define UC_ERR_EFAULT 14
#define UC_ERR_ENOSYS 38

#define UC_CODE_PAGE     0x00400000
#define UC_BOUNDARY      0x00401ff8     /* 16 bytes straddle 0x402000 */
#define UC_BEFORE        0x00401ff0
#define UC_AFTER         0x00402008
#define UC_SEG_END       0x00402ff8     /* 16 bytes cross into 0x403000 */
#define UC_UNMAPPED      0x00403000

#define CLOCK_MONOTONIC 1

/* The known pattern the kernel copy-in self-test compares against. */
#define UC_PATTERN0 0x0123456789abcdef

.text
    .rept 4
    brk #0                       /* the entry is not the segment base */
    .endr
    .balign 16
.global _start
_start:
    /* ── Native identity: x19 = PID, x20 = PPID ───────────────── */
    mov     x8, #SYS_getpid
    svc     #0
    mov     x19, x0

    mov     x8, #SYS_getppid
    svc     #0
    mov     x20, x0

    /* ── Prefill the 16 bytes across the page boundary with a known
     *    pattern; the kernel copy-in self-test reads it back at the
     *    first clock_gettime below. ─────────────────────────────── */
    movz    x1, #0x1ff8
    movk    x1, #0x40, lsl #16          /* x1 = UC_BOUNDARY */
    movz    x2, #0xcdef
    movk    x2, #0x89ab, lsl #16
    movk    x2, #0x4567, lsl #32
    movk    x2, #0x0123, lsl #48        /* x2 = UC_PATTERN0 */
    str     x2, [x1]
    mvn     x2, x2                      /* the complement half */
    str     x2, [x1, #8]

    /* ── Unknown SVC immediate: -ENOSYS ───────────────────────── */
    svc     #0x1234
    cmn     x0, #UC_ERR_ENOSYS
    b.ne    .Lfail

    /* ── A defined but unserved number: SYS_read is hidden by the
     *    AArch64 subset table, so it is -ENOSYS. ───────────────── */
    mov     x8, #SYS_read
    svc     #0
    cmn     x0, #UC_ERR_ENOSYS
    b.ne    .Lfail

    /* ── Full x8: a polluted high half is never a served number ── */
    mov     x8, #SYS_getpid
    movk    x8, #1, lsl #32
    svc     #0
    cmn     x0, #UC_ERR_ENOSYS
    b.ne    .Lfail

    /* ── clock_gettime across the two mapped RW pages ──────────── */
    /* Sentinels immediately below and above the 16-byte target. */
    movz    x3, #0x1ff0
    movk    x3, #0x40, lsl #16          /* x3 = UC_BEFORE */
    movz    x4, #0x7788
    movk    x4, #0x5566, lsl #16
    movk    x4, #0x3344, lsl #32
    movk    x4, #0x1122, lsl #48
    str     x4, [x3]
    movz    x3, #0x2008
    movk    x3, #0x40, lsl #16          /* x3 = UC_AFTER */
    mvn     x4, x4
    str     x4, [x3]

    mov     x0, #CLOCK_MONOTONIC
    movz    x1, #0x1ff8
    movk    x1, #0x40, lsl #16          /* x1 = UC_BOUNDARY */
    mov     x8, #SYS_clock_gettime
    svc     #0
    cbnz    x0, .Lfail

    /* tv_sec must be non-negative and tv_nsec < 1e9. */
    ldr     x2, [x1]
    tbnz    x2, #63, .Lfail
    ldr     x2, [x1, #8]
    movz    x3, #0xca00
    movk    x3, #0x3b9a, lsl #16        /* x3 = 1000000000 */
    cmp     x2, x3
    b.hs    .Lfail

    /* The sentinels around the target must be untouched. */
    ldr     x2, [x1, #-8]
    movz    x3, #0x7788
    movk    x3, #0x5566, lsl #16
    movk    x3, #0x3344, lsl #32
    movk    x3, #0x1122, lsl #48
    cmp     x2, x3
    b.ne    .Lfail
    ldr     x2, [x1, #16]
    mvn     x3, x3
    cmp     x2, x3
    b.ne    .Lfail

    /* ── clock_gettime into the read-only code page: -EFAULT ──── */
    mov     x0, #CLOCK_MONOTONIC
    movz    x1, #0x10
    movk    x1, #0x40, lsl #16          /* x1 = UC_CODE_PAGE + 0x10 */
    mov     x8, #SYS_clock_gettime
    svc     #0
    cmn     x0, #UC_ERR_EFAULT
    b.ne    .Lfail

    /* ── clock_gettime across the unmapped page: -EFAULT and the
     *    prefix sentinel stays intact (the precheck writes nothing) */
    movz    x3, #0x2ff8
    movk    x3, #0x40, lsl #16          /* x3 = UC_SEG_END */
    movz    x4, #0xbeef
    movk    x4, #0xdead, lsl #16
    str     x4, [x3]

    mov     x0, #CLOCK_MONOTONIC
    mov     x1, x3
    mov     x8, #SYS_clock_gettime
    svc     #0
    cmn     x0, #UC_ERR_EFAULT
    b.ne    .Lfail

    ldr     x2, [x3]
    cmp     x2, x4
    b.ne    .Lfail

    /* ── Native exit: the kernel encodes (0x2a & 0xff) << 8 ───── */
    mov     x0, #0x2a
    mov     x8, #SYS_exit
    svc     #0

.Lfail:
    mov     x0, #1
    mov     x8, #SYS_exit
    svc     #0
    b       .

.data
.balign 8
uc_marker:
    .quad 0

.bss
