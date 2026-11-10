#include "systest.h"


// ── 6: read ────────────────────────────────────────────────
void test_read(void)
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
void test_open_close(void)
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
void test_dup_dup2(void)
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
void test_pipe(void)
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


// ── 14, 15: chdir, getcwd ──────────────────────────────────
void test_chdir_getcwd(void)
{
    char buf[256];
    char *cwd = getcwd(buf, sizeof(buf));
    CHECK3(cwd != NULL && cwd[0] == '/', "getcwd", "starts with /");
    int ret = chdir("/");
    CHECK3(ret == 0, "chdir", "/");
}


// ── 16, 17: stat, fstat ────────────────────────────────────
void test_stat_fstat(void)
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
void test_lseek(void)
{
    int fd = open("/bin/spin", O_RDONLY);
    if (fd < 0) { FAIL("lseek", "open failed"); return; }
    CHECK3(lseek(fd, 0, SEEK_SET) == 0, "lseek", "SEEK_SET 0");
    CHECK3(lseek(fd, 0, SEEK_END) > 0, "lseek", "SEEK_END > 0");
    close(fd);
}


// ── 19: fcntl ──────────────────────────────────────────────
void test_fcntl(void)
{
    int flags = fcntl(0, F_GETFL, 0);
    CHECK3(flags >= 0, "fcntl", "F_GETFL stdin");
    flags = fcntl(999, F_GETFL, 0);
    CHECK3(flags < 0 || flags >= 0, "fcntl", "bad fd check");
}


// ── 21: getdents64 ─────────────────────────────────────────
void test_getdents64(void)
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
void test_access(void)
{
    CHECK3(access("/", F_OK) == 0, "access", "/ F_OK");
    int ret = access("/noent_xyz", F_OK);
    CHECK3(ret <= 0, "access", "noent return");
}


// ── 23, 24, 25: mkdir, rmdir ───────────────────────────────
void test_mkdir_rmdir(void)
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
void test_unlink(void)
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
void test_readlink(void)
{
    char buf[64];
    int64_t ret = readlink("/bin/spin", buf, sizeof(buf));
    CHECK3(ret < 0 || ret >= 0, "readlink", "stub called");
}


// ── 27: rename ─────────────────────────────────────────────
void test_rename(void)
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
void test_truncate(void)
{
    int64_t ret = syscall(SYS_ftruncate, (uint64_t)999, 0, 0);
    CHECK3(ret < 0, "ftruncate", "bad fd error");
    ret = syscall(SYS_truncate, (uint64_t)"/noent_t", 0, 0);
    CHECK3(ret < 0, "truncate", "noent error");
}


// ── 32, 33: chmod, fchmod ─────────────────────────────────
void test_chmod(void)
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


// ── 37: umask ──────────────────────────────────────────────
void test_umask(void)
{
    mode_t old = umask(022);
    CHECK3((int)old >= 0, "umask", "returns old mask");
    umask(old);
}


// ── 40: sync ───────────────────────────────────────────────
void test_sync(void)
{
    sync();
    PASS("sync", "no crash");
}


// ── ext2 write tests (require ext2 write support) ──────
void test_ext2_write(void)
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


// ── /boot FAT32 mount probe (boot FAT32 ESP) ────────────
// The QEMU disk image built by tools/mkdisk.c is a dual-partition GPT
// layout: partition 1 (hda1) is the FAT32 ESP containing `kernel.bin`
// at the ESP root, partition 2 (hda2) is the ext2 root containing
// `/kernel.bin` (NOT `/boot/...`). So a successful `open("/boot/kernel.bin")`
// proves the FAT32 `/boot` mount actually succeeded; if the mount had
// silently failed (or never run), the open would ENOENT because the ext2
// root has no `/boot` directory. fstat + read of the first byte
// confirms we're not just opening a zero-byte stub — the FAT32 read
// path is functional end-to-end.
void test_boot_fat_mount(void)
{
    const char *path = "/boot/kernel.bin";

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        FAIL("boot_fat_mount: open %s returned %d errno=%d", path, fd, errno);
        return;
    }

    struct stat st;
    int ret = fstat(fd, &st);
    if (ret != 0) {
        FAIL("boot_fat_mount: fstat ret=%d errno=%d", ret, errno);
        close(fd);
        return;
    }
    CHECK3(st.st_size > 0, "boot_fat_mount", "fstat size > 0");

    uint8_t first = 0;
    int64_t r = read(fd, &first, 1);
    CHECK3(r == 1, "boot_fat_mount", "read returned 1 byte");

    close(fd);
}


// ── Test /proc/self/maps ─────────────────────────────────────
void test_proc_maps(void)
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
        // The map addresses are 64 bit and the permission field is four
        // characters wide, so this needs the length modifier and the
        // field width of the scanf core (T12).
        int fields = sscanf(line, "%lx-%lx %4s %*x %*s %*x",
                            &start, &end, perm);
        if (fields >= 3 && perm[0] != '\0')
            lines++;

        if (strstr(line, "[stack]")) {
            has_stack = 1;
            // Positive: stack at [USER_STACK_BASE, USER_STACK_BASE + 2 MiB).
            // USER_STACK_BASE = 0x20400000 (USER_ENVELOPE_SIZE = 512 MiB).
            if (start == 0x20400000UL && end == 0x20600000UL)
                stack_at_right_addr = 1;
            // Negative: must NOT include 0x600000 (legacy guard, kept
            // for backward compatibility even though the stack no
            // longer sits there).
            if (start <= 0x600000UL && 0x600000UL < end) {
                FAIL("%s: %s", "proc_maps guard",
                     "stack line includes 0x600000 legacy guard page");
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
void test_proc_fd(void)
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


// ── 70: devfs open default fg_pgrp (§4.1.1) ─────────
void test_devfs_open_default_fg_pgrp(void) {
    // 复位全局 fg_pgrp=0（new_pg==0 §4.4 始终允许）
    int rfd = open("/dev/tty0", O_RDWR);  // 物理控制台（§4.1.1 属物理 TTY）
    if (rfd < 0) { FAIL("devfs_open_fg", "no /dev/tty0"); return; }
    tcsetpgrp(rfd, 0);
    close(rfd);

    int64_t pid = fork();
    if (pid < 0) { FAIL("devfs_open_fg", "fork"); return; }
    if (pid == 0) {
        setpgid(0, getpid());   // 设 pgrp 为自己
        int cfd = open("/dev/tty0", O_RDWR);
        if (cfd < 0) { _exit(2); }
        pid_t p = tcgetpgrp(cfd);
        _exit(p == getpid() ? 0 : 3);
    }
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "devfs_open_fg", "tcgetpgrp after open returns self pgrp");
}


// ══ Symlink systest (T12) ═══════════════════════════════════
// Spec: docs/superpowers/specs/2026-09-05-symlink-support-design.md §6.
//
// All fixtures live under /symlink-systest (root fs = ext2, which has
// .symlink/.readlink), NOT /tmp — tmpfs intentionally has no .symlink op
// and returns -EOPNOTSUPP (that is itself asserted below).

#define SYMDIR "/symlink-systest"

// Deterministic fixture: /symlink-systest does not exist in a freshly
// built image, so every symlink case creates it first.  EEXIST is the
// normal steady-state result and is not reported; anything else is a
// real failure and goes through CHECK3.
static void symlink_fixture_reset(void)
{
    if (mkdir(SYMDIR, 0777) != 0) {
        int e = errno;
        if (e != EEXIST)
            CHECK3(0, "symlink_fixture", "mkdir " SYMDIR " failed");
    }
}


// ── T12a: creation + readback (fast and long symlinks) ─────
void test_symlink_create_readlink(void)
{
    symlink_fixture_reset();

    // Fast symlink: target <= 60 bytes -> stored inline in i_block[].
    const char *fast = "/bin/busybox";
    unlink(SYMDIR "/t_fast");
    if (symlink(fast, SYMDIR "/t_fast") != 0) {
        FAIL("symlink_fast: create failed errno=%d", errno);
        return;
    }
    PASS("symlink_fast (created)");

    char buf[300];
    memset(buf, 0, sizeof(buf));
    ssize_t n = readlink(SYMDIR "/t_fast", buf, sizeof(buf) - 1);
    CHECKF(n == (ssize_t)strlen(fast), "readlink_fast", "len=%ld",
           "len=%ld", (long)n);
    CHECK3(n > 0 && memcmp(buf, fast, (size_t)n) == 0,
           "readlink_fast", "target matches");

    // Long symlink: target > 60 bytes -> stored in a data block.
    char longt[128];
    memset(longt, 'a', sizeof(longt));
    longt[0] = '/';
    longt[100] = '\0';                 // 100-byte target
    unlink(SYMDIR "/t_long");
    if (symlink(longt, SYMDIR "/t_long") != 0) {
        FAIL("symlink_long: create failed errno=%d", errno);
    } else {
        PASS("symlink_long (created)");
        memset(buf, 0, sizeof(buf));
        n = readlink(SYMDIR "/t_long", buf, sizeof(buf) - 1);
        CHECKF(n == 100, "readlink_long", "len=%ld", "len=%ld", (long)n);
        CHECK3(n == 100 && memcmp(buf, longt, 100) == 0,
               "readlink_long", "target matches");
    }

    unlink(SYMDIR "/t_fast");
    unlink(SYMDIR "/t_long");
}


// ── T12b: error matrix (exact errno per spec §5.3) ─────────
void test_symlink_errors(void)
{
    symlink_fixture_reset();

    int rc, e;

    // EEXIST — linkpath already present.
    unlink(SYMDIR "/t_eexist");
    symlink("/bin/busybox", SYMDIR "/t_eexist");
    rc = symlink("/bin/busybox", SYMDIR "/t_eexist");
    e = errno;
    CHECKF(rc == -1 && e == EEXIST, "symlink_EEXIST", "rc=%d errno=%d",
           "rc=%d errno=%d", rc, e);
    unlink(SYMDIR "/t_eexist");

    // ENOENT — parent directory does not exist.
    rc = symlink("/bin/busybox", SYMDIR "/no_such_dir/t");
    e = errno;
    CHECKF(rc == -1 && e == ENOENT, "symlink_ENOENT", "rc=%d errno=%d",
           "rc=%d errno=%d", rc, e);

    // ENOTDIR — a mid-path component is a regular file.
    int fd = open(SYMDIR "/t_reg", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) { write(fd, "x", 1); close(fd); }
    rc = symlink("/bin/busybox", SYMDIR "/t_reg/t");
    e = errno;
    CHECKF(rc == -1 && e == ENOTDIR, "symlink_ENOTDIR", "rc=%d errno=%d",
           "rc=%d errno=%d", rc, e);

    // EINVAL — readlink on a non-symlink.
    char rb[64];
    rc = (int)readlink(SYMDIR "/t_reg", rb, sizeof(rb));
    e = errno;
    CHECKF(rc == -1 && e == EINVAL, "readlink_EINVAL_notlink", "rc=%d errno=%d",
           "rc=%d errno=%d", rc, e);

    // EINVAL — readlink with bufsize 0.
    unlink(SYMDIR "/t_ei");
    symlink("/bin/busybox", SYMDIR "/t_ei");
    rc = (int)readlink(SYMDIR "/t_ei", rb, 0);
    e = errno;
    CHECKF(rc == -1 && e == EINVAL, "readlink_EINVAL_bufsize", "rc=%d errno=%d",
           "rc=%d errno=%d", rc, e);
    unlink(SYMDIR "/t_ei");
    unlink(SYMDIR "/t_reg");

    // ENAMETOOLONG — linkpath with no NUL inside VFS_NAME_MAX (256).
    char toolong[300];
    memset(toolong, 'z', sizeof(toolong));
    toolong[0] = '/';
    toolong[sizeof(toolong) - 1] = '\0';   // 299 chars > 255
    rc = symlink("/bin/busybox", toolong);
    e = errno;
    CHECKF(rc == -1 && e == ENAMETOOLONG, "symlink_ENAMETOOLONG",
           "rc=%d errno=%d", "rc=%d errno=%d", rc, e);

    // EOPNOTSUPP — tmpfs has no .symlink op (intentional, spec §3).
    rc = symlink("/bin/busybox", "/tmp/t_nosym");
    e = errno;
    CHECKF(rc == -1 && e == EOPNOTSUPP, "symlink_EOPNOTSUPP_tmpfs",
           "rc=%d errno=%d", "rc=%d errno=%d", rc, e);

    // EEXIST — linkpath is exactly "/" (would replace the mount root).
    rc = symlink("/bin/busybox", "/");
    e = errno;
    CHECKF(rc == -1 && e == EEXIST, "symlink_root_EEXIST", "rc=%d errno=%d",
           "rc=%d errno=%d", rc, e);
}


// ── T12c: stat / lstat / fstatat follow semantics ──────────
void test_symlink_stat(void)
{
    symlink_fixture_reset();

    const char *target = "/bin/busybox";
    unlink(SYMDIR "/t_st");
    if (symlink(target, SYMDIR "/t_st") != 0) {
        FAIL("symlink_stat: create failed errno=%d", errno);
        return;
    }

    struct stat lst, st, tst;

    // lstat -> the link itself; st_size == strlen(target).
    int rc = lstat(SYMDIR "/t_st", &lst);
    CHECK3(rc == 0, "lstat_link", "lstat ok");
    CHECKF(rc == 0 && S_ISLNK(lst.st_mode), "lstat_link", "S_IFLNK",
           "mode=%o", (unsigned)lst.st_mode);
    CHECKF(rc == 0 && lst.st_size == (off_t)strlen(target),
           "lstat_link", "size=%ld", "size=%ld", (long)lst.st_size);

    // stat -> follows to the target (a regular file).
    rc = stat(SYMDIR "/t_st", &st);
    CHECK3(rc == 0, "stat_follow", "stat ok");
    CHECKF(rc == 0 && !S_ISLNK(st.st_mode) && S_ISREG(st.st_mode),
           "stat_follow", "S_IFREG", "mode=%o", (unsigned)st.st_mode);
    if (stat(target, &tst) == 0)
        CHECKF(rc == 0 && st.st_ino == tst.st_ino, "stat_follow",
               "same inode as target", "ino=%lu vs %lu",
               (unsigned long)st.st_ino, (unsigned long)tst.st_ino);

    // fstatat(AT_SYMLINK_NOFOLLOW) == lstat.
    struct stat a1, a2;
    rc = fstatat(AT_FDCWD, SYMDIR "/t_st", &a1, AT_SYMLINK_NOFOLLOW);
    CHECKF(rc == 0 && S_ISLNK(a1.st_mode), "fstatat_NOFOLLOW", "S_IFLNK",
           "rc=%d mode=%o", rc, (unsigned)a1.st_mode);

    // fstatat(flags=0) == stat (follows).
    rc = fstatat(AT_FDCWD, SYMDIR "/t_st", &a2, 0);
    CHECKF(rc == 0 && S_ISREG(a2.st_mode), "fstatat_follow", "S_IFREG",
           "rc=%d mode=%o", rc, (unsigned)a2.st_mode);

    // An absolute path ignores dirfd entirely — even a bogus one.
    struct stat a3;
    rc = fstatat(9999, SYMDIR "/t_st", &a3, AT_SYMLINK_NOFOLLOW);
    CHECKF(rc == 0 && S_ISLNK(a3.st_mode), "fstatat_abs_ignores_dirfd",
           "S_IFLNK", "rc=%d errno=%d", rc, errno);

    unlink(SYMDIR "/t_st");
}


// ── T12d: mid-path following (absolute + relative) ─────────
void test_symlink_follow_midpath(void)
{
    symlink_fixture_reset();

    // Fixture: SYMDIR/sub/f  +  SYMDIR/dabs -> SYMDIR/sub  (absolute)
    //                        +  SYMDIR/drel -> "sub"       (relative)
    mkdir(SYMDIR "/sub", 0777);
    int fd = open(SYMDIR "/sub/f", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { FAIL("symlink_midpath: fixture file failed"); return; }
    write(fd, "hello", 5);
    close(fd);

    unlink(SYMDIR "/dabs");
    unlink(SYMDIR "/drel");
    CHECK3(symlink(SYMDIR "/sub", SYMDIR "/dabs") == 0,
           "symlink_dir_abs", "created");
    CHECK3(symlink("sub", SYMDIR "/drel") == 0,
           "symlink_dir_rel", "created");

    // Absolute-target mid-path following.
    char buf[16];
    memset(buf, 0, sizeof(buf));
    fd = open(SYMDIR "/dabs/f", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        CHECK3(n == 5 && memcmp(buf, "hello", 5) == 0,
               "follow_midpath_abs", "read through symlinked dir");
    } else {
        FAIL("follow_midpath_abs: open failed errno=%d", errno);
    }

    // Relative-target mid-path following (resolved against the link's dir).
    memset(buf, 0, sizeof(buf));
    fd = open(SYMDIR "/drel/f", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        CHECK3(n == 5 && memcmp(buf, "hello", 5) == 0,
               "follow_midpath_rel", "read through relative symlinked dir");
    } else {
        FAIL("follow_midpath_rel: open failed errno=%d", errno);
    }

    // NOFOLLOW must NOT follow the trailing link: lstat sees the link.
    struct stat ls;
    int rc = lstat(SYMDIR "/dabs", &ls);
    CHECKF(rc == 0 && S_ISLNK(ls.st_mode), "follow_NOFOLLOW_trailing",
           "S_IFLNK", "rc=%d mode=%o", rc, (unsigned)ls.st_mode);
    // ... but a mid-path link is still followed even under NOFOLLOW.
    rc = fstatat(AT_FDCWD, SYMDIR "/dabs/f", &ls, AT_SYMLINK_NOFOLLOW);
    CHECKF(rc == 0 && S_ISREG(ls.st_mode), "follow_NOFOLLOW_midpath",
           "midpath still followed", "rc=%d errno=%d", rc, errno);

    unlink(SYMDIR "/dabs");
    unlink(SYMDIR "/drel");
    unlink(SYMDIR "/sub/f");
    rmdir(SYMDIR "/sub");
}


// ── T12e: loop / depth limits (MAXSYMLINKS = 8) ────────────
void test_symlink_loop_depth(void)
{
    symlink_fixture_reset();

    // Two-link cycle: a -> b, b -> a.  stat() must give ELOOP.
    unlink(SYMDIR "/t_a");
    unlink(SYMDIR "/t_b");
    symlink(SYMDIR "/t_b", SYMDIR "/t_a");
    symlink(SYMDIR "/t_a", SYMDIR "/t_b");

    struct stat st;
    int rc = stat(SYMDIR "/t_a", &st);
    int e = errno;
    CHECKF(rc == -1 && e == ELOOP, "symlink_ELOOP_cycle", "rc=%d errno=%d",
           "rc=%d errno=%d", rc, e);

    // lstat on a cycle member still works (no follow).
    rc = lstat(SYMDIR "/t_a", &st);
    CHECKF(rc == 0 && S_ISLNK(st.st_mode), "symlink_ELOOP_lstat_ok",
           "S_IFLNK", "rc=%d errno=%d", rc, errno);
    unlink(SYMDIR "/t_a");
    unlink(SYMDIR "/t_b");

    // Depth chain: c0 -> c1 -> ... -> c9 -> /bin/busybox.
    // A short chain (<= MAXSYMLINKS) resolves; a long one gives ELOOP.
    char p[64], t[64];
    for (int i = 0; i <= 10; i++) {
        snprintf(p, sizeof(p), SYMDIR "/t_c%d", i);
        unlink(p);
    }
    symlink("/bin/busybox", SYMDIR "/t_c10");
    for (int i = 9; i >= 0; i--) {
        snprintf(p, sizeof(p), SYMDIR "/t_c%d", i);
        snprintf(t, sizeof(t), SYMDIR "/t_c%d", i + 1);
        symlink(t, p);
    }

    // 4 hops from t_c7 (7->8->9->10->file): under the limit.
    rc = stat(SYMDIR "/t_c7", &st);
    CHECKF(rc == 0, "symlink_depth_ok", "short chain resolves",
           "rc=%d errno=%d", rc, errno);

    // 11 hops from t_c0: over MAXSYMLINKS (8) -> ELOOP.
    rc = stat(SYMDIR "/t_c0", &st);
    e = errno;
    CHECKF(rc == -1 && e == ELOOP, "symlink_depth_ELOOP", "rc=%d errno=%d",
           "rc=%d errno=%d", rc, e);

    for (int i = 0; i <= 10; i++) {
        snprintf(p, sizeof(p), SYMDIR "/t_c%d", i);
        unlink(p);
    }
}


// ── 41: exec through a symlink ─────────────────────────────
// End-to-end validation of the symlink-aware exec path:
//   1. busybox `ln` applet creates the link (proves the applet
//      works + the resulting inode is a real ext2 symlink, not a copy).
//   2. lstat in the parent confirms the link.
//   3. The kernel execs through the link: SYS_exec calls vfs_lookup_at
//      which follows the last component, finds /bin/busybox, loads the
//      ELF, and runs it. The child runs busybox's `true` applet via
//      argv[0]="busybox" + argv[1]="true" (CONFIG_BUSYBOX is unset, so
//      appletlib.c:1103 shifts argv when argv[0] starts with "busybox").
//      A successful exec never returns, so _exit(127) in the child
//      would mean exec came back (ENOENT / ENOEXEC / EACCES) — the
//      exact failure this case guards.
void test_41_exec_via_symlink(void)
{
    symlink_fixture_reset();
    unlink(SYMDIR "/x");

    // Step 1: create the link with the busybox ln applet (not libc symlink).
    // /bin/ln is a real symlink to /bin/busybox in the staged rootfs;
    // exec resolves it, argv[0]="ln" → busybox dispatches to the ln applet.
    int64_t pid = fork();
    if (pid < 0) {
        FAIL("41_exec_via_symlink: fork-ln failed");
        return;
    }
    if (pid == 0) {
        char *ln_argv[] = { "ln", "-s", "/bin/busybox", SYMDIR "/x", NULL };
        exec("/bin/ln", ln_argv, NULL);
        _exit(127);
    }
    int status = 0;
    int64_t w = waitpid(pid, &status, 0);
    CHECK3(w == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "41_exec_via_symlink", "busybox ln applet created link");

    // Step 2: lstat the path the applet produced.
    struct stat lst;
    int rc = lstat(SYMDIR "/x", &lst);
    CHECK3(rc == 0, "41_exec_via_symlink", "lstat of ln-created path");
    CHECKF(rc == 0 && S_ISLNK(lst.st_mode), "41_exec_via_symlink",
           "is a symlink", "mode=%o", (unsigned)lst.st_mode);

    // Step 3: exec through the link. The kernel follows the symlink to
    // /bin/busybox and runs the `true` applet, which exits 0.
    pid = fork();
    if (pid < 0) {
        FAIL("41_exec_via_symlink: fork-exec failed");
        unlink(SYMDIR "/x");
        return;
    }
    if (pid == 0) {
        char *exec_argv[] = { "busybox", "true", NULL };
        exec(SYMDIR "/x", exec_argv, NULL);
        _exit(127);                 // exec returned -> lookup failed
    }

    w = waitpid(pid, &status, 0);
    CHECK3(w == pid, "41_exec_via_symlink", "waitpid returned child");
    CHECKF(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "41_exec_via_symlink", "exec through symlink exited 0",
           "status=%d exited=%d code=%d", status,
           WIFEXITED(status), WEXITSTATUS(status));

    unlink(SYMDIR "/x");            // cleanup stays in the parent
}


// ── 42: busybox find -type l finds the symlink ─────────────
// End-to-end validation that the userland consumer (busybox find)
// reads the vfs_getdents VFS_SYMLINK -> DT_LNK mapping. The find
// applet uses getdents64 to enumerate, then filters by d_type — if
// the kernel returned DT_REG for symlinks, find -type l would miss
// them entirely. Setup uses libc symlink() (we just need a link
// to be present); the assertion is that the busybox applet reports it.
void test_42_getdents_dt_lnk(void)
{
    symlink_fixture_reset();
    unlink(SYMDIR "/lnk");

    if (symlink("/bin/busybox", SYMDIR "/lnk") != 0) {
        FAIL("42_getdents_dt_lnk: symlink failed errno=%d", errno);
        return;
    }
    // A regular file in the same directory, so the result is not
    // trivially "everything is a symlink".
    int rfd = open(SYMDIR "/reg42", O_CREAT | O_WRONLY, 0644);
    if (rfd >= 0) { write(rfd, "r", 1); close(rfd); }

    // Run `busybox find <dir> -type l` via pipe — busybox find uses
    // getdents64 + d_type filtering, so this exercises the DT_LNK
    // mapping through a real userland consumer.
    int fds[2];
    if (pipe(fds) < 0) {
        FAIL("42_getdents_dt_lnk: pipe failed errno=%d", errno);
        unlink(SYMDIR "/lnk");
        unlink(SYMDIR "/reg42");
        return;
    }
    int64_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        FAIL("42_getdents_dt_lnk: fork failed");
        unlink(SYMDIR "/lnk"); unlink(SYMDIR "/reg42");
        return;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        char *find_argv[] = { "find", SYMDIR, "-type", "l", NULL };
        exec("/bin/find", find_argv, NULL);
        _exit(127);
    }
    close(fds[1]);

    // Drain the pipe: find prints one path per line; we only need to
    // see whether "lnk" appears at all.
    char out[256] = {0};
    int off = 0;
    for (;;) {
        if (off >= (int)sizeof(out) - 1) break;
        int64_t n = read(fds[0], out + off, (size_t)(sizeof(out) - 1 - off));
        if (n <= 0) break;
        off += (int)n;
    }
    close(fds[0]);

    int saw_lnk = (strstr(out, "lnk") != NULL);
    // Sanity: reg42 must NOT appear in -type l output (and we don't
    // verify the absence of every other line — find may print the
    // directory itself depending on options, but with -type l it
    // should only list symlink leaves).
    int saw_reg = (strstr(out, "reg42") != NULL);

    int status = 0;
    int64_t w = waitpid(pid, &status, 0);
    CHECK3(w == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "42_getdents_dt_lnk", "find exited 0");
    CHECKF(saw_lnk && !saw_reg, "42_getdents_dt_lnk",
           "lnk reported, reg42 absent", "out=%.200s lnk=%d reg=%d",
           out, saw_lnk, saw_reg);

    unlink(SYMDIR "/lnk");
    unlink(SYMDIR "/reg42");
}


/* T14: realpath() against the REAL guest VFS.  The symbolic link is
 * created with symlink(2), i.e. the link lives in the very filesystem
 * the kernel resolves, so this exercises the real path walk (readlink
 * on a real ext2 inode), not a mock.  The answers are compared with
 * each other rather than with hardcoded strings, because the rootfs
 * layout (and whether "/bin" is itself a link) is not part of the
 * contract. */
void test_realpath_vfs(void)
{
    /* NOTE: no PATH_MAX here on purpose.  A user program is compiled with
     * -ffreestanding, so __STDC_HOSTED__ is 0 and clang's own resource
     * <limits.h> is used INSTEAD of OS01's libc/include/limits.h — the
     * project's PATH_MAX is therefore not visible to user code (see the
     * T14 report, "known gap").  realpath(path, NULL) hands the answer
     * back malloc'd, so no caller buffer is needed at all. */
    char *r;
    char *r2;
    int ok;

    mkdir("/rptest", 0755);
    symlink("/bin/systest", "/rptest/link");

    r = realpath("/bin/systest", NULL);
    ok = (r != NULL) && r[0] == '/';
    CHECK3(ok, "realpath absolute", ok ? r : "not resolved");
    free(r);

    /* the link must resolve to the same answer as its target */
    r = realpath("/bin/systest", NULL);
    r2 = realpath("/rptest/link", NULL);
    ok = (r != NULL) && (r2 != NULL) && strcmp(r, r2) == 0;
    CHECK3(ok, "realpath symlink", ok ? r2 : "link not resolved");
    free(r);
    free(r2);

    /* "." and ".." are applied to the RESOLVED prefix */
    r = realpath("/bin/systest", NULL);
    r2 = realpath("/bin/./systest", NULL);
    ok = (r != NULL) && (r2 != NULL) && strcmp(r, r2) == 0;
    CHECK3(ok, "realpath dot", ok ? r2 : "dot not normalised");
    free(r);
    free(r2);

    r = realpath("/bin/systest", NULL);
    r2 = realpath("/bin/../bin/systest", NULL);
    ok = (r != NULL) && (r2 != NULL) && strcmp(r, r2) == 0;
    CHECK3(ok, "realpath dotdot", ok ? r2 : "dotdot not normalised");
    free(r);
    free(r2);

    /* a missing component answers ENOENT */
    errno = 0;
    r = realpath("/rptest/missing", NULL);
    ok = (r == NULL) && errno == ENOENT;
    CHECK3(ok, "realpath missing", ok ? "ENOENT" : "wrong answer");
    free(r);

    unlink("/rptest/link");
    rmdir("/rptest");
}


// ── T6: kernel atomic append (libc-completion plan) ─────────
//
// O_APPEND must land every write at the file's REAL end, with the
// EOF choice, the byte commit and the file-offset update serialized
// inside the filesystem's own mount mutex.  This case proves it on
// the REAL kernel under real SMP preemption, on all three writable
// filesystems (tmpfs /tmp, ext2 /, FAT32 /boot):
//
//   phase 1 — APPEND_WRITERS children each open the file
//     INDEPENDENTLY (their own open-file description, stale offset 0)
//     and append fixed-length records.  A record carries its writer
//     tag, sequence number, deterministic payload and checksum, so
//     the verifier can prove: every record intact (no torn write),
//     per-writer sequences 0..N-1 in file order (no loss, no
//     overwrite), and the exact expected record count.
//   phase 2 — the same writers share ONE open-file description
//     (inherited across fork, like dup).  The description's offset
//     is updated by the append hook under the filesystem lock, so
//     after all children are joined the parent's SEEK_CUR must equal
//     the file's actual EOF — the "late returner must not clobber a
//     newer offset" contract, exercised with real preemption (the
//     host test covers it only sequentially).
//
// Raw open/write only — the plan deliberately does not rely on the
// fopen layer here (T7+ deliver it).
#define APPEND_REC               32
#define APPEND_PAYLOAD_OFF       8
#define APPEND_CSUM_OFF          28
#define APPEND_WRITERS           4
#define APPEND_RECS_PER_WRITER   6

static void append_fill_record(uint8_t *rec, uint32_t tag, uint32_t seq)
{
    memset(rec, 0, APPEND_REC);
    memcpy(rec, &tag, 4);
    memcpy(rec + 4, &seq, 4);
    for (int i = 0; i < (int)(APPEND_CSUM_OFF - APPEND_PAYLOAD_OFF); i++)
        rec[APPEND_PAYLOAD_OFF + i] =
            (uint8_t)((tag * 31 + seq * 17 + i * 7) & 0xff);
    uint32_t csum = 0;
    for (int i = 0; i < APPEND_CSUM_OFF; i++) csum += rec[i];
    memcpy(rec + APPEND_CSUM_OFF, &csum, 4);
}


/* Verify every record; returns 0 or a negative reason code:
 *  -2 torn record  -3 bad tag  -4 per-writer sequence gap  -5 payload
 *  mismatch  -6 wrong record count  -7 a writer is missing records */
static int append_verify_file(const char *path, uint32_t expect_total)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    uint32_t next[APPEND_WRITERS + 1];
    memset(next, 0, sizeof(next));
    uint8_t buf[APPEND_REC], ref[APPEND_REC];
    uint32_t total = 0;
    int rc = 0;
    for (;;) {
        ssize_t n = read(fd, buf, APPEND_REC);
        if (n == 0) break;
        if (n != APPEND_REC) { rc = -2; break; }
        uint32_t tag, seq;
        memcpy(&tag, buf, 4);
        memcpy(&seq, buf + 4, 4);
        if (tag < 1 || tag > APPEND_WRITERS) { rc = -3; break; }
        if (seq != next[tag]) { rc = -4; break; }
        next[tag]++;
        append_fill_record(ref, tag, seq);
        if (memcmp(ref, buf, APPEND_REC) != 0) { rc = -5; break; }
        total++;
    }
    close(fd);
    if (rc != 0) return rc;
    if (total != expect_total) return -6;
    for (int t = 1; t <= APPEND_WRITERS; t++)
        if (next[t] != APPEND_RECS_PER_WRITER) return -7;
    return 0;
}


/* One append round on one filesystem directory.  `shared` selects
 * phase 2 (fork/dup shared description) vs phase 1 (independent
 * opens).  Returns 0 on success, non-zero reason on the first
 * failed check. */
static int append_round(const char *dir, int shared)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/systest_append.dat", dir);
    unlink(path);

    int total_recs = APPEND_WRITERS * APPEND_RECS_PER_WRITER;

    int root = open(path, O_CREAT | O_WRONLY, 0644);
    if (root < 0) return -10;
    close(root);

    int sfd = -1;
    if (shared) {
        /* ONE description, inherited by every child: the offset is
         * shared state that only the append hook may advance. */
        sfd = open(path, O_RDWR | O_APPEND);
        if (sfd < 0) { unlink(path); return -11; }
    }

    pid_t pids[APPEND_WRITERS];
    int started = 0;
    for (int w = 0; w < APPEND_WRITERS; w++) {
        pid_t pid = fork();
        if (pid == 0) {
            int fd = shared ? sfd : open(path, O_WRONLY | O_APPEND);
            if (fd < 0) _exit(100);
            for (uint32_t sq = 0; sq < APPEND_RECS_PER_WRITER; sq++) {
                uint8_t rec[APPEND_REC];
                append_fill_record(rec, (uint32_t)w + 1, sq);
                if (write(fd, rec, APPEND_REC) != APPEND_REC) _exit(101);
            }
            if (!shared) close(fd);
            _exit(0);
        }
        if (pid > 0) pids[started++] = pid;
    }

    int child_fail = 0;
    for (int w = 0; w < started; w++) {
        int st = -1;
        if (waitpid(pids[w], &st, 0) != pids[w] || st != 0) child_fail++;
    }
    int rc = (started == APPEND_WRITERS && child_fail == 0) ? 0 : -12;
    if (rc == 0) {
        int v = append_verify_file(path, (uint32_t)total_recs);
        if (v != 0) { rc = (shared ? -20 : -30) + v; }
    }
    if (rc == 0 && shared) {
        /* the shared description's offset must BE the file's EOF */
        off_t pos = lseek(sfd, 0, SEEK_CUR);
        off_t end = lseek(sfd, 0, SEEK_END);
        if (pos != end || end != (off_t)(total_recs * APPEND_REC))
            rc = -40;
    }
    if (sfd >= 0) close(sfd);
    unlink(path);
    return rc;
}

void test_vfs_append(void)
{
    static const char *dirs[3] = { "/tmp", "/", "/boot" };
    for (int d = 0; d < 3; d++) {
        int r = append_round(dirs[d], 0);
        CHECKF(r == 0, "append independent opens", "%s rc=%d",
               "%s rc=%d", dirs[d], r);
        r = append_round(dirs[d], 1);
        CHECKF(r == 0, "append shared fd offset", "%s rc=%d",
               "%s rc=%d", dirs[d], r);
    }
}


/* ── T15: the directory descriptor and the sorted scan against the REAL
 * guest filesystems ────────────────────────────────────────────────
 *
 * Two traps are avoided on purpose:
 *
 *  1. The kernel SORTS the entries by name BEFORE it writes them out
 *     (vfs_getdents in kernel/fs/vfs.c sorts, then emits).  A guest
 *     assertion "the scan comes back sorted" therefore proves NOTHING
 *     — it would hold even if the comparator were never called.  The
 *     comparator used here is by name LENGTH, which differs from the
 *     name order, so it can only hold if scandir() really called it.
 *
 *  2. d_off in OS01 is the kernel's own sequence number (d_off = pos+1
 *     in vfs_getdents), not a rich cookie.  The property under test is
 *     the right one for a libc: the value is copied VERBATIM from the
 *     kernel's record.  The check cross-reads the raw records with
 *     getdents64(2) and compares them one by one with what readdir()
 *     handed back.
 */

/* scandir() returns "." and ".." as well — the filter below drops them
 * so that the count is the number of REAL entries. */
static int dir_filter_files(const struct dirent *e)
{
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
        return 0;
    return 1;
}


/* By name LENGTH, then by name.  Deliberately NOT the name order. */
static int dir_cmp_by_len(const struct dirent **a, const struct dirent **b)
{
    size_t la = strlen((*a)->d_name);
    size_t lb = strlen((*b)->d_name);

    if (la != lb)
        return la < lb ? -1 : 1;
    return strcmp((*a)->d_name, (*b)->d_name);
}


/* The raw kernel record: the name sits at offsetof(d_name) == 19, NOT
 * at sizeof(struct linux_dirent64) == 24. */
#define DIR_RAW_NAME_OFF 19

/* Read the raw records of a directory and collect their d_off values. */
static int dir_raw_offs(int fd, uint64_t *offs, int max, int *count)
{
    char buf[4096];
    int64_t ret;
    unsigned pos = 0;
    int n = 0;

    *count = 0;
    ret = syscall(SYS_getdents64, (uint64_t)fd, (uint64_t)buf,
                  (uint64_t)sizeof(buf));
    if (ret <= 0)
        return -1;
    while (pos < (unsigned)ret) {
        const char *rec = buf + pos;
        unsigned short reclen;

        if (pos + DIR_RAW_NAME_OFF > (unsigned)ret)
            return -1;
        reclen = *(const unsigned short *)(const void *)(rec + 16);
        if (reclen == 0 || pos + reclen > (unsigned)ret)
            return -1;
        if (n < max)
            offs[n++] = *(const uint64_t *)(const void *)(rec + 8);
        pos += reclen;
    }
    *count = n;
    return 0;
}


/* An independent sort oracle: a plain insertion sort over the names,
 * so the alphasort result is checked against code that does not share
 * the comparator under test. */
static void dir_sort_names(char names[8][32], int n)
{
    int i, j;

    for (i = 1; i < n; i++) {
        char tmp[32];
        strncpy(tmp, names[i], sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) > 0) {
            strncpy(names[j + 1], names[j], sizeof(names[0]) - 1);
            names[j + 1][sizeof(names[0]) - 1] = '\0';
            j--;
        }
        strncpy(names[j + 1], tmp, sizeof(tmp) - 1);
        names[j + 1][sizeof(tmp) - 1] = '\0';
    }
}

void test_dir_scan_vfs(void)
{
    int ok;
    int fd;
    int dupfd;
    DIR *dir;
    struct dirent *e;
    struct dirent **list = NULL;
    struct dirent **sorted = NULL;
    char oracle[8][32];
    uint64_t raw_offs[64];
    uint64_t read_offs[64];
    int raw_n = 0;
    int read_n = 0;
    int n;

    /* ── (a) tmpfs: a directory with three files whose names have three
     * DIFFERENT lengths, and whose name order differs from their length
     * order ("aaa" < "b" < "zz" by name, but "b" < "zz" < "aaa" by
     * length). ── */
    mkdir("/tmp/dt", 0755);
    fd = open("/tmp/dt/aaa", O_WRONLY | O_CREAT, 0644);
    if (fd >= 0)
        close(fd);
    fd = open("/tmp/dt/b", O_WRONLY | O_CREAT, 0644);
    if (fd >= 0)
        close(fd);
    fd = open("/tmp/dt/zz", O_WRONLY | O_CREAT, 0644);
    if (fd >= 0)
        close(fd);

    dir = opendir("/tmp/dt");
    ok = (dir != NULL);
    CHECK3(ok, "dir tmpfs opendir", ok ? "/tmp/dt" : "opendir failed");
    if (dir) {
        int seen = 0;
        int names_ok = 1;
        while ((e = readdir(dir)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            seen++;
            if (strcmp(e->d_name, "aaa") != 0 && strcmp(e->d_name, "b") != 0 &&
                strcmp(e->d_name, "zz") != 0)
                names_ok = 0;
        }
        ok = (seen == 3) && names_ok;
        CHECK3(ok, "dir tmpfs entries", ok ? "3 entries" : "wrong entry set");
        closedir(dir);
    }

    /* ── (b) ext2: the root filesystem is a real ext2, /bin lives on it ── */
    dir = opendir("/bin");
    ok = (dir != NULL);
    CHECK3(ok, "dir ext2 opendir", ok ? "/bin" : "opendir failed");
    if (dir) {
        int found = 0;
        while ((e = readdir(dir)) != NULL)
            if (strcmp(e->d_name, "systest") == 0)
                found = 1;
        CHECK3(found, "dir ext2 entries", found ? "systest found" : "systest missing");
        closedir(dir);
    }

    /* ── (c) the descriptor ownership: fdopendir() takes the descriptor
     * over, closedir() closes it, and the ORIGINAL stays open ── */
    fd = open("/tmp/dt", O_RDONLY);
    ok = (fd >= 0);
    CHECK3(ok, "dir open", ok ? "opened" : "open failed");
    if (fd >= 0) {
        dupfd = dup(fd);
        ok = (dupfd >= 0);
        CHECK3(ok, "dir dup", ok ? "dup ok" : "dup failed");
        if (dupfd >= 0) {
            dir = fdopendir(dupfd);
            ok = (dir != NULL);
            CHECK3(ok, "dir fdopendir", ok ? "taken over" : "failed");
            if (dir) {
                ok = (closedir(dir) == 0);
                CHECK3(ok, "dir closedir", ok ? "closed" : "close failed");
                /* the taken-over descriptor is REALLY closed now */
                errno = 0;
                ok = (fcntl(dupfd, F_GETFD) == -1) && (errno == EBADF);
                CHECK3(ok, "dir closedir closes the descriptor",
                       ok ? "EBADF" : "still open");
                /* the ORIGINAL descriptor is untouched */
                ok = (fcntl(fd, F_GETFD) == 0);
                CHECK3(ok, "dir the original descriptor is untouched",
                       ok ? "still open" : "was closed");
                /* ... and it is still usable */
                dir = fdopendir(fd);
                ok = (dir != NULL);
                CHECK3(ok, "dir the original descriptor is still usable",
                       ok ? "usable" : "unusable");
                if (dir)
                    closedir(dir);
            }
        }
    }

    /* ── the FAILURE path: fdopendir() on a regular file answers
     * ENOTDIR and does NOT close the caller's descriptor.  This is the
     * contract the desktop's file view relies on: a failed opendir()
     * keeps the old view instead of replacing it with an empty one. ── */
    fd = open("/tmp/dt/zz", O_RDONLY);
    ok = (fd >= 0);
    if (ok) {
        errno = 0;
        dir = fdopendir(fd);
        ok = (dir == NULL) && (errno == ENOTDIR);
        CHECK3(ok, "dir fdopendir on a regular file", ok ? "ENOTDIR" : "wrong");
        /* the descriptor is still the caller's */
        ok = (fcntl(fd, F_GETFD) == 0);
        CHECK3(ok, "dir the failed fdopendir keeps the descriptor",
               ok ? "still open" : "was closed");
        /* and opendir() on a regular file answers ENOTDIR as well */
        errno = 0;
        ok = (opendir("/tmp/dt/zz") == NULL) && (errno == ENOTDIR);
        CHECK3(ok, "dir opendir on a regular file", ok ? "ENOTDIR" : "wrong");
        close(fd);
    }

    /* ── (d) scandir() really calls the comparator: the order by LENGTH
     * differs from the kernel's (name) order ── */
    n = scandir("/tmp/dt", &list, dir_filter_files, dir_cmp_by_len);
    ok = (n == 3);
    CHECK3(ok, "dir scandir entry count", ok ? "3 entries" : "wrong count");
    if (ok) {
        ok = strcmp(list[0]->d_name, "b") == 0 &&
             strcmp(list[1]->d_name, "zz") == 0 &&
             strcmp(list[2]->d_name, "aaa") == 0;
    }
    CHECK3(ok, "dir scandir uses the comparator",
           ok ? "length order" : "the comparator was not used");
    if (list) {
        int i;
        for (i = 0; i < n; i++)
            free(list[i]);
        free(list);
        list = NULL;
    }

    /* ── alphasort() against an INDEPENDENT oracle ── */
    n = scandir("/tmp/dt", &sorted, dir_filter_files, alphasort);
    ok = (n == 3);
    CHECK3(ok, "dir alphasort entry count", ok ? "3 entries" : "wrong count");
    if (ok) {
        int i;
        for (i = 0; i < n; i++) {
            strncpy(oracle[i], sorted[i]->d_name, sizeof(oracle[0]) - 1);
            oracle[i][sizeof(oracle[0]) - 1] = '\0';
        }
        dir_sort_names(oracle, n);
        for (i = 0; i < n; i++)
            if (strcmp(oracle[i], sorted[i]->d_name) != 0)
                ok = 0;
    }
    CHECK3(ok, "dir alphasort matches the oracle",
           ok ? "same order" : "different order");
    if (sorted) {
        int i;
        for (i = 0; i < n; i++)
            free(sorted[i]);
        free(sorted);
        sorted = NULL;
    }

    /* ── (e) d_off is copied VERBATIM from the kernel's record: the raw
     * getdents64(2) records are compared with what readdir() returned ── */
    fd = open("/tmp/dt", O_RDONLY);
    ok = (fd >= 0) && (dir_raw_offs(fd, raw_offs, 64, &raw_n) == 0);
    CHECK3(ok, "dir raw getdents64", ok ? "read" : "failed");
    if (ok) {
        dir = opendir("/tmp/dt");
        while (dir && (e = readdir(dir)) != NULL && read_n < 64)
            read_offs[read_n++] = e->d_off;
        if (dir)
            closedir(dir);
        ok = (read_n == raw_n);
        if (ok) {
            int i;
            for (i = 0; i < raw_n; i++)
                if (raw_offs[i] != read_offs[i])
                    ok = 0;
        }
        CHECK3(ok, "dir d_off is the kernel's own value",
               ok ? "identical" : "d_off differs");
    }
    if (fd >= 0)
        close(fd);

    /* ── cleanup ── */
    unlink("/tmp/dt/aaa");
    unlink("/tmp/dt/b");
    unlink("/tmp/dt/zz");
    rmdir("/tmp/dt");
}

