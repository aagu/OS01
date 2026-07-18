// kernel/fs/ext2.c
#include <fs/ext2.h>
#include <kernel/debug.h>
#include <kernel/slab.h>     // kmalloc, kfree
#include <string.h>
#include <stdlib.h>          // calloc
#include <errno.h>

// ── Helper: resolve node->fs_data to inode number ────────
// Root mount node has fs_data=NULL (set by vfs_mount).
// Subdirectory nodes have fs_data=(void*)(uintptr_t)ino.
static uint32_t ext2_node_ino(vfs_node_t *node)
{
    if (!node->fs_data) return EXT2_ROOT_INO;  // root inode is 2
    return (uint32_t)(uintptr_t)node->fs_data;
}

// ── Block I/O helper ───────────────────────────────────
static int ext2_read_block(ext2_fs_t *fs, uint32_t block, void *buf)
{
    uint64_t lba = (uint64_t)block * fs->sectors_per_block;
    return block_device_read(fs->dev, lba, fs->sectors_per_block, buf);
}

// ── Block I/O helper (write) ───────────────────────────
static int ext2_write_block(ext2_fs_t *fs, uint32_t block, const void *buf)
{
    uint64_t lba = (uint64_t)block * fs->sectors_per_block;
    return block_device_write(fs->dev, lba, fs->sectors_per_block, buf);
}

// ── Read an inode from disk ─────────────────────────────
static int ext2_read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out)
{
    if (ino == 0) return -1;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;
    if (group >= fs->num_block_groups) return -1;

    uint32_t table_start    = fs->bgdesc_table[group].bg_inode_table;
    uint32_t inodes_per_blk = fs->block_size / fs->inode_size;
    uint32_t block_off      = index / inodes_per_blk;
    uint32_t inode_off      = (index % inodes_per_blk) * fs->inode_size;

    // Inode may cross a sector boundary.  ext2_read_block reads
    // fs->sectors_per_block * 512 bytes (up to 4096 with 4KB blocks)
    // regardless of the caller's buffer size — so we need the full
    // block-sized buffer here, same as every other ext2 function.
    uint8_t buf[4096];
    if (ext2_read_block(fs, table_start + block_off, buf) != 0)
        return -1;

    memcpy(out, buf + inode_off, sizeof(ext2_inode_t));
    return 0;
}

// ── Write an inode to disk ─────────────────────────────
static int ext2_write_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode)
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
    uint8_t buf[4096];
    if (ext2_read_block(fs, table_start + block_off, buf) != 0)
        return -1;

    memcpy(buf + inode_off, inode, sizeof(ext2_inode_t));
    return ext2_write_block(fs, table_start + block_off, buf);
}

// ── Write superblock and bgdesc table to disk ──────────
static int ext2_write_superblock(ext2_fs_t *fs)
{
    // ext2_superblock_t is ~204 bytes packed, but the on-disk
    // superblock occupies 1024 bytes.  Serialize into a zero-filled
    // 1024-byte buffer to avoid writing stack/junk beyond the struct.
    uint8_t sb_buf[1024];
    memset(sb_buf, 0, 1024);
    memcpy(sb_buf, &fs->sb_raw, sizeof(ext2_superblock_t));
    if (block_device_write(fs->dev, 2, 2, sb_buf) != 0)
        return -1;

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
static uint32_t alloc_block(ext2_fs_t *fs)
{
    for (uint32_t g = 0; g < fs->num_block_groups; g++) {
        if (fs->bgdesc_table[g].bg_free_blocks_count == 0)
            continue;

        uint32_t bitmap_block = fs->bgdesc_table[g].bg_block_bitmap;
        uint8_t buf[4096];
        if (ext2_read_block(fs, bitmap_block, buf) != 0)
            continue;

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

                    uint8_t zero[4096];
                    memset(zero, 0, fs->block_size);
                    ext2_write_block(fs, block, zero);

                    return block;
                }
            }
        }
    }
    return 0;
}

// ── Free a data block ──────────────────────────────────
static void free_block(ext2_fs_t *fs, uint32_t block)
{
    if (block == 0) return;

    // Block numbers are 0-based: group = block / blocks_per_group
    uint32_t group = block / fs->blocks_per_group;
    if (group >= fs->num_block_groups) return;

    uint32_t index = block % fs->blocks_per_group;
    uint32_t byte_idx = index / 8;
    uint32_t bit      = index % 8;

    uint32_t bitmap_block = fs->bgdesc_table[group].bg_block_bitmap;
    uint8_t buf[4096];
    if (ext2_read_block(fs, bitmap_block, buf) != 0)
        return;

    buf[byte_idx] &= ~(1u << bit);
    ext2_write_block(fs, bitmap_block, buf);

    fs->bgdesc_table[group].bg_free_blocks_count++;
    fs->sb_raw.s_free_blocks_count++;
    ext2_write_superblock(fs);
}

// ── Allocate and initialize an inode ────────────────────
// Returns inode number or 0 on failure.
static uint32_t alloc_inode(ext2_fs_t *fs, uint16_t mode)
{
    for (uint32_t g = 0; g < fs->num_block_groups; g++) {
        if (fs->bgdesc_table[g].bg_free_inodes_count == 0)
            continue;

        uint32_t bitmap_block = fs->bgdesc_table[g].bg_inode_bitmap;
        uint8_t buf[4096];
        if (ext2_read_block(fs, bitmap_block, buf) != 0)
            continue;

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
    }
    return 0;
}

// ── Free an inode ──────────────────────────────────────
static void free_inode(ext2_fs_t *fs, uint32_t ino)
{
    if (ino == 0) return;

    uint32_t group = (ino - 1) / fs->inodes_per_group;
    if (group >= fs->num_block_groups) return;

    uint32_t index = (ino - 1) % fs->inodes_per_group;
    uint32_t byte_idx = index / 8;
    uint32_t bit      = index % 8;

    uint32_t bitmap_block = fs->bgdesc_table[group].bg_inode_bitmap;
    uint8_t buf[4096];
    if (ext2_read_block(fs, bitmap_block, buf) != 0)
        return;

    buf[byte_idx] &= ~(1u << bit);
    ext2_write_block(fs, bitmap_block, buf);

    fs->bgdesc_table[group].bg_free_inodes_count++;
    fs->sb_raw.s_free_inodes_count++;
    ext2_write_superblock(fs);
}

// ── Map logical block → physical (direct + single indirect) ─
static uint32_t ext2_bmap(ext2_fs_t *fs, ext2_inode_t *inode,
                          uint32_t logical_block)
{
    if (logical_block < 12)
        return inode->i_block[logical_block];

    uint32_t ptrs_per_block = fs->block_size / sizeof(uint32_t);
    if (logical_block < 12 + ptrs_per_block) {
        uint32_t indirect_blk = inode->i_block[12];
        if (indirect_blk == 0) return 0;

        uint32_t indirect[1024];  // up to 4096/4 = 1024 entries
        if (ext2_read_block(fs, indirect_blk, indirect) != 0)
            return 0;
        return indirect[logical_block - 12];
    }

    return 0;  // double/triple indirect not implemented
}

// ── Map logical block → physical (allocating variant) ──
// Like ext2_bmap but allocates blocks as needed.  Only modifies
// the in-memory inode; caller must call ext2_write_inode to persist.
static uint32_t ext2_bmap_alloc(ext2_fs_t *fs, ext2_inode_t *inode,
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

        uint32_t indirect[1024];
        if (ext2_read_block(fs, inode->i_block[12], indirect) != 0)
            return 0;

        uint32_t idx = logical_block - 12;
        if (indirect[idx] == 0) {
            indirect[idx] = alloc_block(fs);
            if (indirect[idx] == 0) return 0;
            inode->i_blocks += fs->block_size / 512;
            ext2_write_block(fs, inode->i_block[12], indirect);
        }
        return indirect[idx];
    }

    return 0;  // double/triple indirect not supported
}

// ── Find a directory entry by name ──────────────────────
static int ext2_find_dirent(ext2_fs_t *fs, uint32_t dir_ino, const char *name,
                            uint32_t *out_ino, uint8_t *out_file_type,
                            uint32_t *out_block, uint32_t *out_off)
{
    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, dir_ino, &dir_inode) != 0)
        return -EIO;
    if (!(dir_inode.i_mode & EXT2_S_IFDIR))
        return -ENOTDIR;

    size_t name_len = strlen(name);
    uint8_t block_data[4096];

    for (uint32_t blk_idx = 0; ; blk_idx++) {
        uint32_t phys = ext2_bmap(fs, &dir_inode, blk_idx);
        if (phys == 0) break;
        if (ext2_read_block(fs, phys, block_data) != 0) break;

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
                return 0;
            }
            off += de->rec_len;
        }
    }
    return -ENOENT;
}

// ── VFS read implementation ─────────────────────────────
static int ext2_vfs_read(struct vfs_node *node, uint64_t offset,
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

// ── VFS readdir implementation ──────────────────────────
static int ext2_vfs_readdir(struct vfs_node *node, uint64_t index,
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

// ── VFS operations table ────────────────────────────────
struct vfs_ops ext2_vfs_ops = {
    .flags   = 0,  // case-sensitive
    .read    = ext2_vfs_read,
    .write   = NULL,
    .readdir = ext2_vfs_readdir,
    .create  = NULL,
    .unlink  = NULL,
    .mkdir   = NULL,
    .rmdir   = NULL,
    .rename  = NULL,
    .truncate = NULL,
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
    uint8_t sb_buf[1024];
    if (block_device_read(dev, 2, 2, sb_buf) != 0) {
        kfree(fs); return -1;
    }

    ext2_superblock_t *sb = (ext2_superblock_t *)sb_buf;
    if (sb->s_magic != EXT2_MAGIC) {
        debug_fs("ext2: bad magic %#x\n", sb->s_magic);
        kfree(fs); return -1;
    }

    // Reject incompatible features we can't handle
    // EXT2_FEATURE_INCOMPAT_EXTENTS (0x0040) would cause bmap errors
    if (sb->s_feature_incompat & 0x0040) {
        debug_fs("ext2: unsupported feature (extents)\n");
        kfree(fs); return -1;
    }

    // Cache superblock for writeback
    memcpy(&fs->sb_raw, sb_buf, sizeof(ext2_superblock_t));

    fs->block_size   = 1024u << sb->s_log_block_size;
    if (fs->block_size > 4096) { kfree(fs); return -1; }
    fs->sectors_per_block = fs->block_size / 512;
    fs->blocks_per_group  = sb->s_blocks_per_group;
    fs->inodes_per_group  = sb->s_inodes_per_group;
    fs->num_block_groups  = (sb->s_blocks_count + fs->blocks_per_group - 1)
                            / fs->blocks_per_group;
    fs->inode_size = (sb->s_inode_size != 0) ? sb->s_inode_size : 128;

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
    return 0;
}

#ifdef OS01_SELFTEST
// Tests registered by selftest_run_all() via forward declarations in selftest.c

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
#endif
