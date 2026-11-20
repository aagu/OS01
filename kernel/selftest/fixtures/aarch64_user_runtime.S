/* kernel/selftest/fixtures/aarch64_user_runtime.S
 *
 * Formal EL0 acceptance fixture for AArch64 user runtime (case=irq).
 *
 * Single ELF image spawned as Task A, which forks Task B:
 *   - Task A (x28=1, x27=pid_b): sets GPR/NZCV/SP/FP/TLS/data sentinels,
 *     warms up with SYS_getpid, runs a pure compute loop with NO SVC,
 *     verifies all sentinels, then issues SYS_mmap (which pauses in VM edit
 *     with IRQ enabled via kernel test observation point). After resuming,
 *     verifies sentinels, waitpids Task B, and exits 0x11.
 *   - Task B (x28=2): installs SIGUSR1 handler + sigreturn restorer, sets
 *     distinct GPR/NZCV/SP/FP/TLS/data sentinels, warms up with SYS_getpid,
 *     runs a pure compute loop with NO SVC, verifies sentinels, then spins in
 *     EL0 until SIGUSR1 is delivered via formal IRQ return, verified by
 *     handler + sigreturn, re-verifies sentinels, and exits 0x22.
 */

#include <uapi/syscall.h>

#define SIGUSR1_NUM    10
#define ROLE_A         1
#define ROLE_B         2
#define A_DATA_VAL     0x4141414141414141
#define B_DATA_VAL     0x4242424242424242
#define A_NZCV_VAL     0xa0000000
#define B_NZCV_VAL     0x50000000

.macro URT_EXIT code
    mov     x0, #\code
    mov     x8, #SYS_exit
    svc     #0
.endm

.section .text
.global _start
.type _start, @function
#ifdef AARCH64_USER_RUNTIME_CASE_SMP
#ifdef AARCH64_USER_RUNTIME_COMPANION
_start:
    /* Companion task: pure EL0 compute loop (no SVC) so timer IRQs
     * switch between this companion and the primary task on its CPU. */
    movz    x20, #0x0000
    movk    x20, #0x0200, lsl #16   /* 33.5M iterations */
.Lsmp_comp_loop:
    sub     x20, x20, #1
    cbnz    x20, .Lsmp_comp_loop
    URT_EXIT 0x33
#else /* !AARCH64_USER_RUNTIME_COMPANION */
_start:
    /* Map anonymous page at 0x40000000 and write initial sentinel. */
    movz    x0, #0x4000, lsl #16
    mov     x1, #4096
    mov     x2, #3                  /* PROT_READ | PROT_WRITE */
    mov     x3, #0x32               /* MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED */
    mov     x4, #-1
    mov     x5, #0
    mov     x8, #SYS_mmap
    svc     #0
    movz    x19, #0x4000, lsl #16
    cmp     x0, x19
    b.ne    .Lsmp_mmap_fail

    movz    x21, #0x4444
    movk    x21, #0x3333, lsl #16
    movk    x21, #0x2222, lsl #32
    movk    x21, #0x1111, lsl #48
    str     x21, [x19]

    /* Fork: child must land on CPU 1 via aarch64_user_pick_cpu(). */
    mov     x8, #SYS_fork
    svc     #0
    tbnz    x0, #63, .Lsmp_fork_fail
    cbz     x0, .Lsmp_child

    /* Parent (CPU 0): x27 = child pid */
    mov     x27, x0
    movz    x20, #0x0000
    movk    x20, #0x0100, lsl #16   /* ~16.7M iterations */
.Lsmp_parent_loop:
    sub     x20, x20, #1
    cbnz    x20, .Lsmp_parent_loop

    ldr     x10, [x19]
    cmp     x10, x21
    b.ne    .Lsmp_cow_init_fail

    movz    x22, #0x1111
    movk    x22, #0xaaaa, lsl #16
    movk    x22, #0x1111, lsl #32
    movk    x22, #0xaaaa, lsl #48
    str     x22, [x19]              /* COW write fault on CPU 0 */
    ldr     x10, [x19]
    cmp     x10, x22
    b.ne    .Lsmp_cow_write_fail

    sub     sp, sp, #16
    str     xzr, [sp, #0]
    mov     x0, x27
    mov     x1, sp
    mov     x2, #0
    mov     x8, #SYS_waitpid
    svc     #0
    cmp     x0, x27
    b.ne    .Lsmp_wait_fail
    ldr     x9, [sp, #0]
    add     sp, sp, #16
    mov     x10, #(42 << 8)
    cmp     x9, x10
    b.ne    .Lsmp_status_fail
    URT_EXIT 0x11

.Lsmp_child:
    /* Child (CPU 1) */
    movz    x20, #0x0000
    movk    x20, #0x0100, lsl #16   /* ~16.7M iterations */
.Lsmp_child_loop:
    sub     x20, x20, #1
    cbnz    x20, .Lsmp_child_loop

    ldr     x10, [x19]
    cmp     x10, x21
    b.ne    .Lsmp_cow_init_fail

    movz    x22, #0x2222
    movk    x22, #0xbbbb, lsl #16
    movk    x22, #0x2222, lsl #32
    movk    x22, #0xbbbb, lsl #48
    str     x22, [x19]              /* COW write fault on CPU 1 */
    ldr     x10, [x19]
    cmp     x10, x22
    b.ne    .Lsmp_cow_write_fail
    URT_EXIT 42

.Lsmp_mmap_fail:
    URT_EXIT 0x61
.Lsmp_fork_fail:
    URT_EXIT 0x62
.Lsmp_cow_init_fail:
    URT_EXIT 0x63
.Lsmp_cow_write_fail:
    URT_EXIT 0x64
.Lsmp_wait_fail:
    URT_EXIT 0x65
.Lsmp_status_fail:
    URT_EXIT 0x66
#endif /* AARCH64_USER_RUNTIME_COMPANION */
#elif defined(AARCH64_USER_RUNTIME_CASE_ASID)
#define ASID_FIXED_VA  0x40000000
_start:
    /* Fork to create Task A (parent, x0 > 0) and Task B (child, x0 == 0). */
    mov     x8, #SYS_fork
    svc     #0
    tbnz    x0, #63, .Lasid_fork_fail
    cbz     x0, .Lasid_is_b

    mov     x27, x0                 /* child pid (B) */
    mov     x28, #ROLE_A
    b       .Lasid_map

.Lasid_is_b:
    mov     x27, #0
    mov     x28, #ROLE_B

.Lasid_map:
    /* Map 0x40000000 (in old boot identity range) with MAP_FIXED. */
    movz    x0, #0x4000, lsl #16    /* 0x40000000 */
    mov     x1, #4096
    mov     x2, #3                  /* PROT_READ | PROT_WRITE */
    mov     x3, #0x32               /* MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED */
    mov     x4, #-1
    mov     x5, #0
    mov     x8, #SYS_mmap
    svc     #0
    movz    x9, #0x4000, lsl #16
    cmp     x0, x9
    b.ne    .Lasid_mmap_fail

    /* Set role-specific pattern in x21 and write to [0x40000000]. */
    cmp     x28, #ROLE_A
    b.ne    1f
    movz    x21, #0x1111
    movk    x21, #0xaaaa, lsl #16
    movk    x21, #0x1111, lsl #32
    movk    x21, #0xaaaa, lsl #48
    b       2f
1:
    movz    x21, #0x2222
    movk    x21, #0xbbbb, lsl #16
    movk    x21, #0x2222, lsl #32
    movk    x21, #0xbbbb, lsl #48
2:
    mov     x19, x9                 /* x19 = 0x40000000 */
    str     x21, [x19]

    /* Warm up with SYS_getpid so both tasks are runnable. */
    mov     x8, #SYS_getpid
    svc     #0

    /* Pure compute loop (~64M iterations, no SVC) verifying [0x40000000]
     * stays isolated across timer IRQ switches. */
    movz    x20, #0x0000
    movk    x20, #0x0400, lsl #16
.Lasid_loop:
    ldr     x10, [x19]
    cmp     x10, x21
    b.ne    .Lasid_data_fail
    sub     x20, x20, #1
    cbnz    x20, .Lasid_loop

    cmp     x28, #ROLE_B
    b.eq    .Lasid_b_done

    /* Task A: same-VA MAP_FIXED replacement at 0x40000000 -> must see zero
     * (new anonymous backing, no stale TLB entry). */
    mov     x0, x19
    mov     x1, #4096
    mov     x2, #3
    mov     x3, #0x32
    mov     x4, #-1
    mov     x5, #0
    mov     x8, #SYS_mmap
    svc     #0
    cmp     x0, x19
    b.ne    .Lasid_mmap_fail

    ldr     x10, [x19]
    cbnz    x10, .Lasid_replace_stale

    /* Write new content and read it back. */
    movz    x22, #0x3333
    movk    x22, #0xcccc, lsl #16
    movk    x22, #0x3333, lsl #32
    movk    x22, #0xcccc, lsl #48
    str     x22, [x19]
    ldr     x10, [x19]
    cmp     x10, x22
    b.ne    .Lasid_replace_fail

    /* Wait for Task B to exit 0x22. */
    sub     sp, sp, #16
    str     xzr, [sp, #0]
    mov     x0, x27
    mov     x1, sp
    mov     x2, #0
    mov     x8, #SYS_waitpid
    svc     #0
    cmp     x0, x27
    b.ne    .Lasid_wait_fail
    ldr     x9, [sp, #0]
    add     sp, sp, #16
    mov     x10, #(0x22 << 8)
    cmp     x9, x10
    b.ne    .Lasid_wait_fail

    URT_EXIT 0x11

.Lasid_b_done:
    ldr     x10, [x19]
    cmp     x10, x21
    b.ne    .Lasid_data_fail
    URT_EXIT 0x22

.Lasid_fork_fail:
    URT_EXIT 0x01
.Lasid_mmap_fail:
    URT_EXIT 0x41
.Lasid_data_fail:
    URT_EXIT 0x42
.Lasid_replace_stale:
    URT_EXIT 0x43
.Lasid_replace_fail:
    URT_EXIT 0x44
.Lasid_wait_fail:
    URT_EXIT 0x45
#else
_start:
    /* Fork to create Task A (parent, x0 > 0) and Task B (child, x0 == 0). */
    mov     x8, #SYS_fork
    svc     #0
    tbnz    x0, #63, .Lfork_fail
    cbz     x0, .Lis_b

    /* Task A */
    mov     x27, x0                 /* child pid (B) */
    mov     x28, #ROLE_A
    b       .Lsetup

.Lis_b:
    mov     x27, #0
    mov     x28, #ROLE_B
    b       .Lsetup

.Lfork_fail:
    URT_EXIT 0x01

.Lsetup:
    /* Write role-specific data sentinel (triggers COW for B). */
    adrp    x9, g_role_sentinel
    add     x9, x9, :lo12:g_role_sentinel
    cmp     x28, #ROLE_A
    b.ne    1f
    movz    x10, #0x4141
    movk    x10, #0x4141, lsl #16
    movk    x10, #0x4141, lsl #32
    movk    x10, #0x4141, lsl #48
    b       2f
1:
    movz    x10, #0x4242
    movk    x10, #0x4242, lsl #16
    movk    x10, #0x4242, lsl #32
    movk    x10, #0x4242, lsl #48
2:
    str     x10, [x9]

    /* Set role-specific TLS (tpidr_el0). */
    movz    x9, #0x715a, lsl #48
    orr     x9, x9, x28
    msr     tpidr_el0, x9

    /* Set role-specific FP/SIMD registers d0, d1. */
    movz    x9, #0x3ff0, lsl #48
    orr     x9, x9, x28
    fmov    d0, x9
    movz    x9, #0x4008, lsl #48
    lsl     x10, x28, #8
    orr     x9, x9, x10
    fmov    d1, x9

    /* Set role-specific SP and stack sentinels. */
    sub     sp, sp, #16
    movz    x9, #0x5a5a, lsl #48
    orr     x9, x9, x28
    movz    x10, #0xa5a5, lsl #48
    orr     x10, x10, x28
    stp     x9, x10, [sp, #0]
    mov     x26, sp

    /* Set callee-saved GPR sentinels x19, x21..x25. */
    movz    x19, #0x1919, lsl #48
    orr     x19, x19, x28
    movz    x21, #0x2121, lsl #48
    orr     x21, x21, x28
    movz    x22, #0x2222, lsl #48
    orr     x22, x22, x28
    movz    x23, #0x2323, lsl #48
    orr     x23, x23, x28
    movz    x24, #0x2424, lsl #48
    orr     x24, x24, x28
    movz    x25, #0x2525, lsl #48
    orr     x25, x25, x28

    /* Task B installs SIGUSR1 handler + sigreturn restorer. */
    cmp     x28, #ROLE_B
    b.ne    .Lwarmup
    sub     sp, sp, #32
    adrp    x9, sigusr1_handler
    add     x9, x9, :lo12:sigusr1_handler
    adrp    x10, sigreturn_restorer
    add     x10, x10, :lo12:sigreturn_restorer
    stp     x9, xzr, [sp, #0]
    stp     x10, xzr, [sp, #16]
    mov     x0, #SIGUSR1_NUM
    mov     x1, sp
    mov     x2, #0
    mov     x8, #SYS_signal
    svc     #0
    add     sp, sp, #32
    cbnz    x0, .Lsig_install_fail

.Lwarmup:
    mov     x8, #SYS_getpid
    svc     #0

    /* Arm NZCV just before entering pure compute loop (no SVC). */
    bl      arm_nzcv

    /* Bounded compute loop (~64M iterations, no SVC) so both tasks get
     * multiple 10ms CNTP timer ticks and formal IRQ-tail preemptions. */
    movz    x20, #0x0000
    movk    x20, #0x0400, lsl #16
.Lcompute_loop:
    sub     x20, x20, #1
    cbnz    x20, .Lcompute_loop

    /* Verify all sentinels survived timer IRQ preemption. */
    bl      verify_sentinels

    cmp     x28, #ROLE_B
    b.eq    .Lphase2_b

    /* ── Phase 2: Task A ── */
    /* Re-arm NZCV after verify_sentinels, then trigger SYS_mmap -> VM edit pause. */
    bl      arm_nzcv
    mov     x0, #0
    mov     x1, #4096
    mov     x2, #3                  /* PROT_READ | PROT_WRITE */
    mov     x3, #0x22               /* MAP_PRIVATE | MAP_ANONYMOUS */
    mov     x4, #-1
    mov     x5, #0
    mov     x8, #SYS_mmap
    svc     #0
    tbnz    x0, #63, .La_mmap_fail

    /* Re-arm NZCV? No, check sentinels (including NZCV preserved across SVC). */
    bl      verify_sentinels

    /* Wait for Task B to exit with 0x22. */
    sub     sp, sp, #16
    str     xzr, [sp, #0]
    mov     x0, x27
    mov     x1, sp
    mov     x2, #0
    mov     x8, #SYS_waitpid
    svc     #0
    cmp     x0, x27
    b.ne    .La_wait_fail
    ldr     x9, [sp, #0]
    add     sp, sp, #16
    mov     x10, #(0x22 << 8)
    cmp     x9, x10
    b.ne    .La_wait_fail

    URT_EXIT 0x11

.La_mmap_fail:
    URT_EXIT 0x12
.La_wait_fail:
    URT_EXIT 0x13
.Lsig_install_fail:
    URT_EXIT 0x14

    /* ── Phase 2: Task B ── */
.Lphase2_b:
    bl      arm_nzcv
    adrp    x10, g_b_sig_handled
    add     x10, x10, :lo12:g_b_sig_handled
    movz    x20, #0x0000
    movk    x20, #0x0400, lsl #16   /* bounded spin waiting for IRQ signal */
.Lb_wait_sig:
    ldr     x9, [x10]
    cbnz    x9, .Lb_sig_done
    sub     x20, x20, #1
    cbnz    x20, .Lb_wait_sig
    URT_EXIT 0x21

.Lb_sig_done:
    bl      verify_sentinels
    URT_EXIT 0x22

/* Helper: arm role-specific NZCV without clobbering x19..x28. */
arm_nzcv:
    cmp     x28, #ROLE_A
    b.ne    1f
    movz    x9, #0xa000, lsl #16
    msr     nzcv, x9
    ret
1:
    movz    x9, #0x5000, lsl #16
    msr     nzcv, x9
    ret

/* Helper: verify NZCV, GPRs, SP, FP, TLS, and data sentinels. */
verify_sentinels:
    /* 1. NZCV (read before any cmp!) */
    mrs     x9, nzcv
    cmp     x28, #ROLE_A
    b.ne    1f
    movz    x10, #0xa000, lsl #16
    b       2f
1:
    movz    x10, #0x5000, lsl #16
2:
    cmp     x9, x10
    b.ne    .Lfail_nzcv

    /* 2. GPRs */
    movz    x9, #0x1919, lsl #48
    orr     x9, x9, x28
    cmp     x19, x9
    b.ne    .Lfail_gpr
    movz    x9, #0x2121, lsl #48
    orr     x9, x9, x28
    cmp     x21, x9
    b.ne    .Lfail_gpr
    movz    x9, #0x2222, lsl #48
    orr     x9, x9, x28
    cmp     x22, x9
    b.ne    .Lfail_gpr
    movz    x9, #0x2323, lsl #48
    orr     x9, x9, x28
    cmp     x23, x9
    b.ne    .Lfail_gpr
    movz    x9, #0x2424, lsl #48
    orr     x9, x9, x28
    cmp     x24, x9
    b.ne    .Lfail_gpr
    movz    x9, #0x2525, lsl #48
    orr     x9, x9, x28
    cmp     x25, x9
    b.ne    .Lfail_gpr

    /* 3. SP and stack words */
    mov     x9, sp
    cmp     x9, x26
    b.ne    .Lfail_sp
    ldp     x9, x10, [sp, #0]
    movz    x11, #0x5a5a, lsl #48
    orr     x11, x11, x28
    cmp     x9, x11
    b.ne    .Lfail_sp
    movz    x11, #0xa5a5, lsl #48
    orr     x11, x11, x28
    cmp     x10, x11
    b.ne    .Lfail_sp

    /* 4. FP/SIMD */
    fmov    x9, d0
    movz    x10, #0x3ff0, lsl #48
    orr     x10, x10, x28
    cmp     x9, x10
    b.ne    .Lfail_fp
    fmov    x9, d1
    movz    x10, #0x4008, lsl #48
    lsl     x11, x28, #8
    orr     x10, x10, x11
    cmp     x9, x10
    b.ne    .Lfail_fp

    /* 5. TLS (tpidr_el0) */
    mrs     x9, tpidr_el0
    movz    x10, #0x715a, lsl #48
    orr     x10, x10, x28
    cmp     x9, x10
    b.ne    .Lfail_tls

    /* 6. Data sentinel */
    adrp    x9, g_role_sentinel
    add     x9, x9, :lo12:g_role_sentinel
    ldr     x9, [x9]
    cmp     x28, #ROLE_A
    b.ne    3f
    movz    x10, #0x4141
    movk    x10, #0x4141, lsl #16
    movk    x10, #0x4141, lsl #32
    movk    x10, #0x4141, lsl #48
    b       4f
3:
    movz    x10, #0x4242
    movk    x10, #0x4242, lsl #16
    movk    x10, #0x4242, lsl #32
    movk    x10, #0x4242, lsl #48
4:
    cmp     x9, x10
    b.ne    .Lfail_data
    ret

.Lfail_nzcv:
    URT_EXIT 0x30
.Lfail_gpr:
    URT_EXIT 0x31
.Lfail_sp:
    URT_EXIT 0x32
.Lfail_fp:
    URT_EXIT 0x33
.Lfail_tls:
    URT_EXIT 0x34
.Lfail_data:
    URT_EXIT 0x35

sigusr1_handler:
    cmp     x0, #SIGUSR1_NUM
    b.ne    .Lsig_bad
    adrp    x9, g_b_sig_handled
    add     x9, x9, :lo12:g_b_sig_handled
    ldr     x10, [x9]
    add     x10, x10, #1
    str     x10, [x9]
    ret

.Lsig_bad:
    URT_EXIT 0x36

sigreturn_restorer:
    mov     x8, #SYS_sigreturn
    svc     #0
    URT_EXIT 0x37

.section .data
.balign 8
g_role_sentinel:
    .quad 0
g_b_sig_handled:
    .quad 0
#endif /* AARCH64_USER_RUNTIME_CASE_ASID */
