#include <fs/procfs.h>
#include <fs/vfs.h>
#include <errno.h>
#include <core/debug.h>
#include <sched/task.h>
#include <memory/vma.h>
#include <percpu/percpu.h>
#include <memory/pmm.h>
#include <memory/memory.h>
#include <memory/slab.h>
#include <kernel.h>
#include <fs/file.h>   // file_t / files_t full definition
#include <tty/pty.h>    // pty_struct full definition (file.h only fwd-declares)
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ── State char for /proc/<pid>/status ───────────────────────
// (Moved to fs/procfs_snap.c in monitor plan Task 4 — the statistics
// snapshot producers live there now.)

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

// ── Strict decimal fd parse ────────────────────────────────
// Rejects negative, leading garbage, trailing chars, and overflow.
// NOT atoi(): we own the name (from readdir) but parse defensively.
static int parse_fd(const char *s)
{
    if (!s || *s < '0' || *s > '9') return -1;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v >= NOFILE) return -1;
    }
    return v;
}

// ── Render an fd's target as a text line ──────────────────
// f must be a stable reference (from files_get_file).
// Returns bytes written (excluding NUL).
static int gen_fd_target(file_t *f, char *buf, int bufsz)
{
    if (!f || !buf || bufsz <= 0) return 0;

    switch (f->type) {
    case FD_VFS:
    case FD_DEV: {
        if (!f->node) return snprintf(buf, bufsz, "?\n");
        char path_buf[280];
        int n = vfs_resolve_path(f->node, path_buf, sizeof(path_buf) - 4);
        return snprintf(buf, bufsz, "%s\n", n < 0 ? "?" : path_buf);
    }
    case FD_PIPE:
        return snprintf(buf, bufsz, "pipe:[?]\n");
    case FD_PTY_MASTER:
        return snprintf(buf, bufsz, "/dev/ptmx\n");
    case FD_PTY_SLAVE: {
        int idx = f->pty ? f->pty->index : -1;
        return snprintf(buf, bufsz, "/dev/pts%d\n", idx);
    }
    case FD_SOCKET:
        return snprintf(buf, bufsz, "socket:[?]\n");
    default:
        return 0;
    }
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
// (Moved to fs/procfs_snap.c in monitor plan Task 4: status is now a
// per-open snapshot via the vfs_ops open_file/read_file/release_file
// hooks, formatted by procfs_fmt_status outside task_list_lock.)

// ── Generate /proc/meminfo ───────────────────────────────────
// (Moved to fs/procfs_snap.c in monitor plan Task 4: meminfo now
// snapshots the locked pmm_get_stats() counters (spec §4.1) per open;
// the kB units are kept, the legacy Zones line is gone because the
// locked snapshot exports bytes, not zone bookkeeping.)

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
            uint64_t sb = mm->start_stack & PAGE_4K_MASK;
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
                uint64_t s = mm->start_code & PAGE_4K_MASK;
                uint64_t e = (mm->end_code + PAGE_4K_SIZE - 1)
                             & PAGE_4K_MASK;
                EMIT(s, e, "rwxp", 0, "00", 0, "");
                break;
            }
            case 1:  // heap — round to PAGE_4K
                brk_done = 1;
                {
                    uint64_t hs = mm->start_brk & PAGE_4K_MASK;
                    uint64_t he = (mm->end_brk + PAGE_4K_SIZE - 1)
                                  & PAGE_4K_MASK;
                    EMIT(hs, he, "rwxp", 0, "00", 0, "[heap]");
                }
                break;
            case 2: {  // stack
                stack_done = 1;
                uint64_t sb = mm->start_stack & PAGE_4K_MASK;
                uint64_t st = sb + 0x200000;  // 2MB stack page
                EMIT(sb, st, "rw-p", 0, "00", 0, "[stack]");
                break;
            }
            }
        } else {
            // Emit vma_list row
            char perm[5];
            perm[0] = (next_vma->vm_flags & VMA_PROT_READ)   ? 'r' : '-';
            perm[1] = (next_vma->vm_flags & VMA_PROT_WRITE)  ? 'w' : '-';
            perm[2] = (next_vma->vm_flags & VMA_PROT_EXEC)   ? 'x' : '-';
            perm[3] = (next_vma->vm_flags & VMA_SHARED) ? 's' : 'p';
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
        type == PROCFS_TYPE_SELF_DIR || type == PROCFS_TYPE_FD_DIR)
        return 0;

    // ── Generate content ──────────────────────────────────
    char *local = kmalloc(4096);
    if (!local) return 0;
    int len = 0;
    int ret = 0;

    switch (type) {
    case PROCFS_TYPE_STATUS:
    case PROCFS_TYPE_STAT:
    case PROCFS_TYPE_THERMAL:
    case PROCFS_TYPE_MEMINFO:
        // Monitor plan Task 4 legacy fallback: a direct node read (no
        // file_t, therefore no open_file snapshot) is still a legal path
        // (Task 1 contract: "无 hook 走旧 node read") — kernel-internal
        // readers and the KERNEL_SELFTEST procfs case use it.  Regenerate
        // through the same snapshot producers the hook path uses, so
        // both paths share one formatting source.  A vanished status PID
        // (-ENOENT) or overflow reads as empty, matching the old
        // behaviour of this regenerate-per-call path.
        len = procfs_generate_node(type, pid, local, 4096);
        if (len < 0)
            len = 0;
        break;
    case PROCFS_TYPE_MAPS: {
        task_t *t = find_task_by_pid((int)pid);
        if (t) len = gen_maps(t, local, 4096);
        break;
    }
    case PROCFS_TYPE_FD_ENTRY: {
        uint32_t p = pid;
        if (p == PROCFS_PID_SELF) { if (!current) break; p = (uint32_t)current->pid; }
        int fd = parse_fd(node->name);
        if (fd < 0) break;

        files_t *fs = task_files_pin_by_pid((int)p);
        if (!fs) break;
        file_t *f = files_get_file(fs, fd);
        files_unpin(fs);                 // drop table ref early
        if (!f) break;

        len = gen_fd_target(f, local, 4096);
        files_put_file(f);
        break;
    }
    default:
        break;
    }

    // ── Return the requested slice ────────────────────────
    if (len > 0 && offset < (uint64_t)len && offset < 4096) {
        uint64_t remain = (uint64_t)len - offset;
        uint64_t n = remain < size ? remain : size;
        uint64_t avail = 4096 - offset;
        if (n > avail)
            n = avail;
        memcpy(buffer, local + offset, n);
        ret = (int)n;
    }

    kfree(local);
    return ret;
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
        case 2:
            // /proc/stat → per-CPU tick accounting (monitor plan Task 4)
            strcpy(entry->name, "stat");
            entry->type = VFS_FILE;
            entry->size = 4096;
            entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(PROCFS_TYPE_STAT, 0);
            return 0;
        case 3:
            // /proc/thermal → cached thermal sample (monitor plan Task 4)
            strcpy(entry->name, "thermal");
            entry->type = VFS_FILE;
            entry->size = 4096;
            entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(PROCFS_TYPE_THERMAL, 0);
            return 0;
        default:
            // /proc/<pid> → per-process directories.  The PID scan runs
            // as one traversal under task_list_lock (monitor plan Task 4,
            // spec §5.2) in fs/procfs_snap.c; across calls it stays
            // dynamic.  User tasks start at index 4.
            return procfs_readdir_tasks(index, entry, 4);
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
        case 2:
            strcpy(entry->name, "fd");
            entry->type = VFS_DIR;
            entry->size = 0;
            entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(
                PROCFS_TYPE_FD_DIR, PROCFS_PID_SELF);
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
        if (index == 2) {
            strcpy(entry->name, "fd");
            entry->type = VFS_DIR;
            entry->size = 0;
            entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(
                PROCFS_TYPE_FD_DIR, pid);
            return 0;
        }
        entry->name[0] = '\0';
        return 0;

    // ── /proc/<pid>/fd/ ──────────────────────────────────
    case PROCFS_TYPE_FD_DIR: {
        uint32_t p = pid;
        if (p == PROCFS_PID_SELF) { if (!current) return -1; p = (uint32_t)current->pid; }

        files_t *fs = task_files_pin_by_pid((int)p);   // pin under task_list_lock
        if (!fs) return -1;

        uint64_t k = index;
        int found = -1;
        {
            uint64_t fl = spin_lock_irqsave(&fs->lock); // single scan, held
            for (int fd = 0; fd < NOFILE; fd++) {
                if (!fs->fd[fd]) continue;
                if (k == 0) { found = fd; break; }
                k--;
            }
            spin_unlock_irqrestore(&fs->lock, fl);
        }

        if (found >= 0) {
            snprintf(entry->name, VFS_NAME_MAX, "%d", found);
            entry->type = VFS_FILE;
            entry->size = 4096;
            entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(
                PROCFS_TYPE_FD_ENTRY, pid);
        } else {
            entry->name[0] = '\0';
        }

        files_unpin(fs);   // outside any lock
        return 0;
    }

    default:
        entry->name[0] = '\0';
        return 0;
    }
}

// ── ops vector ────────────────────────────────────────────────
// Monitor plan Task 4: the statistics nodes (stat/thermal/meminfo/
// status) use the Task-1 per-file snapshot lifecycle hooks; every other
// procfs node keeps the legacy regenerate-per-read behaviour (read_file
// falls back to vfs_read → procfs_read when no snapshot is attached).
static const vfs_inode_ops_t procfs_iops;
static const vfs_file_ops_t procfs_fops;

static int procfs_parse_uint(const char *s, uint32_t *out_val)
{
    if (!s || s[0] == '\0') return -1;
    if (s[0] == '0' && s[1] != '\0') return -1; // reject leading zeros like "01"
    uint64_t val = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        val = val * 10 + (uint64_t)(*p - '0');
        if (val > 0x7FFFFFULL) return -1;
    }
    *out_val = (uint32_t)val;
    return 0;
}

static int procfs_iop_lookup(vfs_node_t *dir, const char *name, vfs_node_t **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!dir || !name) return -EINVAL;
    if (dir->type != VFS_DIR) return -ENOTDIR;

    int      dir_type = PROCFS_TYPE(dir->fs_data);
    uint32_t pid      = PROCFS_PID(dir->fs_data);

    uint8_t  child_type = 0;
    uint64_t child_size = 0;
    void    *child_fs_data = NULL;

    switch (dir_type) {
    case PROCFS_TYPE_ROOT: {
        if (strcmp(name, "self") == 0) {
            child_type = VFS_DIR;
            child_size = 0;
            child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_SELF_DIR, PROCFS_PID_SELF);
            break;
        }
        if (strcmp(name, "meminfo") == 0) {
            child_type = VFS_FILE;
            child_size = 4096;
            child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_MEMINFO, 0);
            break;
        }
        if (strcmp(name, "stat") == 0) {
            child_type = VFS_FILE;
            child_size = 4096;
            child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_STAT, 0);
            break;
        }
        if (strcmp(name, "thermal") == 0) {
            child_type = VFS_FILE;
            child_size = 4096;
            child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_THERMAL, 0);
            break;
        }
        uint32_t target_pid = 0;
        if (procfs_parse_uint(name, &target_pid) != 0 || target_pid == 0)
            return -ENOENT;
        uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
        task_t *t = find_task_by_pid((int)target_pid);
        int valid = (t && t->pid > 0 && !(t->flags & PF_KTHREAD));
        spin_unlock_irqrestore(&task_list_lock, tl_flags);
        if (!valid) return -ENOENT;
        child_type = VFS_DIR;
        child_size = 0;
        child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_PID_DIR, target_pid);
        break;
    }
    case PROCFS_TYPE_SELF_DIR:
    case PROCFS_TYPE_PID_DIR: {
        if (dir_type == PROCFS_TYPE_PID_DIR) {
            uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
            task_t *t = find_task_by_pid((int)pid);
            int valid = (t && t->pid > 0 && !(t->flags & PF_KTHREAD));
            spin_unlock_irqrestore(&task_list_lock, tl_flags);
            if (!valid) return -ENOENT;
        }
        uint32_t enc_pid = (dir_type == PROCFS_TYPE_SELF_DIR) ? PROCFS_PID_SELF : pid;
        if (strcmp(name, "status") == 0) {
            child_type = VFS_FILE;
            child_size = 4096;
            child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_STATUS, enc_pid);
        } else if (strcmp(name, "maps") == 0) {
            child_type = VFS_FILE;
            child_size = 4096;
            child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_MAPS, enc_pid);
        } else if (strcmp(name, "fd") == 0) {
            child_type = VFS_DIR;
            child_size = 0;
            child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_FD_DIR, enc_pid);
        } else {
            return -ENOENT;
        }
        break;
    }
    case PROCFS_TYPE_FD_DIR: {
        int fd_num = parse_fd(name);
        if (fd_num < 0) return -ENOENT;
        uint32_t p = pid;
        if (p == PROCFS_PID_SELF) {
            if (!current) return -ENOENT;
            p = (uint32_t)current->pid;
        }
        files_t *fs = task_files_pin_by_pid((int)p);
        if (!fs) return -ENOENT;
        uint64_t fl = spin_lock_irqsave(&fs->lock);
        int open_ok = (fs->fd[fd_num] != NULL);
        spin_unlock_irqrestore(&fs->lock, fl);
        files_unpin(fs);
        if (!open_ok) return -ENOENT;
        child_type = VFS_FILE;
        child_size = 4096;
        child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_FD_ENTRY, pid);
        break;
    }
    default:
        return -ENOENT;
    }

    vfs_node_t *child = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
    if (!child) return -ENOMEM;
    child->name = strndup(name, VFS_NAME_MAX - 1);
    if (!child->name) {
        kfree(child);
        return -ENOMEM;
    }
    child->type = child_type;
    child->size = child_size;
    child->ino = (uint64_t)(uintptr_t)child_fs_data;
    child->mode = (child_type == VFS_DIR) ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    child->nlink = (child_type == VFS_DIR) ? 2 : 1;
    child->mount = dir->mount;
    child->parent = vfs_node_get(dir);
    child->fs_data = child_fs_data;
    child->i_ops = &procfs_iops;
    child->f_ops = &procfs_fops;
    child->refcount = 1;
    *out = child;
    return 0;
}

static const vfs_inode_ops_t procfs_iops = {
    .lookup = procfs_iop_lookup,
};

static const vfs_file_ops_t procfs_fops = {
    .read         = procfs_read,
    .write        = NULL,
    .readdir      = procfs_readdir,
    .open_file    = procfs_open_file,
    .read_file    = procfs_read_file,
    .release_file = procfs_release_file,
};

static int procfs_fs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
{
    (void)dev;
    (void)data;
    mp->i_ops = &procfs_iops;
    mp->f_ops = &procfs_fops;
    mp->fs_data = NULL;
    mp->root->i_ops = &procfs_iops;
    mp->root->f_ops = &procfs_fops;
    mp->root->ino = VFS_SYNTH_ROOT_INO;
    mp->root->mode = S_IFDIR | 0555;
    mp->root->nlink = 2;
    return 0;
}

static vfs_fs_type_t procfs_fs_type = {
    .name     = "procfs",
    .fs_flags = VFS_FS_NO_DCACHE,
    .mount    = procfs_fs_mount,
    .unmount  = NULL,
};

int procfs_register_fs(void)
{
    return vfs_register_filesystem(&procfs_fs_type);
}

// ── Initialise and mount ─────────────────────────────────────
void procfs_init(void)
{
    procfs_register_fs();
    int ret = vfs_mount_fs("procfs", "/proc", NULL, NULL);
    if (ret == 0) {
        debug_fs("procfs: mounted at /proc\n");
    } else {
        debug_fs("procfs: mount FAILED\n");
    }
}
