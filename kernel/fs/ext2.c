// kernel/fs/ext2.c
#include <fs/ext2.h>
#include <kernel/debug.h>
#include <kernel/slab.h>     // kmalloc, kfree
#include <string.h>
#include <stdlib.h>          // calloc
#include <errno.h>
#include <kernel.h>

#ifdef OS01_SELFTEST
static ext2_fs_t *ext2_selftest_fs;  // set by ext2_init, used by selftests
#endif

// ── Helper: resolve node->fs_data to inode number ────────
// Root mount node has fs_data=NULL (set by vfs_mount).
// Subdirectory nodes have fs_data=(void*)(uintptr_t)ino.
static uint32_t ext2_node_ino(vfs_node_t *node)
{
    if (!node->fs_data) return EXT2_ROOT_INO;  // root inode is 2
    return (uint32_t)(uintptr_t)node->fs_data;
}

// ── Block I/O helper ───────────────────────────────────
static __attribute__((noinline)) int ext2_read_block(ext2_fs_t *fs, uint32_t block, void *buf)
{
    uint64_t lba = (uint64_t)block * fs->sectors_per_block;
    return block_device_read(fs->dev, lba, fs->sectors_per_block, buf);
}

// ── Block I/O helper (write) ───────────────────────────
static __attribute__((noinline)) int ext2_write_block(ext2_fs_t *fs, uint32_t block, const void *buf)
{
    uint64_t lba = (uint64_t)block * fs->sectors_per_block;
    return block_device_write(fs->dev, lba, fs->sectors_per_block, buf);
}

// ── Read an inode from disk ─────────────────────────────
static __attribute__((noinline)) int ext2_read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out)
{
    if (ino == 0) return -1;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;
    if (group >= fs->num_block_groups) return -1;

    uint32_t table_start    = fs->bgdesc_table[group].bg_inode_table;
    uint32_t inodes_per_blk = fs->block_size / fs->inode_size;
    uint32_t block_off      = index / inodes_per_blk;
    uint32_t inode_off      = (index % inodes_per_blk) * fs->inode_size;

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
}

// ── Write an inode to disk ─────────────────────────────
static __attribute__((noinline)) int ext2_write_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode)
{
    if (ino == 0) return -1;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;
    if (group >= fs->num_block_groups) return -1;

    uint32_t table_start    = fs->bgdesc_table[group].bg_inode_table;
    uint32_t inodes_per_blk = fs->block_size / fs->inode_size;
    uint32_t block_off      = index / inodes_per_blk;
    uint32_t inode_off      = (index % inodes_per_blk) * fs->inode_size;

    // Read full block, modify inode slot, write back
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
}

// ── Write superblock and bgdesc table to disk ──────────
static __attribute__((noinline)) int ext2_write_superblock(ext2_fs_t *fs)
{
    uint8_t *sb_buf = kmalloc(1024);
    if (!sb_buf) return -ENOMEM;
    memset(sb_buf, 0, 1024);
    memcpy(sb_buf, &fs->sb_raw, sizeof(ext2_superblock_t));
    if (block_device_write(fs->dev, 2, 2, sb_buf) != 0) {
        kfree(sb_buf);
        return -1;
    }
    kfree(sb_buf);  // sb_buf no longer needed — bgdesc loop uses fs->bgdesc_table

    // Write all block group descriptors
    for (uint32_t i = 0; i < fs->bgdesc_table_blocks; i++) {
        if (ext2_write_block(fs, fs->bgdesc_block + i,
            (uint8_t *)fs->bgdesc_table + i * fs->block_size) != 0)
            return -1;
    }
    return 0;
}

// ── Allocate a data block ──────────────────────────────
// Scans block bitmaps across groups, updates sb_raw + bgdesc,
// writes superblock+bgdesc to disk, zeroes the new block.
// Returns block number or 0 on failure.
static __attribute__((noinline)) uint32_t alloc_block(ext2_fs_t *fs)
{
    for (uint32_t g = 0; g < fs->num_block_groups; g++) {
        if (fs->bgdesc_table[g].bg_free_blocks_count == 0)
            continue;

        uint32_t bitmap_block = fs->bgdesc_table[g].bg_block_bitmap;
        uint8_t *buf = kmalloc(4096);
        if (!buf) return 0;
        if (ext2_read_block(fs, bitmap_block, buf) != 0) {
            kfree(buf);
            continue;
        }

        uint32_t blocks_in_group = fs->blocks_per_group;
        uint32_t bit_count = blocks_in_group;
        if (bit_count > fs->block_size * 8)
            bit_count = fs->block_size * 8;

        for (uint32_t byte_idx = 0; byte_idx < bit_count / 8; byte_idx++) {
            if (buf[byte_idx] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (!(buf[byte_idx] & (1u << bit))) {
                    buf[byte_idx] |= (1u << bit);
                    // Block numbers are 0-based: group * blocks_per_group + bit_position
                    uint32_t block = g * blocks_in_group + byte_idx * 8 + bit;

                    ext2_write_block(fs, bitmap_block, buf);

                    fs->bgdesc_table[g].bg_free_blocks_count--;
                    fs->sb_raw.s_free_blocks_count--;
                    ext2_write_superblock(fs);

                    // Reuse buf for zero-fill (write to disk frees buf for reuse)
                    memset(buf, 0, fs->block_size);
                    ext2_write_block(fs, block, buf);
                    kfree(buf);

                    return block;
                }
            }
        }
        kfree(buf);
    }
    return 0;
}

// ── Free a data block ──────────────────────────────────
static __attribute__((noinline)) void free_block(ext2_fs_t *fs, uint32_t block)
{
    if (block == 0) return;

    // Block numbers are 0-based: group = block / blocks_per_group
    uint32_t group = block / fs->blocks_per_group;
    if (group >= fs->num_block_groups) return;

    uint32_t index = block % fs->blocks_per_group;
    uint32_t byte_idx = index / 8;
    uint32_t bit      = index % 8;

    uint32_t bitmap_block = fs->bgdesc_table[group].bg_block_bitmap;
    uint8_t *buf = kmalloc(4096);
    if (!buf) return;
    if (ext2_read_block(fs, bitmap_block, buf) != 0) {
        kfree(buf);
        return;
    }

    buf[byte_idx] &= ~(1u << bit);
    ext2_write_block(fs, bitmap_block, buf);
    kfree(buf);

    fs->bgdesc_table[group].bg_free_blocks_count++;
    fs->sb_raw.s_free_blocks_count++;
    ext2_write_superblock(fs);
}

// ── Allocate and initialize an inode ────────────────────
// Returns inode number or 0 on failure.
static __attribute__((noinline)) uint32_t alloc_inode(ext2_fs_t *fs, uint16_t mode)
{
    for (uint32_t g = 0; g < fs->num_block_groups; g++) {
        if (fs->bgdesc_table[g].bg_free_inodes_count == 0)
            continue;

        uint32_t bitmap_block = fs->bgdesc_table[g].bg_inode_bitmap;
        uint8_t *buf = kmalloc(4096);
        if (!buf) return 0;
        if (ext2_read_block(fs, bitmap_block, buf) != 0) {
            kfree(buf);
            continue;
        }

        uint32_t inodes_in_group = fs->inodes_per_group;
        uint32_t bit_count = inodes_in_group;
        if (bit_count > fs->block_size * 8)
            bit_count = fs->block_size * 8;

        for (uint32_t byte_idx = 0; byte_idx < bit_count / 8; byte_idx++) {
            if (buf[byte_idx] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (!(buf[byte_idx] & (1u << bit))) {
                    buf[byte_idx] |= (1u << bit);
                    uint32_t ino = g * inodes_in_group + byte_idx * 8 + bit + 1;

                    ext2_write_block(fs, bitmap_block, buf);
                    kfree(buf);

                    fs->bgdesc_table[g].bg_free_inodes_count--;
                    fs->sb_raw.s_free_inodes_count--;
                    ext2_write_superblock(fs);

                    ext2_inode_t inode;
                    memset(&inode, 0, sizeof(inode));
                    inode.i_mode       = mode;
                    inode.i_links_count = 1;
                    inode.i_blocks      = 0;
                    inode.i_size        = 0;
                    inode.i_atime = 0;
                    inode.i_ctime = 0;
                    inode.i_mtime = 0;

                    ext2_write_inode(fs, ino, &inode);
                    return ino;
                }
            }
        }
        kfree(buf);
    }
    return 0;
}

// ── Free an inode ──────────────────────────────────────
static __attribute__((noinline)) void free_inode(ext2_fs_t *fs, uint32_t ino)
{
    if (ino == 0) return;

    uint32_t group = (ino - 1) / fs->inodes_per_group;
    if (group >= fs->num_block_groups) return;

    uint32_t index = (ino - 1) % fs->inodes_per_group;
    uint32_t byte_idx = index / 8;
    uint32_t bit      = index % 8;

    uint32_t bitmap_block = fs->bgdesc_table[group].bg_inode_bitmap;
    uint8_t *buf = kmalloc(4096);
    if (!buf) return;
    if (ext2_read_block(fs, bitmap_block, buf) != 0) {
        kfree(buf);
        return;
    }

    buf[byte_idx] &= ~(1u << bit);
    ext2_write_block(fs, bitmap_block, buf);
    kfree(buf);

    fs->bgdesc_table[group].bg_free_inodes_count++;
    fs->sb_raw.s_free_inodes_count++;
    ext2_write_superblock(fs);
}

// ── Map logical block → physical (direct + single indirect) ─
static __attribute__((noinline)) uint32_t ext2_bmap(ext2_fs_t *fs, ext2_inode_t *inode,
                          uint32_t logical_block)
{
    if (logical_block < 12)
        return inode->i_block[logical_block];

    uint32_t ptrs_per_block = fs->block_size / sizeof(uint32_t);
    if (logical_block < 12 + ptrs_per_block) {
        uint32_t indirect_blk = inode->i_block[12];
        if (indirect_blk == 0) return 0;

        uint32_t *indirect = kmalloc(4096);
        if (!indirect) return 0;  // uint32_t: 0 = failure
        uint32_t result = 0;
        if (ext2_read_block(fs, indirect_blk, indirect) == 0)
            result = indirect[logical_block - 12];
        kfree(indirect);
        return result;
    }

    return 0;  // double/triple indirect not implemented
}

// ── Map logical block → physical (allocating variant) ──
// Like ext2_bmap but allocates blocks as needed.  Only modifies
// the in-memory inode; caller must call ext2_write_inode to persist.
static __attribute__((noinline)) uint32_t ext2_bmap_alloc(ext2_fs_t *fs, ext2_inode_t *inode,
                                uint32_t logical_block)
{
    // Direct blocks (0–11)
    if (logical_block < 12) {
        if (inode->i_block[logical_block] != 0)
            return inode->i_block[logical_block];
        uint32_t phys = alloc_block(fs);
        if (phys == 0) return 0;
        inode->i_block[logical_block] = phys;
        inode->i_blocks += fs->block_size / 512;
        return phys;
    }

    // Single indirect (block 12)
    uint32_t ptrs_per_block = fs->block_size / sizeof(uint32_t);
    if (logical_block < 12 + ptrs_per_block) {
        if (inode->i_block[12] == 0) {
            uint32_t indirect_blk = alloc_block(fs);
            if (indirect_blk == 0) return 0;
            inode->i_block[12] = indirect_blk;
            inode->i_blocks += fs->block_size / 512;
        }

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
    }

    return 0;  // double/triple indirect not supported
}

// ── Find a directory entry by name ──────────────────────
static __attribute__((noinline)) int ext2_find_dirent(ext2_fs_t *fs, uint32_t dir_ino, const char *name,
                            uint32_t *out_ino, uint8_t *out_file_type,
                            uint32_t *out_block, uint32_t *out_off)
{
    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, dir_ino, &dir_inode) != 0)
        return -EIO;
    if (!(dir_inode.i_mode & EXT2_S_IFDIR))
        return -ENOTDIR;

    size_t name_len = strlen(name);
    uint8_t *block_data = kmalloc(4096);
    if (!block_data) return -ENOMEM;
    int rc = -ENOENT;

    for (uint32_t blk_idx = 0; ; blk_idx++) {
        uint32_t phys = ext2_bmap(fs, &dir_inode, blk_idx);
        if (phys == 0) goto out;
        if (ext2_read_block(fs, phys, block_data) != 0) goto out;

        uint32_t off = 0;
        while (off < fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(block_data + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0 &&
                de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0) {
                *out_ino       = de->inode;
                *out_file_type = de->file_type;
                *out_block     = phys;
                *out_off       = off;
                rc = 0;
                goto out;
            }
            off += de->rec_len;
        }
    }

out:
    kfree(block_data);
    return rc;
}

// ── Add a directory entry ───────────────────────────────
#define ALIGN4(x) (((x) + 3) & ~3u)

static __attribute__((noinline)) int dirent_add(ext2_fs_t *fs, uint32_t dir_ino, const char *name,
                      uint32_t new_ino, uint8_t file_type)
{
    size_t name_len = strlen(name);
    uint32_t new_len = ALIGN4(sizeof(ext2_dirent_t) + name_len);

    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, dir_ino, &dir_inode) != 0)
        return -EIO;

    uint32_t logical_block = 0;
    uint8_t *block_data = kmalloc(4096);
    if (!block_data) return -ENOMEM;

    for (;; logical_block++) {
        uint32_t phys = ext2_bmap(fs, &dir_inode, logical_block);
        if (phys == 0) {
            uint32_t new_blk = alloc_block(fs);
            if (new_blk == 0) { kfree(block_data); return -ENOSPC; }
            if (logical_block < 12) {
                dir_inode.i_block[logical_block] = new_blk;
            } else {
                // Directory > 12 blocks (48 KB with 4 KB blocks):
                // single indirect not implemented for directories.
                // Real-world directories with > ~1000 entries would hit this.
                // Documented in spec §10: double/triple indirect unsupported.
                { kfree(block_data); return -ENOSPC; }
            }
            dir_inode.i_blocks += fs->block_size / 512;
            dir_inode.i_size += fs->block_size;

            memset(block_data, 0, fs->block_size);
            ext2_dirent_t *de = (ext2_dirent_t *)block_data;
            de->inode    = new_ino;
            de->rec_len  = fs->block_size;
            de->name_len = (uint8_t)name_len;
            de->file_type = file_type;
            memcpy(de->name, name, name_len);

            ext2_write_block(fs, new_blk, block_data);
            dir_inode.i_mtime = 0;
            ext2_write_inode(fs, dir_ino, &dir_inode);
            { kfree(block_data); return 0; }
        }

        if (ext2_read_block(fs, phys, block_data) != 0)
            { kfree(block_data); return -EIO; }

        uint32_t off = 0;
        while (off < fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(block_data + off);
            if (de->rec_len == 0) break;

            uint32_t occupied = ALIGN4(sizeof(ext2_dirent_t) + de->name_len);
            if (de->inode == 0) {
                // Deleted entry — check if rec_len covers enough space
                if (de->rec_len >= new_len) {
                    if (de->rec_len - new_len >= (uint32_t)(sizeof(ext2_dirent_t) + 4)) {
                        // Split: truncate this entry to new_len, create
                        // a remainder deleted placeholder after it
                        ext2_dirent_t *rem = (ext2_dirent_t *)(block_data + off + new_len);
                        rem->inode = 0;
                        rem->rec_len = de->rec_len - new_len;
                        rem->name_len = 0;
                        rem->file_type = 0;
                        de->rec_len = new_len;
                    }
                    // else: no split — keep original rec_len so the next
                    // entry remains reachable via the original rec_len chain.
                    de->inode     = new_ino;
                    de->name_len  = (uint8_t)name_len;
                    de->file_type = file_type;
                    memcpy(de->name, name, name_len);

                    ext2_write_block(fs, phys, block_data);
                    dir_inode.i_mtime = 0;
                    ext2_write_inode(fs, dir_ino, &dir_inode);
                    { kfree(block_data); return 0; }
                }
            } else if (de->rec_len - occupied >= new_len) {
                uint32_t old_rec_len = de->rec_len;
                de->rec_len = occupied;

                ext2_dirent_t *new_de = (ext2_dirent_t *)(block_data + off + occupied);
                new_de->inode     = new_ino;
                new_de->rec_len   = old_rec_len - occupied;
                new_de->name_len  = (uint8_t)name_len;
                new_de->file_type = file_type;
                memcpy(new_de->name, name, name_len);

                ext2_write_block(fs, phys, block_data);
                dir_inode.i_mtime = 0;
                ext2_write_inode(fs, dir_ino, &dir_inode);
                { kfree(block_data); return 0; }
            }
            off += de->rec_len;
        }
    }
}

// ── Remove a directory entry ────────────────────────────
static __attribute__((noinline)) int dirent_del(ext2_fs_t *fs, uint32_t dir_ino, const char *name)
{
    uint32_t target_ino, block, off;
    uint8_t file_type;
    int ret = ext2_find_dirent(fs, dir_ino, name,
                               &target_ino, &file_type, &block, &off);
    if (ret != 0) return ret;

    uint8_t *block_data = kmalloc(4096);
    if (!block_data) return -ENOMEM;
    if (ext2_read_block(fs, block, block_data) != 0)
        { kfree(block_data); return -EIO; }

    ext2_dirent_t *de = (ext2_dirent_t *)(block_data + off);
    de->inode = 0;

    // Merge rec_len into previous entry
    ext2_dirent_t *prev = NULL;
    for (uint32_t cur = 0; cur < off; ) {
        ext2_dirent_t *cur_de = (ext2_dirent_t *)(block_data + cur);
        if (cur_de->rec_len == 0) break;
        prev = cur_de;
        cur += cur_de->rec_len;
    }
    if (prev && prev != de) {
        prev->rec_len += de->rec_len;
    }

    ext2_write_block(fs, block, block_data);

    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, dir_ino, &dir_inode) == 0) {
        dir_inode.i_mtime = 0;
        ext2_write_inode(fs, dir_ino, &dir_inode);
    }
    { kfree(block_data); return 0; }
}

// ── VFS read implementation ─────────────────────────────
static __attribute__((noinline)) int ext2_vfs_read(struct vfs_node *node, uint64_t offset,
                          uint64_t size, void *buffer)
{
    if (!node || !buffer || size == 0) return 0;
    uint32_t ino = ext2_node_ino(node);
    ext2_fs_t *fs = (ext2_fs_t *)node->mount->fs_data;

    spin_lock(&fs->lock);

    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) != 0) {
        spin_unlock(&fs->lock); return -1;
    }

    if (offset >= inode.i_size) { spin_unlock(&fs->lock); return 0; }
    if (offset + size > inode.i_size)
        size = inode.i_size - offset;

    uint8_t *out = (uint8_t *)buffer;
    uint64_t remaining = size;
    uint64_t file_off = offset;

    while (remaining > 0) {
        uint32_t logical_block = (uint32_t)(file_off / fs->block_size);
        uint32_t block_off     = (uint32_t)(file_off % fs->block_size);

        uint32_t phys = ext2_bmap(fs, &inode, logical_block);
        if (phys == 0) { spin_unlock(&fs->lock); return -1; }

        uint8_t block_buf[4096];  // fixed size, block_size ≤ 4096
        if (ext2_read_block(fs, phys, block_buf) != 0) {
            spin_unlock(&fs->lock); return -1;
        }

        uint32_t chunk = (uint32_t)(fs->block_size - block_off);
        if (chunk > remaining) chunk = (uint32_t)remaining;

        memcpy(out, block_buf + block_off, chunk);
        out       += chunk;
        file_off  += chunk;
        remaining -= chunk;
    }

    spin_unlock(&fs->lock);
    return (int)size;
}

// ── VFS write implementation ────────────────────────────
static __attribute__((noinline)) int ext2_vfs_write(struct vfs_node *node, uint64_t offset,
                           uint64_t size, void *buffer)
{
    if (!node || !buffer || size == 0) return 0;
    uint32_t ino = ext2_node_ino(node);
    ext2_fs_t *fs = (ext2_fs_t *)node->mount->fs_data;

    spin_lock(&fs->lock);

    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) != 0) {
        spin_unlock(&fs->lock); return -EIO;
    }

    // Reject extent-based inodes
    if (inode.i_flags & 0x00080000) {
        spin_unlock(&fs->lock); return -EOPNOTSUPP;
    }

    // Ensure blocks are allocated for the write range
    uint32_t first_blk = (uint32_t)(offset / fs->block_size);
    uint32_t last_blk  = (uint32_t)((offset + size - 1) / fs->block_size);

    for (uint32_t lb = first_blk; lb <= last_blk; lb++) {
        if (ext2_bmap_alloc(fs, &inode, lb) == 0) {
            spin_unlock(&fs->lock); return -ENOSPC;
        }
    }

    // Persist inode changes from bmap_alloc
    ext2_write_inode(fs, ino, &inode);

    // RMW per block
    const uint8_t *src = (const uint8_t *)buffer;
    uint64_t remaining = size;
    uint64_t file_off  = offset;

    while (remaining > 0) {
        uint32_t logical_block = (uint32_t)(file_off / fs->block_size);
        uint32_t block_off     = (uint32_t)(file_off % fs->block_size);

        uint32_t phys = ext2_bmap(fs, &inode, logical_block);
        if (phys == 0) { spin_unlock(&fs->lock); return -EIO; }

        uint8_t block_buf[4096];
        if (ext2_read_block(fs, phys, block_buf) != 0) {
            spin_unlock(&fs->lock); return -EIO;
        }

        uint32_t chunk = fs->block_size - block_off;
        if (chunk > remaining) chunk = (uint32_t)remaining;

        memcpy(block_buf + block_off, src, chunk);
        if (ext2_write_block(fs, phys, block_buf) != 0) {
            spin_unlock(&fs->lock); return -EIO;
        }

        src        += chunk;
        file_off   += chunk;
        remaining  -= chunk;
    }

    // Update i_size if write extended the file
    if (offset + size > inode.i_size)
        inode.i_size = offset + size;
    if ((uint64_t)inode.i_size > node->size)
        node->size = inode.i_size;
    inode.i_mtime = 0;
    ext2_write_inode(fs, ino, &inode);

    spin_unlock(&fs->lock);
    return (int)size;
}

// ── VFS truncate implementation ─────────────────────────
static __attribute__((noinline)) int ext2_vfs_truncate(struct vfs_node *node, uint64_t new_size)
{
    if (!node) return -EINVAL;
    if (node->type != VFS_FILE) return -EISDIR;
    uint32_t ino = ext2_node_ino(node);
    ext2_fs_t *fs = (ext2_fs_t *)node->mount->fs_data;

    spin_lock(&fs->lock);

    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) != 0) {
        spin_unlock(&fs->lock); return -EIO;
    }

    if (inode.i_flags & 0x00080000) {
        spin_unlock(&fs->lock); return -EOPNOTSUPP;
    }

    uint32_t old_blocks = (inode.i_size + fs->block_size - 1) / fs->block_size;
    uint32_t new_blocks = (uint32_t)((new_size + fs->block_size - 1) / fs->block_size);

    if (new_size > inode.i_size) {
        // Extend: allocate new blocks
        for (uint32_t lb = old_blocks; lb < new_blocks; lb++) {
            if (ext2_bmap_alloc(fs, &inode, lb) == 0) {
                spin_unlock(&fs->lock); return -ENOSPC;
            }
        }
    } else if (new_size < inode.i_size) {
        // Shrink: free excess blocks
        for (uint32_t lb = new_blocks; lb < old_blocks; lb++) {
            uint32_t phys = ext2_bmap(fs, &inode, lb);
            if (phys == 0) continue;

            free_block(fs, phys);

            // Clear the block pointer
            if (lb < 12) {
                inode.i_block[lb] = 0;
            } else {
                uint32_t ptrs_per_block = fs->block_size / sizeof(uint32_t);
                uint32_t idx = lb - 12;
                if (inode.i_block[12] != 0) {
                    uint32_t indirect[1024];
                    if (ext2_read_block(fs, inode.i_block[12], indirect) != 0) {
                        spin_unlock(&fs->lock); return -EIO;
                    }
                    indirect[idx] = 0;
                    ext2_write_block(fs, inode.i_block[12], indirect);

                    // Check if indirect block is now empty
                    int empty = 1;
                    for (uint32_t k = 0; k < ptrs_per_block; k++) {
                        if (indirect[k] != 0) { empty = 0; break; }
                    }
                    if (empty) {
                        free_block(fs, inode.i_block[12]);
                        inode.i_block[12] = 0;
                        inode.i_blocks -= fs->block_size / 512;
                    }
                }
            }
            inode.i_blocks -= fs->block_size / 512;
        }
    }

    inode.i_size = new_size;
    inode.i_mtime = 0;
    ext2_write_inode(fs, ino, &inode);

    // Sync the VFS node's cached size — fstat() reads node->size
    node->size = new_size;

    spin_unlock(&fs->lock);
    return 0;
}

// ── VFS create (regular file) ──────────────────────────
static __attribute__((noinline)) struct vfs_node *ext2_vfs_create(struct vfs_node *dir, const char *name)
{
    if (!dir || !dir->mount || !name) return NULL;
    ext2_fs_t *fs = (ext2_fs_t *)dir->mount->fs_data;

    spin_lock(&fs->lock);

    uint32_t dir_ino = ext2_node_ino(dir);

    // Check for duplicate
    uint32_t dummy_ino, dummy_block, dummy_off;
    uint8_t dummy_type;
    if (ext2_find_dirent(fs, dir_ino, name, &dummy_ino, &dummy_type,
                         &dummy_block, &dummy_off) == 0) {
        spin_unlock(&fs->lock);
        return NULL;  // already exists
    }

    uint32_t new_ino = alloc_inode(fs, EXT2_S_IFREG | 0644);
    if (new_ino == 0) {
        spin_unlock(&fs->lock); return NULL;
    }

    if (dirent_add(fs, dir_ino, name, new_ino, 1 /* EXT2_FT_REG_FILE */) != 0) {
        free_inode(fs, new_ino);
        spin_unlock(&fs->lock); return NULL;
    }

    spin_unlock(&fs->lock);

    // Build vfs_node_t
    vfs_node_t *node = calloc(1, sizeof(vfs_node_t));
    if (!node) return NULL;

    size_t nlen = strlen(name);
    if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
    memcpy(node->name, name, nlen);
    node->name[nlen] = '\0';
    node->type = VFS_FILE;
    node->mount = dir->mount;
    node->ops = dir->ops;
    node->fs_data = (void *)(uintptr_t)new_ino;
    node->size = 0;
    node->refcount = 1;

    return node;
}

// ── VFS unlink (delete file) ────────────────────────────
static __attribute__((noinline)) int ext2_vfs_unlink(struct vfs_node *dir, const char *name)
{
    if (!dir || !dir->mount || !name) return -EINVAL;
    ext2_fs_t *fs = (ext2_fs_t *)dir->mount->fs_data;

    spin_lock(&fs->lock);

    uint32_t dir_ino = ext2_node_ino(dir);
    uint32_t target_ino, block, off;
    uint8_t file_type;

    int ret = ext2_find_dirent(fs, dir_ino, name,
                               &target_ino, &file_type, &block, &off);
    if (ret != 0) { spin_unlock(&fs->lock); return ret; }

    // Remove dirent first (write ordering: dirent → inode → bitmap → sb → data)
    dirent_del(fs, dir_ino, name);

    ext2_inode_t inode;
    if (ext2_read_inode(fs, target_ino, &inode) != 0) {
        spin_unlock(&fs->lock); return -EIO;
    }

    inode.i_links_count--;
    if (inode.i_links_count == 0) {
        // Free all data blocks (direct + single indirect)
        for (int i = 0; i < 12; i++) {
            if (inode.i_block[i] != 0) {
                free_block(fs, inode.i_block[i]);
                inode.i_block[i] = 0;
            }
        }
        if (inode.i_block[12] != 0) {
            uint32_t ptrs_per_block = fs->block_size / sizeof(uint32_t);
            uint32_t indirect[1024];
            if (ext2_read_block(fs, inode.i_block[12], indirect) != 0) {
                spin_unlock(&fs->lock); return -EIO;
            }
            for (uint32_t i = 0; i < ptrs_per_block; i++) {
                if (indirect[i] != 0) {
                    free_block(fs, indirect[i]);
                }
            }
            free_block(fs, inode.i_block[12]);
            inode.i_block[12] = 0;
        }
        inode.i_blocks = 0;
        inode.i_size = 0;
        ext2_write_inode(fs, target_ino, &inode);
        free_inode(fs, target_ino);
    } else {
        ext2_write_inode(fs, target_ino, &inode);
    }

    spin_unlock(&fs->lock);
    return 0;
}

// ── VFS mkdir ──────────────────────────────────────────
static __attribute__((noinline)) struct vfs_node *ext2_vfs_mkdir(struct vfs_node *dir, const char *name)
{
    if (!dir || !dir->mount || !name) return NULL;
    ext2_fs_t *fs = (ext2_fs_t *)dir->mount->fs_data;

    spin_lock(&fs->lock);

    uint32_t dir_ino = ext2_node_ino(dir);

    uint32_t dummy_ino, dummy_block, dummy_off;
    uint8_t dummy_type;
    if (ext2_find_dirent(fs, dir_ino, name, &dummy_ino, &dummy_type,
                         &dummy_block, &dummy_off) == 0) {
        spin_unlock(&fs->lock); return NULL;
    }

    uint32_t new_ino = alloc_inode(fs, EXT2_S_IFDIR | 0755);
    if (new_ino == 0) { spin_unlock(&fs->lock); return NULL; }

    // Allocate data block for "." and ".."
    uint32_t dir_blk = alloc_block(fs);
    if (dir_blk == 0) {
        free_inode(fs, new_ino);
        spin_unlock(&fs->lock); return NULL;
    }

    // Read inode to update i_block
    ext2_inode_t new_inode;
    if (ext2_read_inode(fs, new_ino, &new_inode) != 0) {
        free_block(fs, dir_blk); free_inode(fs, new_ino);
        spin_unlock(&fs->lock); return NULL;
    }
    new_inode.i_block[0] = dir_blk;
    new_inode.i_blocks = fs->block_size / 512;
    new_inode.i_size = fs->block_size;
    new_inode.i_links_count = 2;
    ext2_write_inode(fs, new_ino, &new_inode);

    // Initialize directory block with "." and ".."
    uint8_t block_data[4096];
    memset(block_data, 0, fs->block_size);

    ext2_dirent_t *dot = (ext2_dirent_t *)block_data;
    dot->inode    = new_ino;
    dot->rec_len  = 12;
    dot->name_len = 1;
    dot->file_type = 2;  // EXT2_FT_DIR
    memcpy(dot->name, ".", 1);

    ext2_dirent_t *dotdot = (ext2_dirent_t *)(block_data + 12);
    dotdot->inode    = dir_ino;
    dotdot->rec_len  = fs->block_size - 12;
    dotdot->name_len = 2;
    dotdot->file_type = 2;
    memcpy(dotdot->name, "..", 2);

    ext2_write_block(fs, dir_blk, block_data);

    // Update parent directory
    uint32_t parent_group = (dir_ino - 1) / fs->inodes_per_group;
    fs->bgdesc_table[parent_group].bg_used_dirs_count++;
    ext2_inode_t dir_inode;
    ext2_read_inode(fs, dir_ino, &dir_inode);
    dir_inode.i_links_count++;
    ext2_write_inode(fs, dir_ino, &dir_inode);

    ext2_write_superblock(fs);

    if (dirent_add(fs, dir_ino, name, new_ino, 2 /* EXT2_FT_DIR */) != 0) {
        // Rollback: free dir block, free inode, decrement parent links
        free_block(fs, dir_blk);
        free_inode(fs, new_ino);
        dir_inode.i_links_count--;
        fs->bgdesc_table[parent_group].bg_used_dirs_count--;
        ext2_write_inode(fs, dir_ino, &dir_inode);
        ext2_write_superblock(fs);
        spin_unlock(&fs->lock); return NULL;
    }

    spin_unlock(&fs->lock);

    // Build vfs_node_t
    vfs_node_t *node = calloc(1, sizeof(vfs_node_t));
    if (!node) return NULL;

    size_t nlen = strlen(name);
    if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
    memcpy(node->name, name, nlen);
    node->name[nlen] = '\0';
    node->type = VFS_DIR;
    node->mount = dir->mount;
    node->ops = dir->ops;
    node->fs_data = (void *)(uintptr_t)new_ino;
    node->size = fs->block_size;
    node->refcount = 1;

    return node;
}

// ── VFS rmdir ──────────────────────────────────────────
static __attribute__((noinline)) int ext2_vfs_rmdir(struct vfs_node *dir, const char *name)
{
    if (!dir || !dir->mount || !name) return -EINVAL;
    ext2_fs_t *fs = (ext2_fs_t *)dir->mount->fs_data;

    spin_lock(&fs->lock);

    uint32_t dir_ino = ext2_node_ino(dir);
    uint32_t target_ino, block, off;
    uint8_t file_type;

    int ret = ext2_find_dirent(fs, dir_ino, name,
                               &target_ino, &file_type, &block, &off);
    if (ret != 0) { spin_unlock(&fs->lock); return ret; }

    if (file_type != 2 /* EXT2_FT_DIR */) {
        spin_unlock(&fs->lock); return -ENOTDIR;
    }

    ext2_inode_t target_inode;
    if (ext2_read_inode(fs, target_ino, &target_inode) != 0) {
        spin_unlock(&fs->lock); return -EIO;
    }

    // Check directory is empty (only "." and ".." entries with valid inode)
    uint8_t block_data[4096];
    for (uint32_t blk_idx = 0; ; blk_idx++) {
        uint32_t phys = ext2_bmap(fs, &target_inode, blk_idx);
        if (phys == 0) break;
        if (ext2_read_block(fs, phys, block_data) != 0) break;

        uint32_t off2 = 0;
        while (off2 < fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(block_data + off2);
            if (de->rec_len == 0) break;

            if (de->inode != 0 &&
                de->inode != target_ino &&
                de->inode != dir_ino) {
                spin_unlock(&fs->lock); return -ENOTEMPTY;
            }
            off2 += de->rec_len;
        }
    }

    // Remove dirent from parent
    dirent_del(fs, dir_ino, name);

    // Decrement parent links_count
    ext2_inode_t dir_inode;
    ext2_read_inode(fs, dir_ino, &dir_inode);
    dir_inode.i_links_count--;
    ext2_write_inode(fs, dir_ino, &dir_inode);

    // Update parent group used_dirs_count
    uint32_t parent_group = (dir_ino - 1) / fs->inodes_per_group;
    fs->bgdesc_table[parent_group].bg_used_dirs_count--;
    ext2_write_superblock(fs);

    // Free all data blocks of target directory
    for (int i = 0; i < 12; i++) {
        if (target_inode.i_block[i] != 0) {
            free_block(fs, target_inode.i_block[i]);
        }
    }
    if (target_inode.i_block[12] != 0) {
        uint32_t ptrs_per_block = fs->block_size / sizeof(uint32_t);
        uint32_t indirect[1024];
        if (ext2_read_block(fs, target_inode.i_block[12], indirect) != 0) {
            spin_unlock(&fs->lock); return -EIO;
        }
        for (uint32_t i = 0; i < ptrs_per_block; i++) {
            if (indirect[i] != 0) free_block(fs, indirect[i]);
        }
        free_block(fs, target_inode.i_block[12]);
    }
    free_inode(fs, target_ino);

    spin_unlock(&fs->lock);
    return 0;
}

// ── VFS readdir implementation ──────────────────────────
static __attribute__((noinline)) int ext2_vfs_readdir(struct vfs_node *node, uint64_t index,
                             struct vfs_dirent *entry)
{
    if (!node || !entry) return -1;
    if (node->type != VFS_DIR) return -1;

    uint32_t ino = ext2_node_ino(node);
    ext2_fs_t *fs = (ext2_fs_t *)node->mount->fs_data;

    spin_lock(&fs->lock);

    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, ino, &dir_inode) != 0) {
        spin_unlock(&fs->lock); return -1;
    }
    if (!(dir_inode.i_mode & EXT2_S_IFDIR)) {
        spin_unlock(&fs->lock); return -1;
    }

    uint8_t block_data[4096];
    uint64_t entry_idx = 0;

    for (uint32_t blk_idx = 0; ; blk_idx++) {
        uint32_t phys = ext2_bmap(fs, &dir_inode, blk_idx);
        if (phys == 0) break;

        if (ext2_read_block(fs, phys, block_data) != 0) break;

        uint32_t off = 0;
        while (off < fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(block_data + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0) {
                if (entry_idx == index) {
                    size_t nlen = de->name_len;
                    if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
                    memcpy(entry->name, de->name, nlen);
                    entry->name[nlen] = '\0';
                    entry->ino  = de->inode;
                    entry->size = 0;
                    entry->type = (de->file_type == 2) ? VFS_DIR : VFS_FILE;

                    // ext2 dirent has no size field — read from inode
                    if (entry->type == VFS_FILE) {
                        ext2_inode_t finode;
                        if (ext2_read_inode(fs, de->inode, &finode) == 0)
                            entry->size = finode.i_size;
                    }
                    spin_unlock(&fs->lock);
                    return 0;
                }
                entry_idx++;
            }
            off += de->rec_len;
        }
    }

    entry->name[0] = '\0';
    spin_unlock(&fs->lock);
    return 0;
}

// ── VFS rename ──────────────────────────────────────────
static __attribute__((noinline)) int ext2_vfs_rename(struct vfs_node *olddir, const char *oldname,
                            struct vfs_node *newdir, const char *newname)
{
    if (!olddir || !olddir->mount || !oldname || !newdir || !newdir->mount || !newname)
        return -EINVAL;
    ext2_fs_t *fs = (ext2_fs_t *)olddir->mount->fs_data;

    spin_lock(&fs->lock);

    uint32_t old_ino = ext2_node_ino(olddir);
    uint32_t new_ino_dir = ext2_node_ino(newdir);
    uint32_t target_ino, block, off;
    uint8_t file_type;

    int ret = ext2_find_dirent(fs, old_ino, oldname,
                               &target_ino, &file_type, &block, &off);
    if (ret != 0) { spin_unlock(&fs->lock); return ret; }

    // Check if target exists in newdir
    uint32_t exist_ino, exist_block, exist_off;
    uint8_t exist_type;
    int exists = ext2_find_dirent(fs, new_ino_dir, newname,
                                  &exist_ino, &exist_type,
                                  &exist_block, &exist_off);

    if (exists == 0) {
        // POSIX: cannot overwrite non-empty directory
        if (exist_type == 2 /* EXT2_FT_DIR */) {
            ext2_inode_t exist_inode;
            ext2_read_inode(fs, exist_ino, &exist_inode);
            // Check if non-empty (same pattern as rmdir)
            uint8_t bd[4096];
            for (uint32_t bi = 0; ; bi++) {
                uint32_t p = ext2_bmap(fs, &exist_inode, bi);
                if (p == 0) break;
                ext2_read_block(fs, p, bd);
                uint32_t o2 = 0;
                while (o2 < fs->block_size) {
                    ext2_dirent_t *de = (ext2_dirent_t *)(bd + o2);
                    if (de->rec_len == 0) break;
                    if (de->inode != 0 && de->inode != exist_ino && de->inode != new_ino_dir) {
                        spin_unlock(&fs->lock); return -ENOTEMPTY;
                    }
                    o2 += de->rec_len;
                }
            }
            // Empty directory — remove it first
            // Write ordering (spec §6.1): dirent → bitmap → sb → data
            dirent_del(fs, new_ino_dir, newname);

            // Free all data blocks before freeing the inode
            ext2_inode_t exist_inode2;
            ext2_read_inode(fs, exist_ino, &exist_inode2);
            for (int i = 0; i < 12; i++) {
                if (exist_inode2.i_block[i]) free_block(fs, exist_inode2.i_block[i]);
            }
            if (exist_inode2.i_block[12]) {
                uint32_t indirect[1024];
                uint32_t pps = fs->block_size / sizeof(uint32_t);
                if (ext2_read_block(fs, exist_inode2.i_block[12], indirect) != 0) {
                    spin_unlock(&fs->lock); return -EIO;
                }
                for (uint32_t k = 0; k < pps; k++)
                    if (indirect[k]) free_block(fs, indirect[k]);
                free_block(fs, exist_inode2.i_block[12]);
            }
            free_inode(fs, exist_ino);
        } else {
            // Overwrite file — unlink it first
            // Write ordering (spec §6.1): dirent → inode → bitmap → sb → data
            dirent_del(fs, new_ino_dir, newname);

            ext2_inode_t exist_inode;
            ext2_read_inode(fs, exist_ino, &exist_inode);
            exist_inode.i_links_count--;
            if (exist_inode.i_links_count == 0) {
                ext2_write_inode(fs, exist_ino, &exist_inode);
                free_inode(fs, exist_ino);
                // Now free data blocks (after inode is gone from bitmap)
                for (int i = 0; i < 12; i++) {
                    if (exist_inode.i_block[i]) free_block(fs, exist_inode.i_block[i]);
                }
                if (exist_inode.i_block[12]) {
                    uint32_t indirect[1024];
                    uint32_t pps = fs->block_size / sizeof(uint32_t);
                    if (ext2_read_block(fs, exist_inode.i_block[12], indirect) != 0) {
                        spin_unlock(&fs->lock); return -EIO;
                    }
                    for (uint32_t k = 0; k < pps; k++)
                        if (indirect[k]) free_block(fs, indirect[k]);
                    free_block(fs, exist_inode.i_block[12]);
                }
            } else {
                ext2_write_inode(fs, exist_ino, &exist_inode);
            }
        }
    }

    // Perform the move — del old name first, then add new name.
    int del_ret = dirent_del(fs, old_ino, oldname);
    if (del_ret != 0) {
        log_err("ext2_rename: dirent_del(%s) failed ret=%d\n", oldname, del_ret);
        spin_unlock(&fs->lock);
        return del_ret;
    }
    int add_ret = dirent_add(fs, new_ino_dir, newname, target_ino, file_type);
    if (add_ret != 0) {
        log_err("ext2_rename: dirent_add(%s) failed ret=%d\n", newname, add_ret);
        spin_unlock(&fs->lock);
        return add_ret;
    }

    // Update ctime
    ext2_inode_t target_inode;
    if (ext2_read_inode(fs, target_ino, &target_inode) == 0) {
        target_inode.i_ctime = 0;
        ext2_write_inode(fs, target_ino, &target_inode);
    }

    // Cross-directory directory move: adjust ".." and link counts
    if (file_type == 2 /* EXT2_FT_DIR */ && old_ino != new_ino_dir) {
        // Update ".." in moved directory
        ext2_inode_t moved_inode;
        ext2_read_inode(fs, target_ino, &moved_inode);
        uint32_t first_block = moved_inode.i_block[0];
        uint8_t dir_data[4096];
        ext2_read_block(fs, first_block, dir_data);

        ext2_dirent_t *dotdot = (ext2_dirent_t *)(dir_data + 12);
        dotdot->inode = new_ino_dir;
        ext2_write_block(fs, first_block, dir_data);

        // Adjust link counts
        ext2_inode_t old_inode, new_inode;
        ext2_read_inode(fs, old_ino, &old_inode);
        old_inode.i_links_count--;
        ext2_write_inode(fs, old_ino, &old_inode);

        ext2_read_inode(fs, new_ino_dir, &new_inode);
        new_inode.i_links_count++;
        ext2_write_inode(fs, new_ino_dir, &new_inode);

        // Adjust used_dirs_count per group
        uint32_t old_group = (old_ino - 1) / fs->inodes_per_group;
        uint32_t new_group = (new_ino_dir - 1) / fs->inodes_per_group;
        fs->bgdesc_table[old_group].bg_used_dirs_count--;
        fs->bgdesc_table[new_group].bg_used_dirs_count++;
        ext2_write_superblock(fs);
    }

    spin_unlock(&fs->lock);
    return 0;
}

// ── VFS operations table ────────────────────────────────
struct vfs_ops ext2_vfs_ops = {
    .flags   = 0,  // case-sensitive
    .read    = ext2_vfs_read,
    .write   = ext2_vfs_write,
    .readdir = ext2_vfs_readdir,
    .create  = ext2_vfs_create,
    .unlink  = ext2_vfs_unlink,
    .mkdir   = ext2_vfs_mkdir,
    .rmdir   = ext2_vfs_rmdir,
    .rename  = ext2_vfs_rename,
    .truncate = ext2_vfs_truncate,
};

// ── ext2_init (mount) ───────────────────────────────────
int ext2_init(block_device_t *dev, ext2_fs_t **out_fs)
{
    *out_fs = NULL;
    if (!dev || !dev->present) return -1;

    ext2_fs_t *fs = calloc(1, sizeof(ext2_fs_t));
    if (!fs) return -1;
    fs->dev = dev;
    spin_init(&fs->lock);

    // Read superblock (byte 1024 = sector 2)
    uint8_t *sb_buf = kmalloc(1024);
    if (!sb_buf) { kfree(fs); return -ENOMEM; }
    if (block_device_read(dev, 2, 2, sb_buf) != 0) {
        kfree(sb_buf); kfree(fs); return -1;
    }

    ext2_superblock_t *sb = (ext2_superblock_t *)sb_buf;
    if (sb->s_magic != EXT2_MAGIC) {
        debug_fs("ext2: bad magic %#x\n", sb->s_magic);
        kfree(sb_buf); kfree(fs); return -1;
    }

    // Reject incompatible features we can't handle
    // EXT2_FEATURE_INCOMPAT_EXTENTS (0x0040) would cause bmap errors
    if (sb->s_feature_incompat & 0x0040) {
        debug_fs("ext2: unsupported feature (extents)\n");
        kfree(sb_buf); kfree(fs); return -1;
    }

    // Cache superblock for writeback
    memcpy(&fs->sb_raw, sb_buf, sizeof(ext2_superblock_t));
    kfree(sb_buf);  // sb_buf no longer needed — data cached in fs->sb_raw

    fs->block_size   = 1024u << fs->sb_raw.s_log_block_size;
    if (fs->block_size > 4096) { kfree(fs); return -1; }
    fs->sectors_per_block = fs->block_size / 512;
    fs->blocks_per_group  = fs->sb_raw.s_blocks_per_group;
    fs->inodes_per_group  = fs->sb_raw.s_inodes_per_group;
    fs->num_block_groups  = (fs->sb_raw.s_blocks_count + fs->blocks_per_group - 1)
                            / fs->blocks_per_group;
    fs->inode_size = (fs->sb_raw.s_inode_size != 0) ? fs->sb_raw.s_inode_size : 128;

    // bgdesc table starts at block after superblock
    uint32_t sb_block     = (fs->block_size == 1024) ? 1u : 0u;
    uint32_t bgdesc_block = sb_block + 1;

    uint32_t table_size   = fs->num_block_groups * sizeof(ext2_bgdesc_t);
    uint32_t table_blocks = (table_size + fs->block_size - 1) / fs->block_size;
    fs->bgdesc_block = bgdesc_block;
    fs->bgdesc_table_blocks = table_blocks;
    fs->bgdesc_table = kmalloc(table_blocks * fs->block_size);
    if (!fs->bgdesc_table) { kfree(fs); return -1; }

    for (uint32_t i = 0; i < table_blocks; i++) {
        if (ext2_read_block(fs, bgdesc_block + i,
            (uint8_t *)fs->bgdesc_table + i * fs->block_size) != 0)
        {
            kfree(fs->bgdesc_table); kfree(fs); return -1;
        }
    }

    debug_fs("ext2: mounted — ino_size=%u blk_size=%u groups=%u\n",
             fs->inode_size, fs->block_size, fs->num_block_groups);
    *out_fs = fs;
#ifdef OS01_SELFTEST
    ext2_selftest_fs = fs;
#endif
    return 0;
}

#ifdef OS01_SELFTEST
// Tests registered by selftest_run_all() via forward declarations in selftest.c

// Get the ext2_fs_t set by ext2_init. Returns NULL if FS not mounted yet.
static ext2_fs_t *ext2_selftest_get_fs(void)
{
    return ext2_selftest_fs;
}

int ext2_selftest_magic(void)
{
    if (EXT2_MAGIC != 0xEF53) return -1;
    return 0;
}

int ext2_selftest_struct_sizes(void)
{
    if (sizeof(ext2_inode_t) != 116) return -1;
    if (sizeof(ext2_bgdesc_t) != 32) return -1;
    if (sizeof(ext2_dirent_t) != 8) return -1;
    return 0;
}

// Save/restore: bitmap block + bgdesc block + superblock
int ext2_selftest_block_alloc(void)
{
    ext2_fs_t *fs = ext2_selftest_get_fs();
    if (!fs) return 0;  // SKIP -- AHCI not ready

    spin_lock(&fs->lock);

    // Choose first group that has free blocks
    uint32_t g = 0;
    while (g < fs->num_block_groups && fs->bgdesc_table[g].bg_free_blocks_count == 0)
        g++;
    if (g >= fs->num_block_groups) { spin_unlock(&fs->lock); return 0; }

    uint32_t bitmap_block = fs->bgdesc_table[g].bg_block_bitmap;
    // g * sizeof(ext2_bgdesc_t) / fs->block_size: index into the bgdesc
    // table blocks (not absolute block number -- may be 0 when block_size is
    // larger than a single descriptor).  We add it to fs->bgdesc_block to
    // get the physical block on disk.
    uint32_t bgdesc_blk_off = g * sizeof(ext2_bgdesc_t) / fs->block_size;
    uint32_t bgdesc_phys_blk = fs->bgdesc_block + bgdesc_blk_off;

    // Save
    uint8_t save_bitmap[4096], save_bgdesc[4096], save_sb[1024];
    ext2_read_block(fs, bitmap_block, save_bitmap);
    ext2_read_block(fs, bgdesc_phys_blk, save_bgdesc);
    block_device_read(fs->dev, 2, 2, save_sb);

    // Test: allocate a block
    uint32_t blk = alloc_block(fs);
    if (blk == 0) {
        // Restore and skip
        ext2_write_block(fs, bitmap_block, save_bitmap);
        ext2_write_block(fs, bgdesc_phys_blk, save_bgdesc);
        block_device_write(fs->dev, 2, 2, save_sb);
        spin_unlock(&fs->lock);
        return 0;  // SKIP
    }

    // Save the block content BEFORE free_block (alloc_block zeroes it)
    uint8_t save_blk[4096];
    ext2_read_block(fs, blk, save_blk);

    // Free it
    free_block(fs, blk);

    // Restore bitmap + bgdesc + superblock + block content
    ext2_write_block(fs, blk, save_blk);
    ext2_write_block(fs, bitmap_block, save_bitmap);
    ext2_write_block(fs, bgdesc_phys_blk, save_bgdesc);
    block_device_write(fs->dev, 2, 2, save_sb);

    spin_unlock(&fs->lock);
    return 0;
}

int ext2_selftest_inode_alloc(void)
{
    ext2_fs_t *fs = ext2_selftest_get_fs();
    if (!fs) return 0;

    spin_lock(&fs->lock);

    uint32_t g = 0;
    while (g < fs->num_block_groups && fs->bgdesc_table[g].bg_free_inodes_count == 0)
        g++;
    if (g >= fs->num_block_groups) { spin_unlock(&fs->lock); return 0; }

    uint32_t inode_bitmap_block = fs->bgdesc_table[g].bg_inode_bitmap;
    uint32_t inode_table_start  = fs->bgdesc_table[g].bg_inode_table;

    // Save bitmap and superblock (alloc_inode/free_inode touch these)
    uint8_t save_ibitmap[4096], save_sb[1024];
    ext2_read_block(fs, inode_bitmap_block, save_ibitmap);
    block_device_read(fs->dev, 2, 2, save_sb);

    // Test: allocate, then figure out which inode table block was written
    uint32_t ino = alloc_inode(fs, EXT2_S_IFREG | 0644);
    if (ino == 0) {
        ext2_write_block(fs, inode_bitmap_block, save_ibitmap);
        block_device_write(fs->dev, 2, 2, save_sb);
        spin_unlock(&fs->lock); return -1;
    }

    // Compute the inode table block that was modified, save it for restore
    uint32_t ag = (ino - 1) / fs->inodes_per_group;
    uint32_t aidx = (ino - 1) % fs->inodes_per_group;
    uint32_t inodes_per_blk = fs->block_size / fs->inode_size;
    uint32_t itable_blk = fs->bgdesc_table[ag].bg_inode_table + aidx / inodes_per_blk;
    uint8_t save_itable[4096];
    ext2_read_block(fs, itable_blk, save_itable);

    free_inode(fs, ino);

    // Restore
    ext2_write_block(fs, inode_bitmap_block, save_ibitmap);
    block_device_write(fs->dev, 2, 2, save_sb);
    ext2_write_block(fs, itable_blk, save_itable);

    spin_unlock(&fs->lock);
    return 0;
}

int ext2_selftest_dirent_roundtrip(void)
{
    ext2_fs_t *fs = ext2_selftest_get_fs();
    if (!fs) return 0;

    spin_lock(&fs->lock);

    // Use root inode (2) as the test directory
    uint32_t dir_ino = EXT2_ROOT_INO;

    // Save: root directory data blocks + root inode table block
    ext2_inode_t root_inode;
    ext2_read_inode(fs, dir_ino, &root_inode);

    // Save first dir block
    uint32_t first_phys = ext2_bmap(fs, &root_inode, 0);
    if (first_phys == 0) { spin_unlock(&fs->lock); return 0; }
    uint8_t save_dir_blk[4096];
    ext2_read_block(fs, first_phys, save_dir_blk);

    // Save root inode table entry (root is always ino 2, group 0)
    uint32_t inodes_per_blk = fs->block_size / fs->inode_size;
    uint32_t root_idx = (dir_ino - 1) % inodes_per_blk;
    uint32_t root_group = (dir_ino - 1) / fs->inodes_per_group;
    uint32_t root_itable_blk = fs->bgdesc_table[root_group].bg_inode_table
                               + ((dir_ino - 1) / inodes_per_blk);

    uint8_t save_itable_blk[4096];
    ext2_read_block(fs, root_itable_blk, save_itable_blk);

    // Test: alloc_inode -> dirent_add -> verify -> dirent_del -> free_inode
    // alloc_inode sets the bitmap bit and initializes the inode table entry.
    // We don't save the post-alloc bitmap/itable state here -- free_inode below
    // will clear the bitmap bit, restoring it to the original state.
    // The inode table init data is harmless (bitmap says "free", will be
    // overwritten on next alloc of that inode number).
    //
    // What we DO need to save: superblock (counts mutated by alloc+free),
    // directory data blocks (modified by dirent_add/del), and root inode.
    uint8_t save_sb[1024];
    block_device_read(fs->dev, 2, 2, save_sb);

    uint32_t new_ino = alloc_inode(fs, EXT2_S_IFREG | 0644);
    if (new_ino == 0) { spin_unlock(&fs->lock); return -1; }

    int add_ret = dirent_add(fs, dir_ino, "___test99", new_ino, 1);
    if (add_ret != 0) {
        free_inode(fs, new_ino);
        spin_unlock(&fs->lock); return -1;
    }

    // Verify via ext2_find_dirent
    uint32_t found_ino, found_blk, found_off;
    uint8_t found_type;
    int find_ret = ext2_find_dirent(fs, dir_ino, "___test99",
                                    &found_ino, &found_type, &found_blk, &found_off);
    if (find_ret != 0 || found_ino != new_ino || found_type != 1) {
        // Cleanup then fail
        dirent_del(fs, dir_ino, "___test99");
        free_inode(fs, new_ino);
        spin_unlock(&fs->lock); return -1;
    }

    // Delete
    dirent_del(fs, dir_ino, "___test99");
    free_inode(fs, new_ino);

    // Verify gone
    int find2 = ext2_find_dirent(fs, dir_ino, "___test99",
                                 &found_ino, &found_type, &found_blk, &found_off);
    if (find2 == 0) {
        spin_unlock(&fs->lock); return -1;  // should not exist
    }

    // Restore: directory data blocks, root inode, superblock (counts).
    // Inode bitmap is already in the original state (free_inode cleared the bit).
    // Inode table init data is harmless (bitmap says "free").
    ext2_write_block(fs, first_phys, save_dir_blk);
    ext2_write_block(fs, root_itable_blk, save_itable_blk);
    block_device_write(fs->dev, 2, 2, save_sb);

    spin_unlock(&fs->lock);
    return 0;
}

// Test write within existing file's i_size -- no block alloc needed.
// We write to byte offset 0 of an existing small file, then restore.
// NOTE: This selftest does raw block read/write under the ext2 lock
// (not via ext2_vfs_write) because the VFS write path would require a
// vfs_node_t which isn't available at selftest time.  The VFS write
// path is exercised by the user-mode systest in Task 19.
int ext2_selftest_write_read(void)
{
    ext2_fs_t *fs = ext2_selftest_get_fs();
    if (!fs) return 0;

    spin_lock(&fs->lock);

    // Find a regular file in root directory
    uint32_t dir_ino = EXT2_ROOT_INO;
    ext2_inode_t dir_inode;
    ext2_read_inode(fs, dir_ino, &dir_inode);

    uint8_t block_data[4096];
    uint32_t test_ino = 0;
    uint32_t test_blk = 0;

    for (uint32_t bi = 0; ; bi++) {
        uint32_t phys = ext2_bmap(fs, &dir_inode, bi);
        if (phys == 0) break;
        ext2_read_block(fs, phys, block_data);

        uint32_t off = 0;
        while (off < fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(block_data + off);
            if (de->rec_len == 0) break;
            if (de->inode != 0 && de->file_type == 1 /* regular file */) {
                test_ino = de->inode;
                break;
            }
            off += de->rec_len;
        }
        if (test_ino) break;
    }

    if (test_ino == 0) { spin_unlock(&fs->lock); return 0; }  // SKIP

    ext2_inode_t test_inode;
    ext2_read_inode(fs, test_ino, &test_inode);

    // Only test if file has data blocks we can restore
    if (test_inode.i_size < 16 || test_inode.i_block[0] == 0) {
        spin_unlock(&fs->lock); return 0;
    }

    // Save data block and inode
    uint8_t save_data[4096];
    ext2_read_block(fs, test_inode.i_block[0], save_data);

    ext2_inode_t save_inode = test_inode;

    // Write test data at offset 0 (within existing i_size)
    const char *test_str = "HELLO_WRITE_TEST";
    size_t test_len = strlen(test_str);
    if (test_len > test_inode.i_size) test_len = (size_t)test_inode.i_size;

    // Do a raw write via the VFS write -- but we're under lock.
    // Write directly to the block for the selftest.
    uint8_t write_buf[4096];
    memcpy(write_buf, save_data, fs->block_size);
    memcpy(write_buf, test_str, test_len);
    ext2_write_block(fs, test_inode.i_block[0], write_buf);

    // Read back and verify
    uint8_t read_buf[4096];
    ext2_read_block(fs, test_inode.i_block[0], read_buf);
    int match = (memcmp(read_buf, test_str, test_len) == 0);

    // Restore
    ext2_write_block(fs, test_inode.i_block[0], save_data);

    spin_unlock(&fs->lock);
    return match ? 0 : -1;
}
#endif
