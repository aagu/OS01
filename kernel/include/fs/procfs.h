#ifndef _FS_PROCFS_H
#define _FS_PROCFS_H

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

#define PROCFS_PID_SELF       0xFFFFFF   // sentinel: resolve to current->pid in read()

// ── Public API ─────────────────────────────────────────────────

void procfs_init(void);

#endif // _FS_PROCFS_H
