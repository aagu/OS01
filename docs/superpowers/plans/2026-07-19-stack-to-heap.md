# 内核栈空间改造 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将内核栈上的大块缓冲（ext2 4KB 块缓冲、vfs_getdents entries、selftest save_*）迁移到堆，配合 -O2 将 STACK_SIZE 从 64KB 降至 32KB。

**Architecture:** 逐函数改造：每个 ext2 函数将栈上的 4KB 缓冲改为 `kmalloc(4096)`/`kfree` 对；`vfs_getdents` 的 17KB entries 数组迁移到堆；selftest 的 save_* 缓冲改为 `kmalloc`/`kfree` 并用 `goto cleanup` 统一释放。实现 `kzalloc`，NDEBUG 时启用 -O2。

**Tech Stack:** C (clang), ld.lld, x86_64 kernel, slab allocator (kmalloc/kfree)

## Global Constraints

- STACK_SIZE: 最终 32 KB（无条件，不与 NDEBUG 联动）
- `NDEBUG=1` 时 -O2，否则 -O0
- ext2 所有函数保留 `noinline`
- 所有 kmalloc 调用必须有 NULL 检查
- `uint32_t` 返回函数失败 sentinel 为 `0`（不是 `-ENOMEM`）
- 实施顺序：先堆迁移 → 再 -O2 → 最后确认栈大小

---

### Task 1: 安全措施 — 临时还原 STACK_SIZE 至 64KB

**Files:**
- Modify: `kernel/include/kernel/task.h:40`

**Interfaces:**
- Produces: `STACK_SIZE = 64 * 1024`（临时安全值，最后一个 task 改回 32KB）

**⚠️ 背景**: `task.h:40` 当前已是 `(32 * 1024)`，但堆迁移尚未实现。rename 路径峰值 ~34KB 会溢出。在堆迁移完成前必须先还原为 64KB。

- [ ] **Step 1: 还原 STACK_SIZE 为 64KB**

```c
// kernel/include/kernel/task.h:40
#define STACK_SIZE (64 * 1024) // 64KB — ext2 4KB block buffers need ~24KB peak
```

- [ ] **Step 2: 编译验证**

```bash
make -C kernel kernel.bin 2>&1 | tail -5
```

Expected: 编译成功。

- [ ] **Step 3: Commit**

```bash
git add kernel/include/kernel/task.h
git commit -m "fix: temporary revert STACK_SIZE to 64KB before heap migration

Heap migration hasn't been implemented yet. The rename path peaks at
~34KB with current -O0 code, exceeding the prematurely-set 32KB limit.
Will shrink back to 32KB after all heap migrations are complete."
```

---

### Task 2: 实现 kzalloc

**Files:**
- Modify: `kernel/memory/slab.c` (append)
- Modify: `kernel/include/kernel/slab.h:37`

**Interfaces:**
- Produces: `void *kzalloc(size_t size)` — kmalloc + memset to 0

- [ ] **Step 1: 重命名声明并实现**

```c
// kernel/include/kernel/slab.h:37 — 将 kcalloc 改为 kzalloc
void * kzalloc(size_t size);
```

```c
// kernel/memory/slab.c — 在 kfree 之后追加
void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}
```

- [ ] **Step 2: 编译验证**

```bash
make -C kernel kernel.bin 2>&1 | tail -5
```

Expected: 编译成功（无调用者，不会有链接错误）。

- [ ] **Step 3: Commit**

```bash
git add kernel/memory/slab.c kernel/include/kernel/slab.h
git commit -m "feat: implement kzalloc (was kcalloc stub)

Rename single-arg kcalloc to kzalloc since it doesn't match the
standard calloc(nmemb, size) signature. kzalloc = kmalloc + memset(0)."
```

---

### Task 3: ext2 I/O 辅助函数 — int 返回型

**Files:**
- Modify: `kernel/fs/ext2.c:38-62` (ext2_read_inode)
- Modify: `kernel/fs/ext2.c:63-84` (ext2_write_inode)
- Modify: `kernel/fs/ext2.c:85-108` (ext2_write_superblock)
- Modify: `kernel/fs/ext2.c:1222-1251` (ext2_init)

**Interfaces:**
- Consumes: `kzalloc` (Task 2)
- Produces: 4 个函数栈分配迁移到 kmalloc/kfree

- [ ] **Step 1: ext2_read_inode — `uint8_t buf[4096]` → kmalloc**

```c
// kernel/fs/ext2.c:54 — 替换
// Before:
    uint8_t buf[4096];
    if (ext2_read_block(fs, table_start + block_off, buf) != 0)
        return -1;
    memcpy(out, buf + inode_off, sizeof(ext2_inode_t));
    return 0;

// After:
    uint8_t *buf = kmalloc(4096);
    if (!buf) return -ENOMEM;

    int rc = -1;
    if (ext2_read_block(fs, table_start + block_off, buf) != 0)
        goto out;
    memcpy(out, buf + inode_off, sizeof(ext2_inode_t));
    rc = 0;
out:
    kfree(buf);
    return rc;
```

- [ ] **Step 2: ext2_write_inode — `uint8_t buf[4096]` → kmalloc**

```c
// kernel/fs/ext2.c:76 — 替换
// Before:
    uint8_t buf[4096];
    if (ext2_read_block(fs, table_start + block_off, buf) != 0)
        return -1;
    memcpy(buf + inode_off, inode, sizeof(ext2_inode_t));
    return ext2_write_block(fs, table_start + block_off, buf);

// After:
    uint8_t *buf = kmalloc(4096);
    if (!buf) return -ENOMEM;

    if (ext2_read_block(fs, table_start + block_off, buf) != 0) {
        kfree(buf);
        return -1;
    }
    memcpy(buf + inode_off, inode, sizeof(ext2_inode_t));
    int rc = ext2_write_block(fs, table_start + block_off, buf);
    kfree(buf);
    return rc;
```

- [ ] **Step 3: ext2_write_superblock — `sb_buf[1024]` → kmalloc**

```c
// kernel/fs/ext2.c:90-103 — 替换
// Before:
    uint8_t sb_buf[1024];
    memset(sb_buf, 0, 1024);
    memcpy(sb_buf, &fs->sb_raw, sizeof(ext2_superblock_t));
    if (block_device_write(fs->dev, 2, 2, sb_buf) != 0)
        return -1;
    // ... write bg descriptors ...

// After:
    uint8_t *sb_buf = kmalloc(1024);
    if (!sb_buf) return -ENOMEM;
    memset(sb_buf, 0, 1024);
    memcpy(sb_buf, &fs->sb_raw, sizeof(ext2_superblock_t));
    if (block_device_write(fs->dev, 2, 2, sb_buf) != 0) {
        kfree(sb_buf);
        return -1;
    }
    kfree(sb_buf);  // sb_buf no longer needed — bgdesc loop uses fs->bgdesc_table

    for (uint32_t i = 0; i < fs->bgdesc_table_blocks; i++) {
        if (ext2_write_block(fs, fs->bgdesc_block + i,
            (uint8_t *)fs->bgdesc_table + i * fs->block_size) != 0)
            return -1;
    }
    return 0;
```

kfree(sb_buf) 放在 bgdesc 循环之前是安全的——sb_buf 仅用于 superblock 写回，bgdesc 使用 `fs->bgdesc_table`（独立内存）。 ✅

- [ ] **Step 4: ext2_init — `sb_buf[1024]` → kmalloc，在最后使用点立即释放**

```c
// kernel/fs/ext2.c:1233-1236 — 替换
// Before:
    uint8_t sb_buf[1024];
    if (block_device_read(dev, 2, 2, sb_buf) != 0) {
        kfree(fs); return -1;
    }

// After:
    uint8_t *sb_buf = kmalloc(1024);
    if (!sb_buf) { kfree(fs); return -ENOMEM; }
    if (block_device_read(dev, 2, 2, sb_buf) != 0) {
        kfree(sb_buf); kfree(fs); return -1;
    }
```

然后 `sb_buf` 引用保持不变，直到 `memcpy(&fs->sb_raw, sb_buf, sizeof(ext2_superblock_t))`（约 line 1252）。**在该行之后立即加 `kfree(sb_buf)`**——sb_buf 之后不再使用。后续的所有 return 点（bgdesc 分配失败等）无需再关心 `sb_buf`。

- [ ] **Step 5: 编译 & Commit**

```bash
make -C kernel kernel.bin 2>&1 | tail -5
# Expected: 编译成功
git add kernel/fs/ext2.c
git commit -m "refactor(ext2): move read/write_inode, write_superblock, init buffers to heap

ext2_read_inode, ext2_write_inode: uint8_t buf[4096] → kmalloc(4096)
ext2_write_superblock, ext2_init: uint8_t sb_buf[1024] → kmalloc(1024)"
```

---

### Task 4: ext2 bmap 函数 — uint32_t 返回型

**Files:**
- Modify: `kernel/fs/ext2.c:253-275` (ext2_bmap)
- Modify: `kernel/fs/ext2.c:276-317` (ext2_bmap_alloc)

**Interfaces:**
- Returns `uint32_t` — failure sentinel = `0`（not -ENOMEM）

- [ ] **Step 1: ext2_bmap — `uint32_t indirect[1024]` → kmalloc**

```c
// kernel/fs/ext2.c:264-268 — 替换
// Before:
        uint32_t indirect[1024];  // up to 4096/4 = 1024 entries
        if (ext2_read_block(fs, indirect_blk, indirect) != 0)
            return 0;
        return indirect[logical_block - 12];

// After:
        uint32_t *indirect = kmalloc(4096);
        if (!indirect) return 0;  // uint32_t: 0 = failure
        uint32_t result = 0;
        if (ext2_read_block(fs, indirect_blk, indirect) == 0)
            result = indirect[logical_block - 12];
        kfree(indirect);
        return result;
```

- [ ] **Step 2: ext2_bmap_alloc — `uint32_t indirect[1024]` → kmalloc**

```c
// kernel/fs/ext2.c:300-311 — 替换
// Before:
        uint32_t indirect[1024];
        if (ext2_read_block(fs, inode->i_block[12], indirect) != 0)
            return 0;
        // ... modify indirect[idx] ...
        ext2_write_block(fs, inode->i_block[12], indirect);
        return indirect[idx];

// After:
        uint32_t *indirect = kmalloc(4096);
        if (!indirect) return 0;

        if (ext2_read_block(fs, inode->i_block[12], indirect) != 0) {
            kfree(indirect);
            return 0;
        }

        uint32_t idx = logical_block - 12;
        if (indirect[idx] == 0) {
            indirect[idx] = alloc_block(fs);
            if (indirect[idx] == 0) { kfree(indirect); return 0; }
            inode->i_blocks += fs->block_size / 512;
            ext2_write_block(fs, inode->i_block[12], indirect);
        }
        uint32_t result = indirect[idx];
        kfree(indirect);
        return result;
```

- [ ] **Step 3: 编译 & Commit**

```bash
make -C kernel kernel.bin 2>&1 | tail -5
git add kernel/fs/ext2.c
git commit -m "refactor(ext2): move bmap indirect[1024] to heap

ext2_bmap, ext2_bmap_alloc: uint32_t indirect[1024] → kmalloc(4096)
Return 0 on alloc failure (uint32_t sentinel)."
```

---

### Task 5: ext2 alloc/free 函数 — 混合返回型

**Files:**
- Modify: `kernel/fs/ext2.c:109-151` (alloc_block)
- Modify: `kernel/fs/ext2.c:152-178` (free_block)
- Modify: `kernel/fs/ext2.c:179-227` (alloc_inode)
- Modify: `kernel/fs/ext2.c:228-252` (free_inode)

**Interfaces:**
- `alloc_block` / `alloc_inode`: `uint32_t` return → failure sentinel = `0`
- `free_block` / `free_inode`: `void` return → kmalloc 失败时静默 `return`
- `alloc_block`: `buf[4096]` 和 `zero[4096]` 合并为单次 kmalloc（复用）

- [ ] **Step 1: alloc_block — `buf[4096]` + `zero[4096]` → 单次 kmalloc**

```c
// kernel/fs/ext2.c:116-143 — 替换
// Before:
        uint8_t buf[4096];
        if (ext2_read_block(fs, bitmap_block, buf) != 0)
            continue;
        // ... find free bit in buf ...
        ext2_write_block(fs, bitmap_block, buf);
        // ... update counts ...
        ext2_write_superblock(fs);
        uint8_t zero[4096];
        memset(zero, 0, fs->block_size);
        ext2_write_block(fs, block, zero);
        return block;

// After:
        uint8_t *buf = kmalloc(4096);
        if (!buf) return 0;  // OOM
        if (ext2_read_block(fs, bitmap_block, buf) != 0) {
            kfree(buf);
            continue;
        }
        // ... find free bit in buf ...
        ext2_write_block(fs, bitmap_block, buf);
        // ... update counts ...
        ext2_write_superblock(fs);
        // Reuse buf for zero-fill (write to disk frees buf for reuse)
        memset(buf, 0, fs->block_size);
        ext2_write_block(fs, block, buf);
        kfree(buf);
        return block;
```

- [ ] **Step 2: free_block — `buf[4096]` → kmalloc（void 返回，静默失败）**

```c
// kernel/fs/ext2.c:165-170 — 替换
// Before:
    uint8_t buf[4096];
    if (ext2_read_block(fs, bitmap_block, buf) != 0)
        return;
    buf[byte_idx] &= ~(1u << bit);
    ext2_write_block(fs, bitmap_block, buf);

// After:
    uint8_t *buf = kmalloc(4096);
    if (!buf) return;  // void: silent failure
    if (ext2_read_block(fs, bitmap_block, buf) != 0) {
        kfree(buf);
        return;
    }
    buf[byte_idx] &= ~(1u << bit);
    ext2_write_block(fs, bitmap_block, buf);
    kfree(buf);
```

- [ ] **Step 3: alloc_inode — `buf[4096]` → kmalloc**

```c
// kernel/fs/ext2.c:186-190 — 替换
// Before:
        uint8_t buf[4096];
        if (ext2_read_block(fs, bitmap_block, buf) != 0)
            continue;

// After:
        uint8_t *buf = kmalloc(4096);
        if (!buf) return 0;
        if (ext2_read_block(fs, bitmap_block, buf) != 0) {
            kfree(buf);
            continue;
        }
```

然后在 `return ino;` 前加 `kfree(buf);`。

- [ ] **Step 4: free_inode — `buf[4096]` → kmalloc**

```c
// kernel/fs/ext2.c:240-244 — 替换
// Before:
    uint8_t buf[4096];
    if (ext2_read_block(fs, bitmap_block, buf) != 0)
        return;
    buf[byte_idx] &= ~(1u << bit);
    ext2_write_block(fs, bitmap_block, buf);

// After:
    uint8_t *buf = kmalloc(4096);
    if (!buf) return;
    if (ext2_read_block(fs, bitmap_block, buf) != 0) {
        kfree(buf);
        return;
    }
    buf[byte_idx] &= ~(1u << bit);
    ext2_write_block(fs, bitmap_block, buf);
    kfree(buf);
```

- [ ] **Step 5: 编译 & Commit**

```bash
make -C kernel kernel.bin 2>&1 | tail -5
git add kernel/fs/ext2.c
git commit -m "refactor(ext2): move alloc/free block/inode buffers to heap

alloc_block: merge buf[4096]+zero[4096] into single kmalloc(4096)
free_block: void return, silent failure on OOM
alloc_inode, free_inode: uint8_t buf[4096] → kmalloc(4096)"
```

---

### Task 6: ext2 目录操作辅助函数 — find_dirent/dirent_add/dirent_del

**Files:**
- Modify: `kernel/fs/ext2.c:318-358` (ext2_find_dirent)
- Modify: `kernel/fs/ext2.c:359-458` (dirent_add)
- Modify: `kernel/fs/ext2.c:459-496` (dirent_del)

**Interfaces:**
- 三者均返回 `int`，失败 sentinel = `-ENOMEM`

- [ ] **Step 1: ext2_find_dirent — `block_data[4096]` → kmalloc**

```c
// kernel/fs/ext2.c:329 — 替换
// Before:
    uint8_t block_data[4096];
    // for loop reads blocks into block_data...

// After:
    uint8_t *block_data = kmalloc(4096);
    if (!block_data) return -ENOMEM;
    int rc = -ENOENT;

    for (uint32_t blk_idx = 0; ; blk_idx++) {
        // ... same loop logic, replace return with goto out ...
        // if found: rc = 0; goto out;
    }

out:
    kfree(block_data);
    return rc;
```

- [ ] **Step 2: dirent_add — `block_data[4096]` → kmalloc**

```c
// kernel/fs/ext2.c:370 — 替换
// Before:
    uint8_t block_data[4096];

// After:
    uint8_t *block_data = kmalloc(4096);
    if (!block_data) return -ENOMEM;
```

所有 `return` 改为 `{ kfree(block_data); return ...; }`。函数中的 `return 0` / `return -ENOSPC` / `return -EIO` 均需加 kfree。

- [ ] **Step 3: dirent_del — `block_data[4096]` → kmalloc**

```c
// kernel/fs/ext2.c:467 — 替换
// Before:
    uint8_t block_data[4096];

// After:
    uint8_t *block_data = kmalloc(4096);
    if (!block_data) return -ENOMEM;
```

所有 `return` 改为 `{ kfree(block_data); return ...; }`。

- [ ] **Step 4: 编译 & Commit**

```bash
make -C kernel kernel.bin 2>&1 | tail -5
git add kernel/fs/ext2.c
git commit -m "refactor(ext2): move find_dirent/dirent_add/dirent_del block_data to heap

ext2_find_dirent, dirent_add, dirent_del: uint8_t block_data[4096] → kmalloc(4096)"
```

---

### Task 7: ext2 VFS read/write/truncate/unlink

**Files:**
- Modify: `kernel/fs/ext2.c:497-544` (ext2_vfs_read)
- Modify: `kernel/fs/ext2.c:545-618` (ext2_vfs_write)
- Modify: `kernel/fs/ext2.c:620-697` (ext2_vfs_truncate)
- Modify: `kernel/fs/ext2.c:747-805` (ext2_vfs_unlink)

**Interfaces:**
- 四者均返回 `int`，失败 sentinel = `-ENOMEM`

- [ ] **Step 1: ext2_vfs_read — `block_buf[4096]` → kmalloc（循环外一次分配，复用到循环结束）**

```c
// kernel/fs/ext2.c:519-535 — Before (scan for logical_block in while loop):
    // ... existing ...

// Replace the block_buf declaration AND the while loop body:
// Before:
    while (remaining > 0) {
        // ...
        uint8_t block_buf[4096];  // fixed size, block_size ≤ 4096
        if (ext2_read_block(fs, phys, block_buf) != 0) {
            spin_unlock(&fs->lock); return -1;
        }
        // ... use block_buf ...
    }

// After:
    uint8_t *block_buf = kmalloc(4096);
    if (!block_buf) {
        spin_unlock(&fs->lock);
        return -ENOMEM;
    }

    while (remaining > 0) {
        // ... same loop logic ...
        if (ext2_read_block(fs, phys, block_buf) != 0) {
            kfree(block_buf);
            spin_unlock(&fs->lock);
            return -1;
        }
        // ... use block_buf ...
    }

    kfree(block_buf);
    spin_unlock(&fs->lock);
    return (int)size;
```

循环一次分配、多次迭代复用，避免 N 次 kmalloc/kfree。循环内 `return -1` 需改为 `{ kfree(block_buf); spin_unlock(&fs->lock); return -1; }`。

- [ ] **Step 2: ext2_vfs_write — `block_buf[4096]` → kmalloc（循环外一次分配）**

与 read 同样模式：`block_buf` 在 `while` 之前分配一次，循环结束后释放。hot path 上避免重复 kmalloc/kfree。

```c
// kernel/fs/ext2.c:577-614 — 在 while 之前分配，循环结束后释放
// Before:
    while (remaining > 0) {
        // ...
        uint8_t block_buf[4096];
        if (ext2_read_block(fs, phys, block_buf) != 0) { ... }
        // ... RMW: read → modify → write ...
    }

// After:
    uint8_t *block_buf = kmalloc(4096);
    if (!block_buf) { spin_unlock(&fs->lock); return -ENOMEM; }

    while (remaining > 0) {
        // ... same logic, block_buf overwritten each iteration ...
        if (ext2_read_block(fs, phys, block_buf) != 0) { ... }
        memcpy(block_buf + block_off, src, chunk);
        // ...
    }

    kfree(block_buf);
    spin_unlock(&fs->lock);
    return (int)size;
```

循环内 `return` 点加 `kfree(block_buf)`。

- [ ] **Step 3: ext2_vfs_truncate — `indirect[1024]` → kmalloc（循环内）**

```c
// kernel/fs/ext2.c:663-669 — 替换
// Before:
                    uint32_t indirect[1024];
                    if (ext2_read_block(fs, inode.i_block[12], indirect) != 0) {

// After:
                    uint32_t *indirect = kmalloc(4096);
                    if (!indirect) { spin_unlock(&fs->lock); return -ENOMEM; }
                    if (ext2_read_block(fs, inode.i_block[12], indirect) != 0) {
                        kfree(indirect);
                        spin_unlock(&fs->lock);
                        return -EIO;
                    }
```

然后循环体结束后 `kfree(indirect)`。

- [ ] **Step 4: ext2_vfs_unlink 的 `block_data[4096]` → kmalloc**

在 ext2_vfs_unlink 中找到 `uint8_t block_data[4096]` 声明，替换为 kmalloc 并加 NULL 检查 + kfree 在函数出口。

- [ ] **Step 5: 编译 & Commit**

```bash
make -C kernel kernel.bin 2>&1 | tail -5
git add kernel/fs/ext2.c
git commit -m "refactor(ext2): move read/write/truncate/unlink block buffers to heap"
```

---

### Task 8: ext2 VFS mkdir/readdir + vfs_getdents

**Files:**
- Modify: `kernel/fs/ext2.c:806-905` (ext2_vfs_mkdir)
- Modify: `kernel/fs/ext2.c:989-1052` (ext2_vfs_readdir)
- Modify: `kernel/fs/vfs.c:391-460` (vfs_getdents)

- [ ] **Step 1: ext2_vfs_mkdir — `block_data[4096]` → kmalloc**

在 mkdir 中找到 `uint8_t block_data[4096]` 并替换：
```c
// Before:
    uint8_t block_data[4096];

// After:
    uint8_t *block_data = kmalloc(4096);
    if (!block_data) return NULL;  // returns struct vfs_node *, NULL = failure
```
所有 return 点加 `kfree(block_data)`。

- [ ] **Step 2: ext2_vfs_readdir — `block_data[4096]` → kmalloc**

类似模式，`uint8_t block_data[4096]` → `kmalloc(4096)`，失败返回 `-ENOMEM`。

- [ ] **Step 3: vfs_getdents — `entries[64]` → kmalloc**

```c
// kernel/fs/vfs.c:398 — 替换
// Before:
    vfs_dirent_t entries[VFS_GETDENTS_SORT_MAX];

// After:
    vfs_dirent_t *entries = kmalloc(sizeof(vfs_dirent_t) * VFS_GETDENTS_SORT_MAX);
    if (!entries) return -ENOMEM;
```

然后 `entries[total].name` → `entries[total].name`（指针语义不变），函数所有 return 点前加 `kfree(entries)`。

- [ ] **Step 4: 编译 & Commit**

```bash
make -C kernel kernel.bin 2>&1 | tail -5
git add kernel/fs/ext2.c kernel/fs/vfs.c
git commit -m "refactor: move ext2 mkdir/readdir + vfs_getdents buffers to heap

ext2_vfs_mkdir, ext2_vfs_readdir: block_data[4096] → kmalloc
vfs_getdents: entries[64] → kmalloc (17KB saved on stack)"
```

---

### Task 9: ext2 VFS rename + rmdir

**Files:**
- Modify: `kernel/fs/ext2.c:1053-1221` (ext2_vfs_rename)
- Modify: `kernel/fs/ext2.c:906-988` (ext2_vfs_rmdir)

**Interfaces:**
- 两者均返回 `int`，失败 sentinel = `-ENOMEM`

- [ ] **Step 1: ext2_vfs_rename — 多处堆栈分配**

rename 有 3 个需要改造的栈分配：
1. `uint8_t bd[4096]` (line ~1084) — 遍历目录块
2. `uint32_t indirect[1024]` (line ~1110) — 释放间接块中的块（if 分支）
3. `uint32_t indirect[1024]` (line ~1136) — 另一分支释放间接块
4. `uint8_t dir_data[4096]` (line ~1178) — 更新 `..` 条目

2 和 3 互斥，用一个 `uint32_t *indirect = kmalloc(4096)` 在需要前分配，用完 kfree 即可。

逐一替换，每个分配点添加 NULL 检查后 `{ spin_unlock(&fs->lock); kfree(之前分配的); return -ENOMEM; }`。

- [ ] **Step 2: ext2_vfs_rmdir — `bd[4096]` + `indirect[1024]`**

```c
// rmdir 中的 bd[4096] (line ~1084 in rmdir body):
// Before:
    uint8_t bd[4096];

// After:
    uint8_t *bd = kmalloc(4096);
    if (!bd) { spin_unlock(&fs->lock); return -ENOMEM; }
```

所有 return 点加 `kfree(bd)`。

类似的 `indirect[1024]` 替换为 `kmalloc(4096)`。

- [ ] **Step 3: 编译 & Commit**

```bash
make -C kernel kernel.bin 2>&1 | tail -5
git add kernel/fs/ext2.c
git commit -m "refactor(ext2): move rename/rmdir block buffers to heap

ext2_vfs_rename: bd[4096] + dir_data[4096] + two indirect[1024] → kmalloc
  (the two indirect[1024] are in mutually exclusive branches)
ext2_vfs_rmdir: bd[4096] + indirect[1024] → kmalloc"
```

---

### Task 10: ext2 selftest save_* 缓冲

**Files:**
- Modify: `kernel/fs/ext2.c:1315-1368` (ext2_selftest_block_alloc)
- Modify: `kernel/fs/ext2.c:1370-1415` (ext2_selftest_inode_alloc)
- Modify: `kernel/fs/ext2.c:1417-1496` (ext2_selftest_dirent_roundtrip)
- Modify: `kernel/fs/ext2.c:1508-1581` (ext2_selftest_write_read)

**Interfaces:**
- selftest 函数仅在 `#ifdef OS01_SELFTEST` 时编译
- 所有改 `goto cleanup` 统一释放

- [ ] **Step 1: ext2_selftest_block_alloc — 4 个 save_* 改 kmalloc**

```c
// kernel/fs/ext2.c:1337 — 替换
// Before:
    uint8_t save_bitmap[4096], save_bgdesc[4096], save_sb[1024];
    // ... later ...
    uint8_t save_blk[4096];

// After (函数顶部分配，末尾 cleanup label 释放):
    uint8_t *save_bitmap = kmalloc(4096);
    uint8_t *save_bgdesc = kmalloc(4096);
    uint8_t *save_sb = kmalloc(1024);
    uint8_t *save_blk = NULL;
    if (!save_bitmap || !save_bgdesc || !save_sb) goto cleanup;

    // ... 原有逻辑 ...

    save_blk = kmalloc(4096);
    if (!save_blk) goto cleanup;
    // ... 原有逻辑 ...

cleanup:
    if (save_blk) kfree(save_blk);
    if (save_sb) kfree(save_sb);
    if (save_bgdesc) kfree(save_bgdesc);
    if (save_bitmap) kfree(save_bitmap);
    spin_unlock(&fs->lock);
    return 0;
```

- [ ] **Step 2: ext2_selftest_inode_alloc**

同样模式：`save_ibitmap[4096]` + `save_sb[1024]` + `save_itable[4096]` → kmalloc，goto cleanup。

- [ ] **Step 3: ext2_selftest_dirent_roundtrip**

`save_dir_blk[4096]` + `save_itable_blk[4096]` + `save_sb[1024]` → kmalloc，goto cleanup。

- [ ] **Step 4: ext2_selftest_write_read**

`block_data[4096]` + `save_data[4096]` + `write_buf[4096]` + `read_buf[4096]` → kmalloc，goto cleanup。

- [ ] **Step 5: 编译验证（NDEBUG=1 + KERNEL_SELFTEST=1）**

```bash
make -C kernel kernel.bin NDEBUG=1 KERNEL_SELFTEST=1 2>&1 | tail -5
# Expected: 无 selftest 相关的编译错误
```

- [ ] **Step 6: Commit**

```bash
git add kernel/fs/ext2.c
git commit -m "refactor(ext2): move selftest save_* buffers to heap

All four selftest functions: save_bitmap/bgdesc/sb/blk/ibitmap/itable/
dir_blk/itable_blk/data/write_buf/read_buf → kmalloc + goto cleanup."
```

---

### Task 11: 构建系统 — NDEBUG 时启用 -O2

**Files:**
- Modify: `kernel/Makefile` (在现存的 `ifdef NDEBUG` 块附近追加)

**Interfaces:**
- Debug (`NDEBUG` 未定义): `-O0`（不变）
- Release (`NDEBUG=1`): `-O2`

**上下文**: `kernel/Makefile:90` 已有 `ifdef NDEBUG` 块（追加 `-DNDEBUG`）。本 task 在该块内追加优化标志，与既有风格一致。

- [ ] **Step 1: 在 kernel/Makefile 现有 `ifdef NDEBUG` 块内追加**

```makefile
# kernel/Makefile ~line 90 — 在现有 ifdef NDEBUG 块内追加
ifdef NDEBUG
ALL_CFLAGS += -DNDEBUG
ALL_CFLAGS += -O2     # Release: shrink stack frames significantly
else
ALL_CFLAGS += -O0     # Debug: easy GDB single-stepping
endif
```

变量是 `ALL_CFLAGS`（见 line 65 定义），不是 `KERNEL_CFLAGS`。条件使用 `ifdef NDEBUG`（与既有 line 90 一致），不用 `ifeq ($(NDEBUG),)`。

- [ ] **Step 2: 验证两种模式编译**

```bash
# Release
make -C kernel clean && make -C kernel kernel.bin NDEBUG=1 2>&1 | tail -5
# Expected: 编译成功（-O2 生效，warning-free）

# Debug
make -C kernel clean && make -C kernel kernel.bin 2>&1 | tail -5
# Expected: 编译成功（-O0）
```

- [ ] **Step 3: Commit**

```bash
git add kernel/Makefile
git commit -m "build: enable -O2 when NDEBUG, -O0 otherwise

-O2 reduces do_system_call frame from 3224B to 568B and shrinks all
ext2 function frames significantly. Critical for 32KB stack safety."
```

---

### Task 12: RSP 栈溢出守卫（Debug 模式）

**Files:**
- Modify: `kernel/arch/x86_64/trap.c:804` (do_system_call 入口)

**Interfaces:**
- Debug 模式：RSP 在栈底 2KB 内时打印警告
- Release 模式：无开销

- [ ] **Step 1: 在 do_system_call 入口添加守卫**

```c
// kernel/arch/x86_64/trap.c:804 — 在函数体开头（`switch` 之前）添加
void do_system_call(pt_regs_t *regs, uint64_t error_code __attribute__((unused)))
{
#ifndef NDEBUG
    // Stack overflow guard: warn when RSP is within 2KB of stack bottom.
    // Keep this for at least one release cycle to catch regressions.
    task_t *cur = get_current_task();
    if (cur) {
        uint64_t stack_bottom = ((uint64_t)cur) & ~(STACK_SIZE - 1);
        if ((uint64_t)__builtin_frame_address(0) - stack_bottom < 2048)
            log_err("WARNING: RSP within 2KB of stack bottom! pid=%d\n",
                    (int)cur->pid);
    }
#endif

    // ... existing code (Linux ABI translation, switch statement) ...
```

- [ ] **Step 2: 编译 & Commit**

```bash
make -C kernel kernel.bin 2>&1 | tail -5
# Expected: 编译成功
git add kernel/arch/x86_64/trap.c
git commit -m "feat: add RSP stack overflow guard in do_system_call (Debug only)

Warns when RSP is within 2KB of stack bottom. Kept for at least one
release cycle to catch regressions from the 64KB→32KB stack shrink."
```

---

### Task 13: 确认 STACK_SIZE = 32KB + 更新注释

**Files:**
- Modify: `kernel/include/kernel/task.h:40`

**Interfaces:**
- `STACK_SIZE` 最终值: `32 * 1024`

⚠️ 当前 STACK_SIZE 已是 32KB（在 Task 1 中还原为 64KB 后，现在改回 32KB）。如果 Task 1 被跳过（因为 STACK_SIZE 本来就是 32KB），这一步只需更新注释即可。

- [ ] **Step 1: 更新 STACK_SIZE 和注释**

```c
// kernel/include/kernel/task.h:40
#define STACK_SIZE (32 * 1024) // 32KB — heap migration + -O2 makes this safe
```

- [ ] **Step 2: Commit**

```bash
git add kernel/include/kernel/task.h
git commit -m "chore: finalize STACK_SIZE at 32KB with updated comment

All ext2 4KB buffers, vfs_getdents entries, and selftest save_* arrays
now live on the heap. Combined with -O2, peak stack depth is ~3KB."
```

---

### Task 14: 全量验证

**Files (no new modifications):**
- Verify only, no code changes.

- [ ] **Step 1: Release 编译 + 启动**

```bash
make clean && make NDEBUG=1 2>&1 | tail -20
make run &
sleep 5
# 在 QEMU 窗口中确认启动到 shell
# Ctrl-A X 退出 QEMU
```

Expected: 正常启动到 shell。

- [ ] **Step 2: systest 全量跑**

```bash
make test-syscall 2>&1
```

Expected: 70/70 pass（覆盖 open/read/write/rename/unlink/mkdir/rmdir/getdents 等路径）。

- [ ] **Step 3: selftest（NDEBUG=1）**

```bash
KERNEL_SELFTEST=1 NDEBUG=1 make run 2>&1 | grep selftest
```

Expected: selftest 全部 pass（无 #PF、无 kmalloc OOM）。

- [ ] **Step 4: Debug 模式编译**

```bash
make -C kernel clean && make -C kernel kernel.bin 2>&1 | tail -5
```

Expected: -O0 编译成功。

- [ ] **Step 5: Commit（验证通过标记）**

```bash
git commit --allow-empty -m "test: verify 32KB stack with heap migration passes systest"
```
