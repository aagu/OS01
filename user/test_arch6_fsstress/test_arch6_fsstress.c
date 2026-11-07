// ARCH-6: four workers, three filesystems, parent-controlled pipe barriers.
// Every release requires one successful event from EACH worker for the
// current filesystem/phase. The filesystem under test is never a barrier.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <dirent.h>
#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#define WORKERS 4
#define FILESYSTEMS 3
#define SEGS 32
#define SEG_SIZE 512
#define EVT_BUDGET_MS 120000

enum phase { FILES, CROSS, DELETE, SHARED_READY, SHARED_CREATE, SHARED_DELETE, PHASES };
struct event {
    uint32_t worker, fs, phase;
    int32_t result;                       // 0 = success, negative = failure
};
_Static_assert(sizeof(struct event) == 16, "pipe event must be one small atomic write");

static const char *FS_ROOT[FILESYSTEMS] = {
    "/arch6stress", "/tmp/arch6stress", "/boot/arch6stress"
};
static const char *FS_NAME[FILESYSTEMS] = { "ext2", "tmpfs", "vfat" };
static const char *PHASE_NAME[PHASES] = {
    "files", "cross", "delete", "shared-ready", "shared-create", "shared-delete"
};
static int g_worker_id;
static int64_t deadline_ms;

static int64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Pipe reads may be short or interrupted. Every read/poll uses the SAME
// absolute deadline, including reads after POLLIN and final child reaping.
static int read_record(int fd, void *buf, size_t size)
{
    size_t off = 0;
    while (off < size) {
        int64_t now = now_ms();
        if (now < 0 || now >= deadline_ms) return -1;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int rc = poll(&pfd, 1, (int)(deadline_ms - now));
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return -1;
        ssize_t n = read(fd, (char *)buf + off, size - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void fill_pattern(unsigned char *buf, int worker, int seg)
{
    memset(buf, 0x40 + worker * 8 + (seg & 7), SEG_SIZE);
    buf[0] = (unsigned char)(worker * SEGS + seg);
}

static int verify_file(const char *path, int worker)
{
    unsigned char expected[SEG_SIZE], got[SEG_SIZE];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int rc = 0;
    for (int seg = 0; seg < SEGS; seg++) {
        fill_pattern(expected, worker, seg);
        if (read(fd, got, sizeof(got)) != sizeof(got) ||
            memcmp(expected, got, sizeof(got)) != 0) { rc = -1; break; }
    }
    if (close(fd) != 0) rc = -1;
    return rc;
}

static int phase_files(const char *root, int id)
{
    char path[256], renamed[256];
    unsigned char buf[SEG_SIZE];
    snprintf(path, sizeof(path), "%s/files/f%d", root, id);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int rc = 0;
    for (int seg = 0; seg < SEGS; seg++) {
        fill_pattern(buf, id, seg);
        if (write(fd, buf, sizeof(buf)) != sizeof(buf)) { rc = -1; break; }
    }
    if (close(fd) != 0) rc = -1;
    if (rc || verify_file(path, id) != 0) return -1;
    snprintf(renamed, sizeof(renamed), "%s/files/f%d.done", root, id);
    return rename(path, renamed);
}

static int phase_cross(const char *root, int id)
{
    char path[256];
    int peer = id % WORKERS + 1;
    snprintf(path, sizeof(path), "%s/files/f%d.done", root, peer);
    return verify_file(path, peer);        // exactly one open; no retry
}

static int remove_own(const char *root, const char *dir, const char *name, int id)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/%s%d%s", root, dir, name, id,
             strcmp(dir, "files") == 0 ? ".done" : "");
    return unlink(path);                  // ENOENT is a failure too
}

static int phase_shared(const char *root, int id)
{
    char path[256];
    unsigned char buf[SEG_SIZE], got[SEG_SIZE];
    snprintf(path, sizeof(path), "%s/shared/u%d", root, id);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    fill_pattern(buf, id, 0);
    int rc = write(fd, buf, sizeof(buf)) == sizeof(buf) ? 0 : -1;
    if (close(fd) != 0) rc = -1;
    if (rc) return rc;
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    rc = read(fd, got, sizeof(got)) == sizeof(got) &&
         memcmp(buf, got, sizeof(got)) == 0 ? 0 : -1;
    if (close(fd) != 0) rc = -1;
    return rc;
}

static void worker_barrier(int evfd, int relfd, unsigned fs, unsigned phase, int rc)
{
    struct event event = { (unsigned)g_worker_id, fs, phase, rc ? -1 : 0 };
    if (rc) {
        printf("[arch6-fsstress] %s worker%d FAIL(%s errno=%d)\n",
               FS_NAME[fs], g_worker_id, PHASE_NAME[phase], errno);
        fflush(stdout);
    }
    ssize_t n;
    do { n = write(evfd, &event, sizeof(event)); } while (n < 0 && errno == EINTR);
    if (rc || n != sizeof(event)) _exit(1);
    char release;
    if (read_record(relfd, &release, 1) != 0 || release != 'r') _exit(1);
}

static void worker_main(int id, int evfd, int relfd)
{
    g_worker_id = id;
    for (unsigned fs = 0; fs < FILESYSTEMS; fs++) {
        const char *root = FS_ROOT[fs];
        worker_barrier(evfd, relfd, fs, FILES, phase_files(root, id));
        worker_barrier(evfd, relfd, fs, CROSS, phase_cross(root, id));
        worker_barrier(evfd, relfd, fs, DELETE, remove_own(root, "files", "f", id));
        worker_barrier(evfd, relfd, fs, SHARED_READY, 0);
        worker_barrier(evfd, relfd, fs, SHARED_CREATE, phase_shared(root, id));
        worker_barrier(evfd, relfd, fs, SHARED_DELETE, remove_own(root, "shared", "u", id));
        printf("[arch6-fsstress] %s worker%d PASS\n", FS_NAME[fs], id);
        fflush(stdout);
    }
    _exit(0);
}

static int dir_is_empty(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return 0;
    int empty = 1;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            empty = 0;
            break;
        }
    }
    if (errno) empty = 0;
    if (closedir(dir) != 0) empty = 0;
    return empty;
}

static int collect_barrier(int evfd, unsigned fs, unsigned phase)
{
    unsigned seen = 0;
    while (seen != (1U << WORKERS) - 1) {
        struct event event;
        if (read_record(evfd, &event, sizeof(event)) != 0) return -1;
        if (event.worker < 1 || event.worker > WORKERS ||
            event.fs != fs || event.phase != phase || event.result != 0)
            return -1;
        unsigned bit = 1U << (event.worker - 1);
        if (seen & bit) return -1;
        seen |= bit;
    }
    return 0;                            // release only when ALL FOUR arrived
}

static int reap_child(pid_t pid)
{
    int status;
    for (;;) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
        if (result < 0 && errno != EINTR) return -1;
        int64_t now = now_ms();
        if (now < 0 || now >= deadline_ms) return -1;
        poll(NULL, 0, 1);
    }
}

int main(void)
{
    int ev[2] = { -1, -1 }, rel[WORKERS][2];
    pid_t pids[WORKERS] = { 0 };
    int failure = 1;
    for (int i = 0; i < WORKERS; i++) rel[i][0] = rel[i][1] = -1;
    signal(SIGPIPE, SIG_IGN);              // retain control of EPIPE cleanup
    int64_t now = now_ms();
    if (now < 0) goto cleanup;
    deadline_ms = now + EVT_BUDGET_MS;
    for (int fs = 0; fs < FILESYSTEMS; fs++) {
        char path[256];
        if (mkdir(FS_ROOT[fs], 0755) != 0) goto cleanup;
        snprintf(path, sizeof(path), "%s/files", FS_ROOT[fs]);
        if (mkdir(path, 0755) != 0) goto cleanup;
        snprintf(path, sizeof(path), "%s/shared", FS_ROOT[fs]);
        if (mkdir(path, 0755) != 0) goto cleanup;
    }
    if (pipe(ev) != 0) goto cleanup;
    for (int i = 0; i < WORKERS; i++) if (pipe(rel[i]) != 0) goto cleanup;
    // 10 pipe FDs + 3 standard FDs = 13, below the default limit of 16.
    for (int i = 0; i < WORKERS; i++) {
        pids[i] = fork();
        if (pids[i] < 0) goto cleanup;
        if (pids[i] == 0) {
            close(ev[0]);
            for (int j = 0; j < WORKERS; j++) {
                close(rel[j][1]);
                if (j != i) close(rel[j][0]);
            }
            worker_main(i + 1, ev[1], rel[i][0]);
        }
    }
    close(ev[1]); ev[1] = -1;
    for (int i = 0; i < WORKERS; i++) { close(rel[i][0]); rel[i][0] = -1; }

    for (unsigned fs = 0; fs < FILESYSTEMS; fs++) {
        for (unsigned phase = 0; phase < PHASES; phase++) {
            if (collect_barrier(ev[0], fs, phase) != 0) {
                printf("[arch6-fsstress] FAIL(barrier %s/%s)\n", FS_NAME[fs], PHASE_NAME[phase]);
                goto cleanup;
            }
            if (phase == DELETE || phase == SHARED_DELETE) {
                char path[256];
                snprintf(path, sizeof(path), "%s/%s", FS_ROOT[fs],
                         phase == DELETE ? "files" : "shared");
                if (!dir_is_empty(path)) goto cleanup;
            }
            for (int i = 0; i < WORKERS; i++) {
                char release = 'r';
                if (write(rel[i][1], &release, 1) != 1) goto cleanup;
            }
        }
    }
    for (int i = 0; i < WORKERS; i++) {
        int rc = reap_child(pids[i]);
        if (rc == 0) pids[i] = 0;
        else goto cleanup;
    }
    for (int fs = 0; fs < FILESYSTEMS; fs++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/files", FS_ROOT[fs]);
        if (rmdir(path) != 0) goto cleanup;
        snprintf(path, sizeof(path), "%s/shared", FS_ROOT[fs]);
        if (rmdir(path) != 0 || rmdir(FS_ROOT[fs]) != 0) goto cleanup;
    }
    failure = 0;

cleanup:
    for (int i = 0; i < WORKERS; i++) {
        if (rel[i][0] >= 0) close(rel[i][0]);
        if (rel[i][1] >= 0) close(rel[i][1]);
    }
    if (ev[0] >= 0) close(ev[0]);
    if (ev[1] >= 0) close(ev[1]);
    // Include partially-created sets and reap even when a pipe broke.
    for (int i = 0; i < WORKERS; i++)
        if (pids[i] > 0) kill(pids[i], SIGKILL);
    for (int i = 0; i < WORKERS; i++) {
        if (pids[i] <= 0) continue;
        int status;
        while (waitpid(pids[i], &status, 0) < 0 && errno == EINTR) {}
    }
    printf("[arch6-fsstress] %s\n", failure ? "FAIL" : "PASS");
    fflush(stdout);
    return failure;
}
