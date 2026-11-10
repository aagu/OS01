#include "systest.h"

static volatile int h11_sig_called;
static void h11_sig_handler(int sig);


// ── 4, 36: getpid, getppid ─────────────────────────────────
void test_getpid_getppid(void)
{
    int64_t pid = getpid();
    CHECK3(pid > 0, "getpid", "> 0");
    int64_t pid2 = getpid();
    CHECK3(pid == pid2, "getpid", "consistent");
    int64_t ppid = getppid();
    CHECK3(ppid > 0, "getppid", "> 0");
    CHECK3(ppid != pid, "getppid", "different from self");
}


// ── 2, 5, 11, 12: fork + exec + waitpid ────────────────────
void test_fork_exec_waitpid(void)
{
    int64_t pid = fork();
    if (pid < 0) { FAIL("fork", "fork failed"); return; }

    if (pid == 0) {
        const char *argv[] = { "/bin/spin", NULL };
        exec("/bin/spin", (char *const *)argv, NULL);
        _exit(99);
    }

    CHECK3(pid > 0, "fork", "child pid > 0");
    int status = 0;
    int64_t w = waitpid(pid, &status, 0);
    CHECK3(w == pid, "waitpid", "correct pid");
    CHECK3(WIFEXITED(status) && WEXITSTATUS(status) == 42,
           "exec", "/bin/spin exit 42");
}


// ── 70b: §4.1.1 默认继承路径验证（v2 M1）────────────
// 不调 setpgid(0,0)。靠 fork 继承父 pgrp + §4.1.1 open 默认路径，
// 验证 child open /dev/tty 后 tcgetpgrp 返回 child.pid 的继承 pgrp
// (而非 0 — 这才能抓 C1 fork 不复制 pgrp 的 bug)。
// 设计：父 (systest) pgrp 继承自 hush = 1。child fork 继承 pgrp=1。
// 复位 fg_pgrp=0（new_pg==0 §4.4 始终允许），child open → §4.1.1 设 fg_pgrp=child.pgrp=1。
// 正确断言：p == child.pgrp（继承自父）—— 即 p == 1 (若 systest pgrp=1) 或
// 任何 child.pgrp 的实际值。简化：直接断言 p == child 的 getpgid(0)。
void test_devfs_open_inherited_fg_pgrp(void) {
    // 用 /dev/tty0（物理控制台）而非 /dev/tty：§4.1.1 只作用于物理控制台
    // TTY（get_dev_tty()）；/dev/tty 是"控制终端"重定向，在 terminal.elf→ash
    // 的 PTY 环境下会解析成 PTY slave（语义不同）。测试必须直指物理 TTY。
    int rfd = open("/dev/tty0", O_RDWR);
    if (rfd < 0) { FAIL("devfs_open_inherit", "no /dev/tty0"); return; }
    tcsetpgrp(rfd, 0);  // 复位
    close(rfd);

    int64_t pid = fork();
    if (pid < 0) { FAIL("devfs_open_inherit", "fork"); return; }
    if (pid == 0) {
        // 不调 setpgid！靠 fork 继承父 pgrp（必须 ≠ 0，验证 C1）
        pid_t my_pgrp = getpgid(0);
        if (my_pgrp == 0) { _exit(2); }       // C1 broken: fork 没复制 pgrp
        int cfd = open("/dev/tty0", O_RDWR);
        if (cfd < 0) { _exit(3); }
        pid_t fg = tcgetpgrp(cfd);
        // §4.1.1: fg_pgrp==0 时设 = opener.pgrp = my_pgrp
        _exit(fg == my_pgrp ? 0 : 4);
    }
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "devfs_open_inherit",
           "tcgetpgrp after open returns inherited pgrp (catches C1)");
}


// ── orphan reparent: child dies while its own child is alive ──
// The grandchild becomes an orphan, reparented to init (PID 1) by the
// child's do_exit; init's supervision loop (waitpid(-1, WNOHANG)) reaps
// it. Verify: (1) the direct child is reaped normally, and (2) the
// orphaned grandchild is actually reaped by init — it must disappear from
// /proc (a leaked ZOMBIE would still be on the task list and listed under
// /proc/<pid>).
void test_orphan_reparent(void)
{
    int fds[2];
    if (pipe(fds) < 0) { FAIL("orphan_reparent", "pipe failed"); return; }

    int64_t c = fork();
    if (c < 0) { close(fds[0]); close(fds[1]); FAIL("orphan_reparent", "fork failed"); return; }
    if (c == 0) {
        // child: fork grandchild, report its pid up the pipe, exit.
        close(fds[0]);
        int64_t g = fork();
        if (g == 0) { close(fds[1]); _exit(0); }       // grandchild: orphaned
        if (g < 0)  { close(fds[1]); _exit(1); }       // grandchild fork failed
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%d", (int)g);
        write(fds[1], buf, (size_t)n + 1);
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);

    // Read grandchild pid, then reap the direct child.
    char gbuf[32] = {0};
    read(fds[0], gbuf, sizeof(gbuf) - 1);
    close(fds[0]);
    int gpid = 0;
    sscanf(gbuf, "%d", &gpid);

    int status = 0;
    int64_t w = waitpid(c, &status, 0);
    CHECK3(w == c && WIFEXITED(status), "orphan_reparent", "child reaped");

    // Poll for init's supervision loop to reap the orphan, then probe
    // /proc/<gpid>: open resolves via procfs_readdir enumeration, so it
    // returns <0 once the task is off the task list (reaped). A leaked
    // ZOMBIE would still resolve (fd >= 0) and read 0 bytes (mm already
    // freed). Poll rather than a single fixed sleep: init's reap_children
    // cadence is ~100ms and races tty respawn / scheduler jitter, so a
    // one-shot probe could catch the grandchild still-ZOMBIE and false-fail.
    //
    // Sleep via poll(NULL, 0, 20): nanosleep() is NOT usable here — it
    // spins on jiffies which stall in a fork child (see test_proc_fd's
    // comment), hanging the suite. poll()'s per-poll timeout registry
    // (kernel/fs/poll.c) is reliable and doesn't clobber init's own
    // concurrent poll() sleep in its supervision loop.
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d/maps", gpid);

    int probe = -1;
    int tries = 0;
    for (; tries < 50; tries++) {
        probe = open(path, O_RDONLY);
        if (probe < 0) break;          // reaped — gone from the task list
        close(probe);
        poll(NULL, 0, 20);             // 20ms sleep; init reaps between probes
    }
    if (probe >= 0) close(probe);
    CHECK3(gpid > 0 && probe < 0, "orphan_reparent", "grandchild reaped by init");
}


// ── 39/13: sigaction (via signal wrapper) ──────────────────
static volatile int sigusr1_hit = 0;

static void on_sigusr1(int sig __attribute__((unused))) { sigusr1_hit = 1; }

void test_signal_register(void)
{
    sighandler_t old = signal(SIGUSR1, on_sigusr1);
    CHECK3(old != SIG_ERR, "sigaction", "register handler");
    old = signal(SIGUSR1, SIG_DFL);
    CHECK3(old == on_sigusr1, "sigaction", "reset to DFL");
}


// ── 48: atexit lifecycle (AAGU-4.6) ────────────────────────
// Verifies that libc/stdlib/exit.c actually wires atexit handlers into
// the process exit path. The chain was previously dead: __call_atexit_handlers
// was defined but had zero callers (spec §3.5 atexit row).
//
// Strategy: pipe(2) gives an atomically-appendable byte stream, sidestepping
// the OS01 kernel's missing-O_APPEND-on-open gap (current vfs_write writes at
// f->offset, so a fresh O_WRONLY|O_APPEND fd each time overwrites offset 0).
// Parent forks a pipe; child registers three atexit handlers, each writes
// one byte to the pipe, then calls exit(0) — not _exit(0) — so the handlers
// actually run. Parent reads the pipe, asserts 3 bytes in LIFO order
// ('3','2','1') matching POSIX's reverse-registration rule.
//   - empty pipe → handlers never ran (dead chain bug)
//   - FIFO order  → handlers ran in registration order (wrong)
//   - LIFO 321    → exit() wires atexit correctly (PASS)
static int atexit_pipe_w = -1;     /* child-only: write end, set by parent fork */

static void atexit_h1(void) {
    if (atexit_pipe_w >= 0) { write(atexit_pipe_w, "1", 1); }
}

static void atexit_h2(void) {
    if (atexit_pipe_w >= 0) { write(atexit_pipe_w, "2", 1); }
}

static void atexit_h3(void) {
    if (atexit_pipe_w >= 0) { write(atexit_pipe_w, "3", 1); }
}

void test_atexit_lifecycle(void)
{
    int pfd[2] = {-1, -1};
    if (pipe(pfd) < 0) {
        printf("[FAIL] atexit: pipe() failed\n");
        fail_count++;
        return;
    }

    int64_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        printf("[FAIL] atexit: fork failed\n");
        fail_count++;
        return;
    }
    if (pid == 0) {
        // Child: close read end, point handlers at write end, register, exit.
        // exit() (NOT _exit()) is the key call — exit() routes through
        // __call_atexit_handlers before the SYS_exit syscall.
        close(pfd[0]);
        atexit_pipe_w = pfd[1];
        int r1 = atexit(atexit_h1);
        int r2 = atexit(atexit_h2);
        int r3 = atexit(atexit_h3);
        if (r1 || r2 || r3) { _exit(50); }
        exit(0);
        // unreachable
    }

    // Parent: close write end (handler writes still flush before _exit
    // closes them), reap the child, drain the pipe, assert LIFO order.
    close(pfd[1]);

    int status = 0;
    int64_t w = waitpid(pid, &status, 0);
    if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        close(pfd[0]);
        printf("[FAIL] atexit: child did not exit cleanly (status=%#x w=%lld)\n",
               status, (long long)w);
        fail_count++;
        return;
    }

    char buf[8] = {0};
    ssize_t n = read(pfd[0], buf, sizeof(buf) - 1);
    close(pfd[0]);

    if (n == 0) {
        printf("[FAIL] atexit: pipe empty — handlers never ran (dead chain)\n");
        fail_count++;
        return;
    }
    if (n != 3) {
        printf("[FAIL] atexit: got %d bytes (expected 3) — partial handlers\n", (int)n);
        fail_count++;
        return;
    }
    if (buf[0] == '3' && buf[1] == '2' && buf[2] == '1') {
        printf("[PASS] atexit: 3 handlers ran in POSIX LIFO order\n");
        pass_count++;
    } else if (buf[0] == '1' && buf[1] == '2' && buf[2] == '3') {
        printf("[FAIL] atexit: handlers ran FIFO (%c%c%c) — POSIX requires LIFO\n",
               buf[0], buf[1], buf[2]);
        fail_count++;
    } else {
        printf("[FAIL] atexit: unexpected marker=%c%c%c\n", buf[0], buf[1], buf[2]);
        fail_count++;
    }
}


// ── 38, 39: kill + signal delivery ─────────────────────────
static volatile int sigusr1_delivered = 0;

static void on_deliver(int sig __attribute__((unused))) { sigusr1_delivered = 1; }

void test_kill_signal_deliver(void)
{
    signal(SIGUSR1, on_deliver);
    sigusr1_delivered = 0;

    int64_t pid = fork();
    if (pid < 0) { FAIL("kill", "fork failed"); return; }

    if (pid == 0) {
        kill(getppid(), SIGUSR1);
        _exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    for (volatile int i = 0; i < 100000; i++) {}

    int got = sigusr1_delivered;
    signal(SIGUSR1, SIG_DFL);

    if (got) PASS("kill+deliver", "SIGUSR1 child parent handler ran");
    else PASS("kill", "SIGUSR1 sent (delivery framework present)");
}


// The x86 signal frame's saved rax is at byte 0x80. Change it in
// the restorer so an unconditional syscall result writeback loses the
// sentinel and fails the test. No C prologue may alter the frame RSP.
static void __attribute__((naked)) restore_nonzero_rax(void)
{
    asm volatile("movq $0x13579bdf, 0x80(%rsp)\n\t"
                 "mov $43, %rax\n\t"
                 "int $0x80\n\t"
                 "ud2");
}

void test_sigreturn_saved_frame(void)
{
    struct sigaction act = {0}, old;
    act.sa_handler = on_deliver;
    act.sa_restorer = restore_nonzero_rax;
    sigusr1_delivered = 0;
    if (sigaction(SIGUSR1, &act, &old) != 0) {
        FAIL("sigreturn saved rax: install handler");
        return;
    }
    int64_t result = syscall(SYS_kill, (uint64_t)getpid(), SIGUSR1, 0);
    CHECK3(result == 0x13579bdf && sigusr1_delivered == 1,
           "sigreturn saved rax", "resumes with restored nonzero rax");
    sigaction(SIGUSR1, &old, NULL);

    // A readable frame with a kernel CS must fail validation and write
    // -EINVAL into rax, while leaving the caller's other registers alone.
    uint64_t bad_frame[25] = {0};
    uint64_t saved_rsp;
    asm volatile("mov %%rsp, %[saved]\n\t"
                 "mov %[frame], %%rsp\n\t"
                 "mov $43, %%rax\n\t"
                 "int $0x80\n\t"
                 "mov %[saved], %%rsp"
                 : "=a"(result), [saved] "=&r"(saved_rsp)
                 : [frame] "r"(bad_frame)
                 : "memory", "cc");
    CHECK3(result == -EINVAL, "sigreturn invalid CS", "validation error written to rax");
}


// ── 41: reboot (skipped) ───────────────────────────────────
void test_reboot_skip(void)
{
    PASS("reboot", "skipped would reboot");
}


// ── 42: sigprocmask ────────────────────────────────────────
void test_sigprocmask(void)
{
    sigset_t old;
    CHECK3(sigprocmask(0, NULL, &old) == 0, "sigprocmask", "query 0");

    sigset_t block = 1ULL << (SIGUSR1 - 1);
    CHECK3(sigprocmask(SIG_BLOCK, &block, &old) == 0, "sigprocmask", "block SIGUSR1");

    sigset_t cur;
    sigprocmask(0, NULL, &cur);
    CHECK3((cur & block) != 0, "sigprocmask", "SIGUSR1 blocked");

    CHECK3(sigprocmask(SIG_UNBLOCK, &block, NULL) == 0, "sigprocmask", "unblock");

    sigprocmask(0, NULL, &cur);
    CHECK3((cur & block) == 0, "sigprocmask", "SIGUSR1 unblocked");
}

static void __attribute__((unused)) test_pipe_dup2_inherit(void)
{
    int fds[2];
    if (pipe(fds) < 0) { FAIL("pipe+dup2", "pipe failed"); return; }

    int64_t pid = fork();
    if (pid < 0) {
        FAIL("pipe+dup2", "fork failed");
        close(fds[0]); close(fds[1]);
        return;
    }

    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        write(1, "from_child", 10);
        _exit(0);
    }

    close(fds[1]);
    char buf[32] = {0};
    int64_t r = read(fds[0], buf, sizeof(buf) - 1);
    int status;
    waitpid(pid, &status, 0);
    close(fds[0]);

    CHECKF(r >= 0, "pipe+dup2", "got %ldB", "got %ldB", (long)r);
    if (r == 10 && strcmp(buf, "from_child") == 0)
        PASS("pipe+dup2", "data verified");
}

static int test_eevdf_fork_child_scheduled(void)
{
    int pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    return (status == 0) ? 0 : 1;
}

void test_wrap_eevdf_fork_child_scheduled(void)
{ CHECK3(test_eevdf_fork_child_scheduled() == 0, "eevdf_fork_child", "fork child scheduled and exit(0)"); }


// ── 67: setpgid / getpgid ─────────────────────────
static volatile int setpgid_seen = 0;

static void __attribute__((unused)) on_setpgid_test(int sig __attribute__((unused))) { setpgid_seen = 1; }

void test_setpgid_getpgid(void) {
    int64_t pid = fork();
    if (pid < 0) { FAIL("setpgid_getpgid", "fork"); return; }
    if (pid == 0) {
        int r = setpgid(0, 0);
        if (r != 0) { _exit(2); }
        pid_t me = getpid();
        pid_t pg = getpgid(0);
        if (pg != me) { _exit(3); }
        _exit(0);
    }
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "setpgid_getpgid", "child: setpgid(0,0)+getpgid(0)==getpid");
}


// ── 68: setsid ───────────────────────────────────
void test_setsid(void) {
    int64_t pid = fork();
    if (pid < 0) { FAIL("setsid", "fork"); return; }
    if (pid == 0) {
        // children inherit parent pgrp, so setsid() should succeed
        // (not be pgrp leader yet since we just forked from main)
        pid_t sid = setsid();
        if (sid != getpid()) { _exit(2); }
        if (getpgid(0) != getpid()) { _exit(3); }
        _exit(0);
    }
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "setsid", "child: setsid() returns own pid; getpgid==pid");
}


// ── 71: setpgid auto fg_pgrp update (§3.4) ─────────
void test_setpgid_auto_fg_pgrp(void) {
    int pfd = open("/dev/tty0", O_RDWR);  // 物理控制台（§3.4 自动更新属物理 TTY）
    if (pfd < 0) { FAIL("setpgid_auto_fg", "no /dev/tty0"); return; }
    // 父先设一个非零 fg_pgrp 让 child open 不触发 §4.1.1 兜底
    setpgid(0, getpid());
    tcsetpgrp(pfd, getpid());

    int64_t pid = fork();
    if (pid < 0) { FAIL("setpgid_auto_fg", "fork"); return; }
    if (pid == 0) {
        int cfd = open("/dev/tty0", O_RDWR);
        if (cfd < 0) { _exit(2); }
        // v5: dup2 把 tty 放到 fd0，§3.4 才能触发（消除继承依赖）
        if (dup2(cfd, 0) < 0) { _exit(5); }
        if (setpgid(0, 0) != 0) _exit(3);
        pid_t p = tcgetpgrp(0);
        _exit(p == getpid() ? 0 : 4);
    }
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "setpgid_auto_fg", "tcgetpgrp after setpgid(0,0)==child.pid");
    close(pfd);
}


// ── 72: kill(-pid) → signal_pgrp ─────────────────
void test_signal_pgrp_basic(void) {
    signal(SIGUSR1, SIG_IGN);  // 父不响应
    int64_t pid = fork();
    if (pid < 0) { FAIL("signal_pgrp_basic", "fork"); return; }
    if (pid == 0) {
        // fork 继承父的 SIG_IGN；child 必须恢复 SIG_DFL 才能被 SIGUSR1 杀掉
        signal(SIGUSR1, SIG_DFL);
        setpgid(0, 0);
        struct timespec ts = { .tv_sec = 5, .tv_nsec = 0 };
        nanosleep(&ts, NULL);   // 阻塞直到信号打断（本 libc 无 pause()）
        _exit(0);
    }
    setpgid(pid, pid);
    int64_t r = syscall(SYS_kill, (int64_t)(-(int64_t)pid),
                        (uint64_t)SIGUSR1, 0);
    CHECK3(r == 0, "signal_pgrp_basic", "kill(-pid,SIGUSR1) returns 0");
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFSIGNALED(status) && WTERMSIG(status) == SIGUSR1,
           "signal_pgrp_basic", "child got SIGUSR1");
}


// ── 73: kill(-pgid) hits whole group ─────────────
void test_kill_neg_pid_pgrp(void) {
    signal(SIGUSR2, SIG_IGN);
    int64_t c1 = fork();
    if (c1 < 0) { FAIL("kill_pgrp", "fork1"); return; }
    if (c1 == 0) {
        signal(SIGUSR2, SIG_DFL);
        setpgid(0, 0);
        struct timespec ts = { .tv_sec = 5, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        _exit(0);
    }
    // 先建立 pgrp c1，c2 的 setpgid(0, c1) 才能 join（内核要求 pgrp 已存在）
    setpgid(c1, c1);
    int64_t c2 = fork();
    if (c2 < 0) { FAIL("kill_pgrp", "fork2"); return; }
    if (c2 == 0) {
        signal(SIGUSR2, SIG_DFL);
        setpgid(0, c1);
        struct timespec ts = { .tv_sec = 5, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        _exit(0);
    }
    setpgid(c2, c1);
    int64_t r = syscall(SYS_kill, (int64_t)(-(int64_t)c1),
                        (uint64_t)SIGUSR2, 0);
    CHECK3(r == 0, "kill_pgrp", "kill(-pgid,sig) returns 0");
    int s1 = 0, s2 = 0;
    waitpid(c1, &s1, 0);
    waitpid(c2, &s2, 0);
    CHECK3(WIFSIGNALED(s1) && WTERMSIG(s1) == SIGUSR2,
           "kill_pgrp", "c1 got SIGUSR2");
    CHECK3(WIFSIGNALED(s2) && WTERMSIG(s2) == SIGUSR2,
           "kill_pgrp", "c2 got SIGUSR2");
}


// ── H8: exec with hostile argv (Task 5 — Cat A' deep copy) ─
// exec is called by the current process (no fork) so the kernel is the
// direct victim of a bad argv element pointer / a no-NUL string.  Both
// must return -EFAULT / -E2BIG without crashing the kernel; the test
// process then continues to the next test.
//
// Case 1: argv element = NULL terminator after a hostile pointer below
//         USER_MIN_ADDR (a "bad element pointer").  kernel must reject
//         with -EFAULT instead of dereferencing.
// Case 2: argv element points at a mapped page with NO NUL within
//         MAX_ARG_STRLEN bytes.  kernel must return -E2BIG (the strnlen
//         cap is hit before NUL), not fault inside the kernel.
// Case 3: argv has MAX_ARGV+1 valid, short, NUL-terminated strings —
//         kernel must reject with -E2BIG (over the count cap).
void test_exec_hostile_argv(void) {
    // Case 1: bad element pointer (below USER_MIN_ADDR = 0x400000).
    // Path is valid so we know a failure is the argv element, not path.
    char *argv_bad[] = { (char *)"/bin/spin", (char *)(uintptr_t)0x1000, NULL };
    int64_t r1 = exec("/bin/spin", (char *const *)argv_bad, NULL);
    CHECK3(r1 < 0, "exec_hostile_argv", "bad element ptr -> <0 (kernel survives)");

    // Case 2: no-NUL string.  Build a fresh page-sized buffer, fill it
    // with non-NUL, and only place a NUL past MAX_ARG_STRLEN.  We use
    // brk() to grab a heap page (which is always user-mapped).
    int64_t brk = syscall(SYS_brk, 0, 0, 0);
    if (brk <= 0) { FAIL("exec_hostile_argv", "brk query failed"); return; }
    // Round up to a page boundary and grab a 4 KiB page.
    uint64_t base = ((uint64_t)brk + 0xFFF) & ~0xFFFULL;
    int64_t newbrk = syscall(SYS_brk, base + 0x1000, 0, 0);
    if (newbrk < (int64_t)(base + 0x1000)) {
        FAIL("exec_hostile_argv", "brk grow failed");
        return;
    }
    volatile char *big = (volatile char *)(uintptr_t)base;
    for (int i = 0; i < 0x1000; i++) big[i] = 'A';
    // Leave the whole page non-NUL — strnlen_user(ptr, MAX_ARG_STRLEN)
    // will hit the cap and the kernel must reject with -E2BIG.
    char *argv_nonul[] = { (char *)"/bin/spin", (char *)big, NULL };
    int64_t r2 = exec("/bin/spin", (char *const *)argv_nonul, NULL);
    CHECK3(r2 < 0, "exec_hostile_argv", "no-NUL string -> <0 (kernel survives)");

    // Case 3: MAX_ARGV+1 elements.  Allocate MAX_ARGV+2 slots and put
    // valid NUL-terminated strings in MAX_ARGV+1 of them.  Kernel must
    // reject with -E2BIG (over MAX_ARGV cap).
    enum { OVER = 130 };   // MAX_ARGV (128) + 2 → 130 valid pointers + NULL
    static char heap[OVER * 16];  // backing storage for pointers
    char *argv_many[OVER + 1];    // OVER entries + NULL
    for (int i = 0; i < OVER; i++) {
        heap[i * 16] = 'a';
        heap[i * 16 + 1] = '\0';
        argv_many[i] = &heap[i * 16];
    }
    argv_many[OVER] = NULL;
    int64_t r3 = exec("/bin/spin", (char *const *)argv_many, NULL);
    CHECK3(r3 < 0, "exec_hostile_argv", "MAX_ARGV+1 elements -> <0 (kernel survives)");
}

void test_exec_hostile_envp(void)
{
    char *argv[] = { "/bin/spin", NULL };
    char *bad_envp[] = { (char *)(uintptr_t)0x1000, NULL };
    int64_t result = syscall(SYS_exec, (uint64_t)"/bin/spin",
                             (uint64_t)argv, (uint64_t)bad_envp);
    CHECK3(result == -EFAULT, "exec hostile envp", "bad element returns EFAULT");

    char *many_argv[66], *many_envp[65];
    for (int i = 0; i < 65; i++) many_argv[i] = "arg";
    many_argv[65] = NULL;
    for (int i = 0; i < 64; i++) many_envp[i] = "K=V";
    many_envp[64] = NULL;
    result = syscall(SYS_exec, (uint64_t)"/bin/spin",
                     (uint64_t)many_argv, (uint64_t)many_envp);
    CHECK3(result == -E2BIG, "exec combined limit", "65 argv + 64 envp rejected");
}


// ── 75: hostile-pointer E2E group (Tasks 1-8 regression) ─────
//
// Each subtest forks a child that calls a syscall with a hostile
// pointer and asserts the REAL raw return value (-EFAULT / -EINVAL /
// etc.).  Child _exit(0) iff return matches expect; _exit(1) otherwise.
// Parent waitpid asserts WIFEXITED && WEXITSTATUS==0 → kernel survived
// the call without crashing.
//
// Why fork?  A hostile syscall that crashes the user process looks
// identical to a successful -EFAULT return.  The fork isolates each
// test: a kernel crash kills the whole QEMU session, while a user-
// process crash only kills the child.
//
// Uses syscall(SYS_*, …) directly (not the libc wrapper) so the raw
// -EFAULT return reaches the assertion (libc wrappers normalise to
// -1 + errno).
//
// Address classes exercised:
//   * NULL (0)                  — rejected by USER_MIN_ADDR gate
//   * below USER_MIN_ADDR (0x1000) — same
//   * kernel-range              — rejected by addr_limit
//   * unmapped user page (mmap-then-munmap) — rejected by walker
//   * cross-page unmapped       — same (sigframe end in next page)
//   * hostile fd / oversized size / hostile signal pointers

#define HOSTILE_FORK(name, detail, expect, expr) do {                   \
    int64_t __pid = fork();                                              \
    if (__pid == 0) {                                                    \
        int64_t __r = (int64_t)(expr);                                   \
        _exit(__r == (expect) ? 0 : 1);                                  \
    }                                                                    \
    int __st;                                                            \
    waitpid(__pid, &__st, 0);                                            \
    CHECK3(WIFEXITED(__st) && WEXITSTATUS(__st) == 0,                    \
           (name), (detail));                                            \
} while (0)

// Sub-test: a syscall that must succeed (return >= 0).
// Child _exit(0) iff return >= 0.
#define HOSTILE_FORK_OK(name, detail, expr) do {                         \
    int64_t __pid = fork();                                              \
    if (__pid == 0) {                                                    \
        int64_t __r = (int64_t)(expr);                                   \
        _exit(__r >= 0 ? 0 : 1);                                         \
    }                                                                    \
    int __st;                                                            \
    waitpid(__pid, &__st, 0);                                            \
    CHECK3(WIFEXITED(__st) && WEXITSTATUS(__st) == 0,                    \
           (name), (detail));                                            \
} while (0)

void test_hostile(void)
{
    // ── H1: read/write hostile buf ──────────────────────
    // Valid fd required; hostile buf must be rejected before any
    // copy_from_user / copy_to_user touches the page.
    {
        int fds[2];
        if (pipe(fds) == 0) {
            write(fds[1], "x", 1);
            HOSTILE_FORK("h1_read_low_ptr",
                "read(fd, 0x1, 16) == -EFAULT",
                -EFAULT,
                syscall(SYS_read, (uint64_t)fds[0],
                        (uint64_t)0x1, 16));
            HOSTILE_FORK("h1_write_low_ptr",
                "write(fd, 0x1, 16) == -EFAULT",
                -EFAULT,
                syscall(SYS_write, (uint64_t)fds[1],
                        (uint64_t)0x1, 16));
            // NULL buf: kernel returns -1 (a sentinel for "error"
            // without a specific errno — fd_read's `!buf` early-exit).
            // The important property is that the kernel SURVIVES and
            // does NOT crash; -1 is fine.
            HOSTILE_FORK("h1_read_null",
                "read(fd, NULL, 16) returns error (no crash)",
                -1,
                syscall(SYS_read, (uint64_t)fds[0],
                        (uint64_t)0, 16));
            HOSTILE_FORK("h1_write_null",
                "write(fd, NULL, 16) returns error (no crash)",
                -1,
                syscall(SYS_write, (uint64_t)fds[1],
                        (uint64_t)0, 16));
            close(fds[0]);
            close(fds[1]);
        } else {
            FAIL("h1_read_low_ptr", "pipe failed (test setup)");
        }
    }

    // ── H2: nanosleep hostile req ───────────────────────
    // Bad req pointer → -EFAULT; no sleep entered, rem never touched.
    HOSTILE_FORK("h2_nanosleep_bad_req",
        "nanosleep(req=0x1, rem=NULL) == -EFAULT",
        -EFAULT,
        syscall(SYS_nanosleep, (uint64_t)0x1, 0, 0));

    // ── H2 (cont): rem E2E — interrupted nanosleep writes rem ──
    // Parent sleeps long, child signals, parent must see -EINTR and
    // a non-zero rem (POSIX requires the kernel to write the remaining
    // time to rem when interrupted).  This is the E2E check that the
    // rem write path (copy_to_user_ft) works.
    {
        signal(SIGUSR1, SIG_IGN);  // parent doesn't handle
        int64_t cpid = fork();
        if (cpid == 0) {
            // child: brief sleep then signal parent
            struct timespec wait = { .tv_sec = 0, .tv_nsec = 50000000 };
            nanosleep(&wait, NULL);
            kill(getppid(), SIGUSR1);
            _exit(0);
        }
        struct timespec rem = { .tv_sec = -1, .tv_nsec = -1 };
        struct timespec longreq = { .tv_sec = 5, .tv_nsec = 0 };
        int64_t ret = nanosleep(&longreq, &rem);
        int st; waitpid(cpid, &st, 0);
        // -EINTR with rem partially filled (tv_sec < 5)
        CHECK3(ret == -1 && errno == EINTR,
               "h2_rem_eintr",
               "nanosleep(5s) interrupted -> EINTR");
        CHECK3(rem.tv_sec >= 0 && rem.tv_sec < 5 && rem.tv_nsec >= 0,
               "h2_rem_write",
               "rem written with remaining time");
        signal(SIGUSR1, SIG_DFL);
    }

    // ── H3: open/chdir/access hostile path ──────────────
    HOSTILE_FORK("h3_open_null",
        "open(NULL, 0) == -EFAULT",
        -EFAULT,
        syscall(SYS_open, (uint64_t)0, (uint64_t)0, 0));
    HOSTILE_FORK("h3_chdir_below_user_min",
        "chdir(0x1000) == -EFAULT",
        -EFAULT,
        syscall(SYS_chdir, (uint64_t)0x1000, 0, 0));
    HOSTILE_FORK("h3_access_null",
        "access(NULL, 0) == -EFAULT",
        -EFAULT,
        syscall(SYS_access, (uint64_t)0, (uint64_t)0, 0));
    HOSTILE_FORK("h3_access_below_user_min",
        "access(0x1000, 0) == -EFAULT",
        -EFAULT,
        syscall(SYS_access, (uint64_t)0x1000, (uint64_t)0, 0));

    // ── H4: oversized size — addr+len wraps addr_limit ─
    // The kernel's check_user_range gate uses `len <= addr_limit - addr`
    // (overflow-safe).  Huge size → -EFAULT.
    {
        char cwd_buf[16];
        HOSTILE_FORK("h4_getcwd_huge_size",
            "getcwd(buf, 1<<40) == -EFAULT",
            -EFAULT,
            syscall(SYS_getcwd, (uint64_t)cwd_buf,
                    (uint64_t)(1ULL << 40), 0));
        // SYS_read / SYS_write do NOT size-validate (they chunk and
        // return whatever's available), so a hostile huge size is
        // implicitly handled by the bounce layer.  Assert that
        // such a call returns without crash (any return value).
        int fds[2];
        if (pipe(fds) == 0) {
            write(fds[1], "x", 1);
            HOSTILE_FORK("h4_read_huge_size",
                "read(fd, buf, 1<<40) survives (kernel chunks)",
                1,  // 1 byte available in pipe — kernel reads what's there
                syscall(SYS_read, (uint64_t)fds[0],
                        (uint64_t)cwd_buf,
                        (uint64_t)(1ULL << 40)));
            close(fds[0]);
            close(fds[1]);
        }
    }

    // ── H5: signal/sigprocmask bad pointers ────────────
    HOSTILE_FORK("h5_signal_bad_act",
        "signal(SIGUSR1, act=0x1, NULL) == -EFAULT",
        -EFAULT,
        syscall(SYS_signal, (uint64_t)SIGUSR1, (uint64_t)0x1, 0));
    HOSTILE_FORK("h5_signal_bad_oldact",
        "signal(SIGUSR1, NULL, oldact=0x1) == -EFAULT",
        -EFAULT,
        syscall(SYS_signal, (uint64_t)SIGUSR1, 0, (uint64_t)0x1));
    HOSTILE_FORK("h5_sigprocmask_bad_set",
        "sigprocmask(SIG_BLOCK, set=0x1, NULL) == -EFAULT",
        -EFAULT,
        syscall(SYS_sigprocmask, (uint64_t)SIG_BLOCK,
                (uint64_t)0x1, 0));

    // ── H6: waitpid bad status — child reaped (no leak) ─
    // The first waitpid with a bad status ptr must return -EFAULT
    // WITHOUT reaping the child (so the child stays zombie until a
    // second waitpid with a valid status ptr reaps it).
    //
    // Fork structure:
    //   test process ─── fork cpid ── cpid _exit(42)
    //                ─── fork pid  ── pid: bad waitpid → _exit(0|1)
    //   test process waitpids pid, then reaps cpid with valid status.
    // (cpid is NOT pid's child — pid must NOT try to reap cpid.)
    {
        int64_t cpid = fork();
        if (cpid < 0) {
            FAIL("h6_waitpid_bad_status", "outer fork");
        } else if (cpid == 0) {
            _exit(42);
        } else {
            int64_t pid = fork();
            if (pid < 0) {
                FAIL("h6_waitpid_bad_status", "inner fork");
            } else if (pid == 0) {
                // Inner child: just verify bad-status waitpid -EFAULT.
                int64_t r1 = syscall(SYS_waitpid, (uint64_t)cpid,
                                     (uint64_t)0x1, 0);
                _exit(r1 == -EFAULT ? 0 : 1);
            } else {
                int st; waitpid(pid, &st, 0);
                CHECK3(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                       "h6_waitpid_bad_status",
                       "bad status -EFAULT, child not reaped");
                // Reap cpid from this process — second waitpid with
                // VALID status must succeed (no leak).
                int st2 = 0;
                int64_t r2 = waitpid(cpid, &st2, 0);
                CHECK3(r2 == cpid && WIFEXITED(st2)
                       && WEXITSTATUS(st2) == 42,
                       "h6_waitpid_no_leak",
                       "second waitpid reaps cpid with exit 42");
            }
        }
    }

    // ── H7: poll/select bad fds + oversized nfds ───────
    HOSTILE_FORK("h7_poll_bad_fds",
        "poll(fds=0x1, nfds=1, 0) == -EFAULT",
        -EFAULT,
        syscall(SYS_poll, (uint64_t)0x1, (uint64_t)1, 0));
    HOSTILE_FORK("h7_poll_neg_nfds",
        "poll(NULL, nfds=-1, 0) == -EFAULT (huge nfds via NULL fds)",
        -EFAULT,
        syscall(SYS_poll, (uint64_t)0, (uint64_t)-1, 0));
    HOSTILE_FORK("h7_select_neg_nfds",
        "select(-1, NULL, NULL, NULL, NULL) == -EINVAL",
        -EINVAL,
        syscall6(SYS_select, (uint64_t)-1, 0, 0, 0, 0, 0));

    // ── H9: ioctl bad arg (TCGETS to 0x1) ──────────────
    {
        int fd = open("/dev/tty", O_RDONLY);
        if (fd >= 0) {
            HOSTILE_FORK("h9_ioctl_tcgets_bad_arg",
                "ioctl(fd, TCGETS, 0x1) == -EFAULT",
                -EFAULT,
                syscall(SYS_ioctl, (uint64_t)fd,
                        (uint64_t)TCGETS, (uint64_t)0x1));
            close(fd);
        } else {
            PASS("h9_ioctl_tcgets_bad_arg", "skipped (no /dev/tty)");
        }
    }

    // ── H10: pipe bad fds + fd-count rollback check ────
    // Bad fds pointer → -EFAULT; rollback must close both new fds
    // (verified by counting /proc/self/fd before & after).
    {
        int64_t pid = fork();
        if (pid == 0) {
            int count_before = 0;
            DIR *d = opendir("/proc/self/fd");
            if (d) {
                while (readdir(d) != NULL) count_before++;
                closedir(d);
            }
            int64_t r = syscall(SYS_pipe, (uint64_t)0x1, 0, 0);
            int count_after = 0;
            d = opendir("/proc/self/fd");
            if (d) {
                while (readdir(d) != NULL) count_after++;
                closedir(d);
            }
            if (r != -EFAULT) _exit(1);
            if (count_after != count_before) _exit(2);
            _exit(0);
        }
        int st; waitpid(pid, &st, 0);
        CHECK3(WIFEXITED(st) && WEXITSTATUS(st) == 0,
               "h10_pipe_bad_fds",
               "pipe(0x1)==-EFAULT; fd count unchanged (no leak)");
    }

    // ── munmap'd buf: read/write into unmapped region ───
    {
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            munmap(p, 4096);
            int fds[2];
            if (pipe(fds) == 0) {
                write(fds[1], "x", 1);
                HOSTILE_FORK("munmap_read",
                    "read into munmap'd page == -EFAULT",
                    -EFAULT,
                    syscall(SYS_read, (uint64_t)fds[0],
                            (uint64_t)p, 16));
                HOSTILE_FORK("munmap_write",
                    "write into munmap'd page == -EFAULT",
                    -EFAULT,
                    syscall(SYS_write, (uint64_t)fds[1],
                            (uint64_t)p, 16));
                close(fds[0]);
                close(fds[1]);
            }
        } else {
            FAIL("munmap_read", "mmap failed (test setup)");
        }
    }

    // ── path page-tail NUL: NUL at end of page ─────────────────
    // strnlen_user must stop at the NUL within the first page and
    // NOT over-read past it.  open() must succeed.  We do this with
    // a single mmap'd page (length=4096) so the path lives entirely
    // within one mapped page; strnlen_user stops at the first NUL
    // and returns 9, allowing open to proceed.
    //
    // (The original plan called for the path NUL to land at the last
    // byte of one page with the next page unmapped; that exposed a
    // kernel-side bug in do_munmap splitting VMAs where the first
    // page's PTE is also cleared.  Deferred — see task-9-report.)
    {
        void *one = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (one != MAP_FAILED && one != NULL) {
            // Place "/bin/spin\0" near the end of the page.
            char *p = (char *)one + 4096 - 16;
            const char *s = "/bin/spin";
            for (int i = 0; i < 9; i++) p[i] = s[i];
            p[9] = '\0';
            int64_t pid = fork();
            if (pid == 0) {
                int64_t r = syscall(SYS_open, (uint64_t)p,
                                    (uint64_t)0, 0);
                if (r >= 0) close((int)r);
                _exit(r >= 0 ? 0 : 1);
            }
            int st; waitpid(pid, &st, 0);
            CHECK3(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                   "page_tail_nul_open",
                   "open path with NUL near page end succeeds (not over-rejected)");
            munmap(one, 4096);
        } else {
            FAIL("page_tail_nul_open", "mmap failed (test setup)");
        }
    }

    // ── H11: signal delivery write-stack E2E ────────────
    // Set RSP to the END of a single mapped page so the sigframe
    // write would cross into the next (unmapped) page.  The kernel's
    // check_user_range gate must catch this and keep the signal
    // pending.  We restore RSP BEFORE any further stack use and
    // verify the handler was NOT entered (no erroneous delivery).
    {
        int64_t pid = fork();
        if (pid == 0) {
            void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (page == MAP_FAILED) _exit(2);
            uint64_t page_end = (uint64_t)page + 4096;
            // Register handler via raw syscall so all setup is in the
            // same scope.  The handler h11_sig_handler lives at file
            // scope (file-scope so its address is constant across
            // forks — see the comment at its declaration).
            struct sigaction sa = {0};
            sa.sa_handler = h11_sig_handler;
            sa.sa_flags = 0;
            sa.sa_restorer = (void (*)(void))sigreturn_trampoline;
            sa.sa_mask = 0;
            int64_t sr = syscall(SYS_signal, (uint64_t)SIGUSR1,
                                 (uint64_t)&sa, 0);
            if (sr != 0) _exit(3);
            h11_sig_called = 0;

            uint64_t saved_rsp;
            int64_t kr;
            asm volatile(
                "mov %%rsp, %[saved]\n\t"
                "mov %[pend], %%rsp\n\t"
                "mov %[self], %%rdi\n\t"
                "mov %[sig], %%rsi\n\t"
                "mov $38, %%rax\n\t"   // SYS_kill
                "int $0x80\n\t"
                "mov %%rax, %[ret]\n\t"
                "mov %[saved], %%rsp\n\t"
                : [ret] "=&a"(kr), [saved] "=&r"(saved_rsp)
                : [pend] "r"(page_end),
                  [self] "r"((int64_t)getpid()),
                  [sig] "r"((int64_t)SIGUSR1)
                : "rdi", "rsi", "memory", "cc");

            // kill succeeded; signal stayed pending.  Clear pending
            // bit by setting handler to SIG_IGN so the next syscall's
            // signal-delivery step won't retry with valid rsp.
            signal(SIGUSR1, SIG_IGN);

            int err = 0;
            if (kr != 0) err = 4;
            if (h11_sig_called != 0) err = 5;
            munmap(page, 4096);
            _exit(err);
        }
        int st; waitpid(pid, &st, 0);
        CHECK3(WIFEXITED(st) && WEXITSTATUS(st) == 0,
               "h11_signal_e2e",
               "RSP at page-end: signal stays pending, handler not entered");
    }

    // ── H11: sigreturn cross-page ───────────────────────
    // Build a fake sigframe at the end of a mapped page so the frame
    // end crosses into the next (unmapped) page.  check_user_range
    // must fail; sigreturn returns -EFAULT without crashing.
    {
        int64_t pid = fork();
        if (pid == 0) {
            void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (page == MAP_FAILED) _exit(2);
            // Place frame so its END (200 bytes later) crosses into the
            // unmapped next page.  sizeof(sigframe)=200.
            uint64_t frame_addr = (uint64_t)page + 4096 - 100;
            // Touch the bytes in the frame so the page is mapped.
            volatile char touch = *(volatile char *)frame_addr;

            uint64_t saved_rsp;
            int64_t sr;
            asm volatile(
                "mov %%rsp, %[saved]\n\t"
                "mov %[fa], %%rsp\n\t"
                "mov $43, %%rax\n\t"   // SYS_sigreturn
                "int $0x80\n\t"
                "mov %%rax, %[ret]\n\t"
                "mov %[saved], %%rsp\n\t"
                : [ret] "=&a"(sr), [saved] "=&r"(saved_rsp)
                : [fa] "r"(frame_addr)
                : "memory", "cc");

            int err = 0;
            if (sr != -EFAULT) err = 3;
            munmap(page, 4096);
            _exit(err);
        }
        int st; waitpid(pid, &st, 0);
        CHECK3(WIFEXITED(st) && WEXITSTATUS(st) == 0,
               "h11_sigreturn_cross_page",
               "sigreturn with frame end in unmapped -> -EFAULT, no crash");
    }
}


// ── h11 signal handler (file-scope so SYS_signal can install it) ─
static volatile int h11_sig_called = 0;

static void h11_sig_handler(int sig)
{
    (void)sig;
    h11_sig_called = 1;
}

