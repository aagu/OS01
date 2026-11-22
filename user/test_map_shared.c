/*
 * user/test_map_shared.c — product EL0 probe for inode-level MAP_SHARED
 * (aarch64 cross-process sharing acceptance).
 *
 * Run with no arguments: the coordinator drives every phase, prints the
 * single success line "test_map_shared: PASS" and exits 0.  Any stage
 * failure prints "test_map_shared: FAIL <stage>" and exits non-zero —
 * in children as well, so the coordinator's waitpid sees it.
 *
 * Sub-roles run the same ELF through execve with argv[1] = role name;
 * pipe endpoints travel as "<rfd>:<wfd>:..." argv strings.  Mappings
 * use page-aligned fixed VAs well apart per party, different file page
 * offsets, and fixed byte values, so both visibility directions and
 * offset isolation are checked on real product paths (ext2 root, tmpfs
 * /tmp, vfat /boot).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE 4096
#define EXT2_FIXTURE "/mapt_ext2.bin"
#define TMP_FIXTURE  "/tmp/mapt_tmp.bin"
#define LIFE_FIXTURE "/mapt_life.bin"
#define LIFE_RENAMED "/mapt_life2.bin"
#define EXEC_FIXTURE "/mapt_exec.bin"
#define EXIT_FIXTURE "/mapt_exit.bin"
#define VFAT_A       "/boot/maptst.txt"
#define VFAT_ALIAS   "/boot/MAPTST.TXT"
#define VFAT_RENAMED "/boot/MPTST2.TXT"

static void fail(const char *stage)
{
    printf("test_map_shared: FAIL %s\n", stage);
    exit(1);
}

static void xwrite(int fd, const void *buf, size_t n, const char *stage)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        int64_t w = write(fd, p, n);
        if (w <= 0)
            fail(stage);
        p += w;
        n -= (size_t)w;
    }
}

static void xread(int fd, void *buf, size_t n, const char *stage)
{
    char *p = (char *)buf;
    while (n > 0) {
        int64_t r = read(fd, p, n);
        if (r <= 0)
            fail(stage);
        p += r;
        n -= (size_t)r;
    }
}

/* Fill n bytes of fd with the byte pattern. */
static void fill_fd(int fd, int pattern, size_t n, const char *stage)
{
    static char zero[PAGE];
    memset(zero, pattern, sizeof(zero));
    size_t done = 0;
    while (done < n) {
        size_t chunk = (n - done > sizeof(zero)) ? sizeof(zero) : n - done;
        xwrite(fd, zero, chunk, stage);
        done += chunk;
    }
}

static void *map_at(uint64_t va, size_t len, int prot, int fd,
                    int64_t off, const char *stage)
{
    void *p = mmap((void *)(uintptr_t)va, len, prot,
                   MAP_SHARED | MAP_FIXED, fd, off);
    if (p == MAP_FAILED)
        fail(stage);
    return p;
}

/* ── Phase 1 role: ext2 rendezvous with a uaccess-first fill ───── */
static void role_ext2b(char *path, char *fdspec)
{
    int pr1, pw1, pr2, pw2;
    if (sscanf(fdspec, "%d:%d:%d:%d", &pr1, &pw1, &pr2, &pw2) != 4)
        fail("ext2b-fds");
    close(pw1);
    close(pr2);

    int fd = open(path, O_RDWR);
    if (fd < 0)
        fail("ext2b-open");
    /* This mapping covers file pages 1..2 from a different VA and a
     * different page offset than the coordinator's mapping. */
    volatile uint8_t *va = (volatile uint8_t *)map_at(
        0x41000000ULL, 2 * PAGE, PROT_READ | PROT_WRITE, fd, PAGE,
        "ext2b-mmap");

    /* uaccess direction: the coordinator sends bytes over the pipe and
     * this read() lands them INSIDE the not-yet-resident shared page —
     * the kernel's uaccess pre-instantiation fills the object page.
     * This mapping starts at file page 1, so in-mapping offset 0x100
     * is file offset 0x1100 — the same byte the coordinator addresses
     * at PAGE+0x100 in ITS mapping. */
    xread(pr1, (void *)(uintptr_t)(va + 0x100), 4, "ext2b-pipe1");
    xwrite(pw2, "ok", 2, "ext2b-sig");

    /* Store direction: the coordinator stored 0x5A into file page 2
     * (in-mapping offset PAGE+0x10 here). */
    char sig[4];
    xread(pr1, sig, 2, "ext2b-go");
    if (va[PAGE + 0x10] != 0x5A)
        fail("ext2b-store-read");
    /* ... and this side stores 0xA5 into file page 1 (in-mapping
     * offset 0x10 here). */
    va[0x10] = 0xA5;
    xwrite(pw2, "sd", 2, "ext2b-sig2");

    xread(pr1, sig, 3, "ext2b-bye");
    close(pr1);
    close(pw2);
    close(fd);
    exit(0);
}

/* ── Phase 2 role: tmpfs sparse rendezvous ─────────────────────── */
static void role_tmpb(char *path, char *fdspec)
{
    int pr, pw;
    if (sscanf(fdspec, "%d:%d", &pr, &pw) != 2)
        fail("tmpb-fds");

    int fd = open(path, O_RDWR);
    if (fd < 0)
        fail("tmpb-open");
    volatile uint8_t *va = (volatile uint8_t *)map_at(
        0x41100000ULL, 2 * PAGE, PROT_READ | PROT_WRITE, fd, PAGE,
        "tmpb-mmap");

    /* The file only ever grew through ftruncate: every page the child
     * sees starts as a hole (zero).  Store and signal. */
    if (va[0] != 0x00)
        fail("tmpb-hole");
    va[0] = 0x33;
    xwrite(pw, "ok", 2, "tmpb-sig");

    char sig[4];
    xread(pr, sig, 3, "tmpb-bye");
    close(pr);
    close(pw);
    close(fd);
    exit(0);
}

/* ── Phase 3 role: lifecycle (rename rendezvous + post-unlink fill) ── */
static void role_lifb(char *path, char *fdspec)
{
    int pr, pw;
    if (sscanf(fdspec, "%d:%d", &pr, &pw) != 2)
        fail("lifb-fds");

    /* The coordinator renamed the fixture before this open: the path
     * resolves to the ORIGINAL inode, so both mappings rendezvous. */
    int fd = open(path, O_RDWR);
    if (fd < 0)
        fail("lifb-open");
    volatile uint8_t *va = (volatile uint8_t *)map_at(
        0x41200000ULL, 3 * PAGE, PROT_READ | PROT_WRITE, fd, 0,
        "lifb-mmap");

    if (va[PAGE] != 0x61)
        fail("lifb-rename-rendezvous");
    xwrite(pw, "rn", 2, "lifb-sig");

    /* The coordinator closed its fd and unlinked the last name BEFORE
     * this first touch of page 2: the mapping survives and the page
     * fills from the orphaned inode's backend. */
    char sig[4];
    xread(pr, sig, 2, "lifb-go");
    if (va[2 * PAGE] != 0x52)
        fail("lifb-post-unlink-fill");
    xwrite(pw, "ok", 2, "lifb-sig2");

    xread(pr, sig, 3, "lifb-bye");
    close(pr);
    close(pw);
    close(fd);
    exit(0);
}

/* ── Teardown role A: unique modified VMA, then exec ──────────────── */
static void role_execrole(char *path, char *fdspec, char *mode)
{
    int pr, pw;
    if (sscanf(fdspec, "%d:%d", &pr, &pw) != 2)
        fail("exec-fds");

    if (strcmp(mode, "first") == 0) {
        int fd = open(path, O_RDWR);
        if (fd < 0)
            fail("exec-open");
        volatile uint8_t *va = (volatile uint8_t *)map_at(
            0x41400000ULL, 2 * PAGE, PROT_READ | PROT_WRITE, fd, 0,
            "exec-mmap");
        va[0] = 0x88;                       /* the only live mapping's
                                             * only modification */
        xwrite(pw, "rd", 2, "exec-ready");
        char go[4];
        xread(pr, go, 2, "exec-go");
        close(fd);
        /* The formal exec teardown: this process's only shared VMA is
         * destroyed with the address space; the object loses its last
         * reference and its pages are reclaimed. */
        char *argv[] = { "/bin/test_map_shared", "execrole", path,
                         fdspec, "post", NULL };
        execv("/bin/test_map_shared", argv);
        fail("exec-reexec");
    }
    /* post-exec image: remap and read the backend's original bytes. */
    int fd = open(path, O_RDWR);
    if (fd < 0)
        fail("exec-post-open");
    volatile uint8_t *va = (volatile uint8_t *)map_at(
        0x41500000ULL, PAGE, PROT_READ, fd, 0, "exec-post-mmap");
    if (va[0] != 0x3A)
        fail("exec-post-content");
    xwrite(pw, "ok", 2, "exec-post-sig");
    close(pr);
    close(pw);
    close(fd);
    exit(0);
}

/* ── Teardown role B: unique modified VMA, then exit ──────────────── */
static void role_exitrole(char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0)
        fail("exit-open");
    volatile uint8_t *va = (volatile uint8_t *)map_at(
        0x41600000ULL, PAGE, PROT_READ | PROT_WRITE, fd, 0, "exit-mmap");
    va[0] = 0x99;
    /* Formal exit teardown: same as exec, via the exit path. */
    close(fd);
    exit(0);
}

/* argv: [0]=elf [1]=role [2..] role-specific */
int main(int argc, char **argv)
{
    const char *role = (argc > 1) ? argv[1] : "";

    if (strcmp(role, "ext2b") == 0 && argc >= 4)
        role_ext2b(argv[2], argv[3]);
    if (strcmp(role, "tmpb") == 0 && argc >= 4)
        role_tmpb(argv[2], argv[3]);
    if (strcmp(role, "lifb") == 0 && argc >= 4)
        role_lifb(argv[2], argv[3]);
    if (strcmp(role, "execrole") == 0 && argc >= 5)
        role_execrole(argv[2], argv[3], argv[4]);
    if (strcmp(role, "exitrole") == 0 && argc >= 3)
        role_exitrole(argv[2]);
    if (role[0] != '\0')
        fail("unknown-role");

    /* ════════════════════════════════════════════════════════════
     * Coordinator
     * ════════════════════════════════════════════════════════════ */
    int st = 0;

    /* ── Phase 1: ext2 cross-process rendezvous ─────────────────── */
    int fd = open(EXT2_FIXTURE, O_CREAT | O_RDWR);
    if (fd < 0)
        fail("p1-create");
    fill_fd(fd, 0x41, 3 * PAGE, "p1-init");

    void *va_c = map_at(0x40000000ULL, 3 * PAGE, PROT_READ | PROT_WRITE,
                        fd, 0, "p1-mmap");

    int p1[2], p2[2];
    if (pipe(p1) || pipe(p2))
        fail("p1-pipe");
    char fdspec[64];
    snprintf(fdspec, sizeof(fdspec), "%d:%d:%d:%d", p1[0], p1[1],
             p2[0], p2[1]);
    int64_t pid = fork();
    if (pid == 0) {
        close(p1[1]);
        close(p2[0]);
        char *args[] = { "/bin/test_map_shared", "ext2b",
                         (char *)EXT2_FIXTURE, fdspec, NULL };
        execv("/bin/test_map_shared", args);
        fail("p1-exec");
    }
    close(p1[0]);
    close(p2[1]);

    xwrite(p1[1], "WRIT", 4, "p1-send");        /* the uaccess payload */
    char sig[4];
    xread(p2[0], sig, 2, "p1-wait");
    if (memcmp((const void *)((uintptr_t)va_c + PAGE + 0x100), "WRIT", 4))
        fail("p1-uaccess-visible");

    *(volatile uint8_t *)((uintptr_t)va_c + 2 * PAGE + 0x10) = 0x5A;
    xwrite(p1[1], "st", 2, "p1-go");
    xread(p2[0], sig, 2, "p1-wait2");
    if (*(volatile uint8_t *)((uintptr_t)va_c + PAGE + 0x10) != 0xA5)
        fail("p1-store-visible");
    if (*(volatile uint8_t *)((uintptr_t)va_c) != 0x41)
        fail("p1-isolation");                   /* page 0 untouched */

    xwrite(p1[1], "bye", 3, "p1-bye");
    close(p1[1]);
    close(p2[0]);
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) ||
        WEXITSTATUS(st) != 0)
        fail("p1-wait");
    munmap(va_c, 3 * PAGE);
    close(fd);
    unlink(EXT2_FIXTURE);

    /* ── Phase 2: tmpfs sparse (ftruncate-only) rendezvous ──────── */
    fd = open(TMP_FIXTURE, O_CREAT | O_RDWR);
    if (fd < 0)
        fail("p2-create");
    if (ftruncate(fd, 3 * PAGE) != 0)
        fail("p2-truncate");

    void *tva = map_at(0x40200000ULL, 3 * PAGE, PROT_READ | PROT_WRITE,
                       fd, 0, "p2-mmap");
    if (pipe(p1) || pipe(p2))
        fail("p2-pipe");
    snprintf(fdspec, sizeof(fdspec), "%d:%d", p1[0], p2[1]);
    pid = fork();
    if (pid == 0) {
        close(p1[1]);
        close(p2[0]);
        char *args[] = { "/bin/test_map_shared", "tmpb",
                         (char *)TMP_FIXTURE, fdspec, NULL };
        execv("/bin/test_map_shared", args);
        fail("p2-exec");
    }
    close(p1[0]);
    close(p2[1]);
    xread(p2[0], sig, 2, "p2-wait");
    if (*(volatile uint8_t *)((uintptr_t)tva + PAGE) != 0x33)
        fail("p2-visible");
    if (*(volatile uint8_t *)((uintptr_t)tva) != 0x00)
        fail("p2-hole0");
    if (*(volatile uint8_t *)((uintptr_t)tva + 2 * PAGE) != 0x00)
        fail("p2-hole2");
    xwrite(p1[1], "bye", 3, "p2-bye");
    close(p1[1]);
    close(p2[0]);
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) ||
        WEXITSTATUS(st) != 0)
        fail("p2-wait");
    munmap(tva, 3 * PAGE);
    close(fd);
    unlink(TMP_FIXTURE);

    /* ── Phase 3: lifecycle — rename, unlink, rebuild, ceilings ─── */
    fd = open(LIFE_FIXTURE, O_CREAT | O_RDWR);
    if (fd < 0)
        fail("p3-create");
    fill_fd(fd, 0, 3 * PAGE, "p3-init");
    if (lseek(fd, 0, SEEK_SET) < 0)
        fail("p3-seek");
    {
        char zero[PAGE];
        memset(zero, 0x51, sizeof(zero));
        xwrite(fd, zero, PAGE, "p3-bk0");
        xwrite(fd, zero, PAGE, "p3-bk1");
        memset(zero, 0x52, sizeof(zero));
        xwrite(fd, zero, PAGE, "p3-bk2");
    }
    if (lseek(fd, 0, SEEK_SET) < 0)
        fail("p3-seek2");

    void *lva = map_at(0x40300000ULL, 3 * PAGE, PROT_READ | PROT_WRITE,
                       fd, 0, "p3-mmap");
    *(volatile uint8_t *)((uintptr_t)lva + PAGE) = 0x61;   /* page 1
                                             * filled AND modified */

    if (rename(LIFE_FIXTURE, LIFE_RENAMED) != 0)
        fail("p3-rename");
    if (pipe(p1) || pipe(p2))
        fail("p3-pipe");
    snprintf(fdspec, sizeof(fdspec), "%d:%d", p1[0], p2[1]);
    pid = fork();
    if (pid == 0) {
        close(p1[1]);
        close(p2[0]);
        char *args[] = { "/bin/test_map_shared", "lifb",
                         (char *)LIFE_RENAMED, fdspec, NULL };
        execv("/bin/test_map_shared", args);
        fail("p3-exec");
    }
    close(p1[0]);
    close(p2[1]);
    xread(p2[0], sig, 2, "p3-wait");            /* child rendezvoused
                                                 * through the new path */

    /* Close the coordinator's fd and remove the last name: the child's
     * first touch of page 2 still fills from the old inode. */
    close(fd);
    if (unlink(LIFE_RENAMED) != 0)
        fail("p3-unlink");
    xwrite(p1[1], "go", 2, "p3-go");
    xread(p2[0], sig, 2, "p3-wait2");
    if (*(volatile uint8_t *)((uintptr_t)lva + PAGE) != 0x61)
        fail("p3-survives");
    xwrite(p1[1], "bye", 3, "p3-bye");
    close(p1[1]);
    close(p2[0]);
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) ||
        WEXITSTATUS(st) != 0)
        fail("p3-wait");
    munmap(lva, 3 * PAGE);

    /* Same-path rebuild: a NEW inode under the old name is an
     * independent object; the old mapping (if any) keeps its content. */
    fd = open(LIFE_RENAMED, O_CREAT | O_RDWR);
    if (fd < 0)
        fail("p3b-create");
    {
        char blk[PAGE];
        memset(blk, 0x99, sizeof(blk));
        xwrite(fd, blk, PAGE, "p3b-bk0");
    }
    void *rva = map_at(0x40400000ULL, 3 * PAGE, PROT_READ | PROT_WRITE,
                       fd, 0, "p3b-mmap");
    if (*(volatile uint8_t *)((uintptr_t)rva) != 0x99)
        fail("p3b-independtent");

    /* Backend edits: a filled page keeps the mapped view, an unfilled
     * page observes the backend on first fill. */
    if (lseek(fd, 0, SEEK_SET) < 0)
        fail("p3b-seek0");
    if (write(fd, &(char){0x71}, 1) != 1)
        fail("p3b-bkwrite0");
    if (*(volatile uint8_t *)((uintptr_t)rva) != 0x99)
        fail("p3b-cache-keeps");
    if (lseek(fd, PAGE, SEEK_SET) < 0)
        fail("p3b-seek1");
    if (write(fd, &(char){0x72}, 1) != 1)
        fail("p3b-bkwrite1");
    if (*(volatile uint8_t *)((uintptr_t)rva + PAGE) != 0x72)
        fail("p3b-unfilled-sees");
    if (*(volatile uint8_t *)((uintptr_t)rva + 2 * PAGE) != 0x00)
        fail("p3b-hole2");

    /* Last mapping gone → object destroyed → refill from backend. */
    munmap(rva, 3 * PAGE);
    void *rva2 = map_at(0x40500000ULL, 3 * PAGE, PROT_READ | PROT_WRITE,
                        fd, 0, "p3c-mmap");
    if (*(volatile uint8_t *)((uintptr_t)rva2) != 0x71)
        fail("p3c-refill");
    if (*(volatile uint8_t *)((uintptr_t)rva2 + PAGE) != 0x72)
        fail("p3c-refill1");
    munmap(rva2, 3 * PAGE);

    /* Access-mode ceilings. */
    int fd_wo = open(LIFE_RENAMED, O_WRONLY);
    if (fd_wo < 0)
        fail("p3d-wo-open");
    if (mmap((void *)0x40600000ULL, PAGE, PROT_READ,
             MAP_SHARED | MAP_FIXED, fd_wo, 0) != MAP_FAILED)
        fail("p3d-wo-mmap");
    close(fd_wo);

    int fd_rd = open(LIFE_RENAMED, O_RDONLY);
    if (fd_rd < 0)
        fail("p3d-rd-open");
    if (mmap((void *)0x40600000ULL, PAGE, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_FIXED, fd_rd, 0) != MAP_FAILED)
        fail("p3d-rd-rw-mmap");
    void *rova = map_at(0x40600000ULL, PAGE, PROT_READ, fd_rd, 0,
                        "p3d-ro-mmap");
    if (mprotect(rova, PAGE, PROT_READ | PROT_WRITE) != -1)
        fail("p3d-expand");
    munmap(rova, PAGE);
    close(fd_rd);

    int fd_rw2 = open(LIFE_RENAMED, O_RDWR);
    if (fd_rw2 < 0)
        fail("p3e-open");
    void *rwva = map_at(0x40700000ULL, PAGE, PROT_READ | PROT_WRITE,
                        fd_rw2, 0, "p3e-mmap");
    if (mprotect(rwva, PAGE, PROT_READ) != 0)
        fail("p3e-downgrade");
    if (mprotect(rwva, PAGE, PROT_READ | PROT_WRITE) != 0)
        fail("p3e-restore");
    munmap(rwva, PAGE);
    close(fd_rw2);
    close(fd);
    unlink(LIFE_RENAMED);

    /* ── Phase 4: anonymous shared — late touch and isolation ───── */
    void *ava = mmap((void *)0x40800000ULL, PAGE,
                     PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
                     -1, 0);
    if (ava == MAP_FAILED)
        fail("p4-mmap");
    int p3[2];
    if (pipe(p3))
        fail("p4-pipe");
    pid = fork();                               /* plain fork: the anon
                             * object is inherited with NO touched page */
    if (pid == 0) {
        close(p3[0]);
        *(volatile uint8_t *)ava = 0x44;        /* child's first touch */
        xwrite(p3[1], "ok", 2, "p4-sig");
        close(p3[1]);
        exit(0);
    }
    close(p3[1]);
    xread(p3[0], sig, 2, "p4-wait");
    if (*(volatile uint8_t *)ava != 0x44)
        fail("p4-late-touch");
    close(p3[0]);
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) ||
        WEXITSTATUS(st) != 0)
        fail("p4-wait");
    munmap(ava, PAGE);

    void *ava2 = mmap((void *)0x40900000ULL, PAGE,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    void *ava3 = mmap((void *)0x40a00000ULL, PAGE,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ava2 == MAP_FAILED || ava3 == MAP_FAILED)
        fail("p4b-mmap");
    *(volatile uint8_t *)ava2 = 0x55;
    *(volatile uint8_t *)ava3 = 0x66;
    if (*(volatile uint8_t *)ava2 != 0x55 ||
        *(volatile uint8_t *)ava3 != 0x66)
        fail("p4b-isolation");
    munmap(ava2, PAGE);
    munmap(ava3, PAGE);

    /* ── Phase 5: vfat /boot — 8.3 case alias and ordinary rename ── */
    fd = open(VFAT_A, O_CREAT | O_RDWR);
    if (fd < 0)
        fail("p5-create");
    fill_fd(fd, 0x40, PAGE, "p5-init");
    void *vva = map_at(0x40b00000ULL, PAGE, PROT_READ | PROT_WRITE,
                       fd, 0, "p5-mmap");
    *(volatile uint8_t *)vva = 0x77;

    if (rename(VFAT_A, VFAT_ALIAS) != 0)
        fail("p5-case-rename");
    int vfd = open(VFAT_ALIAS, O_RDONLY);
    if (vfd < 0)
        fail("p5-alias-open");
    void *vrova = map_at(0x40c00000ULL, PAGE, PROT_READ, vfd, 0,
                         "p5-alias-mmap");
    if (*(volatile uint8_t *)vrova != 0x77)
        fail("p5-alias-rendezvous");
    munmap(vrova, PAGE);
    close(vfd);

    if (rename(VFAT_ALIAS, VFAT_RENAMED) != 0)
        fail("p5-plain-rename");
    vfd = open(VFAT_RENAMED, O_RDONLY);
    if (vfd < 0)
        fail("p5-renamed-open");
    vrova = map_at(0x40d00000ULL, PAGE, PROT_READ, vfd, 0,
                   "p5-renamed-mmap");
    if (*(volatile uint8_t *)vrova != 0x77)
        fail("p5-renamed-rendezvous");
    munmap(vrova, PAGE);
    close(vfd);
    munmap(vva, PAGE);
    close(fd);
    if (unlink(VFAT_RENAMED) != 0)
        fail("p5-cleanup");

    /* ── Phase 6: exec teardown ─────────────────────────────────── */
    fd = open(EXEC_FIXTURE, O_CREAT | O_RDWR);
    if (fd < 0)
        fail("p6-create");
    {
        char blk[PAGE];
        memset(blk, 0x3A, sizeof(blk));
        xwrite(fd, blk, PAGE, "p6-bk0");
        memset(blk, 0x3B, sizeof(blk));
        xwrite(fd, blk, PAGE, "p6-bk1");
    }
    if (pipe(p1) || pipe(p2))
        fail("p6-pipe");
    snprintf(fdspec, sizeof(fdspec), "%d:%d", p1[0], p2[1]);
    pid = fork();
    if (pid == 0) {
        close(p1[1]);
        close(p2[0]);
        char *args[] = { "/bin/test_map_shared", "execrole",
                         (char *)EXEC_FIXTURE, fdspec, "first", NULL };
        execv("/bin/test_map_shared", args);
        fail("p6-exec");
    }
    close(p1[0]);
    close(p2[1]);
    xread(p2[0], sig, 2, "p6-wait");            /* child modified + is
                                                 * waiting in its only VMA */
    xwrite(p1[1], "go", 2, "p6-go");            /* triggers the exec */
    xread(p2[0], sig, 2, "p6-wait2");           /* post-exec remap saw
                                                 * the backend bytes */
    close(p1[1]);
    close(p2[0]);
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) ||
        WEXITSTATUS(st) != 0)
        fail("p6-wait");
    close(fd);
    unlink(EXEC_FIXTURE);

    /* ── Phase 7: exit teardown ─────────────────────────────────── */
    fd = open(EXIT_FIXTURE, O_CREAT | O_RDWR);
    if (fd < 0)
        fail("p7-create");
    {
        char blk[PAGE];
        memset(blk, 0x4A, sizeof(blk));
        xwrite(fd, blk, PAGE, "p7-bk0");
    }
    pid = fork();
    if (pid == 0) {
        char *args[] = { "/bin/test_map_shared", "exitrole",
                         (char *)EXIT_FIXTURE, NULL };
        execv("/bin/test_map_shared", args);
        fail("p7-exec");
    }
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) ||
        WEXITSTATUS(st) != 0)
        fail("p7-wait");
    /* The child's exit dropped the last reference: the parent's remap
     * refills from the backend, not from the child's modification. */
    void *xva = map_at(0x41700000ULL, PAGE, PROT_READ | PROT_WRITE,
                       fd, 0, "p7-remap");
    if (*(volatile uint8_t *)xva != 0x4A)
        fail("p7-exit-teardown");
    munmap(xva, PAGE);
    close(fd);
    unlink(EXIT_FIXTURE);

    printf("test_map_shared: PASS\n");
    return 0;
}
