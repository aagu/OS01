#include "systest.h"


// ── 3: brk ─────────────────────────────────────────────────
void test_brk(void)
{
    int64_t cur = syscall(SYS_brk, 0, 0, 0);
    CHECK3(cur > 0x400000 && (uint64_t)cur < 0xFFFF800000000000ULL, "brk", "query current");
    int64_t cur2 = syscall(SYS_brk, 0, 0, 0);
    CHECK3(cur == cur2, "brk", "idempotent query");
}


// ── 49: brk → read into freshly-committed page (Task 4) ────
// Brief step 4 (user/systest): grow break by one 4 KiB page, then
// pass that page directly as the destination buffer of a read()
// syscall into a file with known content.  Validates the spec §5.2
// contract: "newly committed page is immediately usable as a
// kernel-write target".  Under the OLD brk semantics, the page
// is unmapped until first touch (demand-mapped by the read's
// PF path).  Under the NEW brk semantics (Task 4), the page is
// pre-mapped before the new end_brk is published.
//
// Either way, the read should return the expected bytes — this
// case is a regression test for the new contract, not a strict
// RED mechanism.  The strict RED for the spec contract lives
// in hosttests/cases/test_brk_pages.c (production-linked against
// the real vma.c + observable PTE state).
void test_brk_read_fresh_page(void)
{
    /* Step 1: create a file with known content on a writable FS. */
    const char *path = "/tmp/t_brk_read";
    const char *want = "OS01_BRKBUF_PAYLOAD_2026";   /* 25 bytes */
    int n_want = 0;
    for (int i = 0; want[i]; i++) n_want++;

    unlink(path);
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { FAIL("brk_read_fresh_page", "create file failed"); return; }
    int64_t wn = write(fd, want, n_want);
    int werr = (wn < 0) ? errno : 0;
    close(fd);
    if (wn != n_want) {
        FAIL("brk_read_fresh_page", "write file failed rc=%ld errno=%d",
             (long)wn, werr);
        unlink(path);
        return;
    }

    /* Step 2: grow break by exactly one 4 KiB page. */
    int64_t cur = syscall(SYS_brk, 0, 0, 0);
    if (cur <= 0) {
        FAIL("brk_read_fresh_page", "brk query failed rc=%ld", (long)cur);
        unlink(path);
        return;
    }
    uint64_t base = ((uint64_t)cur + 0xFFF) & ~(uint64_t)0xFFF;
    int64_t newbrk = syscall(SYS_brk, base + 0x1000, 0, 0);
    if (newbrk < (int64_t)(base + 0x1000)) {
        FAIL("brk_read_fresh_page", "brk grow failed rc=%ld", (long)newbrk);
        unlink(path);
        return;
    }

    /* Step 3: read the file into the freshly-committed heap page.
     * Do NOT touch the page first (the test is only valid if the
     * page is committed by brk alone, not by a pre-read mapping). */
    char *buf = (char *)(uintptr_t)base;
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        FAIL("brk_read_fresh_page", "re-open file failed errno=%d", errno);
        unlink(path);
        return;
    }
    int64_t rn = read(fd, buf, n_want);
    int rerr = (rn < 0) ? errno : 0;
    close(fd);
    unlink(path);

    CHECK3(rn == n_want, "brk_read_fresh_page",
           "read returned n_want bytes");
    if (rn == n_want) {
        CHECK3(memcmp(buf, want, n_want) == 0, "brk_read_fresh_page",
               "freshly-committed page holds file content");
    } else {
        FAIL("brk_read_fresh_page", "read failed rc=%ld errno=%d",
             (long)rn, rerr);
    }
}


// ── 43-53: 用户态栈 canary / AT_RANDOM（spec 2026-09-17-user-stack-canary）──

/* hex nibble（canary_dump 的 "GUARD %016lx" 是严格的定宽十六进制字段，
 * 用手写解析可以同时检查"恰好 16 位"这个格式约束） */
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}


/* fork → dup2(pipe写端,1) → exec(path) → 父读全部输出 → waitpid。
 * 返回读到的字节数（<0 失败）；*out_status 收原始 waitpid status。 */
static int exec_capture(const char *path, char *out, size_t outsz, int *out_status)
{
    int fds[2];
    if (pipe(fds) < 0) return -1;
    int64_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        char *argv[] = { (char *)path, NULL };
        exec(path, argv, NULL);
        _exit(126);                          /* exec 返回即失败 */
    }
    close(fds[1]);
    size_t n = 0;
    while (n + 1 < outsz) {
        int64_t r = read(fds[0], out + n, outsz - 1 - n);
        if (r <= 0) break;                   /* EOF 或 -1 都终止 */
        n += (size_t)r;
    }
    out[n] = '\0';
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (out_status) *out_status = st;
    return (int)n;
}


/* systest.c 自身被 -fstack-protector-strong 编译后的内联溢出帧 */
__attribute__((noinline))
static void ssp_smash_frame(void)
{
    volatile char buf[16];
    for (int i = 0; i < 64; i++)
        buf[i] = 'S';
}

void test_ssp_trip_sigabrt_exec(void)
{
    int64_t pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)"/bin/canary_smash", NULL };
        exec("/bin/canary_smash", argv, NULL);
        _exit(126);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECKF(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT,
           "45_ssp_trip_sigabrt_exec", "status=%#x",
           "want WIFSIGNALED&&SIGABRT(6), status=%#x", st);
}

void test_ssp_trip_sigabrt_fork(void)
{
    int64_t pid = fork();
    if (pid == 0) {
        /* fork 不经 exec：guard 继承自父（spec R9 已知弱点），
         * 但 trip 检测必须仍然工作。 */
        ssp_smash_frame();
        _exit(0);                            /* SSP 下不可达 */
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECKF(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT,
           "46_ssp_trip_sigabrt_fork", "status=%#x",
           "want WIFSIGNALED&&SIGABRT(6), status=%#x", st);
}

void test_ssp_no_false_trip(void)
{
    int64_t pid = fork();
    if (pid == 0) {
        volatile char buf[64];               /* SSP 覆盖的正常局部数组 */
        int sum = 0;
        for (int i = 0; i < 64; i++) { buf[i] = (char)i; sum += buf[i]; }
        _exit(42 + (sum & 0));               /* 42，且 buf 不可被优化掉 */
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECKF(WIFEXITED(st) && WEXITSTATUS(st) == 42,
           "47_ssp_no_false_trip", "status=%#x",
           "want WIFEXITED&&42, status=%#x", st);
}


/* ── 54b/54c/54d/54e: 用户态异常路径 waitpid 信号终止语义 ─────────
 * 四种 user-mode fault 各应被父 waitpid 识别为 WIFSIGNALED && 对应 WTERMSIG：
 *   54b. #DE（除零） → SIGFPE (8)
 *   54c. #PF（写只读页） → SIGSEGV (11)
 *   54d. #UD（ud2） → SIGILL (4)
 *   54e. #GP（用户态执行 cli） → SIGSEGV (11)
 * 若 fix 未生效，child 会被 waitpid 看到 status=0（WIFEXITED 假象），
 * 整套测试 FAIL，root cause 是 kernel/arch/x86_64/intr/trap.c 之前把
 * do_exit(0) 硬编码成退出码（exit_code 高字节），shell/init/busybox
 * wait/trap 全部失真（roadmap parked 项，2026-10-11）。
 * 54e 同时是 intr-dispatch phase 1 的用户 #GP 回归：#GP 现在经过
 * do_general_protection 薄入口 → arch_intr_dispatch() → x86 后端。
 */
static void __attribute__((noinline)) trap_divzero(void)
{
    volatile int a = 1, b = 0;
    volatile int c = a / b;                  /* #DE */
    (void)c;
    _exit(123);                              /* 不可达 */
}

static void __attribute__((noinline)) trap_write_ro(void)
{
    /* Keep a large lazy RO VMA: exit must walk its absent leaves. This
     * exposes timer preemption during teardown without consuming 128 MiB
     * of physical memory. Only the first page is demand-faulted. */
    void *p = mmap(NULL, 128UL * 1024 * 1024, PROT_READ,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == (void *)-1) _exit(99);
    *(volatile int *)p = 42;                 /* #PF, write to RO → SIGSEGV */
    _exit(124);                              /* 不可达 */
}

static void __attribute__((noinline)) trap_ud2(void)
{
    /* ud2 = Invalid Opcode (#UD) → SIGILL。noinline 防止编译器把 ud2
     * 优化掉（gcc/clang 都不会，但显式标注意图）。 */
    __asm__ __volatile__("ud2");
    _exit(125);                              /* 不可达 */
}

static void __attribute__((noinline)) trap_gp_cli(void)
{
    /* cli 是特权指令：CPL=3 执行触发 #GP(0) → SIGSEGV。volatile 防
     * 止编译器把这条 asm 当作副作用自由的指令删掉。 */
    __asm__ __volatile__("cli");
    _exit(126);                              /* 不可达 */
}

void test_user_fault_divzero(void)
{
    int64_t pid = fork();
    if (pid < 0) { FAIL("fork_divzero", "fork failed"); return; }
    if (pid == 0) trap_divzero();
    int st = 0;
    waitpid(pid, &st, 0);
    CHECKF(WIFSIGNALED(st) && WTERMSIG(st) == SIGFPE,
           "54b_fault_divzero", "status=%#x",
           "want WIFSIGNALED&&SIGFPE(8), status=%#x", st);
}

void test_user_fault_write_ro(void)
{
    // Repeat to cover teardown straddling a timer tick on both UP and SMP.
    for (int attempt = 0; attempt < 8; ++attempt) {
        int64_t pid = fork();
        if (pid < 0) { FAIL("fork_wp", "fork failed"); return; }
        if (pid == 0) trap_write_ro();
        int st = 0;
        int64_t waited = waitpid(pid, &st, 0);
        CHECKF(waited == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV,
               "54c_fault_write_ro", "pid=%ld waited=%ld errno=%d status=%#x",
               "want waited==pid && WIFSIGNALED&&SIGSEGV(11), pid=%ld waited=%ld errno=%d status=%#x",
               (long)pid, (long)waited, waited < 0 ? errno : 0, st);
    }
}

void test_user_fault_ud2(void)
{
    int64_t pid = fork();
    if (pid < 0) { FAIL("fork_ud2", "fork failed"); return; }
    if (pid == 0) trap_ud2();
    int st = 0;
    waitpid(pid, &st, 0);
    CHECKF(WIFSIGNALED(st) && WTERMSIG(st) == SIGILL,
           "54d_fault_ud2", "status=%#x",
           "want WIFSIGNALED&&SIGILL(4), status=%#x", st);
}

void test_user_fault_gp(void)
{
    int64_t pid = fork();
    if (pid < 0) { FAIL("fork_gp", "fork failed"); return; }
    if (pid == 0) trap_gp_cli();
    int st = 0;
    int64_t waited = waitpid(pid, &st, 0);
    CHECKF(waited == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV,
           "54e_fault_gp", "pid=%ld waited=%ld status=%#x",
           "want waited==pid && WIFSIGNALED&&SIGSEGV(11), pid=%ld waited=%ld status=%#x",
           (long)pid, (long)waited, st);
    /* 父进程继续执行：再跑一次受控 fork/wait 往返，证明 #GP 终止
     * 子任务后调度与 waitpid 依然可用。 */
    int64_t pid2 = fork();
    if (pid2 < 0) { FAIL("fork_gp_after", "fork failed"); return; }
    if (pid2 == 0) _exit(7);
    int st2 = 0;
    int64_t waited2 = waitpid(pid2, &st2, 0);
    CHECKF(waited2 == pid2 && WIFEXITED(st2) && WEXITSTATUS(st2) == 7,
           "54e_parent_continues", "pid=%ld waited=%ld status=%#x",
           "want waited==pid && WIFEXITED&&7, pid=%ld waited=%ld status=%#x",
           (long)pid2, (long)waited2, st2);
}


/* 扫描 "TAG <16hex>[\n|\0]"（canary_dump 的输出格式 "GUARD %016lx\n"）。
 * 返回 0 OK，-1 格式不符——不修改 *out。 */
static int parse_hex16_line(const char *buf, const char *tag, uint64_t *out)
{
    size_t taglen = 0;
    while (tag[taglen]) taglen++;
    if (strncmp(buf, tag, taglen) != 0) return -1;
    const char *p = buf + taglen;
    if (*p != ' ') return -1;
    p++;
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        int h = hexval((unsigned char)p[i]);
        if (h < 0) return -1;
        v = (v << 4) | (uint64_t)h;
    }
    if (p[16] != '\n' && p[16] != '\0') return -1;
    *out = v;
    return 0;
}

void test_ssp_guard_seeded(void)
{
    CHECKF(__stack_chk_guard != 0, "43_ssp_guard_seeded",
           "guard=%016lx", "guard must be nonzero, got %016lx",
           (unsigned long)__stack_chk_guard);
}

void test_ssp_guard_distinct_across_exec(void)
{
    uint64_t seen[10];
    int dup = 0;
    for (int i = 0; i < 10; i++) {
        char buf[64];
        int st = 0;
        int n = exec_capture("/bin/canary_dump", buf, sizeof(buf), &st);
        uint64_t g = 0;
        if (n <= 0 || parse_hex16_line(buf, "GUARD", &g) != 0
                || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            CHECKF(0, "44_ssp_guard_distinct_across_exec",
                   "run %d ok", "run %d: bad canary_dump output/status", i);
            return;
        }
        for (int j = 0; j < i; j++)
            if (seen[j] == g) dup++;
        seen[i] = g;
    }
    CHECKF(dup == 0, "44_ssp_guard_distinct_across_exec",
           "dups=0", "10 execs, duplicate guard values (dups=%d)", dup);
}


// ── 50: protected ranges (Task 5, heap/ELF isolation plan) ─



// The kernel reserves [0x400000, 0x20600000) for the ELF image
// (envelope incl. segment gaps), the heap (committed + reserve),
// the 0x203ff000 guard page and the 2 MiB user stack.  No mapping
// API may hand out or mutate any part of that window:
//   * MAP_FIXED into the envelope / heap reserve / guard → -EINVAL,
//     and a preexisting mapping must survive untouched;
//   * munmap / mprotect on the guard → -EINVAL;
//   * plain mmap must return an address outside the window;
//   * after all the rejections the heap VMA (brk still grows) and
//     the ELF envelope (/proc/self/maps still lists the image) are
//     intact.
void test_protected_ranges(void)
{
    /* 1. A preexisting mapping that must survive every rejection. */
    void *keep = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (keep == MAP_FAILED) {
        FAIL("prot_ranges", "setup mmap failed");
        return;
    }
    volatile unsigned char *kp = (volatile unsigned char *)keep;
    kp[0] = 0x5A;
    kp[0x1fff] = 0xA5;

    /* 2. MAP_FIXED into the guard page, partly reaching into the
     * heap reserve and the stack — rejected, existing mapping
     * untouched. */
    errno = 0;
    void *bad = mmap((void *)0x203fe000UL, 0x2000,
                     PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    int ok_guard = (bad == MAP_FAILED) && (errno == EINVAL);

    /* 3. MAP_FIXED into the heap reserve (uncommitted part). */
    errno = 0;
    bad = mmap((void *)0x500000UL, 0x1000, PROT_READ | PROT_WRITE,
               MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    int ok_heap = (bad == MAP_FAILED) && (errno == EINVAL);

    /* 4. MAP_FIXED into the ELF envelope (image base page). */
    errno = 0;
    bad = mmap((void *)0x400000UL, 0x1000, PROT_READ | PROT_WRITE,
               MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    int ok_elf = (bad == MAP_FAILED) && (errno == EINVAL);

    /* 5. The preexisting mapping kept its bytes and is still
     * writable (no munmap, no PTE change happened). */
    int ok_keep = (kp[0] == 0x5A) && (kp[0x1fff] == 0xA5);
    kp[0] = 0x33;
    ok_keep = ok_keep && (kp[0] == 0x33);
    kp[0] = 0x5A;

    CHECK3(ok_guard, "map_fixed_guard_rejected",
           "MAP_FIXED [203fe000,20400000) → -EINVAL");
    CHECK3(ok_heap, "map_fixed_heap_rejected",
           "MAP_FIXED into heap reserve → -EINVAL");
    CHECK3(ok_elf, "map_fixed_elf_rejected",
           "MAP_FIXED into ELF envelope → -EINVAL");
    CHECK3(ok_keep, "map_fixed_reject_keeps_mapping",
           "preexisting mapping intact after rejects");

    /* 6. munmap / mprotect on the guard page rejected. */
    errno = 0;
    int r = munmap((void *)0x203ff000UL, 0x1000);
    int ok_mun = (r == -1) && (errno == EINVAL);
    errno = 0;
    r = mprotect((void *)0x203ff000UL, 0x1000, PROT_READ);
    int ok_mprot = (r == -1) && (errno == EINVAL);
    CHECK3(ok_mun, "munmap_guard_rejected",
           "munmap guard page → -EINVAL");
    CHECK3(ok_mprot, "mprotect_guard_rejected",
           "mprotect guard page → -EINVAL");

    /* 7. Plain mmap (no hint) must stay outside the whole
     * reserved window [0x400000, 0x20600000). */
    void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    int ok_auto = (p != MAP_FAILED) &&
                  ((uint64_t)p >= 0x20600000UL) &&
                  ((uint64_t)p < 0xffff800000000000UL);
    CHECK3(ok_auto, "auto_mmap_outside_reserve",
           ok_auto ? "regular mmap landed above the reserve"
                   : "regular mmap inside reserve or failed");

    /* 8. Heap VMA still intact: brk grows, the new page is
     * writable, and it shrinks back. */
    int64_t cur = syscall(SYS_brk, 0, 0, 0);
    uint64_t base = ((uint64_t)cur + 0xFFF) & ~(uint64_t)0xFFF;
    int64_t grown = syscall(SYS_brk, base + 0x1000, 0, 0);
    int ok_heapvma = (grown == (int64_t)(base + 0x1000));
    if (ok_heapvma) {
        volatile unsigned char *hp = (volatile unsigned char *)base;
        hp[0x100] = 0xC3;
        ok_heapvma = (hp[0x100] == 0xC3);
    }
    syscall(SYS_brk, cur, 0, 0);
    CHECK3(ok_heapvma, "heap_vma_intact",
           "brk grow/write/read/shrink after rejected mmaps");

    /* 9. ELF envelope still intact: /proc/self/maps still lists
     * the image at 0x400000 and (with brk grown) a [heap] row. */
    int64_t cur2 = syscall(SYS_brk, 0, 0, 0);
    uint64_t base2 = ((uint64_t)cur2 + 0xFFF) & ~(uint64_t)0xFFF;
    syscall(SYS_brk, base2 + 0x1000, 0, 0);

    char buf[4096];
    int fd = open("/proc/self/maps", O_RDONLY);
    int ok_maps = 0;
    if (fd >= 0) {
        int n = (int)read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            int has_image = 0, has_heap = 0;
            char *q = buf;
            while (*q) {
                char *line = q;
                char *nl = strchr(q, '\n');
                if (nl) { *nl = '\0'; q = nl + 1; }
                else    { q = line + strlen(line); }
                unsigned long s64, e64;
                if (sscanf(line, "%lx-%lx", &s64, &e64) >= 1) {
                    if (s64 == 0x400000UL) has_image = 1;
                }
                if (strstr(line, "[heap]")) has_heap = 1;
            }
            ok_maps = has_image && has_heap;
        }
    }
    syscall(SYS_brk, cur2, 0, 0);
    CHECK3(ok_maps, "envelope_and_heap_in_maps",
           "/proc/self/maps lists image at 0x400000 + [heap]");
}


// ── 51: fork brk-grown page isolation (Task 6) ───────────────
// Parent grows brk by 2 pages, writes its own magic bytes to each,
// forks.  Child writes DIFFERENT bytes to the SAME addresses (which
// must COW-privatize), then writes back the parent bytes via a
// pipe and exits.  Parent reads the pipe and verifies its OWN bytes
// were unchanged (no shared phys between parent and child after
// the COW write).  Asserts:
//   - Parent's heap bytes are unchanged after child COW writes
//   - Child sees a different heap (writes don't leak into parent)
//   - The two processes have independent mappings
void test_fork_brk_isolation(void)
{
    /* Stage 1: grow brk by 2 pages and put parent-magic bytes in them. */
    int64_t cur_brk = syscall(SYS_brk, 0, 0, 0);
    if (cur_brk <= 0) {
        FAIL("fork_brk_isolation", "brk query failed rc=%ld", (long)cur_brk);
        return;
    }
    uint64_t base = ((uint64_t)cur_brk + 0xFFF) & ~(uint64_t)0xFFF;
    int64_t grown = syscall(SYS_brk, base + 0x2000, 0, 0);
    if (grown != (int64_t)(base + 0x2000)) {
        FAIL("fork_brk_isolation", "brk grow failed rc=%ld", (long)grown);
        return;
    }
    volatile unsigned char *p0 = (volatile unsigned char *)(uintptr_t)base;
    volatile unsigned char *p1 = (volatile unsigned char *)(uintptr_t)(base + 0x1000);
    p0[0] = 0xA0; p0[1] = 0xA1; p0[0xFF] = 0xAF;
    p1[0] = 0xB0; p1[1] = 0xB1; p1[0xFF] = 0xBF;

    /* Stage 2: a pipe so the child can signal its COW-write result
     * back to the parent without the child needing to printk. */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        FAIL("fork_brk_isolation", "pipe failed errno=%d", errno);
        syscall(SYS_brk, cur_brk, 0, 0);
        return;
    }

    int64_t pid = fork();
    if (pid < 0) {
        FAIL("fork_brk_isolation", "fork failed");
        close(pipefd[0]); close(pipefd[1]);
        syscall(SYS_brk, cur_brk, 0, 0);
        return;
    }

    if (pid == 0) {
        /* Child: close read end, write the COW-privatized bytes
         * (must be ours now — child wrote its own magic), and exit. */
        close(pipefd[0]);
        /* COW writes — these must trigger the COW fault on VMA-present
         * writable pages.  Brief: "writable VMA leaves use COW as today"
         * — the child's write must NOT corrupt the parent's bytes. */
        p0[0] = 0xC0; p0[0xFF] = 0xCF;
        p1[0] = 0xD0; p1[0xFF] = 0xDF;
        /* Sanity: child sees its own writes. */
        unsigned char csig[4];
        csig[0] = p0[0]; csig[1] = p0[0xFF];
        csig[2] = p1[0]; csig[3] = p1[0xFF];
        write(pipefd[1], csig, 4);
        close(pipefd[1]);
        _exit(0);
    }

    /* Parent: read the child's 4 signature bytes. */
    close(pipefd[1]);
    unsigned char csig[4] = {0, 0, 0, 0};
    int64_t got_n = read(pipefd[0], csig, 4);
    close(pipefd[0]);
    int status = 0;
    int64_t w = waitpid(pid, &status, 0);

    int ok_child = (w == pid) && (status == 0) && (got_n == 4)
                && (csig[0] == 0xC0) && (csig[1] == 0xCF)
                && (csig[2] == 0xD0) && (csig[3] == 0xDF);
    int ok_parent = (p0[0] == 0xA0) && (p0[1] == 0xA1) && (p0[0xFF] == 0xAF)
                 && (p1[0] == 0xB0) && (p1[1] == 0xB1) && (p1[0xFF] == 0xBF);
    CHECK3(ok_child, "fork_brk_child_writes",
           "child's COW writes produced its own bytes");
    CHECK3(ok_parent, "fork_brk_parent_unchanged",
           "parent's heap bytes intact after child COW writes");

    syscall(SYS_brk, cur_brk, 0, 0);
}


// ── 52: fork of writable mmaps preserves COW isolation (Task 6) ─
// Parent mmaps two writable pages, writes its own bytes, forks.
// Child COW-writes its own bytes.  Parent's bytes must be unchanged
// after the child exits.  Validates the "writable VMA leaves use
// COW" contract on the mmap path (not just the heap path).
void test_fork_mmap_cow_isolation(void)
{
    /* Stage 1: two writable mmaps. */
    void *m0 = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    void *m1 = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (m0 == MAP_FAILED || m1 == MAP_FAILED) {
        FAIL("fork_mmap_cow", "mmap failed");
        if (m0 != MAP_FAILED) munmap(m0, 0x1000);
        if (m1 != MAP_FAILED) munmap(m1, 0x1000);
        return;
    }
    volatile unsigned char *p0 = (volatile unsigned char *)m0;
    volatile unsigned char *p1 = (volatile unsigned char *)m1;
    p0[0] = 0x10; p0[0xFF] = 0x1F;
    p1[0] = 0x20; p1[0xFF] = 0x2F;

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        FAIL("fork_mmap_cow", "pipe failed errno=%d", errno);
        munmap(m0, 0x1000); munmap(m1, 0x1000);
        return;
    }

    int64_t pid = fork();
    if (pid < 0) {
        FAIL("fork_mmap_cow", "fork failed");
        close(pipefd[0]); close(pipefd[1]);
        munmap(m0, 0x1000); munmap(m1, 0x1000);
        return;
    }

    if (pid == 0) {
        close(pipefd[0]);
        /* COW-privatize via write. */
        p0[0] = 0x30; p0[0xFF] = 0x3F;
        p1[0] = 0x40; p1[0xFF] = 0x4F;
        unsigned char csig[4];
        csig[0] = p0[0]; csig[1] = p0[0xFF];
        csig[2] = p1[0]; csig[3] = p1[0xFF];
        write(pipefd[1], csig, 4);
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    unsigned char csig[4] = {0, 0, 0, 0};
    int64_t got_n = read(pipefd[0], csig, 4);
    close(pipefd[0]);
    int status = 0;
    int64_t w = waitpid(pid, &status, 0);

    int ok_child = (w == pid) && (status == 0) && (got_n == 4)
                && (csig[0] == 0x30) && (csig[1] == 0x3F)
                && (csig[2] == 0x40) && (csig[3] == 0x4F);
    int ok_parent = (p0[0] == 0x10) && (p0[0xFF] == 0x1F)
                 && (p1[0] == 0x20) && (p1[0xFF] == 0x2F);
    CHECK3(ok_child, "fork_mmap_child_writes",
           "child's COW writes on mmap pages produced its own bytes");
    CHECK3(ok_parent, "fork_mmap_parent_unchanged",
           "parent's mmap bytes intact after child COW writes");

    munmap(m0, 0x1000);
    munmap(m1, 0x1000);
}

// ── mprotect 子区间 + COW 保留（2026-11-13 plan 任务 2）─────
// 三页 RW 匿名映射只预触中页。fork 后子进程把中页设为只读：
//   * getrandom 走内核代写路径，必须按 VMA 逻辑权限拒绝（EFAULT）；
//   * 左右两页（fork 时仍未驻留）的首次写入仍成功 —— 证明拆分没有
//     把邻居 VMA 一起改成只读；
//   * 中页 NONE -> RW 后再写，父进程仍看到 fork 前的字节 —— 证明
//     mprotect 保留 COW 引用，私有化延迟到第一次真正写入。
// 随后用四页文件映射验证拆分后的文件段偏移：保护第 2 页造成右段
// 起点前移，右段首次缺页读必须按页首 VA 计算文件偏移（页内 +37
// 读到原文件对应字节，而不是整体错位 37 字节）。
void test_mprotect_subrange_cow(void)
{
    /* ── A. 匿名三页：中页 COW 语义 ── */
    unsigned char *p = mmap(NULL, 0x3000, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        FAIL("mprotect_subrange", "setup mmap failed errno=%d", errno);
        return;
    }
    p[0x1000] = 0x5A;           /* 只预触中页 */

    int64_t pid = fork();
    if (pid < 0) {
        FAIL("mprotect_subrange", "fork failed errno=%d", errno);
        munmap(p, 0x3000);
        return;
    }
    if (pid == 0) {
        /* 子进程：错误路径直接 _exit，父进程只等状态，不读管道。 */
        if (mprotect(p + 0x1000, 0x1000, PROT_READ) != 0)
            _exit(10);
        errno = 0;
        ssize_t g = getrandom(p + 0x1000, 1, 0);
        if (g != -1 || errno != EFAULT)
            _exit(11);
        /* 左右页在 fork 时未驻留；首次写入必须仍然成功。 */
        p[0] = 0x4C;
        p[0x2000] = 0x52;
        if (p[0] != 0x4C || p[0x2000] != 0x52)
            _exit(12);
        /* 中页 NONE -> RW 后写入：COW 必须在首次写时私有化。 */
        if (mprotect(p + 0x1000, 0x1000, PROT_NONE) != 0)
            _exit(13);
        if (mprotect(p + 0x1000, 0x1000,
                     PROT_READ | PROT_WRITE) != 0)
            _exit(14);
        p[0x1000] = 0xC7;
        if (p[0x1000] != 0xC7)
            _exit(15);
        _exit(0);
    }

    int st = 0;
    int64_t waited = waitpid(pid, &st, 0);
    CHECKF(waited == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0,
           "mprotect_subrange_child", "pid=%ld waited=%ld status=%#x",
           "want waited==pid && WIFEXITED&&0, pid=%ld waited=%ld status=%#x",
           (long)pid, (long)waited, st);

    /* 父进程保持映射，等子进程退出后再读。 */
    CHECKF(p[0x1000] == 0x5A, "mprotect_subrange_parent_mid",
           "parent middle byte=%#x", "want 0x5a, got %#x", p[0x1000]);
    /* 左右页父进程从未写过：应为按需零页，而不是子进程的字节。 */
    CHECKF(p[0] == 0 && p[0x2000] == 0,
           "mprotect_subrange_parent_absent",
           "parent absent pages zero", "got left=%#x right=%#x",
           p[0], p[0x2000]);
    CHECK3(munmap(p, 0x3000) == 0, "mprotect_subrange_release",
           "anon range released");

    /* ── B. 四页文件：拆分后右段首次读 +37 ── */
    const char *path = "/tmp/t_mprot_range";
    unlink(path);
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        FAIL("mprotect_file", "create file failed errno=%d", errno);
        return;
    }
    unsigned char page[4096];
    int wrote = 1;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4096; j++)
            page[j] = (unsigned char)(i * 64 + (j & 0x3F));
        if (write(fd, page, 4096) != 4096) {
            wrote = 0;
            break;
        }
    }
    close(fd);
    if (!wrote) {
        FAIL("mprotect_file", "write file failed");
        unlink(path);
        return;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        FAIL("mprotect_file", "re-open file failed errno=%d", errno);
        unlink(path);
        return;
    }
    unsigned char *f = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE, fd, 0);
    close(fd);
    if (f == MAP_FAILED) {
        FAIL("mprotect_file", "file mmap failed errno=%d", errno);
        unlink(path);
        return;
    }

    /* 保护第 2 页：左段 [p0,p1) 保持 pgoff 0，右段 [p2,p4) 变 pgoff 2。 */
    int ok_split = (mprotect(f + 0x1000, 0x1000, PROT_READ) == 0);
    CHECK3(ok_split, "mprotect_file_split",
           "middle page split off and set read-only");

    /* 右段首次读：页 3、页内 +37。拆分后 vm_start=p2，pgoff=2；
     * 只有在缺页路径按页首 VA 计算偏移时才读到原文件 0x3000+37。 */
    unsigned char got_right = f[0x3000 + 37];
    unsigned char want_right = (unsigned char)(3 * 64 + (37 & 0x3F));
    CHECKF(got_right == want_right, "mprotect_file_right_offset",
           "right segment byte=%#x", "want %#x, got %#x (page shifted?)",
           want_right, got_right);

    /* 左段同样保持原偏移。 */
    unsigned char got_left = f[37];
    unsigned char want_left = (unsigned char)(37 & 0x3F);
    CHECKF(got_left == want_left, "mprotect_file_left_offset",
           "left segment byte=%#x", "want %#x, got %#x",
           want_left, got_left);

    CHECK3(munmap(f, 0x4000) == 0, "mprotect_file_release",
           "file range released");
    unlink(path);
}

static unsigned char cow_elf_output[4096] __attribute__((aligned(4096)));


// Kernel output must be the first write to the child's shared COW buffer.
// Cover heap, anonymous/file mmap and ELF globals through all output paths.
void test_cow_kernel_outputs(void)
{
    const unsigned char payload[4] = {'C', 'O', 'W', '!'};
    const unsigned char elf_magic[4] = {0x7f, 'E', 'L', 'F'};
    for (int region = 0; region < 4; ++region) {
        for (int path = 0; path < 4; ++path) {
            int fd = -1, mapfd = -1, pipes[2] = {-1, -1};
            unsigned char *buf = NULL;
            if (region == 0) buf = malloc(4096);
            else if (region == 3) buf = cow_elf_output;
            else {
                if (region == 2) mapfd = open("/bin/spin", O_RDONLY);
                buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | (region == 1 ? MAP_ANONYMOUS : 0),
                           mapfd, 0);
                if (buf == MAP_FAILED) buf = NULL;
            }
            if (!buf) { FAIL("cow_output", "buffer allocation failed"); return; }
            memset(buf, 0x55, 64); // populate the page before fork
            if (path == 0) fd = open("/bin/spin", O_RDONLY);
            if (path == 1) {
                if (pipe(pipes) == 0 && write(pipes[1], payload, 4) == 4)
                    fd = pipes[0];
            }
            if (path == 2) {
                fd = open("/dev/tty0", O_RDONLY);
                struct termios raw = {0};
                raw.c_lflag = ISIG;
                if (fd >= 0 && ioctl(fd, TCSETS, &raw) < 0) {
                    close(fd); fd = -1;
                }
            }
            int64_t pid = (path == 3 || fd >= 0) ? fork() : -1;
            if (pid == 0) {
                ssize_t n;
                if (path == 3) n = getrandom(buf, 64, 0);
                else {
                    if (path == 2) {
                        char ready[40];
                        int len = snprintf(ready, sizeof(ready),
                                           "[COW TTY READY %d]\n", region);
                        write(1, ready, (size_t)len);
                    }
                    n = read(fd, buf, 4);
                }
                if (n != (path == 3 ? 64 : 4)) {
                    printf("[COW OUTPUT] region=%d path=%d read=%ld errno=%d\n",
                           region, path, (long)n, errno);
                    _exit(2);
                }
                if (path != 3 && memcmp(buf, path == 0 ? elf_magic : payload, 4))
                    _exit(3);
                if (path == 3) {
                    int changed = 0;
                    for (int i = 0; i < 64; ++i) changed |= buf[i] != 0x55;
                    if (!changed) _exit(4);
                }
                _exit(0);
            }
            int status = -1;
            int64_t waited = pid > 0 ? waitpid(pid, &status, 0) : -1;
            int intact = 1;
            for (int i = 0; i < 64; ++i) intact &= buf[i] == 0x55;
            char name[64];
            snprintf(name, sizeof(name), "cow_output_r%d_p%d_child", region, path);
            CHECK3(pid > 0 && waited == pid && status == 0, name,
                   "kernel output privatized child's untouched COW buffer");
            snprintf(name, sizeof(name), "cow_output_r%d_p%d_parent", region, path);
            CHECK3(intact, name, "parent's entire buffer remains unchanged");
            if (pipes[0] >= 0) { close(pipes[0]); close(pipes[1]); }
            else if (fd >= 0) close(fd);
            if (mapfd >= 0) close(mapfd);
            if (region == 0) free(buf);
            else if (region != 3) munmap(buf, 4096);
        }
    }
}

