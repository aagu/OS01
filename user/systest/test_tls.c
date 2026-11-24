// ── x86_64 PT_TLS / arch_prctl acceptance ────────────────────────────────
//
// Task 1 of the PT_TLS plan: the raw arch_prctl entry, the FS base switch
// and fork inheritance.  The Linux-numbered entry (158) belongs to the
// GNU-ABI-note probe in 54_linux_abi_compat; this native-ABI image only
// reaches the native number.

#include "systest.h"

#define TLS_NR_NATIVE  SYS_arch_prctl

#define ARCH_SET_FS   0x1002
#define ARCH_GET_FS   0x1003
#define ARCH_SET_GS   0x1001
#define ARCH_GET_GS   0x1004

#define USER_ADDR_LIMIT 0x00007FFFFFFFFFFFULL
#define USER_ADDR_OOB   0x0000800000000000ULL
#define USER_ADDR_HIGH  0xFFFF800000000000ULL
#define OP_HIGH_BITS    0x100001002ULL

static int64_t raw_arch_prctl(uint64_t nr, uint64_t op, uint64_t arg)
{
    return syscall2(nr, op, arg);
}

static uint64_t fs_read_u64(void)
{
    uint64_t v;
    __asm__ __volatile__("movq %%fs:0, %0" : "=r"(v));
    return v;
}

static void fs_write_u64(uint64_t v)
{
    __asm__ __volatile__("movq %0, %%fs:0" :: "r"(v) : "memory");
}

static uint64_t get_fs_or(uint64_t nr, int64_t *rc_out)
{
    uint64_t v = 0;
    int64_t rc = raw_arch_prctl(nr, ARCH_GET_FS, (uint64_t)(uintptr_t)&v);
    if (rc_out)
        *rc_out = rc;
    return v;
}

/* Bounded blocking read/write handshake helpers: a wedged peer turns into
 * a FAIL instead of a hung suite. */
static int wait_readable(int fd, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    return poll(&pfd, 1, timeout_ms);
}

static int handshake_recv(int fd)
{
    if (wait_readable(fd, 5000) != 1)
        return -1;
    unsigned char ch = 0;
    if (read(fd, &ch, 1) != 1)
        return -1;
    return (int)ch;
}

static int handshake_send(int fd, unsigned char ch)
{
    return write(fd, &ch, 1) == 1 ? 0 : -1;
}

static void test_fs_fault_targets(void *mapping)
{
    unsigned char *pages = (unsigned char *)mapping;
    int64_t rc;

    /* A NULL destination never receives the base. */
    rc = raw_arch_prctl(TLS_NR_NATIVE, ARCH_GET_FS, 0);
    CHECK3(rc == -EFAULT, "arch_prctl GET NULL", "returns -EFAULT");

    /* A read-only page is not a writable GET target. */
    if (mprotect(pages + 0x1000, 0x1000, PROT_READ) != 0) {
        FAIL("arch_prctl GET read-only setup", "mprotect failed");
        return;
    }
    rc = raw_arch_prctl(TLS_NR_NATIVE, ARCH_GET_FS,
                        (uint64_t)(uintptr_t)(pages + 0x1000));
    CHECK3(rc == -EFAULT, "arch_prctl GET read-only page", "returns -EFAULT");

    /* The 8-byte write straddling a page boundary needs BOTH pages
     * writable; the second page is read-only here. */
    rc = raw_arch_prctl(TLS_NR_NATIVE, ARCH_GET_FS,
                        (uint64_t)(uintptr_t)(pages + 0x1000 - 4));
    CHECK3(rc == -EFAULT, "arch_prctl GET cross-page RO", "returns -EFAULT");

    if (mprotect(pages + 0x1000, 0x1000, PROT_READ | PROT_WRITE) != 0)
        FAIL("arch_prctl GET cross-page restore", "mprotect failed");
}

static void test_fork_inheritance(void *mapping)
{
    unsigned char *pages = (unsigned char *)mapping;
    uint64_t *word0 = (uint64_t *)pages;
    uint64_t *word1 = (uint64_t *)(pages + 0x1000);
    const uint64_t parent_sentinel = 0x1111222233334444ULL;
    const uint64_t child_sentinel  = 0xAAAABBBBCCCCDDDDULL;

    *word0 = parent_sentinel;
    *word1 = parent_sentinel;

    int p2c[2], c2p[2];
    if (pipe(p2c) != 0 || pipe(c2p) != 0) {
        FAIL("arch_prctl fork pipe", "pipe failed");
        return;
    }

    int64_t rc = raw_arch_prctl(TLS_NR_NATIVE, ARCH_SET_FS,
                                (uint64_t)(uintptr_t)pages);
    if (rc != 0) {
        FAIL("arch_prctl fork parent SET", "SET_FS failed");
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(p2c[1]);
        close(c2p[0]);

        int64_t rc_child;
        uint64_t inherited = get_fs_or(TLS_NR_NATIVE, &rc_child);
        int inherited_ok = rc_child == 0 && inherited == (uint64_t)(uintptr_t)pages;
        int read_ok = inherited_ok && fs_read_u64() == parent_sentinel;

        /* Block until the parent has observed the inherited state. */
        int gate = handshake_recv(p2c[0]);
        /* POST-fork the child moves its OWN FS base and writes through
         * it; the parent's bytes must stay untouched. */
        int set_ok = raw_arch_prctl(TLS_NR_NATIVE, ARCH_SET_FS,
                                    (uint64_t)(uintptr_t)(pages + 0x1000)) == 0;
        uint64_t child_base = get_fs_or(TLS_NR_NATIVE, &rc_child);
        int base_ok = rc_child == 0 &&
                      child_base == (uint64_t)(uintptr_t)(pages + 0x1000);
        fs_write_u64(child_sentinel);
        int write_ok = *word1 == child_sentinel;

        /* GET the 8-byte value across a page boundary (both pages are
         * private and writable). */
        uint64_t *cross = (uint64_t *)(pages + 0x1000 - 4);
        int cross_ok = raw_arch_prctl(TLS_NR_NATIVE, ARCH_GET_FS,
                                      (uint64_t)(uintptr_t)cross) == 0;
        uint64_t cross_val = 0;
        memcpy(&cross_val, cross, sizeof(cross_val));
        cross_ok = cross_ok && cross_val == child_base;

        int report = (inherited_ok && read_ok && gate == '1' && set_ok &&
                      base_ok && write_ok && cross_ok) ? 0 : 1;
        if (handshake_send(c2p[1], (unsigned char)(report ? 'F' : 'O')) != 0)
            report = 1;
        if (handshake_recv(p2c[0]) != '3')
            report = 1;
        _exit(report);
    }
    if (pid < 0) {
        FAIL("arch_prctl fork", "fork failed");
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        return;
    }

    close(p2c[0]);
    close(c2p[1]);

    /* The parent keeps running while the child is blocked on the pipe:
     * both directions are genuinely scheduled, not raced past. */
    int64_t rc_p;
    uint64_t parent_base = get_fs_or(TLS_NR_NATIVE, &rc_p);
    CHECK3(rc_p == 0 && parent_base == (uint64_t)(uintptr_t)pages,
           "arch_prctl fork parent base", "inherited base is parent's");

    int child_report = handshake_send(p2c[1], '1') == 0
                           ? handshake_recv(c2p[0]) : -1;
    CHECK3(child_report == 'O',
           "arch_prctl fork child base/write/cross GET",
           "child inherits, re-SETs and writes through %fs");

    /* Child has written through its own FS base; COW must keep the
     * parent's bytes and base untouched. */
    uint64_t parent_base2 = get_fs_or(TLS_NR_NATIVE, &rc_p);
    CHECK3(rc_p == 0 && parent_base2 == (uint64_t)(uintptr_t)pages &&
               *word0 == parent_sentinel && *word1 == parent_sentinel,
           "arch_prctl fork isolation", "parent bytes/base unchanged");

    /* Parent re-SETs to the second page and proves its own view is
     * positive and isolated. */
    CHECK3(raw_arch_prctl(TLS_NR_NATIVE, ARCH_SET_FS,
                          (uint64_t)(uintptr_t)(pages + 0x1000)) == 0 &&
               fs_read_u64() == parent_sentinel,
           "arch_prctl parent own SET", "second page reads parent value");

    handshake_send(p2c[1], '3');
    int st = -1;
    int64_t waited = waitpid(pid, &st, 0);
    int exit_code = (st >= 0 && (st & 0x7f) == 0) ? ((st >> 8) & 0xff) : -1;
    CHECK3(waited == pid && exit_code == 0, "arch_prctl fork child exit",
           "child exits 0");

    close(p2c[1]);
    close(c2p[0]);

    /* Restore the starting base (0) for the rest of this process. */
    raw_arch_prctl(TLS_NR_NATIVE, ARCH_SET_FS, 0);
}

void test_arch_prctl_fs(void)
{
    unsigned char *pages = (unsigned char *)mmap(NULL, 0x2000,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANONYMOUS,
                                                 -1, 0);
    if (pages == MAP_FAILED) {
        FAIL("arch_prctl fs mapping", "mmap failed");
        return;
    }
    PASS("arch_prctl fs mapping", "mmap succeeded");

    /* Save the initial value: the systest image carries no PT_TLS, so the
     * kernel installed base 0. */
    int64_t rc;
    uint64_t saved = get_fs_or(TLS_NR_NATIVE, &rc);
    CHECK3(rc == 0 && saved == 0, "arch_prctl GET initial",
           "x86 FS base starts at 0");
    /* Without the entry there is no safe %fs base to read from; report the
     * one failure and leave the rest of the suite alone. */
    if (rc != 0) {
        munmap(pages, 0x2000);
        return;
    }

    /* Two distinct mapped user addresses, two distinct sentinels. */
    uint64_t *test0 = (uint64_t *)pages;
    uint64_t *test1 = (uint64_t *)(pages + 0x1000);
    *test0 = 0x1122334455667788ULL;
    *test1 = 0x8877665544332211ULL;

    rc = raw_arch_prctl(TLS_NR_NATIVE, ARCH_SET_FS,
                        (uint64_t)(uintptr_t)pages);
    CHECK3(rc == 0, "arch_prctl SET first base", "SET_FS succeeds");
    if (rc != 0) {
        munmap(pages, 0x2000);
        return;
    }
    uint64_t got = get_fs_or(TLS_NR_NATIVE, &rc);
    CHECK3(rc == 0 && got == (uint64_t)(uintptr_t)pages,
           "arch_prctl GET first base", "reads back first base");
    CHECK3(fs_read_u64() == 0x1122334455667788ULL,
           "arch_prctl %fs read first", "test word reached through FS");

    rc = raw_arch_prctl(TLS_NR_NATIVE, ARCH_SET_FS,
                        (uint64_t)(uintptr_t)(pages + 0x1000));
    CHECK3(rc == 0, "arch_prctl SET second base", "SET_FS succeeds");
    if (rc != 0) {
        munmap(pages, 0x2000);
        return;
    }
    got = get_fs_or(TLS_NR_NATIVE, &rc);
    CHECK3(rc == 0 && got == (uint64_t)(uintptr_t)(pages + 0x1000),
           "arch_prctl GET second base", "reads back second base");
    CHECK3(fs_read_u64() == 0x8877665544332211ULL,
           "arch_prctl %fs read second", "second test word reached");

    /* SET 0 clears the base. */
    CHECK3(raw_arch_prctl(TLS_NR_NATIVE, ARCH_SET_FS, 0) == 0,
           "arch_prctl SET zero", "clears FS base");
    got = get_fs_or(TLS_NR_NATIVE, &rc);
    CHECK3(rc == 0 && got == 0, "arch_prctl GET zero", "base is 0");

    /* Inclusive upper bound is accepted even though unmapped. */
    CHECK3(raw_arch_prctl(TLS_NR_NATIVE, ARCH_SET_FS, USER_ADDR_LIMIT) == 0,
           "arch_prctl SET upper bound", "accepts 0x7FFFFFFFFFFF");
    got = get_fs_or(TLS_NR_NATIVE, &rc);
    CHECK3(rc == 0 && got == USER_ADDR_LIMIT,
           "arch_prctl GET upper bound", "reads back upper bound");

    /* Every rejected value leaves the previous base in place. */
    CHECK3(raw_arch_prctl(TLS_NR_NATIVE, ARCH_SET_FS, USER_ADDR_OOB) == -EPERM,
           "arch_prctl SET above limit", "returns -EPERM");
    got = get_fs_or(TLS_NR_NATIVE, &rc);
    CHECK3(rc == 0 && got == USER_ADDR_LIMIT,
           "arch_prctl GET after -EPERM", "base unchanged");

    CHECK3(raw_arch_prctl(TLS_NR_NATIVE, ARCH_SET_FS, USER_ADDR_HIGH) == -EPERM,
           "arch_prctl SET high half", "returns -EPERM");
    got = get_fs_or(TLS_NR_NATIVE, &rc);
    CHECK3(rc == 0 && got == USER_ADDR_LIMIT,
           "arch_prctl GET after high half", "base unchanged");

    uint64_t bad_ops[] = { ARCH_SET_GS, ARCH_GET_GS, OP_HIGH_BITS, 0x9999 };
    for (unsigned i = 0; i < sizeof(bad_ops) / sizeof(bad_ops[0]); i++) {
        CHECK3(raw_arch_prctl(TLS_NR_NATIVE, bad_ops[i], 0x1234) == -EINVAL,
               "arch_prctl unknown opcode", "returns -EINVAL");
        got = get_fs_or(TLS_NR_NATIVE, &rc);
        CHECK3(rc == 0 && got == USER_ADDR_LIMIT,
               "arch_prctl GET after -EINVAL", "base unchanged");
    }

    /* GET fault targets (NULL / read-only / cross-page RO). */
    test_fs_fault_targets(pages);

    /* Linux number 158 is only reachable from a GNU-ABI-note image;
     * this native-ABI systest binary never routes through the compat
     * table.  The 54_linux_abi_compat probe drives 158 end to end. */

    /* Fork: inherit the live base, then isolate both sides. */
    test_fork_inheritance(pages);

    /* Leave the process exactly as it started. */
    CHECK3(raw_arch_prctl(TLS_NR_NATIVE, ARCH_SET_FS, saved) == 0 &&
               get_fs_or(TLS_NR_NATIVE, &rc) == saved && rc == 0,
           "arch_prctl restore", "original base restored");

    munmap(pages, 0x2000);
}

/* ── PT_TLS loader / spawn / exec acceptance ──────────────────── */

static int run_user_image(const char *path)
{
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)path, NULL };
        exec(path, argv, NULL);
        _exit(127);
    }
    if (pid < 0)
        return -1;
    int st = -1;
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    if ((st & 0x7f) != 0)
        return -2;                       /* killed by a signal */
    return (st >> 8) & 0xff;
}

void test_pt_tls_exec(void)
{
    /* ET_EXEC chain: initial template, manual FS, fork isolation,
     * failed-exec preservation, TLS->TLS re-exec, TLS->plain clear. */
    int rc = run_user_image("/bin/ptls_exec");
    CHECKF(rc == 0, "pt_tls_exec ET_EXEC lifecycle",
           "template/fork/failed-exec/re-exec/plain chain (rc=%d)",
           "template/fork/failed-exec/re-exec/plain chain (rc=%d)", rc);

    /* Static PIE: .tdata initialised + .tbss zero, local-exec access. */
    rc = run_user_image("/bin/ptls_pie");
    CHECKF(rc == 0, "pt_tls_exec static PIE template",
           "tdata+tbss local-exec (rc=%d)",
           "tdata+tbss local-exec (rc=%d)", rc);

    /* A missing image is rejected by exec (child reports 127). */
    rc = run_user_image("/bin/ptls_missing");
    CHECK3(rc == 127, "pt_tls_exec exec rejection",
           "missing image exits 127");
}
