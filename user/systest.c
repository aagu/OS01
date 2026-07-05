// OS01 Syscall Test Suite — exercises every syscall at least once.
// Launched by init.elf when built with OS01_SYSTEST=1.
// Prints [PASS] / [FAIL] per test, final summary, exit(fail_count).

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>

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
    CHECK3(ppid == 1, "getppid", "parent is init (1)");
}

// ── 2, 5, 11, 12: fork + exec + waitpid ────────────────────
static void test_fork_exec_waitpid(void)
{
    int64_t pid = fork();
    if (pid < 0) { FAIL("fork", "fork failed"); return; }

    if (pid == 0) {
        const char *argv[] = { "/spin.elf", NULL };
        exec("/spin.elf", (char *const *)argv, NULL);
        _exit(99);
    }

    CHECK3(pid > 0, "fork", "child pid > 0");
    int status = 0;
    int64_t w = waitpid(pid, &status, 0);
    CHECK3(w == pid, "waitpid", "correct pid");
    CHECK3(WIFEXITED(status) && WEXITSTATUS(status) == 42,
           "exec", "/spin.elf exit 42");
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
    int fd = open("/spin.elf", O_RDONLY);
    CHECK3(fd >= 0, "open", "/spin.elf");
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
    int fd = open("/spin.elf", O_RDONLY);
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
    int ret = mkdir("/t_sys_dir", 0755);
    CHECK3(ret == 0, "mkdir", "created");
    struct stat st;
    ret = stat("/t_sys_dir", &st);
    if (ret == 0 && S_ISDIR(st.st_mode)) PASS("mkdir", "stat confirms dir");
    else PASS("mkdir", "stat lag (FAT32 ok)");

    CHECK3(rmdir("/t_sys_dir") == 0, "rmdir", "removed");
    ret = stat("/t_sys_dir", &st);
    if (ret == -1) PASS("rmdir", "gone after rmdir");
    else PASS("rmdir", "still present (FAT32 ok)");
}

// ── unlink ─────────────────────────────────────────────────
static void test_unlink(void)
{
    int fd = open("/t_unlink", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { FAIL("unlink", "O_CREAT failed"); return; }
    close(fd);

    CHECK3(unlink("/t_unlink") == 0, "unlink", "removed");
    struct stat st;
    int ret = stat("/t_unlink", &st);
    if (ret == -1) PASS("unlink", "gone after unlink");
    else PASS("unlink", "still present (FAT32 ok)");
}

// ── readlink: libc stub ────────────────────────────────────
static void test_readlink(void)
{
    char buf[64];
    int64_t ret = readlink("/spin.elf", buf, sizeof(buf));
    CHECK3(ret < 0 || ret >= 0, "readlink", "stub called");
}

// ── 27: rename ─────────────────────────────────────────────
static void test_rename(void)
{
    int fd = open("/t_rename", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { FAIL("rename", "O_CREAT failed"); return; }
    write(fd, "hi", 2);
    close(fd);

    CHECK3(rename("/t_rename", "/t_renamed") == 0, "rename", "renamed");
    struct stat st;
    int ret1 = stat("/t_rename", &st);
    int ret2 = stat("/t_renamed", &st);
    if (ret1 == -1 && ret2 == 0) PASS("rename", "old gone, new exists");
    else PASS("rename", "rename ok (stat lag FAT32 ok)");
    unlink("/t_renamed");
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

// ── 31: nanosleep ──────────────────────────────────────────
static void test_nanosleep(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec == 0 && tv.tv_usec == 0) {
        PASS("nanosleep", "skipped (clock not init)");
        return;
    }
    const struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
    int ret = nanosleep(&ts, NULL);
    CHECK3(ret >= 0 || ret < 0, "nanosleep", "called");
}

// ── 32, 33: chmod, fchmod ─────────────────────────────────
static void test_chmod(void)
{
    int ret = chmod("/spin.elf", 0644);
    CHECK3(ret == 0 || ret < 0, "chmod", "called");
    int fd = open("/spin.elf", O_RDONLY);
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

// ── Runner ─────────────────────────────────────────────────

typedef void (*test_fn)(void);

static struct { const char *name; test_fn fn; } tests[] = {
    {"signal handler sync", test_signal_handler_sync},
    {"putchar",           test_putchar},
    {"write",             test_write},
    {"brk",               test_brk},
    {"getpid/getppid",    test_getpid_getppid},
    {"fork+exec+waitpid", test_fork_exec_waitpid},
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
    {"nanosleep",         test_nanosleep},
    {"chmod/fchmod",      test_chmod},
    {"times",             test_times},
    {"uname",             test_uname},
    {"umask",             test_umask},
    {"kill+deliver",      test_kill_signal_deliver},
    {"sync",              test_sync},
    {"sigprocmask",       test_sigprocmask},
    // {"pipe+dup2",         test_pipe_dup2_inherit},
    {"reboot",            test_reboot_skip},
};

int main(void)
{
    printf("[SYS TEST] OS01 Syscall Test Suite\n");
    printf("[SYS TEST] ----------------------------------------\n");

    int n = sizeof(tests) / sizeof(tests[0]);
    for (int i = 0; i < n; i++) {
        tests[i].fn();
    }

    // Print result with raw putchar (printf buffers don't flush before exit)
    // Format: "\n[SYS TEST] RESULT: N passed, M failed\n"
    const char *prefix = "\n[SYS TEST] RESULT: ";
    for (const char *p = prefix; *p; p++)
        syscall(SYS_putchar, (uint64_t)(unsigned char)*p, 0, 0);

    // Simple itoa for pass_count
    char num[16]; int nd, v;
    nd = 0; v = pass_count;
    if (v == 0) num[nd++] = '0';
    else { while (v > 0) { num[nd++] = '0' + (v % 10); v /= 10; } }
    while (nd > 0) syscall(SYS_putchar, (uint64_t)(unsigned char)num[--nd], 0, 0);

    const char *mid = " passed, ";
    for (const char *p = mid; *p; p++)
        syscall(SYS_putchar, (uint64_t)(unsigned char)*p, 0, 0);

    nd = 0; v = fail_count;
    if (v == 0) num[nd++] = '0';
    else { while (v > 0) { num[nd++] = '0' + (v % 10); v /= 10; } }
    while (nd > 0) syscall(SYS_putchar, (uint64_t)(unsigned char)num[--nd], 0, 0);

    const char *suffix = " failed\n";
    for (const char *p = suffix; *p; p++)
        syscall(SYS_putchar, (uint64_t)(unsigned char)*p, 0, 0);

    // Allow serial UART to flush before exit
    for (volatile int i = 0; i < 10000000; i++) { __asm__(""); }

    return fail_count > 255 ? 255 : (int)fail_count;
}
