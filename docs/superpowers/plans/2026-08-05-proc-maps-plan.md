# /proc/<pid>/maps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `/proc/<pid>/maps` showing per-process memory layout via `vfs_resolve_path()` + synthetic ELF/stack/heap regions merged with `vma_list`.

**Architecture:** New `vfs_resolve_path()` walks `node->parent` chain to mount root, skipping the root's "/" name. New `gen_maps()` synthesises ELF/stack/heap rows from `mm_t` fields and merges them with `vma_list` entries into a `snprintf`-driven text buffer. `procfs_readdir` registers "maps" at index 1 under PID directories.

**Tech Stack:** C (kernel), no external dependencies.

## Global Constraints

- `vfs_node_t` gets NO new `ino`/`dev` fields — reuse `fs_data` for inode
- ELF region: single `start_code..end_code rwxp` line (elf_load only fills start_code/end_code; 2MB pages are always RWX without XD)
- Stack: `0x800000..0xa00000 rw-p` (guarded by PAGE_XD; 0x600000 guard page is unmapped, NOT shown)
- Heap: `start_brk..end_brk rwxp` (only if start_brk < end_brk; inside ELF's 2MB RWX page for <2MB binaries)
- Output: 7 Linux-compatible columns: `start-end perms offset dev inode path`
- Maps file size: 4096; procfs local buffer: 4096
- Concurrency: no lock — accept TOCTOU risk (same as existing `gen_status`)
- No exe_path tracking — ELF and VMA path columns may be empty
- tmpfs inode (kernel pointer) leak documented, inherited from existing `vfs_stat`
- No merge of adjacent same-perm rows (known Linux divergence)

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `kernel/fs/vfs.c` | Modify | Add `vfs_resolve_path()` at end of file |
| `kernel/include/fs/vfs.h` | Modify | Add `vfs_resolve_path()` declaration |
| `kernel/include/fs/procfs.h` | Modify | Add `PROCFS_TYPE_MAPS = 5` |
| `kernel/fs/procfs.c` | Modify | Enlarge `local[512]`→`[4096]`, add `gen_maps()`, register MAPS in readdir+read |
| `user/systest.c` | Modify | Add `test_proc_maps()` |

---

### Task 1: `vfs_resolve_path()` — reverse node-to-path resolution

**Files:**
- Modify: `kernel/fs/vfs.c` (append after `vfs_truncate`)
- Modify: `kernel/include/fs/vfs.h` (add declaration after `vfs_stat`)

**Interfaces:**
- Produces: `int vfs_resolve_path(vfs_node_t *node, char *path, size_t pathsz)` — fills `path` with resolved absolute path, returns length (excl NUL) or -1 on error. Returns >= pathsz if truncated.

- [ ] **Step 1: Add declaration to vfs.h**

Insert after `vfs_stat` declaration (after current line 134):

```c
// Resolve a VFS node's full path by walking parent chain to mount root.
// Returns path length (excluding NUL), or -1 on error (NULL node/mount/name).
// Returns >= pathsz if truncated.
int vfs_resolve_path(vfs_node_t *node, char *path, size_t pathsz);
```

- [ ] **Step 2: Append implementation to vfs.c**

Add after `vfs_truncate` (at end of file, after line 702):

```c
// ── Resolve full path from a VFS node ─────────────────────────
//
// Walks parent chain up to mount root, collecting names in a
// stack array, then emits mount->path + names top-down + leaf.
// Mount root's own name ("/") is skipped to avoid doubling.
//
// Returns path length (excl NUL).  Returns >= pathsz if truncated.
// Returns -1 on error (node, mount, or name is NULL).
//
// CONTRACT: path is always NUL-terminated on return, even when
// truncated.  Caller should call with pathsz = real_bufsize - 4
// to reserve room for "...\0" appended after truncation.
int vfs_resolve_path(vfs_node_t *node, char *path, size_t pathsz)
{
    if (!node || !node->mount || !node->name)
        return -1;

    // Collect names bottom-up (max 32 depth — far more than needed)
    const char *names[32];
    int depth = 0;
    vfs_node_t *cur = node;

    while (cur) {
        if (depth >= 32)
            break;
        names[depth++] = cur->name;
        if (!cur->parent)
            break;  // reached mount root
        cur = cur->parent;
    }
    // names[0] = leaf, names[depth-1] = mount root ("/")

    // The mount root (names[depth-1]) is "/" — skip it.
    // Build: mount->path + "/" + names[depth-2] + "/" + ... + names[0]

    size_t written = 0;
    const char *mpath = node->mount->path;

    // Helper: safely write a character — NUL-terminates on overflow
    #define PUT(c) do {                                \
        if (written < pathsz) path[written] = (c);     \
        written++;                                      \
    } while (0)
    #define PUTS(s, len) do {                           \
        for (size_t _k = 0; _k < (len); _k++)          \
            PUT((s)[_k]);                               \
    } while (0)
    #define TERM() do {                                 \
        if (pathsz > 0)                                \
            path[(written < pathsz) ? written           \
                                    : pathsz - 1] = '\0'; \
    } while (0)

    if (mpath) {
        size_t mlen = strlen(mpath);
        // Strip trailing '/' from mount path (root mount has "/")
        // so we don't produce "//bin/init.elf".
        if (mlen > 0 && mpath[mlen - 1] == '/')
            mlen--;
        PUTS(mpath, mlen);
    }

    // Emit names from mount root's child down to leaf
    // (skip index depth-1 which is mount root "/")
    for (int i = depth - 2; i >= 0; i--) {
        PUT('/');
        size_t nlen = strlen(names[i]);
        PUTS(names[i], nlen);
    }

    TERM();
    #undef PUT
    #undef PUTS
    #undef TERM

    return (int)written;
}
```

- [ ] **Step 3: Verify it compiles**

```bash
make -j$(nproc) 2>&1 | tail -5
```

Expected: build succeeds. An unused-function warning for `vfs_resolve_path` is expected at this stage (no callers yet); Task 3 adds the caller and resolves it.

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/vfs.c kernel/include/fs/vfs.h
git commit -m "feat(vfs): add vfs_resolve_path() — reverse node-to-path resolution

Walks node->parent chain to mount root, skipping root's '/' name,
prepending mount->path. Returns absolute path like /bin/init.elf."
```

---

### Task 2: Add `PROCFS_TYPE_MAPS` constant

**Files:**
- Modify: `kernel/include/fs/procfs.h`

**Interfaces:**
- Produces: `PROCFS_TYPE_MAPS = 5`

- [ ] **Step 1: Add the constant**

In `kernel/include/fs/procfs.h`, after `#define PROCFS_TYPE_MEMINFO 4` (currently line 18):

```c
#define PROCFS_TYPE_MAPS     5   // /proc/<pid>/maps
```

- [ ] **Step 2: Verify it compiles**

```bash
make -j$(nproc) 2>&1 | tail -3
```

Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add kernel/include/fs/procfs.h
git commit -m "feat(procfs): add PROCFS_TYPE_MAPS=5 constant"
```

---

### Task 3: Implement `gen_maps()` and wire into procfs

**Files:**
- Modify: `kernel/fs/procfs.c`

**Interfaces:**
- Consumes: `vfs_resolve_path()` (Task 1), `PROCFS_TYPE_MAPS` (Task 2), `find_task_by_pid()`, `PROCFS_ENCODE`, `PROCFS_TYPE_PID_DIR`, `PROCFS_TYPE_SELF_DIR`, `PROCFS_PID_SELF`, `list_t` / `container_of` / `mm_t` / `vma_t` / `PAGE_4K_MASK` / `PAGE_4K_SIZE` (all existing)
- Produces: `gen_maps()` static function, MAPS case in `procfs_read`, maps entry in `procfs_readdir`, enlarged `local[]` buffer

- [ ] **Step 1: Enlarge the procfs_read local buffer**

In `procfs_read` (currently line 129), change:

```c
    char  local[512];
```

to:

```c
    char  local[4096];
```

- [ ] **Step 2: Add gen_maps() function**

Insert before `procfs_read` (before current line 106). The function synthesises ELF/stack/heap regions from `mm_t` fields, then merges with `vma_list` entries — both sources are address-ordered for ≤4MB binaries, so a two-way merge avoids allocation:

```c
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
```

- [ ] **Step 3: Add maps entry to procfs_readdir**

**3a.** In `PROCFS_TYPE_PID_DIR` case (around current line 237), add after the status entry:

Replace the current block:
```c
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
```

With:
```c
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
```

**3b.** In `PROCFS_TYPE_SELF_DIR` case (around current lines 222-234), do the same — add the `index == 1` block for maps after the existing `index == 0` status block.

- [ ] **Step 4: Add MAPS case to procfs_read**

In `procfs_read`, in the `switch (type)` block, after the `PROCFS_TYPE_MEMINFO` case:

```c
    case PROCFS_TYPE_MAPS: {
        task_t *t = find_task_by_pid((int)pid);
        if (!t) return 0;
        len = gen_maps(t, local, sizeof(local));
        break;
    }
```

- [ ] **Step 5: Build**

```bash
make -j$(nproc) 2>&1 | tail -10
```

Expected: build succeeds with no warnings (the unused-function warning from Task 1 is now resolved — gen_maps calls vfs_resolve_path).

- [ ] **Step 6: Commit**

```bash
git add kernel/fs/procfs.c
git commit -m "feat(procfs): add /proc/<pid>/maps — memory layout reporting

gen_maps() synthesises ELF, stack, and heap rows from mm_t fields
(which are NOT in vma_list), then two-way-merges with vma_list.
Output is 7-column Linux-compatible format. local buffer enlarged
512->4096."
```

---

### Task 4: Add systest test_proc_maps()

**Files:**
- Modify: `user/systest.c`

**Interfaces:**
- Consumes: `/proc/self/maps` from Task 3

- [ ] **Step 1: Add test function**

Insert before the test-runner/main function in `user/systest.c`:

```c
// ── Test /proc/self/maps ─────────────────────────────────────
static int test_proc_maps(void)
{
    char buf[4096];
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) {
        printf("FAIL: test_maps_open — open /proc/self/maps "
               "returned %d\n", fd);
        return 1;
    }

    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("FAIL: test_maps_read — read returned %d\n", n);
        return 1;
    }
    buf[n] = '\0';

    int lines = 0;
    int has_stack = 0;
    int stack_at_right_addr = 0;
    int fail_guard = 0;
    char *p = buf;
    while (*p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) { *nl = '\0'; p = nl + 1; }
        else    { p = line + strlen(line); }

        if (*line == '\0') continue;

        unsigned long start, end;
        char perm[5];
        // Parse first 6 columns (skip inode): addr-end perms off dev ino
        int fields = sscanf(line, "%lx-%lx %4s %*x %*s %*x",
                            &start, &end, perm);
        if (fields >= 3 && perm[0] != '\0')
            lines++;

        if (strstr(line, "[stack]")) {
            has_stack = 1;
            // Positive: stack at [0x800000, 0xa00000)
            if (start == 0x800000UL && end == 0xa00000UL)
                stack_at_right_addr = 1;
            // Negative: must NOT include guard page 0x600000
            if (start <= 0x600000UL && 0x600000UL < end) {
                printf("FAIL: test_maps_guard — stack line "
                       "includes 0x600000 guard page\n");
                fail_guard = 1;
            }
        }
    }

    if (fail_guard) return 1;

    if (lines < 2) {
        printf("FAIL: test_maps_lines — %d lines (need >=2)\n",
               lines);
        return 1;
    }

    if (!has_stack) {
        printf("FAIL: test_maps_stack — no [stack] label found\n");
        return 1;
    }

    if (!stack_at_right_addr) {
        printf("FAIL: test_maps_stack_addr — [stack] not at "
               "[800000,a00000)\n");
        return 1;
    }

    printf("PASS: test_proc_maps (%d lines)\n", lines);
    return 0;
}
```

- [ ] **Step 2: Register test**

Find the test table (look for `struct { const char *name; int (*fn)(void); }` or similar array of `{ "name", test_fn }` entries) and add:

```c
    { "proc_maps", test_proc_maps },
```

- [ ] **Step 3: Build and run systest**

```bash
make -j$(nproc) 2>&1 | tail -3
make run 2>&1 | grep -E "test_proc_maps|PASS|FAIL|systest.*passed"
```

Expected: `PASS: test_proc_maps (N lines)` where N >= 2, and overall systest passes.

- [ ] **Step 4: Manual smoke test at ash prompt**

After boot:
```
cat /proc/self/maps
```

Expected: 2-3 lines. Verify no `0x600000`, stack starts at `800000`.

- [ ] **Step 5: Commit**

```bash
git add user/systest.c
git commit -m "test: add test_proc_maps() for /proc/self/maps

Verifies open/read, format pattern, [stack] label, correct stack
range [800000,a00000), and absence of 0x600000 guard page."
```

---

## Self-Review Summary

**Spec coverage check:**
| Spec requirement | Covered by |
|-----------------|------------|
| vfs_resolve_path NUL-termination guarantee (incl early returns) | Task 1 Step 2 (PUT/TERM macros) |
| vfs_resolve_path mount->path trailing-slash fix | Task 1 Step 2 (strips '/' from mount->path tail to avoid "//") |
| gen_maps 4-section merge (ELF/stack/heap/vma_list) | Task 3 Step 2 (two-way merge loop) |
| All addresses rounded to PAGE_4K | Task 3 Step 2 (ELF=code block, heap=round both, stack=round base) |
| 7-column Linux format | Task 3 Step 2 (EMIT macro) |
| PROCFS_TYPE_MAPS = 5 | Task 2 Step 1 |
| local[4096] enlargement | Task 3 Step 1 |
| readdir PID_DIR and SELF_DIR entries | Task 3 Step 3a + 3b |
| read MAPS case | Task 3 Step 4 |
| Permissions per PTE (ELF=rwxp, stack=rw-p, heap=rwxp) | Task 3 Step 2 (literal strings in EMIT calls) |
| No ino/dev fields in vfs_node_t | Confirmed — no vfs_node_t changes anywhere |
| Two-way merge (sorted sources) | Task 3 Step 2 (next_synth vs next_vma_start comparison) |
| No merge of adjacent rows | Task 3 Step 2 (each row emitted independently) |
| Path truncation with `...` | Task 3 Step 2 (strlen-based positioning after vfs_resolve_path, PATH_RESOLVE_BUF = sizeof-4) |
| Concurrency risk acceptance | Task 3 Step 2 (gen_maps top comment) |
| Test: open/read/pattern/[stack]/range/guard-page | Task 4 Step 1 |

**Placeholder scan:** No TBD, TODO, or incomplete sections. All code is concrete.

**Type consistency:** `vfs_resolve_path` signature matches across Task 1 (declaration) and Task 3 (call site). `gen_maps` returns `int`. `PAGE_4K_MASK`/`PAGE_4K_SIZE` are existing macros (kernel/include/kernel/pmm.h:16-19). `UINT64_MAX` from `<stdint.h>` already used throughout kernel.
