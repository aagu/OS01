#ifndef _FS_PROCFS_H
#define _FS_PROCFS_H

#include <stddef.h>   // size_t (procfs_generate_node)
#include <stdint.h>

// ── procfs node types (packed in node->fs_data, lower 32 bits) ─
// bits 24-31: node type (8 bits, 0-255)
// bits  0-23: PID or zero for non-per-task nodes

#define PROCFS_TYPE(fsdata)  ((((uint32_t)(uintptr_t)(fsdata)) >> 24) & 0xFF)
#define PROCFS_PID(fsdata)   (((uint32_t)(uintptr_t)(fsdata)) & 0xFFFFFF)
#define PROCFS_ENCODE(t, p)  ((void *)(uintptr_t)((((uint32_t)(t)) << 24) | ((uint32_t)(p) & 0xFFFFFF)))

#define PROCFS_TYPE_ROOT      0   // /proc/
#define PROCFS_TYPE_SELF_DIR  1   // /proc/self/
#define PROCFS_TYPE_PID_DIR   2   // /proc/<pid>/
#define PROCFS_TYPE_STATUS    3   // .../status
#define PROCFS_TYPE_MEMINFO   4   // /proc/meminfo
#define PROCFS_TYPE_MAPS      5   // /proc/<pid>/maps
#define PROCFS_TYPE_FD_DIR    7   // /proc/<pid>/fd/   (directory)
#define PROCFS_TYPE_FD_ENTRY  8   // /proc/<pid>/fd/<N> (synthetic file)
// Monitor plan Task 4 (spec §6): previously unused type numbers — the
// existing values above are unchanged.
#define PROCFS_TYPE_STAT      9   // /proc/stat     (per-CPU tick accounting)
#define PROCFS_TYPE_THERMAL   10  // /proc/thermal  (cached thermal sample)

#define PROCFS_PID_SELF       0xFFFFFF   // sentinel: resolve to current->pid in read()

// ── Public API ─────────────────────────────────────────────────

void procfs_init(void);

// ── Monitor plan Task 4: per-file snapshot lifecycle (spec §6) ──
// The four statistics nodes (stat/thermal/meminfo/status) generate
// their text ONCE per open (procfs_open_file → fs_private snapshot),
// serve slices of that snapshot (procfs_read_file — stable EOF, so
// segmented reads never mix two generations), and free it on the last
// file_put (procfs_release_file, fired from file_free).  Implemented in
// fs/procfs_snap.c, wired into procfs_ops in fs/procfs.c; non-static so
// the host harness can drive them directly.  open_file returns 0,
// -ENOMEM (allocation), -EOVERFLOW (formatting exceeds the fixed
// capacity) or -ENOENT (PID already gone for status).
struct file;
int      procfs_open_file(struct file *f);
int64_t  procfs_read_file(struct file *f, uint64_t offset,
                          uint64_t size, void *buffer);
void     procfs_release_file(struct file *f);

// /proc root task-range readdir: the PID-directory scan (index >= base)
// runs as ONE traversal under task_list_lock, so a task deleted mid-
// scan can never be dereferenced after free; across calls the scan is
// still dynamic (spec §5.2 — users dedups by PID, a later scan fills
// the gaps).  Fills entry, returns 0; entry->name[0]='\0' when the
// index is past the last task.
struct vfs_dirent;
int procfs_readdir_tasks(uint64_t index, struct vfs_dirent *entry,
                         uint64_t base);

// Legacy no-file_t node read fallback: regenerate CURRENT content for a
// statistics node through the same snapshot producers procfs_open_file
// uses (one formatting source).  A direct vfs_read on the node — no
// file_t, no open_file hook — remains a legal path (Task 1 contract),
// so procfs_read() delegates here for stat/thermal/meminfo/status.
// Returns bytes written or a negative errno; procfs_read maps failures
// to an empty read.
int procfs_generate_node(uint32_t type, uint32_t pid, char *buf, size_t cap);

#endif // _FS_PROCFS_H
