#include <fs/procfs.h>
#include <fs/vfs.h>
#include <kernel/debug.h>
#include <kernel/task.h>
#include <kernel/vma.h>
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

// ── Generate /proc/<pid>/maps ────────────────────────────────
//
// Synthesises rows from mm_t fields (ELF code, stack, heap) which
// are NOT in vma_list (elf_load and task.c map them directly with
// 2MB pages).  Then merges with do_mmap-produced vma_list entries.
//
// Two-way merge assumes synthetic addresses increase monotonically
// (ELF < brk < stack), which holds for ≤4MB binaries.  vma_list is
// already sorted by vm_start.  No allocation needed.
//
// Concurrency: find_task_by_pid should be called with a lock, but
// procfs_read does not hold one.  Two TOCTOU windows exist:
//   1. vma_list traversal while another CPU vma_remove()s
//   2. mm teardown after the t->mm NULL check
// Accepted risk — same as existing gen_status().  Proper mm locking
// depends on rwlock (roadmap P1).
//
// Returns bytes written (excluding NUL).  Buffer should be >= 4096.
static int gen_maps(task_t *t, char *buf, int bufsz)
{
    if (!t || !t->mm || t->mm == &init_mm || bufsz <= 0)
        return 0;

    mm_t *mm = t->mm;
    int off = 0;

    // Buffer for vfs_resolve_path — call with pathsz - 4 to
    // reserve room for possible "...\0" truncation marker.
    char path_buf[280];
    #define PATH_RESOLVE_BUF (sizeof(path_buf) - 4)

    // ── Helper: emit one maps line ─────────────────────────
    #define EMIT(s, e, pstr, off_val, dev_str, ino_val, path_str) do {  \
        int _n = snprintf(buf + off, (size_t)(bufsz - off),              \
            "%012lx-%012lx %s %08lx %s %8x %s\n",                        \
            (unsigned long)(s), (unsigned long)(e),                       \
            pstr, (unsigned long)(off_val),                               \
            dev_str, (unsigned)(ino_val), path_str);                      \
        if (_n < 0) return off;                                           \
        off += _n;                                                        \
        if (off >= bufsz) { buf[bufsz - 1] = '\0'; return off; }          \
    } while (0)

    int code_done  = 0;
    int brk_done   = 0;
    int stack_done = 0;
    list_t *vma_pos = mm->vma_list.next;

    // ── Two-way merge: synthetic rows + vma_list rows ──────

    for (;;) {
        // Determine next synthetic row (lowest unseen address)
        uint64_t next_synth = UINT64_MAX;
        int      which      = -1;  // 0=code, 1=brk, 2=stack

        if (!code_done && mm->start_code < mm->end_code) {
            next_synth = mm->start_code;
            which = 0;
        }
        if (!brk_done && mm->start_brk < mm->end_brk) {
            if (mm->start_brk < next_synth) {
                next_synth = mm->start_brk;
                which = 1;
            }
        }
        if (!stack_done && mm->start_stack != 0) {
            uint64_t sb = mm->start_stack & ~(PAGE_4K_MASK);
            if (sb < next_synth) {
                next_synth = sb;
                which = 2;
            }
        }

        // Determine next vma_list row address
        uint64_t next_vma_start = UINT64_MAX;
        vma_t   *next_vma       = NULL;
        if (vma_pos != &mm->vma_list) {
            next_vma = container_of(vma_pos, vma_t, list);
            next_vma_start = next_vma->vm_start;
        }

        // Neither source has more rows
        if (which < 0 && !next_vma)
            break;

        // ── Emit whichever is lower ────────────────────────
        if (which >= 0 && next_synth <= next_vma_start) {
            switch (which) {
            case 0: {  // ELF code — round to PAGE_4K
                code_done = 1;
                uint64_t s = mm->start_code & ~(PAGE_4K_MASK);
                uint64_t e = (mm->end_code + PAGE_4K_SIZE - 1)
                             & ~(PAGE_4K_MASK);
                EMIT(s, e, "rwxp", 0, "00", 0, "");
                break;
            }
            case 1:  // heap — round to PAGE_4K
                brk_done = 1;
                {
                    uint64_t hs = mm->start_brk & ~(PAGE_4K_MASK);
                    uint64_t he = (mm->end_brk + PAGE_4K_SIZE - 1)
                                  & ~(PAGE_4K_MASK);
                    EMIT(hs, he, "rwxp", 0, "00", 0, "[heap]");
                }
                break;
            case 2: {  // stack
                stack_done = 1;
                uint64_t sb = mm->start_stack & ~(PAGE_4K_MASK);
                uint64_t st = sb + 0x200000;  // 2MB stack page
                EMIT(sb, st, "rw-p", 0, "00", 0, "[stack]");
                break;
            }
            }
        } else {
            // Emit vma_list row
            char perm[5];
            perm[0] = (next_vma->vm_flags & VM_READ)   ? 'r' : '-';
            perm[1] = (next_vma->vm_flags & VM_WRITE)  ? 'w' : '-';
            perm[2] = (next_vma->vm_flags & VM_EXEC)   ? 'x' : '-';
            perm[3] = (next_vma->vm_flags & VM_SHARED) ? 's' : 'p';
            perm[4] = '\0';

            uint64_t pgoff = next_vma->vm_pgoff << 12;
            const char *dev_str  = "00";
            unsigned    ino_val  = 0;
            const char *path_str = "";

            if (next_vma->vm_file) {
                ino_val = (unsigned)(uintptr_t)next_vma->vm_file->fs_data;
                int pn = vfs_resolve_path(next_vma->vm_file, path_buf,
                                          PATH_RESOLVE_BUF);
                if (pn >= 0) {
                    // vfs_resolve_path always NUL-terminates.
                    // If truncated (pn >= PATH_RESOLVE_BUF), find the
                    // actual NUL position and append "..." there.
                    // NOTE: at the exact-fit boundary (pn == PATH_RESOLVE_BUF),
                    // the last byte is NUL rather than a path character
                    // (TERM overwrites path[pathsz-1] with '\0'), so 1
                    // character is lost.  This is inherent: a pathsz-byte
                    // buffer cannot hold a pathsz-length string + NUL.
                    // We still flag with "..." since content was lost.
                    if ((size_t)pn >= PATH_RESOLVE_BUF) {
                        size_t r = strlen(path_buf);
                        if (r + 3 < sizeof(path_buf)) {
                            memcpy(path_buf + r, "...", 3);
                            path_buf[r + 3] = '\0';
                        }
                    }
                    path_str = path_buf;
                } else {
                    path_str = "?";
                }
            }

            EMIT(next_vma->vm_start, next_vma->vm_end,
                 perm, pgoff, dev_str, ino_val, path_str);

            vma_pos = vma_pos->next;
        }
    }

    #undef EMIT
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
    char  local[4096];
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
    case PROCFS_TYPE_MAPS: {
        task_t *t = find_task_by_pid((int)pid);
        if (!t) return 0;
        len = gen_maps(t, local, sizeof(local));
        break;
    }
    default:
        return 0;
    }

    // ── Return the requested slice ────────────────────────
    if ((uint64_t)len <= offset)
        return 0;  // EOF

    // Guard: offset past buffer is EOF (pre-empts unsigned underflow
    // in sizeof(local) - offset when len exceeds buffer size).
    if (offset >= (uint64_t)sizeof(local))
        return 0;
    uint64_t remain = (uint64_t)len - offset;
    uint64_t n = remain < size ? remain : size;
    uint64_t avail = (uint64_t)sizeof(local) - offset;
    if (n > avail)
        n = avail;
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
        case 1:
            strcpy(entry->name, "maps");
            entry->type = VFS_FILE;
            entry->size = 4096;
            entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(
                PROCFS_TYPE_MAPS, PROCFS_PID_SELF);
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
        if (index == 1) {
            strcpy(entry->name, "maps");
            entry->type = VFS_FILE;
            entry->size = 4096;
            entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(
                PROCFS_TYPE_MAPS, pid);
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
        debug_fs("procfs: mounted at /proc\n");
    } else {
        debug_fs("procfs: mount FAILED\n");
    }
}
