/*
 * ptls_probe.c — freestanding static-TLS acceptance probe.
 *
 * Three compile branches share one source:
 *   PTLS_EXEC  ET_EXEC, TinyGo-shaped pure .tbss: the only TLS object
 *              is an 8-byte, 8-byte-aligned zero-initialised
 *              `current_task`.  The probe drives the full lifecycle:
 *              initial template, %fs:0 self-pointer, ARCH_SET_FS to a
 *              self-managed block, fork inheritance/isolation, failed
 *              exec preservation, TLS->TLS re-exec, then TLS->plain.
 *   PTLS_PIE   static PIE with an initialised .tdata object and a
 *              zero-initialised .tbss object.
 *   PTLS_PLAIN no TLS at all; ARCH_GET_FS must report 0 after exec.
 *
 * The fixture links against no libc: every syscall is the raw
 * `int $0x80` native ABI (rax=nr, rdi/rsi/rdx=args).
 *
 * Exit codes (distinct on purpose):
 *   0  pass
 *   10 initial GET / TP self-pointer
 *   11 initial current_task template was not zero
 *   12 re-exec template was not re-initialised
 *   13 restored TP did not restore current_task
 *   14 fork inheritance (child view)
 *   15 fork isolation (parent view / child status)
 *   16 failed exec lost the old TLS value or the manual FS base
 *   17 manual ARCH_SET_FS / ARCH_GET_FS round trip
 *   18 local-exec access against the manual base
 *   19 re-exec chain could not reach ptls_plain
 *   20 PIE initial FS / self-pointer
 *   21 PIE .tdata value
 *   22 PIE .tbss value
 *   31 plain image did not start with FS base 0
 */
typedef unsigned long u64;
typedef long i64;

#define NR_EXIT        2
#define NR_EXEC        5
#define NR_FORK       11
#define NR_WAITPID    12
#define NR_ARCH_PRCTL 79

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

static inline i64 sys3(u64 nr, u64 a1, u64 a2, u64 a3)
{
    i64 ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                         : "memory");
    return ret;
}

static void quit(int code) __attribute__((noreturn));
static void quit(int code)
{
    sys3(NR_EXIT, (u64)code, 0, 0);
    for (;;)
        ;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static i64 get_fs(u64 *out)
{
    return sys3(NR_ARCH_PRCTL, ARCH_GET_FS, (u64)out, 0);
}

static i64 set_fs(u64 base)
{
    return sys3(NR_ARCH_PRCTL, ARCH_SET_FS, base, 0);
}

/* ── PTLS_PLAIN: no TLS; exec must clear the FS base ─────────── */
#if defined(PTLS_PLAIN)
void ptls_probe_main(u64 *sp)
{
    (void)sp;
    u64 tp = 0;
    if (get_fs(&tp) != 0 || tp != 0)
        quit(31);
    quit(0);
}

/* ── PTLS_PIE: .tdata + .tbss ────────────────────────────────── */
#elif defined(PTLS_PIE)

#define PIE_INIT_VALUE 0x1122334455667788UL

_Thread_local volatile u64 ptls_init = PIE_INIT_VALUE;
_Thread_local volatile u64 ptls_zero;

void ptls_probe_main(u64 *sp)
{
    (void)sp;
    u64 tp = 0;
    if (get_fs(&tp) != 0 || tp == 0)
        quit(20);
    if (*(volatile u64 *)tp != tp)
        quit(20);
    if (ptls_init != PIE_INIT_VALUE)
        quit(21);
    if (ptls_zero != 0)
        quit(22);
    ptls_zero = 0xCAFEBABE12345678UL;
    if (ptls_zero != 0xCAFEBABE12345678UL)
        quit(22);
    quit(0);
}

/* ── PTLS_EXEC: TinyGo-shaped pure .tbss + full lifecycle ───── */
#else

_Thread_local volatile u64 current_task;

static u64 block[2] __attribute__((aligned(8)));

#define V_PARENT  0x5555666677778888UL
#define V_CHILD   0x9999AAAABBBBCCCCUL
#define V_FAILED  0xDDDDEEEEFFFF0000UL
#define V_BLOCK   0xA5A5A5A5DEADBEEFUL
#define V_WRITTEN 0x1111222233334444UL

void ptls_probe_main(u64 *sp)
{
    u64 argc = sp[0];
    char **argv = (char **)(sp + 1);
    int reexec = (argc > 1 && streq(argv[1], "reexec"));

    /* Initial image: the loader installed a TP, wrote the [TP]
     * self-pointer and left the pure-.tbss template zeroed. */
    u64 tp = 0;
    if (get_fs(&tp) != 0 || tp == 0)
        quit(10);
    if (*(volatile u64 *)tp != tp)
        quit(10);
    if (current_task != 0)
        quit(11);

    if (reexec) {
        /* TLS -> TLS exec: a fresh anonymous template was installed.
         * Chain into the no-TLS image to prove the FS base is cleared. */
        char *plain[] = { "/bin/ptls_plain", 0 };
        sys3(NR_EXEC, (u64)plain[0], (u64)plain, 0);
        quit(19);
    }

    /* Self-managed two-word block: current_task resolves at TP-8, so
     * setting FS to &block[1] must make current_task alias block[0]. */
    block[0] = V_BLOCK;
    block[1] = (u64)&block[1];
    if (set_fs((u64)&block[1]) != 0)
        quit(17);
    u64 got = 0;
    if (get_fs(&got) != 0 || got != (u64)&block[1])
        quit(17);
    if (*(volatile u64 *)got != got)
        quit(17);
    if (current_task != V_BLOCK)
        quit(18);
    current_task = V_WRITTEN;
    if (block[0] != V_WRITTEN)
        quit(18);

    if (set_fs(tp) != 0)
        quit(17);
    if (current_task != 0)
        quit(13);

    /* fork: the child inherits the written value and the live FS base;
     * both sides then write their own value.  The private TLS VMA is
     * COW, so the two views must not alias. */
    current_task = V_PARENT;
    i64 pid = sys3(NR_FORK, 0, 0, 0);
    if (pid < 0)
        quit(14);
    if (pid == 0) {
        if (current_task != V_PARENT)
            quit(14);
        current_task = V_CHILD;
        if (current_task != V_CHILD)
            quit(14);
        u64 ctp = 0;
        if (get_fs(&ctp) != 0 || ctp != tp)
            quit(14);
        quit(0);
    }

    int st = 0;
    i64 w = sys3(NR_WAITPID, (u64)pid, (u64)&st, 0);
    if (w != pid || (st & 0x7f) != 0 || ((st >> 8) & 0xff) != 0)
        quit(15);
    if (current_task != V_PARENT)
        quit(15);                 /* child's write must not leak back */

    /* A failed exec leaves the old mm, TLS bytes and a manual FS base
     * untouched.  With FS pointed at the manual block, current_task
     * resolves there, so read the template slot through the saved TP
     * (current_task sits at TP-8 for this pure-.tbss image). */
    current_task = V_FAILED;
    if (set_fs((u64)&block[1]) != 0)
        quit(17);
    char *missing[] = { "/bin/ptls_missing", 0 };
    if (sys3(NR_EXEC, (u64)missing[0], (u64)missing, 0) >= 0)
        quit(16);
    if (*(volatile u64 *)(tp - 8) != V_FAILED)
        quit(16);
    if (get_fs(&got) != 0 || got != (u64)&block[1])
        quit(16);

    /* Successful TLS -> TLS exec: the "reexec" instance verifies the
     * fresh template and chains into ptls_plain. */
    char *again[] = { "/bin/ptls_exec", "reexec", 0 };
    sys3(NR_EXEC, (u64)again[0], (u64)again, 0);
    quit(12);
}

#endif
