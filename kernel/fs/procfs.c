#include <fs/procfs.h>
#include <fs/vfs.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/memory.h>
#include <kernel.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ── State char for /proc/<pid>/status ───────────────────────
static char state_char(int64_t state)
{
    if (state & TASK_RUNNING)        return 'R';
    if (state & TASK_INTERRUPTIBLE)  return 'S';
    if (state & TASK_UNINTERRUPTIBLE) return 'D';
    if (state & TASK_ZOMBIE)         return 'Z';
    if (state & TASK_STOPPED)        return 'T';
    return '?';
}

// ── Find task by PID (lockless — caller holds spinlock) ──────
static task_t *find_task_by_pid(int pid)
{
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        if ((int)t->pid == pid)
            return t;
        pos = pos->next;
    }
    return NULL;
}

// ── procfs_stat: fill stat buffer for a procfs node ──────────
// Called from the generic vfs_stat when node type is a procfs node.
// Replaces node->ops->stat if present... but vfs_ops_t has no stat ptr.
//
// Instead, procfs nodes set `size` at creation time.  For synthetic
// files we set size=4096 (an upper bound) so ls -l shows a non-zero
// value; the real size is determined by the content generated at
// read() time.  size=0 for directories.

// ── Generate /proc/<pid>/status ──────────────────────────────
// Returns bytes written (excluding NUL).  Buffer must be >= 256 bytes.
static int gen_status(task_t *t, char *buf, int bufsz)
{
    int off = snprintf(buf, bufsz,
        "Name:\ttask\n"
        "Pid:\t%d\n"
        "PPid:\t%d\n"
        "State:\t%c\n"
        "CPU:\t%u\n"
        "Priority:\t%d\n",
        (int)t->pid,
        t->parent ? (int)t->parent->pid : 0,
        state_char(t->state),
        (unsigned)t->cpu,
        (int)t->priority
    );
    return off;
}

// ── Generate /proc/meminfo ───────────────────────────────────
// Returns bytes written (excluding NUL).  Buffer must be >= 512 bytes.
static int gen_meminfo(char *buf, int bufsz)
{
    uint64_t total_pages = 0;
    uint64_t used_pages  = 0;
    uint64_t free_pages  = 0;
    int      zone_count  = 0;

    for (int i = 0; i < (int)PMMngr.zones_size; i++) {
        struct Zone *z = &PMMngr.zones_struct[i];
        total_pages += z->pages_length;
        used_pages  += z->page_using_count;
        free_pages  += z->page_free_count;
        zone_count++;
    }

    uint64_t total_kb = total_pages * 2048;  // 2MB pages → KB
    uint64_t free_kb  = free_pages  * 2048;
    uint64_t used_kb  = used_pages  * 2048;

    int off = snprintf(buf, bufsz,
        "MemTotal:\t%lu kB\n"
        "MemFree:\t%lu kB\n"
        "MemUsed:\t%lu kB\n"
        "PageSize:\t2048 kB\n"
        "Zones:\t%d\n",
        (unsigned long)total_kb,
        (unsigned long)free_kb,
        (unsigned long)used_kb,
        zone_count
    );
    return off;
}

// ── procfs_read: generate file content on-the-fly ────────────
//
// Content is always freshly generated per call — no caching.
// The buffer sizes are small (< 1 KB) so this is cheap.

static int procfs_read(vfs_node_t *node, uint64_t offset,
                       uint64_t size, void *buffer)
{
    if (!node || !buffer || size == 0)
        return 0;

    int      type = PROCFS_TYPE(node->fs_data);
    uint32_t pid  = PROCFS_PID(node->fs_data);

    // Resolve "self"-sentinel to current task PID
    if (pid == PROCFS_PID_SELF) {
        task_t *cur = current;
        if (!cur) return 0;
        pid = (uint32_t)cur->pid;
    }

    // Auto-detect PID from current if a plain PID_DIR node is read
    // (shouldn't happen — PID_DIRs are directories, not files)
    if (type == PROCFS_TYPE_PID_DIR || type == PROCFS_TYPE_ROOT ||
        type == PROCFS_TYPE_SELF_DIR)
        return 0;

    // ── Generate content ──────────────────────────────────
    char  local[512];
    int   len = 0;

    switch (type) {
    case PROCFS_TYPE_STATUS: {
        task_t *t = find_task_by_pid((int)pid);
        if (!t) return 0;
        len = gen_status(t, local, sizeof(local));
        break;
    }
    case PROCFS_TYPE_MEMINFO:
        len = gen_meminfo(local, sizeof(local));
        break;
    default:
        return 0;
    }

    // ── Return the requested slice ────────────────────────
    if ((uint64_t)len <= offset)
        return 0;  // EOF

    uint64_t remain = (uint64_t)len - offset;
    uint64_t n = remain < size ? remain : size;
    memcpy(buffer, local + offset, n);
    return (int)n;
}

// ── procfs_readdir: enumerate files/dirs ─────────────────────
//
// The mapping from index→entry is dynamic for per-pid directories
// because tasks can be created/destroyed.  We re-scan the task
// list on every call.  This is lockless; the worst case is a
// stale or duplicate entry.

static int procfs_readdir(vfs_node_t *node, uint64_t index,
                          vfs_dirent_t *entry)
{
    if (!node || !entry) return -1;
    if (node->type != VFS_DIR) return -1;

    int      type = PROCFS_TYPE(node->fs_data);
    uint32_t pid  = PROCFS_PID(node->fs_data);

    switch (type) {

    // ── /proc/ ─────────────────────────────────────────
    case PROCFS_TYPE_ROOT: {
        switch (index) {
        case 0:
            // /proc/self → magic directory
            strcpy(entry->name, "self");
            entry->type = VFS_DIR;
            entry->size = 0;
            entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(PROCFS_TYPE_SELF_DIR, PROCFS_PID_SELF);
            return 0;
        case 1:
            // /proc/meminfo → synthetic file
            strcpy(entry->name, "meminfo");
            entry->type = VFS_FILE;
            entry->size = 4096;
            entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(PROCFS_TYPE_MEMINFO, 0);
            return 0;
        default: {
            // /proc/<pid> → per-process directories
            uint64_t kidx = index - 2;
            list_t  *pos  = init_task_union.task.list.next;

            while (pos != &init_task_union.task.list) {
                task_t *t = container_of(pos, task_t, list);
                // Skip kernel threads and idle (pid 0)
                if (t->pid > 0 && !(t->flags & PF_KTHREAD)) {
                    if (kidx == 0) {
                        char name[32];
                        int n = snprintf(name, sizeof(name), "%d", (int)t->pid);
                        memcpy(entry->name, name, n + 1);
                        entry->type = VFS_DIR;
                        entry->size = 0;
                        entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(
                            PROCFS_TYPE_PID_DIR, (uint32_t)t->pid);
                        return 0;
                    }
                    kidx--;
                }
                pos = pos->next;
            }
            // No more entries
            entry->name[0] = '\0';
            return 0;
        }
        }
    }

    // ── /proc/self/ ─────────────────────────────────────
    case PROCFS_TYPE_SELF_DIR:
        switch (index) {
        case 0:
            strcpy(entry->name, "status");
            entry->type = VFS_FILE;
            entry->size = 4096;
            entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(
                PROCFS_TYPE_STATUS, PROCFS_PID_SELF);
            return 0;
        default:
            entry->name[0] = '\0';
            return 0;
        }

    // ── /proc/<pid>/ ────────────────────────────────────
    case PROCFS_TYPE_PID_DIR:
        if (index == 0) {
            strcpy(entry->name, "status");
            entry->type = VFS_FILE;
            entry->size = 4096;
            entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(
                PROCFS_TYPE_STATUS, pid);
            return 0;
        }
        entry->name[0] = '\0';
        return 0;

    default:
        entry->name[0] = '\0';
        return 0;
    }
}

// ── ops vector ────────────────────────────────────────────────
static vfs_ops_t procfs_ops = {
    .read    = procfs_read,
    .write   = NULL,
    .readdir = procfs_readdir,
    // create / unlink / mkdir / rmdir / rename / truncate are NULL
};

// ── Initialise and mount ─────────────────────────────────────
void procfs_init(void)
{
    int ret = vfs_mount("/proc", NULL, &procfs_ops, NULL);
    if (ret == 0) {
        serial_printk("procfs: mounted at /proc\n");
    } else {
        serial_printk("procfs: mount FAILED\n");
    }
}
