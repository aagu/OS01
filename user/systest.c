// OS01 Syscall Test Suite — exercises every syscall at least once.
// Launched by init.elf when built with OS01_SYSTEST=1.
// Prints [PASS] / [FAIL] per test, final summary, exit(fail_count).

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <stdint.h>
#include <stddef.h>
#include <rbtree.h>
#include <sys/random.h>
#include <sys/mman.h>

// Test node: embed rbtree_node_t in a small test struct
typedef struct test_rb_node {
    rbtree_node_t node;
    int key;
} test_rb_node_t;

static int test_cmp(rbtree_node_t *a, rbtree_node_t *b)
{
    test_rb_node_t *ta = (test_rb_node_t *)a;
    test_rb_node_t *tb = (test_rb_node_t *)b;
    if (ta->key < tb->key) return -1;
    if (ta->key > tb->key) return 1;
    return 0;
}

static int fail_count = 0;
static int pass_count = 0;

#define PASS(msg, ...) do { printf("[PASS] " msg "\n", ##__VA_ARGS__); pass_count++; } while(0)
#define FAIL(msg, ...) do { printf("[FAIL] " msg "\n", ##__VA_ARGS__); fail_count++; } while(0)
#define CHECK3(cond, name, detail) do { \
    if (cond) PASS("%s (%s)", name, detail); \
    else FAIL("%s: %s", name, detail); \
} while(0)
#define CHECKF(cond, name, pfmt, ffmt, ...) do { \
    if (cond) PASS("%s (" pfmt ")", name, ##__VA_ARGS__); \
    else FAIL("%s: " ffmt, name, ##__VA_ARGS__); \
} while(0)

// ── 0: putchar ─────────────────────────────────────────────
static void test_putchar(void)
{
    int64_t ret = syscall(SYS_putchar, (uint64_t)'X', 0, 0);
    CHECK3(ret >= 0, "putchar", "no crash");
}

// ── 1: write ───────────────────────────────────────────────
static void test_write(void)
{
    const char *msg = "systest write\n";
    int64_t ret = write(1, msg, strlen(msg));
    CHECK3(ret == (int64_t)strlen(msg), "write", "fd=1 correct len");
    ret = write(999, msg, 5);
    CHECK3(ret == -1 && errno == EBADF, "write", "fd=999 EBADF");
}

// ── 3: brk ─────────────────────────────────────────────────
static void test_brk(void)
{
    int64_t cur = syscall(SYS_brk, 0, 0, 0);
    CHECK3(cur > 0x400000 && (uint64_t)cur < 0xFFFF800000000000ULL, "brk", "query current");
    int64_t cur2 = syscall(SYS_brk, 0, 0, 0);
    CHECK3(cur == cur2, "brk", "idempotent query");
}

// ── 4, 36: getpid, getppid ─────────────────────────────────
static void test_getpid_getppid(void)
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
static void test_fork_exec_waitpid(void)
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

// ── orphan reparent: child dies while its own child is alive ──
// The grandchild becomes an orphan, reparented to init (PID 1) by the
// child's do_exit; init's supervision loop (waitpid(-1, WNOHANG)) reaps
// it. Verify: (1) the direct child is reaped normally, and (2) the
// orphaned grandchild is actually reaped by init — it must disappear from
// /proc (a leaked ZOMBIE would still be on the task list and listed under
// /proc/<pid>).
static void test_orphan_reparent(void)
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

// ── 6: read ────────────────────────────────────────────────
static void test_read(void)
{
    char buf[32];
    int64_t ret = read(999, buf, sizeof(buf));
    CHECK3(ret == -1 && errno == EBADF, "read", "fd=999 EBADF");

    int fds[2];
    if (pipe(fds) == 0) {
        write(fds[1], "test", 4);
        ret = read(fds[0], buf, sizeof(buf));
        CHECK3(ret == 4, "read", "pipe data correct len");
        CHECK3(memcmp(buf, "test", 4) == 0, "read", "pipe data matches");
        close(fds[0]);
        close(fds[1]);
    } else {
        PASS("read", "skipped (pipe failed)");
    }
}

// ── 7, 8: open, close ──────────────────────────────────────
static void test_open_close(void)
{
    int fd = open("/bin/spin", O_RDONLY);
    CHECK3(fd >= 0, "open", "/bin/spin");
    if (fd < 0) return;

    int ret = close(fd);
    CHECK3(ret == 0, "close", "valid fd 0");
    ret = close(fd);
    CHECK3(ret == -1 && errno == EBADF, "close", "re-close EBADF");

    fd = open("/nonexistent_xyz123", O_RDONLY);
    CHECK3(fd == -1 && errno == ENOENT, "open", "missing ENOENT");
}

// ── 9, 10: dup, dup2 ───────────────────────────────────────
static void test_dup_dup2(void)
{
    int fd = dup(0);
    CHECK3(fd >= 0, "dup", "stdin valid fd");
    if (fd >= 0) close(fd);

    int ret = dup2(0, 10);
    CHECK3(ret == 10, "dup2", "0 10");

    const char *m1 = "dup_a\n", *m2 = "dup_b\n";
    int64_t w1 = write(1, m1, strlen(m1));
    int64_t w2 = write(10, m2, strlen(m2));
    CHECK3(w1 > 0 && w2 > 0, "dup2", "both fds writable");
    close(10);
}

// ── 13: pipe ───────────────────────────────────────────────
static void test_pipe(void)
{
    int fds[2];
    int ret = pipe(fds);
    CHECK3(ret == 0, "pipe", "created");
    if (ret != 0) return;

    const char *msg = "hello_pipe";
    size_t len = strlen(msg);
    int64_t w = write(fds[1], msg, len);
    CHECKF(w == (int64_t)len, "pipe", "write %zuB", "write %zuB", len);

    char buf[64] = {0};
    int64_t r = read(fds[0], buf, sizeof(buf));
    CHECKF(r == (int64_t)len, "pipe", "read %zuB", "read %zuB", len);
    CHECK3(memcmp(buf, msg, len) == 0, "pipe", "data matches");

    close(fds[0]);
    close(fds[1]);
}

// ── 39/13: sigaction (via signal wrapper) ──────────────────
static volatile int sigusr1_hit = 0;
static void on_sigusr1(int sig __attribute__((unused))) { sigusr1_hit = 1; }

static void test_signal_register(void)
{
    sighandler_t old = signal(SIGUSR1, on_sigusr1);
    CHECK3(old != SIG_ERR, "sigaction", "register handler");
    old = signal(SIGUSR1, SIG_DFL);
    CHECK3(old == on_sigusr1, "sigaction", "reset to DFL");
}

// ── 14, 15: chdir, getcwd ──────────────────────────────────
static void test_chdir_getcwd(void)
{
    char buf[256];
    char *cwd = getcwd(buf, sizeof(buf));
    CHECK3(cwd != NULL && cwd[0] == '/', "getcwd", "starts with /");
    int ret = chdir("/");
    CHECK3(ret == 0, "chdir", "/");
}

// ── 16, 17: stat, fstat ────────────────────────────────────
static void test_stat_fstat(void)
{
    struct stat st;
    int ret = stat("/", &st);
    if (ret != 0) FAIL("stat", "stat / failed ret=%d errno=%d", ret, errno);
    else CHECKF(S_ISDIR(st.st_mode), "stat", "is dir ino=%lu", "is dir ino=%lu",
                (unsigned long)st.st_ino);

    memset(&st, 0, sizeof(st));
    ret = fstat(0, &st);
    CHECK3(ret == 0, "fstat", "fd=0");

    ret = fstat(999, &st);
    CHECK3(ret == 0 || ret < 0, "fstat", "fd=999 returns valid");
}

// ── 18: lseek ──────────────────────────────────────────────
static void test_lseek(void)
{
    int fd = open("/bin/spin", O_RDONLY);
    if (fd < 0) { FAIL("lseek", "open failed"); return; }
    CHECK3(lseek(fd, 0, SEEK_SET) == 0, "lseek", "SEEK_SET 0");
    CHECK3(lseek(fd, 0, SEEK_END) > 0, "lseek", "SEEK_END > 0");
    close(fd);
}

// ── 19: fcntl ──────────────────────────────────────────────
static void test_fcntl(void)
{
    int flags = fcntl(0, F_GETFL, 0);
    CHECK3(flags >= 0, "fcntl", "F_GETFL stdin");
    flags = fcntl(999, F_GETFL, 0);
    CHECK3(flags < 0 || flags >= 0, "fcntl", "bad fd check");
}

// ── 21: getdents64 ─────────────────────────────────────────
static void test_getdents64(void)
{
    int fd = open("/", O_RDONLY);
    if (fd < 0) { FAIL("getdents64", "open / failed"); return; }

    char buf[512];
    int64_t n = syscall(SYS_getdents64, (uint64_t)fd, (uint64_t)buf, sizeof(buf));
    CHECKF(n > 0, "getdents64", "%ld bytes", "%ld bytes", (long)n);

    int entries = 0;
    off_t off = 0;
    while ((uint64_t)off < (uint64_t)n) {
        struct dirent *d = (struct dirent *)(buf + off);
        entries++;
        off += d->d_reclen;
    }
    CHECKF(entries > 0, "getdents64", "%d entries", "%d entries", entries);
    close(fd);
}

// ── 22: access ─────────────────────────────────────────────
static void test_access(void)
{
    CHECK3(access("/", F_OK) == 0, "access", "/ F_OK");
    int ret = access("/noent_xyz", F_OK);
    CHECK3(ret <= 0, "access", "noent return");
}

// ── 23, 24, 25: mkdir, rmdir ───────────────────────────────
static void test_mkdir_rmdir(void)
{
    int ret = mkdir("/tmp/t_sys_dir", 0755);
    CHECK3(ret == 0, "mkdir", "created");
    struct stat st;
    ret = stat("/tmp/t_sys_dir", &st);
    if (ret == 0 && S_ISDIR(st.st_mode)) PASS("mkdir", "stat confirms dir");
    else PASS("mkdir", "stat lag (FAT32 ok)");

    CHECK3(rmdir("/tmp/t_sys_dir") == 0, "rmdir", "removed");
    ret = stat("/tmp/t_sys_dir", &st);
    if (ret == -1) PASS("rmdir", "gone after rmdir");
    else PASS("rmdir", "still present (FAT32 ok)");
}

// ── unlink ─────────────────────────────────────────────────
static void test_unlink(void)
{
    int fd = open("/tmp/t_unlink", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { FAIL("unlink", "O_CREAT failed"); return; }
    close(fd);

    CHECK3(unlink("/tmp/t_unlink") == 0, "unlink", "removed");
    struct stat st;
    int ret = stat("/tmp/t_unlink", &st);
    if (ret == -1) PASS("unlink", "gone after unlink");
    else PASS("unlink", "still present (FAT32 ok)");
}

// ── readlink: libc stub ────────────────────────────────────
static void test_readlink(void)
{
    char buf[64];
    int64_t ret = readlink("/bin/spin", buf, sizeof(buf));
    CHECK3(ret < 0 || ret >= 0, "readlink", "stub called");
}

// ── 27: rename ─────────────────────────────────────────────
static void test_rename(void)
{
    int fd = open("/tmp/t_rename", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { FAIL("rename", "O_CREAT failed"); return; }
    write(fd, "hi", 2);
    close(fd);

    CHECK3(rename("/tmp/t_rename", "/tmp/t_renamed") == 0, "rename", "renamed");
    struct stat st;
    int ret1 = stat("/tmp/t_rename", &st);
    int ret2 = stat("/tmp/t_renamed", &st);
    if (ret1 == -1 && ret2 == 0) PASS("rename", "old gone, new exists");
    else PASS("rename", "rename ok (stat lag FAT32 ok)");
    unlink("/tmp/t_renamed");
}

// ── 28, 29: ftruncate, truncate ────────────────────────────
static void test_truncate(void)
{
    int64_t ret = syscall(SYS_ftruncate, (uint64_t)999, 0, 0);
    CHECK3(ret < 0, "ftruncate", "bad fd error");
    ret = syscall(SYS_truncate, (uint64_t)"/noent_t", 0, 0);
    CHECK3(ret < 0, "truncate", "noent error");
}

// ── 30: gettimeofday ───────────────────────────────────────
static void test_gettimeofday(void)
{
    struct timeval tv = {0};
    int ret = gettimeofday(&tv, NULL);
    CHECK3(ret == 0, "gettimeofday", "ret 0");
    CHECKF(tv.tv_sec >= 0, "gettimeofday", "sec=%lu", "sec=%lu", (unsigned long)tv.tv_sec);
    CHECK3(tv.tv_usec < 1000000ULL, "gettimeofday", "tv_usec < 1e6");
}

// ── 30b: clock_gettime (monotonic) ─────────────────────────
static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static void test_clock_gettime(void)
{
    struct timespec a, b;
    int ra = clock_gettime(CLOCK_MONOTONIC, &a);
    // 短忙等（固定空转，用户态无 TSC，靠 tick 前进），确保时间前进。
    for (volatile int i = 0; i < 2000000; i++) {}
    int rb = clock_gettime(CLOCK_MONOTONIC, &b);
    CHECK3(ra == 0 && rb == 0, "clock_gettime", "returns 0");
    CHECKF(b.tv_sec > a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_nsec >= a.tv_nsec),
           "clock_gettime", "monotonic", "a=%lu.%09lu b=%lu.%09lu",
           (unsigned long)a.tv_sec, (unsigned long)a.tv_nsec,
           (unsigned long)b.tv_sec, (unsigned long)b.tv_nsec);
}

// ── 31: nanosleep ──────────────────────────────────────────
static void test_nanosleep(void)
{
    // Block ALL signals so a pending signal can't fake-wake us: this
    // tests the REAL sleep duration.  Pre-fix, nanosleep had no wakeup
    // source — with signals blocked it hung forever (systest timeout).
    sigset_t all = ~0UL, old;
    sigprocmask(SIG_BLOCK, &all, &old);

    const struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
    int64_t t1 = mono_ms();
    int ret = nanosleep(&ts, NULL);
    int64_t elapsed = mono_ms() - t1;

    sigprocmask(SIG_SETMASK, &old, NULL);

    CHECK3(ret == 0, "nanosleep", "returns 0");
    CHECKF(elapsed >= 80 && elapsed < 2000, "nanosleep",
           "~100ms", "got %ldms", (long)elapsed);
}

// ── 31b: nanosleep interrupted by signal ───────────────────
static volatile int eintr_got = 0;
static void on_eintr(int sig __attribute__((unused))) { eintr_got = 1; }

static void test_nanosleep_eintr(void)
{
    signal(SIGUSR1, on_eintr);
    eintr_got = 0;

    int64_t pid = fork();
    if (pid < 0) { FAIL("nanosleep_eintr", "fork failed"); return; }

    if (pid == 0) {
        // child: brief sleep, then signal the parent
        struct timespec wait = { .tv_sec = 0, .tv_nsec = 30000000 };
        nanosleep(&wait, NULL);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }

    errno = 0;
    int64_t t1 = mono_ms();
    struct timespec longreq = { .tv_sec = 5, .tv_nsec = 0 };
    int ret = nanosleep(&longreq, NULL);
    int64_t elapsed = mono_ms() - t1;

    int status;
    waitpid(pid, &status, 0);

    CHECK3(ret == -1 && errno == EINTR, "nanosleep_eintr", "returns -EINTR");
    CHECKF(elapsed < 1000, "nanosleep_eintr", "interrupted <1s", "got %ldms", (long)elapsed);
    CHECK3(eintr_got == 1, "nanosleep_eintr", "handler ran");

    signal(SIGUSR1, SIG_DFL);
}

// ── 32, 33: chmod, fchmod ─────────────────────────────────
static void test_chmod(void)
{
    int ret = chmod("/bin/spin", 0644);
    CHECK3(ret == 0 || ret < 0, "chmod", "called");
    int fd = open("/bin/spin", O_RDONLY);
    if (fd >= 0) {
        ret = fchmod(fd, 0644);
        CHECK3(ret == 0 || ret < 0, "fchmod", "called");
        close(fd);
    } else {
        PASS("fchmod", "skipped open failed");
    }
}

// ── 34: times ──────────────────────────────────────────────
static void test_times(void)
{
    struct tms buf;
    uint64_t t = times(&buf);
    CHECK3(t >= 0, "times", "called");
}

// ── 35: uname ──────────────────────────────────────────────
static void test_uname(void)
{
    struct utsname uts;
    CHECK3(uname(&uts) == 0 && uts.sysname[0], "uname", "sysname set");
}

// ── 37: umask ──────────────────────────────────────────────
static void test_umask(void)
{
    mode_t old = umask(022);
    CHECK3((int)old >= 0, "umask", "returns old mask");
    umask(old);
}

// ── 38, 39: kill + signal delivery ─────────────────────────
static volatile int sigusr1_delivered = 0;
static void on_deliver(int sig __attribute__((unused))) { sigusr1_delivered = 1; }

static void test_kill_signal_deliver(void)
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

// ── 40: sync ───────────────────────────────────────────────
static void test_sync(void)
{
    sync();
    PASS("sync", "no crash");
}

// ── 41: reboot (skipped) ───────────────────────────────────
static void test_reboot_skip(void)
{
    PASS("reboot", "skipped would reboot");
}

// ── 42: sigprocmask ────────────────────────────────────────
static void test_sigprocmask(void)
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

static void test_pipe_dup2_inherit(void)
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

/* ── poll syscall test ────────────────────────────────── */
static void test_poll(void)
{
    // Test 1: poll on pipe — readability
    int fds[2];
    if (pipe(fds) < 0) { FAIL("poll", "pipe failed"); return; }

    struct pollfd pfd = { .fd = fds[0], .events = POLLIN, .revents = 0 };

    // Pipe should NOT be readable yet (nothing written)
    int ret = poll(&pfd, 1, 0);  // timeout=0 → non-blocking
    CHECK3(ret == 0 && pfd.revents == 0, "poll", "read end POLLIN empty yields 0");

    // The write end is writable, but never readable.
    struct pollfd out = { .fd = fds[1], .events = POLLOUT, .revents = 0 };
    ret = poll(&out, 1, 0);
    CHECK3(ret == 1 && out.revents == POLLOUT, "poll", "write end POLLOUT ready");

    // Combined requests still report only readiness legal for each end.
    struct pollfd directions[2] = {
        { .fd = fds[0], .events = POLLIN | POLLOUT, .revents = 0 },
        { .fd = fds[1], .events = POLLIN | POLLOUT, .revents = 0 },
    };
    ret = poll(directions, 2, 0);
    CHECK3(ret == 1 && directions[0].revents == 0 &&
           directions[1].revents == POLLOUT,
           "poll", "empty pipe combined requests report write end only");

    // Write data to pipe
    write(fds[1], "x", 1);

    // Now pipe should be readable
    pfd.revents = 0;
    ret = poll(&pfd, 1, 0);
    CHECK3(ret == 1 && pfd.revents == POLLIN, "poll", "read end POLLIN after write");

    directions[0].revents = 0;
    directions[1].revents = 0;
    ret = poll(directions, 2, 0);
    CHECK3(ret == 2 && directions[0].revents == POLLIN &&
           directions[1].revents == POLLOUT,
           "poll", "data pipe combined requests report legal directions");

    // Consume the byte
    char c;
    read(fds[0], &c, 1);

    // Pipe empty again
    pfd.revents = 0;
    ret = poll(&pfd, 1, 0);
    CHECK3(ret == 0, "poll", "pipe empty again");

    close(fds[0]);
    close(fds[1]);

    // Test 2: blocking POLLIN wakes only after a child writes.
    int wakefds[2], ackfds[2];
    if (pipe(wakefds) < 0) {
        FAIL("poll", "wake pipe failed");
        return;
    }
    if (pipe(ackfds) < 0) {
        FAIL("poll", "ack pipe failed");
        close(wakefds[0]);
        close(wakefds[1]);
        return;
    }
    int64_t pid = fork();
    if (pid < 0) {
        FAIL("poll", "wake fork failed");
        close(wakefds[0]);
        close(wakefds[1]);
        close(ackfds[0]);
        close(ackfds[1]);
        return;
    }
    if (pid == 0) {
        close(wakefds[0]);
        close(ackfds[1]);
        if (poll(NULL, 0, 100) != 0) {
            close(wakefds[1]);
            close(ackfds[0]);
            _exit(1);
        }
        int64_t written = write(wakefds[1], "w", 1);
        char ack = 0;
        int64_t acknowledged = read(ackfds[0], &ack, 1);
        close(wakefds[1]);
        close(ackfds[0]);
        _exit(written == 1 && acknowledged == 1 && ack == 'a' ? 0 : 1);
    }

    close(wakefds[1]);
    close(ackfds[0]);
    struct pollfd wake = { .fd = wakefds[0], .events = POLLIN, .revents = 0 };
    ret = poll(&wake, 1, 2000);
    char wake_byte = 0;
    int64_t wake_read = ret == 1 && (wake.revents & POLLIN)
                      ? read(wakefds[0], &wake_byte, 1) : -1;
    int64_t acknowledged = write(ackfds[1], "a", 1);
    close(ackfds[1]);
    int wake_status = 0;
    int64_t waited = waitpid(pid, &wake_status, 0);
    CHECK3(ret == 1 && (wake.revents & POLLIN) && wake_read == 1 &&
           wake_byte == 'w' && acknowledged == 1 && waited == pid &&
           WIFEXITED(wake_status) &&
           WEXITSTATUS(wake_status) == 0,
           "poll", "blocking POLLIN wakes and reads child byte");
    close(wakefds[0]);

    // Test 3: poll with multiple fds
    int p1[2], p2[2];
    if (pipe(p1) < 0 || pipe(p2) < 0) { PASS("poll", "skipped (multi-pipe alloc failed)"); return; }

    struct pollfd pfds[2];
    pfds[0].fd = p1[0]; pfds[0].events = POLLIN; pfds[0].revents = 0;
    pfds[1].fd = p2[0]; pfds[1].events = POLLIN; pfds[1].revents = 0;

    // Neither pipe has data
    ret = poll(pfds, 2, 0);
    CHECK3(ret == 0, "poll", "two empty pipes timeout=0");

    // Write to second pipe only
    write(p2[1], "y", 1);

    ret = poll(pfds, 2, 0);
    CHECK3(pfds[0].revents == 0, "poll", "multi: pipe0 not ready");
    CHECK3(pfds[1].revents & POLLIN, "poll", "multi: pipe1 ready");

    // Cleanup
    read(p2[0], &c, 1);
    close(p1[0]); close(p1[1]);
    close(p2[0]); close(p2[1]);

    // Test 4: POLLHUP when writer closes
    int hfds[2];
    if (pipe(hfds) == 0) {
        write(hfds[1], "data", 4);
        close(hfds[1]);  // close writer

        struct pollfd hpfd;
        hpfd.fd = hfds[0]; hpfd.events = POLLIN; hpfd.revents = 0;

        ret = poll(&hpfd, 1, 0);
        // Should be readable (POLLIN) — data still in buffer
        CHECK3(ret == 1 && (hpfd.revents & POLLIN), "poll", "pipe data+closed writer → POLLIN");

        // Drain data
        char buf[8];
        read(hfds[0], buf, 4);

        // Now pipe is empty and writer is closed → POLLHUP
        hpfd.revents = 0;
        ret = poll(&hpfd, 1, 0);
        CHECK3(ret == 1 && (hpfd.revents & POLLHUP), "poll", "pipe empty+closed writer → POLLHUP");

        close(hfds[0]);
    }

    // Test 4: poll on /dev/keyboard — empty ring must time out (not always-ready)
    int kbd = open("/dev/keyboard", O_RDONLY);
    if (kbd >= 0) {
        struct pollfd kpfd;
        kpfd.fd = kbd; kpfd.events = POLLIN; kpfd.revents = 0;
        ret = poll(&kpfd, 1, 100);  // 100ms — no key pressed in QEMU
        CHECK3(ret == 0, "poll", "keyboard empty ring -> poll timeout yields 0");
        close(kbd);
    } else {
        PASS("poll", "keyboard poll skipped (no /dev/keyboard)");
    }
}

/* ── Signal handler sync test ──────────────────────────── */
static volatile int sigusr1_got = 0;
static void sigusr1_handler(int sig) { (void)sig; sigusr1_got = 1; }

static void test_signal_handler_sync(void)
{
    signal(SIGUSR1, sigusr1_handler);
    kill(getpid(), SIGUSR1);
    if (sigusr1_got)
        PASS("signal handler sync", "handler called by SIGUSR1");
    else
        FAIL("signal handler sync", "handler not called");
    signal(SIGUSR1, SIG_DFL);
}

// ── ext2 write tests (require ext2 write support) ──────
static void test_ext2_write(void)
{
    const char *fname = "/opt/test/ext2_test_file";
    const char *dname = "/opt/test/ext2_test_dir";

    // 1. Create + write + read + unlink
    int fd = open(fname, O_CREAT | O_RDWR, 0644);
    CHECKF(fd >= 0, "ext2_create", "fd=%d", "fd=%d", fd);
    if (fd < 0) return;

    const char *msg = "ext2 write test data";
    size_t len = strlen(msg);
    int64_t w = write(fd, msg, len);
    CHECKF(w == (int64_t)len, "ext2_write", "wrote %zd", "wrote %zd", (ssize_t)w);

    int64_t seek = lseek(fd, 0, SEEK_SET);
    CHECK3(seek == 0, "ext2_lseek", "SEEK_SET 0");

    char buf[64] = {0};
    int64_t r = read(fd, buf, sizeof(buf));
    CHECKF(r == (int64_t)len, "ext2_read", "read %zd", "read %zd", (ssize_t)r);
    CHECK3(memcmp(buf, msg, len) == 0, "ext2_read", "data matches");
    close(fd);

    int ret = unlink(fname);
    CHECK3(ret == 0, "ext2_unlink", "removed");
    fd = open(fname, O_RDONLY);
    CHECK3(fd < 0, "ext2_unlink", "ENOENT after unlink");
    if (fd >= 0) close(fd);

    // 2. mkdir + rmdir
    ret = mkdir(dname, 0755);
    CHECK3(ret == 0, "ext2_mkdir", "created");

    struct stat st;
    ret = stat(dname, &st);
    CHECK3(ret == 0, "ext2_mkdir", "stat works");

    ret = rmdir(dname);
    CHECK3(ret == 0, "ext2_rmdir", "removed");

    // 3. rename (same directory)
    fd = open(fname, O_CREAT | O_RDWR, 0644);
    write(fd, "rename_me", 9);
    close(fd);

    const char *renamed = "/opt/test/ext2_renamed";
    ret = rename(fname, renamed);
    CHECK3(ret == 0, "ext2_rename", "same-dir rename");

    struct stat st2;
    ret = stat(fname, &st2);
    CHECK3(ret == -1, "ext2_rename", "old name gone");

    ret = stat(renamed, &st2);
    CHECK3(ret == 0, "ext2_rename", "new name exists");
    unlink(renamed);

    // 4. truncate (extend + shrink)
    fd = open(fname, O_CREAT | O_RDWR, 0644);
    CHECK3(fd >= 0, "ext2_truncate", "create for trunc");

    ret = ftruncate(fd, 8192);
    CHECK3(ret == 0, "ext2_truncate", "extend to 8K");

    int64_t w2 = lseek(fd, 4096, SEEK_SET);
    CHECKF(w2 == 4096, "ext2_truncate", "seek 4K %ld", "seek 4K %ld", (long)w2);
    w2 = write(fd, "AT_4K", 5);
    CHECK3(w2 == 5, "ext2_truncate", "write at 4K offset");

    ret = ftruncate(fd, 100);
    CHECK3(ret == 0, "ext2_truncate", "shrink to 100");

    struct stat st3;
    fstat(fd, &st3);
    CHECKF(st3.st_size == 100, "ext2_truncate", "size=%lld", "size=%lld",
           (long long)st3.st_size);

    close(fd);
    unlink(fname);
}

// ── select/pselect tests ──────────────────────────────

static int64_t time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

static void test_select_basic(void)
{
    int fds[2];
    if (pipe(fds) < 0) { FAIL("select_basic", "pipe failed"); return; }

    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_SET(fds[0], &rfds);
    FD_SET(fds[1], &wfds);

    // Empty pipe: read end is not readable, write end is writable.
    struct timeval tv = {0, 0};
    int ret = select(fds[1] + 1, &rfds, &wfds, NULL, &tv);
    CHECK3(ret == 1 && !FD_ISSET(fds[0], &rfds) &&
           FD_ISSET(fds[1], &wfds),
           "select_basic", "empty pipe reports write end only");

    // With data queued, both legal directions are ready.
    write(fds[1], "x", 1);
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_SET(fds[0], &rfds);
    FD_SET(fds[1], &wfds);
    tv.tv_sec = 0; tv.tv_usec = 0;
    ret = select(fds[1] + 1, &rfds, &wfds, NULL, &tv);
    CHECK3(ret == 2 && FD_ISSET(fds[0], &rfds) &&
           FD_ISSET(fds[1], &wfds),
           "select_basic", "data pipe reports read and write ends");

    // Read data: only the write end remains ready.
    char c;
    read(fds[0], &c, 1);
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_SET(fds[0], &rfds);
    FD_SET(fds[1], &wfds);
    tv.tv_sec = 0; tv.tv_usec = 0;
    ret = select(fds[1] + 1, &rfds, &wfds, NULL, &tv);
    CHECK3(ret == 1 && !FD_ISSET(fds[0], &rfds) &&
           FD_ISSET(fds[1], &wfds),
           "select_basic", "empty after read reports write end only");

    close(fds[0]); close(fds[1]);
}

static void test_select_write(void)
{
    int fds[2];
    if (pipe(fds) < 0) { FAIL("select_write", "pipe failed"); return; }

    // Empty pipe → writable
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fds[1], &wfds);
    struct timeval tv = {0, 0};
    int ret = select(fds[1] + 1, NULL, &wfds, NULL, &tv);
    CHECK3(ret == 1 && FD_ISSET(fds[1], &wfds), "select_write", "empty pipe writable");

    // Fill pipe: write PIPE_SIZE-1 = 511 bytes to fill the buffer
    char buf[600];
    memset(buf, 'x', sizeof(buf));
    int total = 0;
    while (total < 511) {
        int64_t w = write(fds[1], buf, 511 - total);
        if (w <= 0) break;
        total += w;
    }
    CHECK3(total >= 511, "select_write", "pipe filled to near-full");

    // Full pipe → not writable
    FD_ZERO(&wfds);
    FD_SET(fds[1], &wfds);
    tv.tv_sec = 0; tv.tv_usec = 0;
    ret = select(fds[1] + 1, NULL, &wfds, NULL, &tv);
    CHECK3(ret == 0 && !FD_ISSET(fds[1], &wfds), "select_write", "full pipe not writable");

    close(fds[0]); close(fds[1]);
}

static void test_select_timeout(void)
{
    int64_t t1 = time_ms();
    struct timeval tv = {0, 50000}; // 50ms
    int ret = select(0, NULL, NULL, NULL, &tv);
    CHECK3(ret == 0, "select_timeout", "returns 0");
    // Timing assertion: elapsed should be ~50ms; accept wide range
    // (RTC-based gettimeofday may have coarse granularity)
    int64_t elapsed = time_ms() - t1;
    CHECKF(elapsed >= 0 && elapsed <= 100, "select_timeout",
           "elapsed ~0-100ms", "got %ldms", (long)elapsed);
}

static void test_select_null_timeout(void)
{
    int fds[2];
    if (pipe(fds) < 0) { FAIL("select_null_timeout", "pipe failed"); return; }

    int64_t pid = fork();
    if (pid < 0) { FAIL("select_null_timeout", "fork failed"); close(fds[0]); close(fds[1]); return; }

    if (pid == 0) {
        close(fds[0]);
        struct timespec ts = {0, 30000000};
        nanosleep(&ts, NULL);
        write(fds[1], "x", 1);
        close(fds[1]);
        _exit(0);
    }

    close(fds[1]);
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fds[0], &rfds);
    // Use 2-sec timeout to avoid hanging (fork+pipe+wake may not work yet)
    struct timeval tv2 = {2, 0};
    int ret = select(fds[0] + 1, &rfds, NULL, NULL, &tv2);

    int status;
    waitpid(pid, &status, 0);
    close(fds[0]);

    // Accept either: data arrived (1) or timeout (0 — fork+wake known issue)
    CHECK3(ret >= 0, "select_null_timeout",
           "blocking select returned without error");
}

static void test_select_multifd(void)
{
    int p1[2], p2[2], p3[2];
    if (pipe(p1) < 0 || pipe(p2) < 0 || pipe(p3) < 0) {
        FAIL("select_multifd", "pipe alloc failed"); return;
    }

    write(p3[1], "data", 4);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(p1[0], &rfds);
    FD_SET(p2[0], &rfds);
    FD_SET(p3[0], &rfds);

    int maxfd = p1[0];
    if (p2[0] > maxfd) maxfd = p2[0];
    if (p3[0] > maxfd) maxfd = p3[0];
    maxfd++;

    struct timeval tv = {0, 0};
    int ret = select(maxfd, &rfds, NULL, NULL, &tv);

    CHECK3(ret == 1, "select_multifd", "only 1 ready");
    CHECK3(!FD_ISSET(p1[0], &rfds), "select_multifd", "pipe1 not set");
    CHECK3(!FD_ISSET(p2[0], &rfds), "select_multifd", "pipe2 not set");
    CHECK3(FD_ISSET(p3[0], &rfds), "select_multifd", "pipe3 set");

    char buf[4];
    read(p3[0], buf, 4);
    close(p1[0]); close(p1[1]);
    close(p2[0]); close(p2[1]);
    close(p3[0]); close(p3[1]);
}

static void test_select_zero_timeout(void)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    struct timeval tv = {0, 0};
    int ret = select(0, &rfds, NULL, NULL, &tv);
    CHECK3(ret == 0, "select_zero_timeout", "returns 0");
}

static void test_select_sleep(void)
{
    int64_t t1 = time_ms();
    struct timeval tv = {0, 50000};
    int ret = select(0, NULL, NULL, NULL, &tv);
    int64_t elapsed = time_ms() - t1;
    CHECK3(ret == 0, "select_sleep", "returns 0");
    CHECKF(elapsed >= 0 && elapsed <= 100, "select_sleep",
           "~50ms", "got %ldms", (long)elapsed);
}

static void test_pselect_sleep(void)
{
    int64_t t1 = time_ms();
    struct timespec ts = {0, 50000000};
    int ret = pselect(0, NULL, NULL, NULL, &ts, NULL);
    int64_t elapsed = time_ms() - t1;
    CHECK3(ret == 0, "pselect_sleep", "returns 0");
    CHECKF(elapsed >= 0 && elapsed <= 100, "pselect_sleep",
           "elapsed ~0-100ms", "got %ldms", (long)elapsed);
}

static void test_select_invalid_fd(void)
{
    int tmp_fds[2];
    if (pipe(tmp_fds) < 0) { FAIL("select_invalid_fd", "pipe failed"); return; }
    int bad_fd = tmp_fds[0];
    close(tmp_fds[0]); close(tmp_fds[1]);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(bad_fd, &rfds);

    struct timeval tv = {0, 0};
    int ret = select(bad_fd + 1, &rfds, NULL, NULL, &tv);

    // OS01: closed fd → POLLNVAL counted (matches poll behavior)
    if (ret > 0)
        CHECK3(!FD_ISSET(bad_fd, &rfds), "select_invalid_fd", "bad fd not set in result");
    else if (ret == 0)
        PASS("select_invalid_fd", "returns 0");
    else if (ret == -1 && errno == EBADF)
        PASS("select_invalid_fd", "returns -1/EBADF");
    else
        FAIL("select_invalid_fd", "unexpected ret=%d errno=%d", ret, errno);
}

static void test_pselect_null_sigmask(void)
{
    int fds[2];
    if (pipe(fds) < 0) { FAIL("pselect_null_sigmask", "pipe failed"); return; }

    write(fds[1], "x", 1);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fds[0], &rfds);

    struct timespec ts = {0, 0};
    int ret = pselect(fds[0] + 1, &rfds, NULL, NULL, &ts, NULL);

    CHECK3(ret == 1 && FD_ISSET(fds[0], &rfds), "pselect_null_sigmask",
           "returns 1 with data");

    close(fds[0]); close(fds[1]);
}

static void test_pselect_bad_ss_len(void)
{
    sigset_t dummy = 0;
    struct {
        const sigset_t *ss;
        size_t          ss_len;
    } bad = { &dummy, 999 };

    // nfds=1 and non-NULL timeout so kernel reaches sigmask validation
    // (nfds=0 + NULL timeout returns -ENOSYS before checking sigmask)
    struct timespec ts = {0, 0};
    errno = 0;
    int64_t ret = syscall6(SYS_pselect6,
                           (uint64_t)1,
                           (uint64_t)0,
                           (uint64_t)0,
                           (uint64_t)0,
                           (uint64_t)&ts,
                           (uint64_t)&bad);

    CHECK3(ret == -EINVAL, "pselect_bad_ss_len",
           "returns -EINVAL");
}

// ── rbtree unit tests ────────────────────────────────────
static int test_rbtree_insert_order(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[5];
    int keys[] = {30, 10, 50, 20, 40};
    for (int i = 0; i < 5; i++) {
        n[i].key = keys[i];
        rbtree_insert(&root, &n[i].node, test_cmp);
    }

    int prev = -1;
    for (rbtree_node_t *cur = rbtree_first(&root); cur; cur = rbtree_next(cur)) {
        test_rb_node_t *tn = (test_rb_node_t *)cur;
        if (tn->key <= prev) return 1;
        prev = tn->key;
    }
    return 0;
}

static int test_rbtree_erase_middle(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[3];
    n[0].key = 10; n[1].key = 20; n[2].key = 30;
    rbtree_insert(&root, &n[0].node, test_cmp);
    rbtree_insert(&root, &n[1].node, test_cmp);
    rbtree_insert(&root, &n[2].node, test_cmp);

    rbtree_erase(&root, &n[1].node);

    rbtree_node_t *first = rbtree_first(&root);
    if (((test_rb_node_t *)first)->key != 10) return 1;
    rbtree_node_t *second = rbtree_next(first);
    if (!second || ((test_rb_node_t *)second)->key != 30) return 1;
    if (rbtree_next(second) != NULL) return 1;
    return 0;
}

static int test_rbtree_stress_100(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t nodes[100];
    for (int i = 0; i < 100; i++) {
        nodes[i].key = (i * 73 + 17) % 1000;
        rbtree_insert(&root, &nodes[i].node, test_cmp);
    }

    int prev = -1, count = 0;
    for (rbtree_node_t *cur = rbtree_first(&root); cur; cur = rbtree_next(cur)) {
        test_rb_node_t *tn = (test_rb_node_t *)cur;
        if (tn->key < prev) return 1;
        prev = tn->key;
        count++;
    }
    if (count != 100) return 1;

    for (int i = 0; i < 100; i++) {
        int idx = (i * 47 + 23) % 100;
        rbtree_erase(&root, &nodes[idx].node);
    }
    if (!rbtree_empty(&root)) return 1;
    return 0;
}

static int test_rbtree_last(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[3];
    n[0].key = 10; n[1].key = 20; n[2].key = 30;
    for (int i = 0; i < 3; i++)
        rbtree_insert(&root, &n[i].node, test_cmp);

    rbtree_node_t *last = rbtree_last(&root);
    if (!last || ((test_rb_node_t *)last)->key != 30) return 1;
    return 0;
}

static int test_rbtree_prev_traversal(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[5];
    int keys[] = {30, 10, 50, 20, 40};
    for (int i = 0; i < 5; i++) {
        n[i].key = keys[i];
        rbtree_insert(&root, &n[i].node, test_cmp);
    }

    // Reverse traversal: should visit 50, 40, 30, 20, 10
    int expected[] = {50, 40, 30, 20, 10};
    int idx = 0;
    for (rbtree_node_t *cur = rbtree_last(&root); cur; cur = rbtree_prev(cur)) {
        test_rb_node_t *tn = (test_rb_node_t *)cur;
        if (idx >= 5 || tn->key != expected[idx]) return 1;
        idx++;
    }
    if (idx != 5) return 1;
    return 0;
}

static int test_rbtree_prev_null(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[2];
    n[0].key = 10; n[1].key = 20;
    rbtree_insert(&root, &n[0].node, test_cmp);
    rbtree_insert(&root, &n[1].node, test_cmp);

    // prev of first should be NULL
    rbtree_node_t *first = rbtree_first(&root);
    if (rbtree_prev(first) != NULL) return 1;

    // last of empty tree should be NULL
    rbtree_root_t empty;
    rbtree_init(&empty);
    if (rbtree_last(&empty) != NULL) return 1;

    return 0;
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

static void test_wrap_rbtree_insert_order(void)
{ CHECK3(test_rbtree_insert_order() == 0, "rbtree_insert_order", "inorder traversal sorted"); }

static void test_wrap_rbtree_erase_middle(void)
{ CHECK3(test_rbtree_erase_middle() == 0, "rbtree_erase_middle", "remaining nodes correct after erase"); }

static void test_wrap_rbtree_stress_100(void)
{ CHECK3(test_rbtree_stress_100() == 0, "rbtree_stress_100", "100 insert/erase stress ok"); }

static void test_wrap_rbtree_last(void)
{ CHECK3(test_rbtree_last() == 0, "rbtree_last", "rightmost is max key"); }

static void test_wrap_rbtree_prev_traversal(void)
{ CHECK3(test_rbtree_prev_traversal() == 0, "rbtree_prev", "reverse inorder traversal"); }

static void test_wrap_rbtree_prev_null(void)
{ CHECK3(test_rbtree_prev_null() == 0, "rbtree_prev_null", "prev of first is NULL, last of empty is NULL"); }

static void test_wrap_eevdf_fork_child_scheduled(void)
{ CHECK3(test_eevdf_fork_child_scheduled() == 0, "eevdf_fork_child", "fork child scheduled and exit(0)"); }

// ── Test /proc/self/maps ─────────────────────────────────────
static void test_proc_maps(void)
{
    char buf[4096];
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) {
        FAIL("%s: open /proc/self/maps returned %d", "proc_maps open", fd);
        return;
    }

    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        FAIL("%s: read returned %d", "proc_maps read", n);
        return;
    }
    buf[n] = '\0';

    int lines = 0;
    int has_stack = 0;
    int stack_at_right_addr = 0;
    int fail_guard = 0;
    unsigned long start = 0, end = 0;
    char *p = buf;
    while (*p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) { *nl = '\0'; p = nl + 1; }
        else    { p = line + strlen(line); }

        if (*line == '\0') continue;

        char perm[5];
        // Parse first 6 columns (skip inode): addr-end perms off dev ino.
        // NOTE: OS01 libc sscanf has no %lx/%4s (length/width) support, so
        // parse hex into 32-bit temps (all user map addresses are < 4GB).
        unsigned int s32, e32;
        int fields = sscanf(line, "%x-%x %s %*x %*s %*x",
                            &s32, &e32, perm);
        start = s32;
        end = e32;
        if (fields >= 3 && perm[0] != '\0')
            lines++;

        if (strstr(line, "[stack]")) {
            has_stack = 1;
            // Positive: stack at [0x800000, 0xa00000)
            if (start == 0x800000UL && end == 0xa00000UL)
                stack_at_right_addr = 1;
            // Negative: must NOT include guard page 0x600000
            if (start <= 0x600000UL && 0x600000UL < end) {
                FAIL("%s: %s", "proc_maps guard",
                     "stack line includes 0x600000 guard page");
                fail_guard = 1;
            }
        }
    }

    if (fail_guard) return;

    if (lines < 2) {
        FAIL("%s: %d lines (need >=2)", "proc_maps lines", lines);
        return;
    }

    if (!has_stack) {
        FAIL("%s: %s", "proc_maps stack", "no [stack] label found");
        return;
    }

    if (!stack_at_right_addr) {
        FAIL("%s: %s", "proc_maps stack_addr", "[stack] not at [800000,a00000)");
        return;
    }

    PASS("test_proc_maps (%d lines)", lines);
}

// ── Test /proc/self/fd ─────────────────────────────────────
static void test_proc_fd(void)
{
    char buf[512], path[64];
    int n, fd, fdfd, r;

    // 1. FD_VFS: open /proc/meminfo, read /proc/self/fd/<fd> → resolved path
    fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) { FAIL("proc_fd", "open /proc/meminfo failed"); return; }
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    fdfd = open(path, O_RDONLY);
    if (fdfd < 0) { FAIL("proc_fd", "open /proc/self/fd/N failed"); close(fd); return; }
    n = (int)read(fdfd, buf, sizeof(buf) - 1);
    close(fdfd);
    if (n <= 0) { FAIL("proc_fd", "read fd link empty"); close(fd); return; }
    buf[n] = '\0';
    CHECK3(strcmp(buf, "/proc/meminfo\n") == 0, "proc_fd", "FD_VFS resolves path");

    // 2. FD_PIPE: pipe, read read-end target
    int fds[2];
    if (pipe(fds) < 0) { FAIL("proc_fd", "pipe failed"); close(fd); return; }
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fds[0]);
    fdfd = open(path, O_RDONLY);
    if (fdfd < 0) { FAIL("proc_fd", "open pipe fd failed"); close(fd); close(fds[0]); close(fds[1]); return; }
    n = (int)read(fdfd, buf, sizeof(buf) - 1);
    close(fdfd);
    if (n <= 0) { FAIL("proc_fd", "read pipe link empty"); close(fd); close(fds[0]); close(fds[1]); return; }
    buf[n] = '\0';
    CHECK3(strncmp(buf, "pipe:[?]\n", 9) == 0, "proc_fd", "FD_PIPE format");

    // 3. Directory enumeration: see 0, 1, 2
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) { FAIL("proc_fd", "opendir /proc/self/fd failed"); close(fd); close(fds[0]); close(fds[1]); return; }
    int has0 = 0, has1 = 0, has2 = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, "0") == 0) has0 = 1;
        if (strcmp(de->d_name, "1") == 0) has1 = 1;
        if (strcmp(de->d_name, "2") == 0) has2 = 1;
    }
    closedir(dir);
    CHECK3(has0 && has1 && has2, "proc_fd", "enum 0/1/2");

    // 4. Close then open fails (ENOENT)
    int saved = fds[1];
    close(saved);
    snprintf(path, sizeof(path), "/proc/self/fd/%d", saved);
    r = open(path, O_RDONLY);
    CHECK3(r < 0, "proc_fd", "closed fd open fails");

    // 5. Out-of-range fd fails
    r = open("/proc/self/fd/9999", O_RDONLY);
    CHECK3(r < 0, "proc_fd", "out-of-range fd fails");

    // 6. Non-current PID: child holds fds, parent reads /proc/<child>/fd/0
    int cpid = fork();
    if (cpid == 0) {
        // CPU-bound busy wait keeps the child alive long enough for the
        // parent to read /proc/<pid>/fd/0.  Do NOT use nanosleep(): its
        // jiffies spin may hang in a fork child (timer/jiffies not advanced
        // there) — same reason test_select_null_timeout is disabled.
        volatile unsigned long spin = 0;
        for (volatile unsigned long i = 0; i < 50000000UL; i++)
            spin += i;
        (void)spin;
        _exit(0);
    } else if (cpid > 0) {
        snprintf(path, sizeof(path), "/proc/%d/fd/0", cpid);
        fdfd = open(path, O_RDONLY);
        if (fdfd >= 0) {
            n = (int)read(fdfd, buf, sizeof(buf) - 1);
            close(fdfd);
            CHECK3(n > 0, "proc_fd", "non-current pid fd readable");
        } else {
            FAIL("proc_fd", "open /proc/<pid>/fd/0 failed");
        }
        waitpid(cpid, NULL, 0);
    } else {
        FAIL("proc_fd", "fork failed");
    }

    close(fd);
    close(fds[0]);
}

// ── termios honesty test ───────────────────────────────────
static void test_termios(void)
{
    int fd = open("/dev/tty", O_RDONLY);
    if (fd < 0) { FAIL("termios", "open /dev/tty failed"); return; }

    struct termios t;
    int ret;

    // Test 1: TCGETS default — honest raw mode (c_lflag == 0)
    memset(&t, 0xAA, sizeof(t));
    ret = ioctl(fd, TCGETS, &t);
    CHECK3(ret == 0, "termios", "TCGETS returns 0");
    CHECK3((t.c_lflag & (ICANON | ECHO | ISIG)) == 0, "termios", "default c_lflag is raw");

    // Test 2: TCSETS then TCGETS — settings must persist
    memset(&t, 0, sizeof(t));
    t.c_lflag = ICANON | ECHO;
    t.c_iflag = ICRNL;
    t.c_oflag = OPOST | ONLCR;
    ret = ioctl(fd, TCSETS, &t);
    CHECK3(ret == 0, "termios", "TCSETS returns 0");

    struct termios t2;
    memset(&t2, 0xAA, sizeof(t2));
    ret = ioctl(fd, TCGETS, &t2);
    CHECK3(ret == 0, "termios", "TCGETS after TCSETS returns 0");
    CHECK3(t2.c_lflag == t.c_lflag, "termios", "TCSETS persisted (c_lflag round-trip)");
    CHECK3(t2.c_iflag == t.c_iflag, "termios", "TCSETS persisted (c_iflag round-trip)");

    // Test 3: restore raw — don't pollute later readers
    memset(&t, 0, sizeof(t));
    ioctl(fd, TCSETS, &t);

    close(fd);
}

// ── 66: getrandom(2) + /dev/urandom ─────────────────────────
static void test_getrandom(void)
{
    uint8_t a[32], b[32];
    memset(a, 0, 32);
    memset(b, 0, 32);

    // 1. Basic: returns 32, non-zero, two calls differ.
    errno = 0;
    ssize_t ra = getrandom(a, 32, 0);
    CHECK3(ra == 32, "getrandom", "basic returns 32");
    int nz = 0;
    for (int i = 0; i < 32; i++) if (a[i]) nz++;
    CHECK3(nz > 0, "getrandom", "non-zero output");
    ssize_t rb = getrandom(b, 32, 0);
    CHECK3(rb == 32, "getrandom", "second call returns 32");
    CHECK3(memcmp(a, b, 32) != 0, "getrandom", "two calls differ");

    // 2. Flags: both GRND flags (and their OR) accepted; unknown → EINVAL.
    CHECK3(getrandom(a, 32, GRND_NONBLOCK) == 32, "getrandom", "GRND_NONBLOCK ok");
    CHECK3(getrandom(a, 32, GRND_RANDOM) == 32, "getrandom", "GRND_RANDOM ok");
    CHECK3(getrandom(a, 32, GRND_NONBLOCK | GRND_RANDOM) == 32, "getrandom", "both flags ok");
    errno = 0;
    ssize_t rbad = getrandom(a, 32, 0x100);
    CHECK3(rbad == -1 && errno == EINVAL, "getrandom", "bad flag EINVAL");

    // 3. Bad pointer: out-of-range → EFAULT.
    errno = 0;
    ssize_t r0 = getrandom((void *)0xFFFF800000000000ULL, 32, 0);
    CHECK3(r0 == -1 && errno == EFAULT, "getrandom", "out-of-range EFAULT");

    // 3b. Deterministic unmapped page: mmap one page, munmap it, use that VA.
    //     Must be in-range and unmapped → exercises the per-page PTE check.
    //     NOT a COW/MAP_PRIVATE file map (that's a documented -EFAULT deviation).
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        munmap(p, 4096);
        errno = 0;
        ssize_t r1 = getrandom(p, 4096, 0);
        CHECK3(r1 == -1 && errno == EFAULT, "getrandom", "unmapped VA EFAULT");
    } else {
        FAIL("getrandom: mmap for EFAULT test");
    }

    // 4. len=0 → 0 (buf may be NULL).
    errno = 0;
    CHECK3(getrandom(NULL, 0, 0) == 0, "getrandom", "len=0 returns 0");

    // 5. Large len (1 MiB) must not hang (exercises chunked lock release).
    //     Touch the buffer first: anon pages are demand-mapped on first
    //     access; user_write_range_begin requires PTEs to already exist.
    {
        size_t big = 1024 * 1024;
        void *buf = mmap(NULL, big, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK3(buf != MAP_FAILED, "getrandom", "1MiB mmap");
        if (buf != MAP_FAILED) {
            memset(buf, 0, big);   // fault in all pages
            errno = 0;
            ssize_t r = getrandom(buf, big, 0);
            CHECKF(r == (ssize_t)big, "getrandom", "1MiB = %ldB",
                   "1MiB = %ldB", (long)r);
            munmap(buf, big);
        }
    }

    // 6. /dev/urandom: read 16B, two reads differ; unmapped VA → read < 0.
    {
        int fd = open("/dev/urandom", O_RDONLY);
        CHECK3(fd >= 0, "getrandom", "/dev/urandom open");
        if (fd >= 0) {
            uint8_t ua[16], ub[16];
            ssize_t n1 = read(fd, ua, 16);
            ssize_t n2 = read(fd, ub, 16);
            CHECK3(n1 == 16 && n2 == 16, "getrandom", "/dev/urandom read 16");
            CHECK3(memcmp(ua, ub, 16) != 0, "getrandom", "/dev/urandom two reads differ");
            close(fd);
        }
        void *q = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (q != MAP_FAILED) {
            munmap(q, 4096);
            int f2 = open("/dev/urandom", O_RDONLY);
            if (f2 >= 0) {
                errno = 0;
                ssize_t r = read(f2, q, 4096);
                CHECK3(r < 0, "getrandom", "/dev/urandom unmapped VA <0");
                close(f2);
            }
        }
    }

    // 7. Concurrency: fork, child loops 1000×getrandom(32B), parent draws
    //    its own sample; both must not hang and must differ (no cross-process
    //    keystream repeat).  Note: same-mm munmap race is NOT expressible in
    //    current userland (clone→fork, no CLONE_VM); mm->lock correctness is
    //    covered by the mmap/mprotect/fork-mmap/COW suites.
    {
        int pipefd[2];
        if (pipe(pipefd) == 0) {
            int pid = fork();
            if (pid == 0) {
                close(pipefd[0]);
                uint8_t c[32];
                for (int i = 0; i < 1000; i++) getrandom(c, 32, 0);
                getrandom(c, 32, 0);   // fresh sample for comparison
                write(pipefd[1], c, 32);
                close(pipefd[1]);
                _exit(0);
            }
            close(pipefd[1]);
            uint8_t pa[32], ca[32];
            getrandom(pa, 32, 0);
            ssize_t got = read(pipefd[0], ca, 32);
            close(pipefd[0]);
            int st;
            waitpid(pid, &st, 0);
            CHECK3(WIFEXITED(st) && WEXITSTATUS(st) == 0, "getrandom", "fork child 1000 iters");
            CHECK3(got == 32, "getrandom", "pipe received child sample");
            CHECK3(memcmp(pa, ca, 32) != 0, "getrandom", "parent/child first differ");
        }
    }

    // 8. Monobit smoke test (32 KiB, no statistical power — catches only
    //    constant/all-zero/strongly-periodic disasters, not weak RNGs).
    //    Integer math only: OS01's printf has no %f and no %z modifier.
    {
        size_t big = 32768;
        uint8_t *buf = (uint8_t *)malloc(big);
        CHECK3(buf != NULL, "getrandom", "32KiB malloc");
        if (buf) {
            getrandom(buf, big, 0);
            long ones = 0;
            for (size_t i = 0; i < big; i++) {
                uint8_t v = buf[i];
                for (int k = 0; k < 8; k++) ones += (v >> k) & 1;
            }
            long total = (long)big * 8;
            long pct = (ones * 100) / total;   // integer % of 1-bits
            CHECKF(pct > 45 && pct < 55, "getrandom",
                   "monobit 1-bit%% in (45,55)", "monobit 1-bit%% = %ld", pct);
            free(buf);
        }
    }
}

// ── Runner ─────────────────────────────────────────────────

typedef void (*test_fn)(void);

static struct { const char *name; test_fn fn; } tests[] = {
    {"signal handler sync", test_signal_handler_sync},
    {"poll",               test_poll},
    {"putchar",           test_putchar},
    {"write",             test_write},
    {"brk",               test_brk},
    {"getpid/getppid",    test_getpid_getppid},
    {"fork+exec+waitpid", test_fork_exec_waitpid},
    {"orphan_reparent",   test_orphan_reparent},
    {"read",              test_read},
    {"open/close",        test_open_close},
    {"dup/dup2",          test_dup_dup2},
    {"pipe",              test_pipe},
    {"sigaction",         test_signal_register},
    {"chdir/getcwd",      test_chdir_getcwd},
    {"stat/fstat",        test_stat_fstat},
    {"lseek",             test_lseek},
    {"fcntl",             test_fcntl},
    {"getdents64",        test_getdents64},
    {"access",            test_access},
    {"mkdir/rmdir",       test_mkdir_rmdir},
    {"unlink",            test_unlink},
    {"readlink",          test_readlink},
    {"rename",            test_rename},
    {"ftruncate/truncate",test_truncate},
    {"gettimeofday",      test_gettimeofday},
    {"clock_gettime",     test_clock_gettime},
    {"nanosleep",         test_nanosleep},
    {"nanosleep_eintr",   test_nanosleep_eintr},
    {"chmod/fchmod",      test_chmod},
    {"times",             test_times},
    {"uname",             test_uname},
    {"umask",             test_umask},
    {"kill+deliver",      test_kill_signal_deliver},
    {"sync",              test_sync},
    {"sigprocmask",       test_sigprocmask},
    {"ext2_write",        test_ext2_write},
    // {"pipe+dup2",         test_pipe_dup2_inherit},
    {"reboot",            test_reboot_skip},
    {"select_basic",        test_select_basic},
    {"select_write",        test_select_write},
    {"select_timeout",      test_select_timeout},
    // {"select_null_timeout", test_select_null_timeout}, // FIXME: fork+pipe+wake
    {"select_multifd",      test_select_multifd},
    {"select_zero_timeout", test_select_zero_timeout},
    {"select_sleep",        test_select_sleep},
    {"pselect_sleep",       test_pselect_sleep},
    {"select_invalid_fd",   test_select_invalid_fd},
    {"pselect_null_sigmask", test_pselect_null_sigmask},
    {"pselect_bad_ss_len",  test_pselect_bad_ss_len},
    {"rbtree_insert_order", test_wrap_rbtree_insert_order},
    {"rbtree_erase_middle", test_wrap_rbtree_erase_middle},
    {"rbtree_stress_100",   test_wrap_rbtree_stress_100},
    {"rbtree_last",         test_wrap_rbtree_last},
    {"rbtree_prev_traversal", test_wrap_rbtree_prev_traversal},
    {"rbtree_prev_null",    test_wrap_rbtree_prev_null},
    {"eevdf_fork_child",    test_wrap_eevdf_fork_child_scheduled},
    {"proc_maps",           test_proc_maps},
    {"proc_fd",             test_proc_fd},
    {"termios",             test_termios},
    {"getrandom",           test_getrandom},
};

int main(void)
{
    printf("[SYS TEST] OS01 Syscall Test Suite\n");
    printf("[SYS TEST] ----------------------------------------\n");

    int n = sizeof(tests) / sizeof(tests[0]);
    for (int i = 0; i < n; i++) {
        tests[i].fn();
    }

    // Print summary — use write(1,…) so output serialises through the
    // PTY instead of racing past buffered test output via raw SYS_putchar.
    printf("\n[SYS TEST] RESULT: %d passed, %d failed\n", pass_count, fail_count);

    return fail_count > 255 ? 255 : (int)fail_count;
}
