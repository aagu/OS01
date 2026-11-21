/* aarch64_user_migration.S — EL0 fixture for USER_RUNTIME_CASE=migration.
 *
 * Compiled twice:
 *   1. Main fixture (/tmp/urt-migration): parent spawns on CPU1,
 *      forks child1 (CPU0) and child2 (CPU1), uses shared control page
 *      at 0x40002000, shared data page at 0x40000000, COW page at
 *      0x40001000, PROT_NONE page at 0x40003000.
 *   2. Exec target (/tmp/urt-migration-exec, -DAARCH64_USER_MIGRATION_EXEC=1):
 *      exits immediately with status 42.
 */

#include <uapi/syscall.h>

#define CTRL_VA         0x40002000
#define CTRL_C1_STAGE   0x00
#define CTRL_C2_STAGE   0x08
#define CTRL_MOVE1_GO   0x20
#define CTRL_MOVE2_GO   0x28
#define CTRL_MOVE3_GO   0x30
#define CTRL_COPY_BUF   0x40

#define DATA_VA         0x40000000
#define COW_VA          0x40001000
#define PROTNONE_VA     0x40003000

#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_RW         0x3
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20

    .section .text
    .global _start
_start:
#ifdef AARCH64_USER_MIGRATION_EXEC
    mov     x0, #42
    mov     x8, #SYS_exit
    svc     #0
1:  b       1b
#else
    /* 1. Map control page (MAP_SHARED|MAP_ANONYMOUS|MAP_FIXED) at 0x40002000. */
    movz    x0, #0x4000, lsl #16
    add     x0, x0, #0x2000
    mov     x1, #0x1000
    mov     x2, #PROT_RW
    mov     x3, #(MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED)
    mov     x4, #-1
    mov     x5, #0
    mov     x8, #SYS_mmap
    svc     #0
    cmp     x0, #0
    b.lt    .Lfail_exit

    /* 2. Map shared anon page at 0x40000000 and write old pattern 0x1111. */
    movz    x0, #0x4000, lsl #16
    mov     x1, #0x1000
    mov     x2, #PROT_RW
    mov     x3, #(MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED)
    mov     x4, #-1
    mov     x5, #0
    mov     x8, #SYS_mmap
    svc     #0
    cmp     x0, #0
    b.lt    .Lfail_exit
    movz    x20, #0x4000, lsl #16
    mov     x9, #0x1111
    str     x9, [x20]

    /* 3. Map private COW page at 0x40001000 and write 0x1111. */
    add     x0, x20, #0x1000
    mov     x1, #0x1000
    mov     x2, #PROT_RW
    mov     x3, #(MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED)
    mov     x4, #-1
    mov     x5, #0
    mov     x8, #SYS_mmap
    svc     #0
    cmp     x0, #0
    b.lt    .Lfail_exit
    add     x21, x20, #0x1000
    mov     x9, #0x1111
    str     x9, [x21]

    /* x22 = control page base; fault it in before fork so PA is live. */
    add     x22, x20, #0x2000
    str     xzr, [x22, #CTRL_C1_STAGE]
    str     xzr, [x22, #CTRL_C2_STAGE]
    str     xzr, [x22, #CTRL_MOVE1_GO]
    str     xzr, [x22, #CTRL_MOVE2_GO]
    str     xzr, [x22, #CTRL_MOVE3_GO]

    /* 4. First fork: Child 1 (main migration sample, lands on CPU 0). */
    mov     x8, #SYS_fork
    svc     #0
    cmp     x0, #0
    b.lt    .Lfail_exit
    b.eq    .Lchild1

    /* Parent: waitpid(child1_pid, &status, 0). */
    mov     x19, x0
    sub     sp, sp, #16
    mov     x0, x19
    mov     x1, sp
    mov     x2, #0
    mov     x8, #SYS_waitpid
    svc     #0
    cmp     x0, x19
    b.ne    .Lfail_exit
    ldr     w1, [sp]
    mov     w2, #-14
    cmp     w1, w2
    b.ne    .Lfail_exit
    add     sp, sp, #16

    /* Parent checks old page (0x40000000 == 0x1111) and COW page (0x40001000 == 0x1111). */
    ldr     x10, [x20]
    ldr     x11, [x21]
    mov     x8, #SYS_getpid
    svc     #0

    /* 5. Second fork: Child 2 (uaccess + exec sample, lands on CPU 1). */
    mov     x8, #SYS_fork
    svc     #0
    cmp     x0, #0
    b.lt    .Lfail_exit
    b.eq    .Lchild2

    /* Parent: waitpid(child2_pid, &status, 0). */
    mov     x19, x0
    sub     sp, sp, #16
    mov     x0, x19
    mov     x1, sp
    mov     x2, #0
    mov     x8, #SYS_waitpid
    svc     #0
    cmp     x0, x19
    b.ne    .Lfail_exit
    ldr     w1, [sp]
    mov     w2, #(42 << 8)
    cmp     w1, w2
    b.ne    .Lfail_exit
    add     sp, sp, #16

    mov     x0, #0
    mov     x8, #SYS_exit
    svc     #0

/* ── Child 1: CPU0 -> CPU1 (idle 8-tick) -> remap -> CPU0 -> permission abort ─ */
.Lchild1:
    /* Set TLS sentinel + GPR sentinels before any move. */
    movz    x9, #0xbeef, lsl #16
    movk    x9, #0x1234
    msr     tpidr_el0, x9

    /* Verify COW isolation on 0x40001000: write 0x2222, read back. */
    mov     x10, #0x2222
    str     x10, [x21]
    ldr     x11, [x21]
    /* Read old content from 0x40000000 (warm old TLB). */
    ldr     x12, [x20]
    /* Warm control page write permission before opening VM window. */
    str     xzr, [x22, #CTRL_C1_STAGE]
    /* Kernel observes cow_check + old_load on CPU0 via SYS_getpid. */
    mov     x8, #SYS_getpid
    svc     #0

    /* Signal controller: Child 1 ready for move 1 (0 -> 1). */
    mov     x9, #1
    add     x1, x22, #CTRL_C1_STAGE
    stlr    x9, [x1]

    /* Pure EL0 loop (no syscalls) until controller sets CTRL_MOVE1_GO = 1. */
    movz    x13, #0xcafe, lsl #16
    movk    x13, #0x0001
    add     x1, x22, #CTRL_MOVE1_GO
.Lwait_move1:
    ldar    x9, [x1]
    cbz     x9, .Lwait_move1

    /* Landed on CPU1: SYS_getpid lets kernel verify el0_after (x13 + tpidr_el0). */
    mov     x8, #SYS_getpid
    svc     #0

    /* Remap 0x40000000 on CPU1: munmap -> mmap(PRIVATE) -> store 0x3333 -> mprotect(RO). */
    mov     x0, x20
    mov     x1, #0x1000
    mov     x8, #SYS_munmap
    svc     #0
    cbnz    x0, .Lfail_exit

    mov     x0, x20
    mov     x1, #0x1000
    mov     x2, #PROT_RW
    mov     x3, #(MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED)
    mov     x4, #-1
    mov     x5, #0
    mov     x8, #SYS_mmap
    svc     #0
    cmp     x0, x20
    b.ne    .Lfail_exit

    /* Demand-fill + write new pattern 0x3333. */
    mov     x9, #0x3333
    str     x9, [x20]

    /* mprotect(PROT_READ). */
    mov     x0, x20
    mov     x1, #0x1000
    mov     x2, #PROT_READ
    mov     x8, #SYS_mprotect
    svc     #0
    cbnz    x0, .Lfail_exit

    /* Kernel observes remap on CPU1. */
    mov     x8, #SYS_getpid
    svc     #0

    /* Signal controller: Child 1 ready for move 2 (1 -> 0). */
    mov     x9, #3
    add     x1, x22, #CTRL_C1_STAGE
    stlr    x9, [x1]

    movz    x13, #0xcafe, lsl #16
    movk    x13, #0x0002
    add     x1, x22, #CTRL_MOVE2_GO
.Lwait_move2:
    ldar    x9, [x1]
    cbz     x9, .Lwait_move2

    /* Landed back on CPU0: SYS_getpid lets kernel verify el0_after for move 2. */
    mov     x8, #SYS_getpid
    svc     #0

    /* Direct EL0 load of new pattern followed immediately by store to RO page.
     * No syscall or VM edit in between. */
    ldr     x10, [x20]
    mov     x11, #0x4242
    str     x11, [x20]
    /* Unreachable: permission abort terminates with -EFAULT. */
    b       .Lfail_exit

/* ── Child 2: CPU1 -> CPU0 -> copy_ok/copy_denied -> exec ───────────────────── */
.Lchild2:
    movz    x9, #0xbeef, lsl #16
    movk    x9, #0x5678
    msr     tpidr_el0, x9

    /* Open /tmp/urt-migration-copy (O_WRONLY = 1). */
    adr     x0, .Lcopy_path
    mov     x1, #1
    mov     x2, #0
    mov     x8, #SYS_open
    svc     #0
    cmp     x0, #0
    b.lt    .Lfail_open
    mov     x19, x0

    /* Map PROT_NONE page at 0x40003000. */
    add     x0, x20, #0x3000
    mov     x1, #0x1000
    mov     x2, #PROT_NONE
    mov     x3, #(MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED)
    mov     x4, #-1
    mov     x5, #0
    mov     x8, #SYS_mmap
    svc     #0
    cmp     x0, #0
    b.lt    .Lfail_mmap

    /* Populate valid copy buffer at 0x40002040. */
    mov     x9, #0x55
    add     x1, x22, #CTRL_COPY_BUF
    str     x9, [x1]

    /* Signal controller: Child 2 ready for move 3 (1 -> 0). */
    mov     x9, #1
    add     x1, x22, #CTRL_C2_STAGE
    stlr    x9, [x1]

    movz    x13, #0xcafe, lsl #16
    movk    x13, #0x0003
    add     x1, x22, #CTRL_MOVE3_GO
.Lwait_move3:
    ldar    x9, [x1]
    cbz     x9, .Lwait_move3

    /* Landed on CPU0: SYS_getpid lets kernel verify el0_after for move 3. */
    mov     x8, #SYS_getpid
    svc     #0

    /* Valid SYS_write -> 16 bytes. */
    mov     x0, x19
    add     x1, x22, #CTRL_COPY_BUF
    mov     x2, #16
    mov     x8, #SYS_write
    svc     #0
    cmp     x0, #16
    b.ne    .Lfail_exit

    /* PROT_NONE SYS_write -> -EFAULT (-14). */
    mov     x0, x19
    add     x1, x20, #0x3000
    mov     x2, #16
    mov     x8, #SYS_write
    svc     #0
    mov     x2, #-14
    cmp     x0, x2
    b.ne    .Lfail_exit

    mov     x0, x19
    mov     x8, #SYS_close
    svc     #0

    /* Exec /tmp/urt-migration-exec (exits 42). */
    adr     x0, .Lexec_path
    mov     x1, #0
    mov     x2, #0
    mov     x8, #SYS_exec
    svc     #0
    b       .Lfail_exec

.Lfail_open:
    mov     x0, #91
    mov     x8, #SYS_exit
    svc     #0
.Lfail_mmap:
    mov     x0, #92
    mov     x8, #SYS_exit
    svc     #0
.Lfail_exec:
    mov     x0, #93
    mov     x8, #SYS_exit
    svc     #0

.Lfail_exit:
    mov     x0, #99
    mov     x8, #SYS_exit
    svc     #0
2:  b       2b

.Lcopy_path:
    .asciz "/tmp/urt-migration-copy"
.Lexec_path:
    .asciz "/tmp/urt-migration-exec"
#endif
