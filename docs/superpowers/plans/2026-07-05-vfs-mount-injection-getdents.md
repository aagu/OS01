# VFS Root Directory Listing — Mount Point Injection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `ls /` show VFS mount points (`/dev`, `/proc`) alongside FAT32 files, alphabetically sorted.

**Architecture:** Modify `vfs_getdents()` in `kernel/fs/vfs.c` to collect entries from both the underlying filesystem and the mount table, merge them, sort case-insensitively, and output the combined list. No changes to FAT32, devfs, procfs, busybox, or the syscall handler.

**Tech Stack:** C, OS01 VFS

---

### Task 1: Add internal data structures and helper to vfs.c

**Files:**
- Modify: `kernel/fs/vfs.c`

Add a thread-local temp entry struct used only inside `vfs_getdents()`:

```c
// ── Sorted getdents entry (internal, for mount-point injection) ──
#define VFS_GETDENTS_SORT_MAX 128

typedef struct {
    char     name[VFS_NAME_MAX];
    uint64_t size;
    uint8_t  type;
    uint32_t ino;
} vfs_sort_entry_t;
```

Add a helper to determine if a mount point is a direct child of a directory path:

```c
// ── Check if mount_path is a direct child of dir_path ───────
// Returns 1 if mount_path is exactly one component deeper than dir_path.
static int vfs_is_child_mount(const char *dir_path, const char *mount_path)
{
    size_t dlen = strlen(dir_path);

    // Root "/" — path has no components, so child must be exactly one level deep
    if (dlen == 1 && dir_path[0] == '/') {
        if (mount_path[0] != '/' || mount_path[1] == '\0')
            return 0;
        // e.g., "/dev" → "dev" (no '/' after position 1)
        return strchr(mount_path + 1, '/') == NULL;
    }

    // Non-root: mount_path = dir_path + "/" + one_component
    if (strncmp(dir_path, mount_path, dlen) != 0)
        return 0;
    if (mount_path[dlen] != '/')
        return 0;
    return strchr(mount_path + dlen + 1, '/') == NULL;
}
```

- [ ] **Step 1: Add `vfs_sort_entry_t` typedef and `VFS_GETDENTS_SORT_MAX` constant**

Insert after the existing `vfs_dirent_t` usage area, before `vfs_getdents()`:

```c
#define VFS_GETDENTS_SORT_MAX 128

typedef struct {
    char     name[VFS_NAME_MAX];
    uint64_t size;
    uint8_t  type;
    uint32_t ino;
} vfs_sort_entry_t;
```

- [ ] **Step 2: Add `vfs_is_child_mount()` helper function**

Insert before `vfs_getdents()`: the static helper defined above.

---

### Task 2: Rewrite `vfs_getdents()` with mount injection + sorting

**Files:**
- Modify: `kernel/fs/vfs.c:343-387`

Replace the entire `vfs_getdents()` function body. The new implementation:

1. **Phase 1 (Collect):** Read all entries from the underlying FS via `vfs_readdir()`
2. **Phase 2 (Inject):** If this node is a mount root, scan the mount table for direct child mounts and add them as directory entries
3. **Phase 3 (Sort):** Bubble-sort all entries by name using the existing case-insensitive `vfs_name_cmp()`
4. **Phase 4 (Output):** Walk the sorted array starting at `*pos`, writing entries to the user buffer until full

```c
int vfs_getdents(vfs_node_t *dir, struct linux_dirent64 *buf, unsigned int count,
                 uint64_t *pos)
{
    if (!dir || !buf || !pos || dir->type != VFS_DIR)
        return -1;

    // ── Phase 1: Collect entries from the underlying filesystem ──
    vfs_sort_entry_t entries[VFS_GETDENTS_SORT_MAX];
    int total = 0;
    vfs_dirent_t de;
    uint64_t idx = 0;

    while (total < VFS_GETDENTS_SORT_MAX) {
        int ret = vfs_readdir(dir, idx++, &de);
        if (ret != 0) continue;
        if (de.name[0] == '\0') break;

        size_t nlen = strlen(de.name);
        if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
        memcpy(entries[total].name, de.name, nlen);
        entries[total].name[nlen] = '\0';
        entries[total].size = de.size;
        entries[total].type = de.type;
        entries[total].ino  = de.ino;
        total++;
    }

    // ── Phase 2: Inject sub-mount entries ──────────────────────
    if (dir->mount && dir == dir->mount->root) {
        for (int i = 0; i < mount_count && total < VFS_GETDENTS_SORT_MAX; i++) {
            if (&mount_table[i] == dir->mount) continue;  // skip self
            if (!vfs_is_child_mount(dir->mount->path, mount_table[i].path))
                continue;

            // Extract basename (skip leading "/")
            const char *base = mount_table[i].path;
            if (base[0] == '/') base++;
            size_t blen = strlen(base);
            if (blen >= VFS_NAME_MAX) blen = VFS_NAME_MAX - 1;
            memcpy(entries[total].name, base, blen);
            entries[total].name[blen] = '\0';
            entries[total].size = 0;
            entries[total].type = VFS_DIR;
            entries[total].ino  = (uint32_t)(0x80000000 | (uint32_t)i); // sentinel
            total++;
        }
    }

    // ── Phase 3: Sort by name (case-insensitive) ──────────────
    for (int i = 0; i < total - 1; i++) {
        for (int j = 0; j < total - 1 - i; j++) {
            if (vfs_name_cmp(entries[j].name, entries[j + 1].name) > 0) {
                vfs_sort_entry_t tmp = entries[j];
                entries[j]     = entries[j + 1];
                entries[j + 1] = tmp;
            }
        }
    }

    // ── Phase 4: Output from sorted list starting at *pos ────
    unsigned int bytes_written = 0;
    while (*pos < (uint64_t)total) {
        vfs_sort_entry_t *e = &entries[*pos];
        size_t name_len = strlen(e->name);
        uint16_t reclen = (uint16_t)(sizeof(struct linux_dirent64) + name_len + 1);
        reclen = (reclen + 7) & ~7;  // 8-byte align

        if (bytes_written + reclen > count)
            break;

        struct linux_dirent64 *d = (struct linux_dirent64 *)((char *)buf + bytes_written);
        d->d_ino   = e->ino;
        d->d_off   = (int64_t)(*pos + 1);
        d->d_reclen = reclen;

        switch (e->type) {
        case VFS_FILE:   d->d_type = DT_REG; break;
        case VFS_DIR:    d->d_type = DT_DIR; break;
        case VFS_CHRDEV: d->d_type = DT_CHR; break;
        case VFS_BLKDEV: d->d_type = DT_BLK; break;
        default:         d->d_type = DT_UNKNOWN; break;
        }

        memcpy(d->d_name, e->name, name_len + 1);
        bytes_written += reclen;
        (*pos)++;
    }

    return (int)bytes_written;
}
```

- [ ] **Step 1: Delete the old `vfs_getdents()` body**

Remove lines from `int vfs_getdents(vfs_node_t *dir...` through the closing `}` (current lines 343-387).

- [ ] **Step 2: Write the new `vfs_getdents()` body**

Insert the new implementation above (with the 4 phases).

---

### Task 3: Build and smoke test

**Files:**
- Run: `make clean && make`

- [ ] **Step 1: Clean rebuild**

```bash
make clean && make
```

Expected: clean compile, `disk.img` generated.

- [ ] **Step 2: Run QEMU and test `ls /`**

```bash
make run
```

In the shell, type `ls /` and verify:
- Mount points `dev` and `proc` appear in the listing
- FAT32 files (`init.elf`, `sh.elf`, etc.) also appear
- All entries are alphabetically sorted
- `ls -l /dev` still works (individual device files)
- `ls -l /proc` still works (self, meminfo, pid dirs)
- `ls /proc/self/status` content is correct

- [ ] **Step 3: Commit**

```bash
git add kernel/fs/vfs.c
git commit -m "feat(vfs): inject mount points into getdents with alphabetical sorting

Modify vfs_getdents() to collect entries from both the underlying
filesystem and the VFS mount table, merge, sort case-insensitively,
then return the combined list. This makes ls / show /dev, /proc
alongside FAT32 files.

Co-Authored-By: Claude <noreply@anthropic.com>
```
